#!/usr/bin/env bash
# Streaming replication, hot standby and point-in-time recovery checks.
#
# Builds three scratch clusters under $BASE (deleted first!):
#   primary   wal_level=replica, WAL archiving, wal_consistency_checking=generic
#             (every generic WAL record carries a page image that the standby
#             compares with its own replay result)
#   standby   streaming from the primary, hot_standby_feedback OFF and a short
#             max_standby_streaming_delay, so that page reuse on the primary
#             must be handled by recovery conflicts
#   pitr      restored from a base backup and the WAL archive to a named
#             restore point in the middle of the workload
#
# While the primary runs inserts, updates, deletes, merges, compactions and
# VACUUM, clients on the standby continuously check rows nobody modifies.
# A wrong answer fails the run; a query cancelled by a recovery conflict is
# the expected behaviour and is only counted.
#
#   PGBIN=/usr/lib/postgresql/16/bin BASE=/tmp/stomata_repl tools/replication.sh
set -euo pipefail

: "${PGBIN:?set PGBIN to the server bin directory}"
BASE="${BASE:-/tmp/stomata_repl}"
P1="${P1:-5501}"; P2="${P2:-5502}"; P3="${P3:-5503}"
DURATION="${DURATION:-40}"
HOST=/tmp
DB=repl

fail() { echo "FAIL: $*"; exit 1; }
pg() { local port=$1; shift; "$PGBIN/psql" -h $HOST -p "$port" -X -At -v ON_ERROR_STOP=1 -d $DB "$@"; }
stop_all() {
  for d in primary standby pitr; do
    [ -f "$BASE/$d/postmaster.pid" ] && "$PGBIN/pg_ctl" -D "$BASE/$d" -m immediate stop >/dev/null 2>&1 || true
  done
}
trap stop_all EXIT
stop_all
rm -rf "$BASE"; mkdir -p "$BASE/archive"

echo "== primary"
"$PGBIN/initdb" -D "$BASE/primary" -E UTF8 --no-locale -A trust >/dev/null
cat >> "$BASE/primary/postgresql.conf" <<EOF
port = $P1
unix_socket_directories = '$HOST'
wal_level = replica
max_wal_senders = 5
wal_keep_size = '1GB'
archive_mode = on
archive_command = 'cp %p $BASE/archive/%f'
wal_consistency_checking = 'generic'
autovacuum_naptime = 5
shared_buffers = 128MB
EOF
echo "local replication all trust" >> "$BASE/primary/pg_hba.conf"
"$PGBIN/pg_ctl" -D "$BASE/primary" -l "$BASE/primary.log" start -w >/dev/null
"$PGBIN/psql" -h $HOST -p $P1 -X -q -d postgres -c "CREATE DATABASE $DB"
pg $P1 -q <<'SQL'
CREATE EXTENSION stomata;
CREATE TABLE t (id serial PRIMARY KEY, s text);
INSERT INTO t (s) SELECT md5(i::text) FROM generate_series(1, 60000) i;
INSERT INTO t (s) SELECT 'stable-' || i FROM generate_series(1, 3000) i;
CREATE INDEX t_s ON t USING stomata (s) WITH (pending_limit = 64);
CREATE FUNCTION stable_check() RETURNS text LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint; c bigint;
BEGIN
  SELECT count(*) INTO a FROM t WHERE s LIKE 'stable-%';
  SELECT count(*) INTO b FROM t WHERE s LIKE 'stable-1__';
  SELECT count(*) INTO c FROM t WHERE s ILIKE '%STABLE-2%';
  IF a <> 3000 OR b <> 100 OR c <> 1111 THEN
    RAISE EXCEPTION 'WRONG RESULT: % % %', a, b, c;
  END IF;
  RETURN 'ok';
END $$;
-- a second, insert-only table: its index pages are recycled by merges, and
-- nothing else (no dead heap rows) causes recovery conflicts on the standby
CREATE TABLE t2 (id serial PRIMARY KEY, s text);
INSERT INTO t2 (s) SELECT md5(i::text) FROM generate_series(1, 30000) i;
INSERT INTO t2 (s) SELECT 'stable-' || i FROM generate_series(1, 3000) i;
CREATE INDEX t2_s ON t2 USING stomata (s) WITH (pending_limit = 16);
CREATE FUNCTION stable2_check() RETURNS text LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint;
BEGIN
  SELECT count(*) INTO a FROM t2 WHERE s LIKE 'stable-%';
  SELECT count(*) INTO b FROM t2 WHERE s LIKE 'stable-1__';
  IF a <> 3000 OR b <> 100 THEN
    RAISE EXCEPTION 'WRONG RESULT: % %', a, b;
  END IF;
  RETURN 'ok';
END $$;
-- index vs sequential scan for many patterns; usable on a read-only standby
CREATE FUNCTION idx_vs_seq() RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE p text; a bigint; b bigint; bad bigint := 0;
BEGIN
  FOR p IN SELECT DISTINCT 'c' || (i % 997) || '-%' FROM generate_series(1, 997) i
           UNION SELECT '%' || substr(md5(i::text), 1, 3) || '%' FROM generate_series(1, 150) i
           UNION SELECT unnest(ARRAY['%u', '%uu', 'stable-1%', '%-__', 'promoted-%', 'after-pitr-%'])
  LOOP
    PERFORM set_config('enable_bitmapscan', 'off', true);
    PERFORM set_config('enable_seqscan', 'on', true);
    EXECUTE format('SELECT count(*) FROM t WHERE s LIKE %L', p) INTO a;
    PERFORM set_config('enable_bitmapscan', 'on', true);
    PERFORM set_config('enable_seqscan', 'off', true);
    EXECUTE format('SELECT count(*) FROM t WHERE s LIKE %L', p) INTO b;
    IF a <> b THEN
      bad := bad + 1;
      RAISE NOTICE 'mismatch for %: seq % index %', p, a, b;
    END IF;
  END LOOP;
  RETURN bad;
END $$;
CHECKPOINT;
SQL

echo "== standby and PITR base backup"
"$PGBIN/pg_basebackup" -h $HOST -p $P1 -D "$BASE/standby" -R -X stream -c fast
cat >> "$BASE/standby/postgresql.conf" <<EOF
port = $P2
hot_standby = on
hot_standby_feedback = off
max_standby_streaming_delay = 500ms
archive_mode = off
EOF
"$PGBIN/pg_ctl" -D "$BASE/standby" -l "$BASE/standby.log" start -w >/dev/null
"$PGBIN/pg_basebackup" -h $HOST -p $P1 -D "$BASE/backup" -X none -c fast

echo "== workload on the primary, checks on the standby (${DURATION} s)"
TMP="$BASE/tmp"; mkdir -p "$TMP"
cat > "$TMP/ins.sql" <<'SQL'
\set r random(1, 1000000)
INSERT INTO t (s) VALUES ('c' || :r || '-' || md5(:r::text));
SQL
cat > "$TMP/upd.sql" <<'SQL'
\set r random(1, 59990)
UPDATE t SET s = s || 'u' WHERE id = :r;
DELETE FROM t WHERE id = :r + 1;
SQL
PGB="$PGBIN/pgbench -h $HOST -p $P1 -n -T $DURATION"
$PGB -c 4 -j 2 -f "$TMP/ins.sql@3" -f "$TMP/upd.sql@2" $DB > "$TMP/pgb.log" 2>&1 &
( end=$((SECONDS + DURATION)); while [ $SECONDS -lt $end ]; do
    pg $P1 -q -c "SELECT stomata_merge_pending('t_s')" >/dev/null; sleep 0.3; done ) &
( end=$((SECONDS + DURATION)); while [ $SECONDS -lt $end ]; do
    sleep 2; pg $P1 -q -c "SELECT stomata_compact('t_s')" >/dev/null; done ) &
( end=$((SECONDS + DURATION)); while [ $SECONDS -lt $end ]; do
    sleep 5; pg $P1 -q -c "VACUUM t" >/dev/null; done ) &
( sleep $((DURATION / 2)); pg $P1 -q -c "SELECT pg_create_restore_point('mid')" >/dev/null ) &
for c in 1 2 3; do
  ( end=$((SECONDS + DURATION)); while [ $SECONDS -lt $end ]; do
      "$PGBIN/psql" -h $HOST -p $P2 -X -At -d $DB -c "SET enable_seqscan = off" \
        -c "SELECT stable_check()" 2>&1 | grep -v '^SET$' || true
    done ) > "$TMP/standby_$c.log" &
done
wait
grep -q "number of failed transactions: 0" "$TMP/pgb.log" || { cat "$TMP/pgb.log"; fail "primary workload"; }
ok=$(cat "$TMP"/standby_*.log | grep -c '^ok$' || true)
errs="$(cat "$TMP"/standby_*.log | grep -E 'ERROR|FATAL' || true)"
conflicts=$(printf '%s\n' "$errs" | grep -c 'conflict with recovery' || true)
wrong=$(printf '%s\n' "$errs" | grep -c 'WRONG RESULT\|corrupt' || true)
other=$(printf '%s\n' "$errs" | grep -v '^$' | grep -vc 'conflict with recovery\|WRONG RESULT\|corrupt' || true)
echo "standby checks: $ok correct, $conflicts cancelled by recovery conflicts, $wrong wrong, $other other errors"
[ "$wrong" = 0 ] || { grep -h 'WRONG\|corrupt' "$TMP"/standby_*.log | head; fail "wrong results on the standby"; }
[ "$other" = 0 ] || { printf '%s\n' "$errs" | grep -v 'conflict with recovery' | sort | uniq -c | head; fail "unexpected errors on the standby"; }
[ "$ok" -gt 0 ] || fail "no standby check succeeded"
grep -q "conflict with recovery" "$TMP"/standby_*.log && echo "(cancellations are expected: hot_standby_feedback is off)"

echo "== page reuse under slow standby scans (insert-only, ${DURATION2:-20} s)"
# Standby scans pause for a second after reading the run list, while the
# primary merges and compacts continuously and so recycles index pages.  Only
# the index's recovery-conflict records can protect those scans.
D2="${DURATION2:-20}"
cat > "$TMP/ins2.sql" <<'SQL'
\set r random(1, 1000000)
INSERT INTO t2 (s) VALUES ('n' || :r || '-' || md5(:r::text));
SQL
"$PGBIN/pgbench" -h $HOST -p $P1 -n -T $D2 -c 2 -f "$TMP/ins2.sql" $DB > "$TMP/pgb2.log" 2>&1 &
( end=$((SECONDS + D2)); while [ $SECONDS -lt $end ]; do
    pg $P1 -q -c "SELECT stomata_merge_pending('t2_s')" >/dev/null; sleep 0.2; done ) &
( end=$((SECONDS + D2)); while [ $SECONDS -lt $end ]; do
    sleep 0.7; pg $P1 -q -c "SELECT stomata_compact('t2_s')" >/dev/null; done ) &
for c in 1 2 3; do
  ( end=$((SECONDS + D2)); while [ $SECONDS -lt $end ]; do
      "$PGBIN/psql" -h $HOST -p $P2 -X -At -d $DB -c "SET stomata.test_scan_delay_ms = 1000" \
        -c "SET enable_seqscan = off" -c "SELECT stable2_check()" 2>&1 | grep -v '^SET$' || true
    done ) > "$TMP/slow_$c.log" &
done
wait
errs="$(cat "$TMP"/slow_*.log | grep -E 'ERROR|FATAL' || true)"
ok=$(cat "$TMP"/slow_*.log | grep -c '^ok$' || true)
conflicts=$(printf '%s\n' "$errs" | grep -c 'conflict with recovery' || true)
wrong=$(printf '%s\n' "$errs" | grep -c 'WRONG RESULT\|corrupt' || true)
other=$(printf '%s\n' "$errs" | grep -v '^$' | grep -vc 'conflict with recovery\|WRONG RESULT\|corrupt' || true)
echo "slow standby scans: $ok correct, $conflicts cancelled by recovery conflicts, $wrong wrong, $other other errors"
[ "$wrong" = 0 ] || { printf '%s\n' "$errs" | grep 'WRONG\|corrupt' | sort | uniq -c | head; fail "wrong results on the standby"; }
[ "$other" = 0 ] || { printf '%s\n' "$errs" | grep -v 'conflict with recovery' | sort | uniq -c | head; fail "unexpected errors on the standby"; }
[ "$conflicts" -gt 0 ] || fail "expected recovery conflicts from index page reuse"

echo "== the same with hot_standby_feedback = on (${D2} s)"
pg $P2 -q -c "ALTER SYSTEM SET hot_standby_feedback = on" -c "SELECT pg_reload_conf()" >/dev/null
sleep 2
"$PGBIN/pgbench" -h $HOST -p $P1 -n -T $D2 -c 2 -f "$TMP/ins2.sql" $DB > "$TMP/pgb3.log" 2>&1 &
( end=$((SECONDS + D2)); while [ $SECONDS -lt $end ]; do
    pg $P1 -q -c "SELECT stomata_merge_pending('t2_s')" >/dev/null; sleep 0.2; done ) &
( end=$((SECONDS + D2)); while [ $SECONDS -lt $end ]; do
    sleep 0.7; pg $P1 -q -c "SELECT stomata_compact('t2_s')" >/dev/null; done ) &
for c in 1 2 3; do
  ( end=$((SECONDS + D2)); while [ $SECONDS -lt $end ]; do
      "$PGBIN/psql" -h $HOST -p $P2 -X -At -d $DB -c "SET stomata.test_scan_delay_ms = 1000" \
        -c "SET enable_seqscan = off" -c "SELECT stable2_check()" 2>&1 | grep -v '^SET$' || true
    done ) > "$TMP/fb_$c.log" &
done
wait
errs="$(cat "$TMP"/fb_*.log | grep -E 'ERROR|FATAL' || true)"
ok=$(cat "$TMP"/fb_*.log | grep -c '^ok$' || true)
nerr=$(printf '%s\n' "$errs" | grep -vc '^$' || true)
echo "slow standby scans with feedback: $ok correct, $nerr errors"
[ "$nerr" = 0 ] || { printf '%s\n' "$errs" | sort | uniq -c | head; fail "errors on the standby with hot_standby_feedback"; }
[ "$ok" -gt 0 ] || fail "no slow standby scan completed"
pg $P2 -q -c "ALTER SYSTEM RESET hot_standby_feedback" -c "SELECT pg_reload_conf()" >/dev/null
pg $P1 -q -c "CHECKPOINT" -c "SELECT pg_switch_wal()" >/dev/null

# compare index results with sequential scans on one cluster
check_cluster() {
  local port=$1 label=$2
  [ "$(pg $port -c "SELECT stomata_verify('t_s')")" = 0 ] || fail "$label: stomata_verify"
  local mm
  mm="$(pg $port -c "SELECT idx_vs_seq()")"
  [ "$mm" = 0 ] || fail "$label: $mm patterns differ between index and sequential scan"
  echo "$label: index agrees with the heap ($(pg $port -c "SELECT count(*) FROM t") rows, $(pg $port -c "SELECT runs FROM stomata_index_info('t_s')") runs)"
}

echo "== standby after catch-up"
target="$(pg $P1 -c "SELECT pg_current_wal_lsn()")"
for i in $(seq 1 120); do
  [ "$(pg $P2 -c "SELECT pg_last_wal_replay_lsn() >= '$target'")" = t ] && break; sleep 0.5
done
[ "$(pg $P2 -c "SELECT pg_last_wal_replay_lsn() >= '$target'")" = t ] || fail "standby did not catch up"
grep -q "inconsistent page" "$BASE/standby.log" && fail "wal_consistency_checking found an inconsistent page"
[ "$(pg $P1 -c "SELECT count(*) || '/' || sum(length(s)) FROM t")" = \
  "$(pg $P2 -c "SELECT count(*) || '/' || sum(length(s)) FROM t")" ] || fail "standby heap differs"
[ "$(pg $P1 -c "SELECT generation || '/' || runs || '/' || pending_records FROM stomata_index_info('t_s')")" = \
  "$(pg $P2 -c "SELECT generation || '/' || runs || '/' || pending_records FROM stomata_index_info('t_s')")" ] \
  || fail "standby metapage differs"
check_cluster $P2 "standby"
echo "wal_consistency_checking: no inconsistent pages"

echo "== promoted standby"
"$PGBIN/pg_ctl" -D "$BASE/standby" promote -w >/dev/null
pg $P2 -q -c "INSERT INTO t (s) SELECT 'promoted-' || i FROM generate_series(1, 5000) i" \
          -c "VACUUM t" -c "SELECT stomata_compact('t_s')" -c "VACUUM t" >/dev/null
check_cluster $P2 "promoted standby"
[ "$(pg $P2 -c "SELECT count(*) FROM t WHERE s LIKE 'promoted-%'")" = 5000 ] || fail "promoted inserts"

echo "== point-in-time recovery to the restore point 'mid'"
cp -r "$BASE/backup" "$BASE/pitr"
cat >> "$BASE/pitr/postgresql.conf" <<EOF
port = $P3
archive_mode = off
restore_command = 'cp $BASE/archive/%f %p'
recovery_target_name = 'mid'
recovery_target_action = 'promote'
EOF
touch "$BASE/pitr/recovery.signal"
"$PGBIN/pg_ctl" -D "$BASE/pitr" -l "$BASE/pitr.log" start -w -t 300 >/dev/null
for i in $(seq 1 240); do
  [ "$(pg $P3 -c "SELECT pg_is_in_recovery()" 2>/dev/null)" = f ] && break; sleep 0.5
done
[ "$(pg $P3 -c "SELECT pg_is_in_recovery()")" = f ] || fail "PITR did not finish"
grep -q 'recovery stopping at restore point "mid"' "$BASE/pitr.log" || fail "PITR did not stop at the restore point"
check_cluster $P3 "PITR cluster"
pg $P3 -q -c "INSERT INTO t (s) SELECT 'after-pitr-' || i FROM generate_series(1, 2000) i" \
          -c "SELECT stomata_merge_pending('t_s')" -c "VACUUM t" >/dev/null
check_cluster $P3 "PITR cluster after new writes"
echo "ALL REPLICATION CHECKS PASSED"
