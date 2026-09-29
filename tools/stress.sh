#!/usr/bin/env bash
# Crash-recovery and concurrency checks for stomata.
#
# Needs a *scratch* cluster that this script may stop with -m immediate:
#   PGBIN=/usr/lib/postgresql/16/bin PGDATA=/tmp/scratch PGPORT=5499 tools/stress.sh
# The extension must already be installed for that server.
set -euo pipefail

: "${PGBIN:?set PGBIN to the server bin directory}"
: "${PGDATA:?set PGDATA to a scratch data directory}"
PGPORT="${PGPORT:-5499}"
PGHOST="${PGHOST:-/tmp}"
DB=stomata_stress
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ ! -f "$PGDATA/PG_VERSION" ]; then
  "$PGBIN/initdb" -D "$PGDATA" -E UTF8 --no-locale >/dev/null
fi
start() { "$PGBIN/pg_ctl" -D "$PGDATA" -o "-p $PGPORT -k $PGHOST" -l "$PGDATA/stress.log" start -w >/dev/null; }
"$PGBIN/pg_ctl" -D "$PGDATA" status >/dev/null 2>&1 || start

P() { "$PGBIN/psql" -h "$PGHOST" -p "$PGPORT" -X -At -v ON_ERROR_STOP=1 -d "$DB" "$@"; }
fail() { echo "FAIL: $*"; exit 1; }

"$PGBIN/psql" -h "$PGHOST" -p "$PGPORT" -X -q -d postgres -c "DROP DATABASE IF EXISTS $DB" -c "CREATE DATABASE $DB"
P -q -c "CREATE EXTENSION stomata" \
     -c "CREATE TABLE t (id serial PRIMARY KEY, s text)" \
     -c "INSERT INTO t (s) SELECT md5(i::text) FROM generate_series(1, 50000) i" \
     -c "CREATE INDEX t_s ON t USING stomata (s)" \
     -c "CREATE UNLOGGED TABLE u (s text)" \
     -c "INSERT INTO u SELECT md5(i::text) FROM generate_series(1, 5000) i" \
     -c "CREATE INDEX u_s ON u USING stomata (s)"

echo "== crash recovery"
P -q -c "CHECKPOINT" \
     -c "INSERT INTO t (s) SELECT 'crash-' || i FROM generate_series(1, 3000) i" \
     -c "SELECT stomata_merge_pending('t_s')" \
     -c "INSERT INTO t (s) SELECT 'after-' || i FROM generate_series(1, 700) i" \
     -c "DELETE FROM t WHERE id % 10 = 0" >/dev/null
before="$(P -c "SELECT generation || '/' || pending_records FROM stomata_index_info('t_s')")"
"$PGBIN/pg_ctl" -D "$PGDATA" -m immediate stop >/dev/null
start
after="$(P -c "SELECT generation || '/' || pending_records FROM stomata_index_info('t_s')")"
[ "$before" = "$after" ] || fail "metapage changed across recovery: $before -> $after"
[ "$(P -c "SELECT stomata_verify('t_s')")" = 0 ] || fail "verify after recovery"
for pat in 'crash-%' 'after-%' '%ab%'; do
  a="$(P -c "SET enable_seqscan = off" -c "SELECT count(*) FROM t WHERE s LIKE '$pat'" | tail -1)"
  b="$(P -c "SET enable_bitmapscan = off" -c "SELECT count(*) FROM t WHERE s LIKE '$pat'" | tail -1)"
  [ "$a" = "$b" ] || fail "pattern $pat: index $a vs seq $b"
done
[ "$(P -c "SELECT count(*) FROM u")" = 0 ] || fail "unlogged table not reset"

echo "== crash during a compaction"
P -q -c "INSERT INTO t (s) SELECT 'mid-' || i FROM generate_series(1, 20000) i" >/dev/null
( P -q -c "SELECT stomata_compact('t_s')" >/dev/null 2>&1 || true ) &
sleep 0.4
"$PGBIN/pg_ctl" -D "$PGDATA" -m immediate stop >/dev/null
wait || true
start
[ "$(P -c "SELECT stomata_verify('t_s')")" = 0 ] || fail "verify after crash during compaction"
a="$(P -c "SET enable_seqscan = off" -c "SELECT count(*) FROM t WHERE s LIKE 'mid-1%'" | tail -1)"
b="$(P -c "SET enable_bitmapscan = off" -c "SELECT count(*) FROM t WHERE s LIKE 'mid-1%'" | tail -1)"
[ "$a" = "$b" ] || fail "after crash during compaction: index $a vs seq $b"
P -q -c "VACUUM t" -c "SELECT stomata_compact('t_s')" -c "VACUUM t" >/dev/null
size1="$(P -c "SELECT pg_relation_size('t_s')")"
for i in 1 2 3; do
  P -q -c "INSERT INTO t (s) SELECT 'again-' || i FROM generate_series(1, 2000) i" \
       -c "SELECT stomata_compact('t_s')" -c "VACUUM t" >/dev/null
done
size2="$(P -c "SELECT pg_relation_size('t_s')")"
[ "$size2" -le $((size1 * 2)) ] || fail "index file keeps growing after compactions ($size1 -> $size2)"
[ "$(P -c "SELECT stomata_verify('t_s')")" = 0 ] || fail "verify after repeated compactions"
P -q -c "INSERT INTO u VALUES ('unlogged-new')"
[ "$(P -c "SET enable_seqscan = off" -c "SELECT s FROM u WHERE s LIKE 'unlogged%'" | tail -1)" = unlogged-new ] \
  || fail "unlogged index after reset"
echo "ok"

echo "== concurrency (25 s)"
cat > "$TMP/ins.sql" <<'SQL'
\set r random(1, 1000000)
INSERT INTO t (s) VALUES ('c' || :r || '-' || md5(:r::text));
SQL
cat > "$TMP/sel.sql" <<'SQL'
\set r random(1, 999)
SET enable_seqscan = off;
SELECT count(*) FROM t WHERE s LIKE 'c' || :r || '-%';
SQL
cat > "$TMP/upd.sql" <<'SQL'
\set r random(1, 50000)
UPDATE t SET s = s || 'u' WHERE id = :r;
DELETE FROM t WHERE id = :r + 1;
SQL
cat > "$TMP/merge.sql" <<'SQL'
SELECT stomata_merge_pending('t_s');
SELECT pg_sleep(0.2);
SQL
cat > "$TMP/compact.sql" <<'SQL'
SELECT stomata_compact('t_s');
SELECT pg_sleep(1);
SQL
# rows nobody modifies: every scan must see all of them, whatever merges do
cat > "$TMP/stable.sql" <<'SQL'
SET enable_seqscan = off;
SELECT stable_check();
SQL
P -q -c "ALTER INDEX t_s SET (pending_limit = 64)" -c "REINDEX INDEX t_s"
P -q -c "INSERT INTO t (s) SELECT 'stable-' || i FROM generate_series(1, 3000) i" \
     -c "CREATE FUNCTION stable_check() RETURNS void LANGUAGE plpgsql AS \$\$
         DECLARE a bigint; b bigint; c bigint;
         BEGIN
           SELECT count(*) INTO a FROM t WHERE s LIKE 'stable-%';
           SELECT count(*) INTO b FROM t WHERE s LIKE 'stable-1__';
           SELECT count(*) INTO c FROM t WHERE s ILIKE '%STABLE-2%';
           IF a <> 3000 OR b <> 100 OR c <> 1111 THEN
             RAISE EXCEPTION 'stable rows missing: % % %', a, b, c;
           END IF;
         END \$\$"
PGB="$PGBIN/pgbench -h $PGHOST -p $PGPORT -n -T 25"
$PGB -c 8 -j 4 -f "$TMP/ins.sql@5" -f "$TMP/sel.sql@4" -f "$TMP/upd.sql@2" "$DB" > "$TMP/pgb1.log" 2>&1 &
$PGB -c 1 -f "$TMP/merge.sql" "$DB" > "$TMP/pgb2.log" 2>&1 &
$PGB -c 1 -f "$TMP/compact.sql" "$DB" > "$TMP/pgb3.log" 2>&1 &
$PGB -c 2 -f "$TMP/stable.sql" "$DB" > "$TMP/pgb4.log" 2>&1 &
( for i in 1 2 3; do sleep 7; P -q -c "VACUUM t"; done ) &
wait
for f in pgb1 pgb2 pgb3 pgb4; do
  if grep -qiE "aborted|error" "$TMP/$f.log" || ! grep -q "number of failed transactions: 0" "$TMP/$f.log"; then
    cat "$TMP/$f.log"; fail "pgbench workload ($f)"
  fi
done
echo "stable-row checks: $(grep -E '^number of transactions actually processed' "$TMP/pgb4.log")"
[ "$(P -c "SELECT stomata_verify('t_s')")" = 0 ] || fail "verify after concurrency"
mm="$(P -f - <<'SQL' | tail -1
CREATE TEMP TABLE pp AS
  SELECT DISTINCT 'c' || (i % 997) || '-%' AS p FROM generate_series(1, 997) i
  UNION SELECT '%' || substr(md5(i::text), 1, 3) || '%' FROM generate_series(1, 200) i
  UNION SELECT '%u' UNION SELECT '%uu' UNION SELECT 'crash-1%' UNION SELECT 'after-__';
CREATE FUNCTION pg_temp.ids(pat text) RETURNS int[] LANGUAGE sql STABLE
  AS $$ SELECT array_agg(id ORDER BY id) FROM t WHERE s LIKE pat $$;
SET enable_bitmapscan = off;
CREATE TEMP TABLE tr AS SELECT p, pg_temp.ids(p) AS ids FROM pp;
SET enable_bitmapscan = on;
SET enable_seqscan = off;
SELECT count(*) FILTER (WHERE pg_temp.ids(p) IS DISTINCT FROM ids) FROM tr;
SQL
)"
[ "$mm" = 0 ] || fail "$mm patterns differ after concurrency"
grep -E "tps = " "$TMP/pgb1.log" | head -1
echo "ok"
echo "ALL STRESS CHECKS PASSED"
