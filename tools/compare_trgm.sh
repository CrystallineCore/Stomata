#!/usr/bin/env bash
# compare_trgm.sh - unbiased LIKE/ILIKE benchmark: stomata vs pg_trgm.
#
# Every timed sample is an EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) of the
# query, run in a transaction in which every other index on the table has
# been dropped (and rolled back afterwards).  The plan of each sample is
# stored and checked: a sample only counts if the plan used exactly the
# index under test and no sequential scan.  Counts are checked against a
# sequential scan.
#
# Bias controls
#   * one index usable per sample (others dropped inside the transaction,
#     restored by ROLLBACK - no rebuilds);
#   * query order and method order are shuffled independently every round
#     (seeded, reproducible), so neither method always runs "second";
#   * warm-up rounds are discarded; both indexes are pre-warmed equally;
#   * JIT and parallel query are off by default (same plan shape for both);
#   * the comparison is paired per round, and a query is only called faster
#     or slower when the median ratio exceeds the threshold AND a sign test
#     across rounds is significant (p < 0.05);
#   * summaries are reported with and without zero-match queries, and split
#     by whether pg_trgm can extract trigrams from the pattern;
#   * pg_stat_user_indexes idx_scan deltas are checked as a second,
#     independent proof that the indexes were scanned;
#   * optional cold mode restarts the server (and drops OS caches) before
#     every sample.
#
# WARNING: run it on a test copy.  It builds two indexes on the table and
# takes ACCESS EXCLUSIVE locks on it for every sample.
#
# Usage:
#   tools/compare_trgm.sh --table interactions --column username [options]
#
# Options:
#   --dsn STR            libpq connection string (default: PG* environment)
#   --table NAME         table (optionally schema-qualified)          [required]
#   --column NAME        text/varchar column                          [required]
#   --queries FILE       query file (see below; default: built-in set)
#   --rounds N           timed rounds per query and method   (default 15, cold 5)
#   --warmup N           discarded warm-up rounds                     (default 1)
#   --seed N             shuffle seed                                (default 42)
#   --threshold X        ratio that counts as a real difference     (default 1.10)
#   --stomata-with STR   reloptions for stomata, e.g. "exact_bigrams = on"
#   --trgm-kind gin|gist pg_trgm index type                         (default gin)
#   --build-reps N       builds per index, alternating order, median  (default 1)
#   --parallel           allow parallel query and parallel index builds
#   --no-seq             do not time sequential scans (warm-up still checks counts)
#   --cold               restart the server before each sample
#   --restart-cmd CMD    command that restarts the server (required with --cold)
#   --drop-caches-cmd C  command that drops OS caches (optional, cold mode)
#   --out DIR            output directory (default ./stomata_cmp_<timestamp>)
#   --keep               keep the indexes and the stomata_cmp schema
#   --yes                do not ask for confirmation
#
# Query file: one query per line, "kind|label|trgm_extractable|condition",
# where condition uses {col} for the column, e.g.
#   infix|infix 3 chars|t|{col} LIKE '%kim%'
# Blank lines and lines starting with # are ignored.

set -euo pipefail

DSN=""; TABLE=""; COL=""; QFILE=""; ROUNDS=""; WARMUP=1; SEED=42; THRESH=1.10
ST_WITH=""; TRGM_KIND=gin; BUILD_REPS=1; PARALLEL=0; SEQ=1; COLD=0
RESTART_CMD=""; DROP_CACHES_CMD=""; OUT=""; KEEP=0; YES=0

die() { echo "error: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
  case "$1" in
    --dsn) DSN=$2; shift 2;;
    --table) TABLE=$2; shift 2;;
    --column) COL=$2; shift 2;;
    --queries) QFILE=$2; shift 2;;
    --rounds) ROUNDS=$2; shift 2;;
    --warmup) WARMUP=$2; shift 2;;
    --seed) SEED=$2; shift 2;;
    --threshold) THRESH=$2; shift 2;;
    --stomata-with) ST_WITH=$2; shift 2;;
    --trgm-kind) TRGM_KIND=$2; shift 2;;
    --build-reps) BUILD_REPS=$2; shift 2;;
    --parallel) PARALLEL=1; shift;;
    --no-seq) SEQ=0; shift;;
    --cold) COLD=1; shift;;
    --restart-cmd) RESTART_CMD=$2; shift 2;;
    --drop-caches-cmd) DROP_CACHES_CMD=$2; shift 2;;
    --out) OUT=$2; shift 2;;
    --keep) KEEP=1; shift;;
    --yes) YES=1; shift;;
    -h|--help) sed -n '2,60p' "$0"; exit 0;;
    *) die "unknown option $1";;
  esac
done
[ -n "$TABLE" ] && [ -n "$COL" ] || die "--table and --column are required (see --help)"
case "$TRGM_KIND" in gin|gist) ;; *) die "--trgm-kind must be gin or gist";; esac
[ "$COLD" = 1 ] && [ -z "$RESTART_CMD" ] && die "--cold needs --restart-cmd"
[ -z "$ROUNDS" ] && { [ "$COLD" = 1 ] && ROUNDS=5 || ROUNDS=15; }
[ "$WARMUP" -ge 1 ] || die "--warmup must be at least 1 (the warm-up round also checks result counts)"
[ "$ROUNDS" -ge 6 ] || echo "note: fewer than 6 rounds can never reach p < 0.05 in the sign test" >&2
OUT=${OUT:-./stomata_cmp_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$OUT"
REPORT="$OUT/report.txt"

PSQL=(psql -X -q -v ON_ERROR_STOP=1 -v "tbl=$TABLE" -v "col=$COL")
[ -n "$DSN" ] && PSQL+=(-d "$DSN")
q()  { "${PSQL[@]}" -At -c "$1"; }          # single value
qv() { "${PSQL[@]}" -At "$@"; }              # with extra args / stdin
qs() { printf '%s\n' "$1" | "${PSQL[@]}" -At; }  # one script; psql :vars are expanded

TRGM_IDX=stomata_cmp_trgm_idx
ST_IDX=stomata_cmp_stomata_idx

# ---------------------------------------------------------------- preflight
q "SELECT 1" >/dev/null || die "cannot connect"
VNUM=$(q "SHOW server_version_num")
[ "$VNUM" -ge 160000 ] || die "PostgreSQL 16 or later required"
q "SELECT 1 FROM pg_available_extensions WHERE name='stomata'" | grep -q 1 || die "stomata is not installed on the server"
q "SELECT 1 FROM pg_available_extensions WHERE name='pg_trgm'" | grep -q 1 || die "pg_trgm is not available"
qs "SELECT :'tbl'::regclass" >/dev/null || die "table $TABLE not found"
TYP=$(qs "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = :'tbl'::regclass AND attname = :'col' AND NOT attisdropped")
[ -n "$TYP" ] || die "column $COL not found"
case "$TYP" in text|character\ varying*) ;; *) die "column $COL is $TYP (need text or varchar)";; esac
[ "$(qs "SELECT relkind FROM pg_class WHERE oid = :'tbl'::regclass")" = r ] || die "$TABLE must be a plain table"

if [ "$YES" != 1 ]; then
  echo "This builds $TRGM_IDX and $ST_IDX on $TABLE and locks the table exclusively"
  echo "for every sample. Run it on a test copy. Continue? [y/N]"
  read -r ans; [ "$ans" = y ] || [ "$ans" = Y ] || exit 1
fi

cleanup() {
  if [ "$KEEP" != 1 ]; then
    "${PSQL[@]}" -c "DROP INDEX IF EXISTS $(q "SELECT quote_ident(nspname) FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.oid = '$TABLE'::regclass").$TRGM_IDX" \
                 -c "DROP INDEX IF EXISTS $(q "SELECT quote_ident(nspname) FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.oid = '$TABLE'::regclass").$ST_IDX" \
                 -c "DROP SCHEMA IF EXISTS stomata_cmp CASCADE" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# ---------------------------------------------------------------- schema
qv <<'SQL'
CREATE EXTENSION IF NOT EXISTS pg_trgm;
CREATE EXTENSION IF NOT EXISTS stomata;
DROP SCHEMA IF EXISTS stomata_cmp CASCADE;
CREATE SCHEMA stomata_cmp;
CREATE TABLE stomata_cmp.queries (
  qid serial PRIMARY KEY, kind text, label text, trgm_ok bool, cond text, sql text);
CREATE TABLE stomata_cmp.samples (
  round int, qid int, method text, seq int, j jsonb);
CREATE TABLE stomata_cmp.builds (rep int, method text, ms numeric, bytes bigint);
CREATE TABLE stomata_cmp.meta (k text PRIMARY KEY, v text);

CREATE FUNCTION stomata_cmp.sample(p_qid int, p_count bool) RETURNS jsonb
LANGUAGE plpgsql AS $$
DECLARE s text; j json; c bigint;
BEGIN
  SELECT sql INTO s FROM stomata_cmp.queries WHERE qid = p_qid;
  EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, BUFFERS, SUMMARY, FORMAT JSON) ' || s INTO j;
  IF p_count THEN EXECUTE s INTO c; END IF;
  RETURN jsonb_build_object('plan', j, 'count', c);
END $$;

CREATE FUNCTION stomata_cmp.timed(p_sql text) RETURNS numeric
LANGUAGE plpgsql AS $$
DECLARE t0 timestamptz := clock_timestamp();
BEGIN
  EXECUTE p_sql;
  RETURN round((extract(epoch FROM clock_timestamp() - t0) * 1000)::numeric, 1);
END $$;

-- exact two-sided sign test
CREATE FUNCTION stomata_cmp.sign_p(w int, n int) RETURNS numeric
LANGUAGE sql IMMUTABLE AS $$
  SELECT CASE WHEN n = 0 THEN 1::numeric ELSE
    least(1, 2 * sum(factorial(n) / (factorial(k) * factorial(n - k))) / power(2::numeric, n)) END
  FROM generate_series(0, least(w, n - w)) k
$$;
SQL

# ---------------------------------------------------------------- queries
default_queries() {
cat <<'EOF'
prefix|long prefix|t|{col} LIKE 'brandyferg%'
prefix|medium prefix|t|{col} LIKE 'allison%'
prefix|short prefix (3)|t|{col} LIKE 'kim%'
prefix|1-char prefix|t|{col} LIKE 'a%'
suffix|suffix 3|t|{col} LIKE '%son'
suffix|suffix 2|t|{col} LIKE '%ez'
suffix|suffix 1|f|{col} LIKE '%n'
suffix|long suffix|t|{col} LIKE '%anderson'
infix|infix 1 char|f|{col} LIKE '%8%'
infix|infix 2 chars|f|{col} LIKE '%mc%'
infix|infix 3 chars|t|{col} LIKE '%kim%'
infix|infix 3 chars (common)|t|{col} LIKE '%son%'
infix|infix 3 chars (ell)|t|{col} LIKE '%ell%'
infix|infix 5 chars|t|{col} LIKE '%mcdon%'
infix|infix 8 chars|t|{col} LIKE '%anderson%'
infix|infix no-match|t|{col} LIKE '%zzzqx%'
digits|digit suffix 2|t|{col} LIKE '%22'
digits|digit prefix-ish|f|{col} LIKE '%1%2%'
exact|exact hit|t|{col} LIKE 'allisonkelly'
exact|exact miss|t|{col} LIKE 'doesnotexist'
underscore|mid underscore|t|{col} LIKE 'alli_on%'
underscore|leading underscore|t|{col} LIKE '_llison%'
underscore|trailing underscore|f|{col} LIKE '%so_'
underscore|infix underscore|f|{col} LIKE '%k_m%'
underscore|only underscores (len 5)|f|{col} LIKE '_____'
multi|a%son|t|{col} LIKE 'a%son'
multi|j%s%n|t|{col} LIKE 'j%s%n'
multi|%kim%son%|t|{col} LIKE '%kim%son%'
multi|%a%e%r%|f|{col} LIKE '%a%e%r%'
multi|match-all %|f|{col} LIKE '%'
ilike|ILIKE prefix|t|{col} ILIKE 'ALLISON%'
ilike|ILIKE suffix|t|{col} ILIKE '%SON'
ilike|ILIKE infix 3|t|{col} ILIKE '%KIM%'
ilike|ILIKE infix 2|f|{col} ILIKE '%Mc%'
not|NOT LIKE infix|f|{col} NOT LIKE '%son%'
not|NOT LIKE prefix|f|{col} NOT LIKE 'a%'
not|NOT ILIKE infix|f|{col} NOT ILIKE '%KIM%'
compound|AND two infix|t|{col} LIKE '%kim%' AND {col} LIKE '%son%'
compound|OR two infix|t|{col} LIKE '%kim%' OR {col} LIKE '%ell%'
compound|prefix AND suffix|t|{col} LIKE 'a%' AND {col} LIKE '%son'
compound|LIKE ANY(array)|t|{col} LIKE ANY (ARRAY['%kim%','%ell%','%mcd%'])
compound|LIKE ALL(array)|f|{col} LIKE ALL (ARRAY['%kim%','%son%'])
escape|ESCAPE literal _|f|{col} LIKE '%!_%' ESCAPE '!'
escape|ESCAPE literal %|f|{col} LIKE '%!%%' ESCAPE '!'
operator|~~ operator|t|{col} ~~ '%kim%'
operator|!~~ operator|f|{col} !~~ '%kim%'
EOF
}
{
  if [ -n "$QFILE" ]; then cat "$QFILE"; else default_queries; fi
} | awk -F'|' '
  /^[[:space:]]*(#|$)/ { next }
  NF < 4 { printf "bad query line: %s\n", $0 > "/dev/stderr"; exit 1 }
  { cond = $4; for (i = 5; i <= NF; i++) cond = cond "|" $i
    printf "INSERT INTO stomata_cmp.queries(kind,label,trgm_ok,cond) VALUES ($cmpq$%s$cmpq$,$cmpq$%s$cmpq$,%s,$cmpq$%s$cmpq$);\n", $1, $2, ($3 ~ /^[tT1y]/ ? "true" : "false"), cond }
' > "$OUT/queries.sql"
qv -f "$OUT/queries.sql"
qs "UPDATE stomata_cmp.queries SET sql = format('SELECT count(*) FROM %s WHERE %s', :'tbl'::regclass, replace(cond, '{col}', quote_ident(:'col')))"
NQ=$(q "SELECT count(*) FROM stomata_cmp.queries")
echo "loaded $NQ queries"

# ---------------------------------------------------------------- builds
NSP=$(qs "SELECT quote_ident(nspname) FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.oid = :'tbl'::regclass")
TRGM_OPS=${TRGM_KIND}_trgm_ops
ST_WITH_SQL=""; [ -n "$ST_WITH" ] && ST_WITH_SQL=" WITH ($ST_WITH)"
PAR_MAINT="SET max_parallel_maintenance_workers = 0;"; [ "$PARALLEL" = 1 ] && PAR_MAINT=""
TRGM_DDL="CREATE INDEX $TRGM_IDX ON $TABLE USING $TRGM_KIND ($(qs "SELECT quote_ident(:'col')") $TRGM_OPS)"
ST_DDL="CREATE INDEX $ST_IDX ON $TABLE USING stomata ($(qs "SELECT quote_ident(:'col')"))$ST_WITH_SQL"

echo "vacuuming and warming the heap"
qs "VACUUM (ANALYZE) $TABLE"
qs "SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT count(*) FROM $TABLE" >/dev/null
for rep in $(seq 1 "$BUILD_REPS"); do
  qs "DROP INDEX IF EXISTS $NSP.$TRGM_IDX; DROP INDEX IF EXISTS $NSP.$ST_IDX;"
  if [ $((rep % 2)) = 1 ]; then order="trgm stomata"; else order="stomata trgm"; fi
  for m in $order; do
    "${PSQL[@]}" -c "CHECKPOINT" >/dev/null 2>&1 || true
    if [ "$m" = trgm ]; then ddl=$TRGM_DDL; idx=$TRGM_IDX; else ddl=$ST_DDL; idx=$ST_IDX; fi
    echo "build $rep/$BUILD_REPS: $m"
    qv -v "ddl=$ddl" -v "m=$m" -v "rep=$rep" -v "idx=$NSP.$idx" <<SQL
SET maintenance_work_mem = '256MB';
$PAR_MAINT
INSERT INTO stomata_cmp.builds SELECT :rep, :'m', stomata_cmp.timed(:'ddl'), 0;
UPDATE stomata_cmp.builds SET bytes = pg_relation_size(:'idx'::regclass) WHERE rep = :rep AND method = :'m';
SQL
  done
done
qs "VACUUM (ANALYZE) $TABLE"
[ "$TRGM_KIND" = gin ] && q "SELECT gin_clean_pending_list('$NSP.$TRGM_IDX'::regclass)" >/dev/null || true

# every other droppable index on the table (constraint indexes cannot be
# dropped; if the planner picks one the sample is rejected by the plan check)
OTHERS=$(qs "SELECT string_agg(i.indexrelid::regclass::text, ', ')
  FROM pg_index i LEFT JOIN pg_constraint c ON c.conindid = i.indexrelid
  WHERE i.indrelid = :'tbl'::regclass AND c.oid IS NULL
    AND i.indexrelid NOT IN ('$NSP.$TRGM_IDX'::regclass, '$NSP.$ST_IDX'::regclass)")
join() { local a="" x; for x in "$@"; do [ -n "$x" ] && a="${a:+$a, }$x"; done; echo "$a"; }
DROP_FOR_trgm=$(join "$OTHERS" "$NSP.$ST_IDX")
DROP_FOR_stomata=$(join "$OTHERS" "$NSP.$TRGM_IDX")
DROP_FOR_seq=$(join "$OTHERS" "$NSP.$ST_IDX" "$NSP.$TRGM_IDX")
[ -n "$OTHERS" ] && echo "other indexes dropped (and restored) around each sample: $OTHERS"

# ---------------------------------------------------------------- settings
SESSION_SET="SET jit = off; SET max_parallel_workers_per_gather = 0;"
[ "$PARALLEL" = 1 ] && SESSION_SET="SET jit = off;"
set_for() {
  case "$1" in
    seq) echo "SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off;";;
    *)   echo "SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = on; SET LOCAL enable_bitmapscan = on; SET LOCAL enable_indexonlyscan = on;";;
  esac
}
METHODS="trgm stomata"; [ "$SEQ" = 1 ] && METHODS="seq trgm stomata"

# shared_buffers vs working set
qv <<SQL > "$OUT/cache_check.txt"
SELECT CASE WHEN pg_relation_size(:'tbl'::regclass) + pg_relation_size('$NSP.$TRGM_IDX'::regclass)
               + pg_relation_size('$NSP.$ST_IDX'::regclass)
             > (SELECT setting::bigint * 8192 FROM pg_settings WHERE name = 'shared_buffers')
       THEN 'WARNING: table + both indexes exceed shared_buffers; samples also depend on the OS cache (randomised order spreads this evenly)'
       ELSE 'ok: table and both indexes fit in shared_buffers' END
SQL
cat "$OUT/cache_check.txt"
if [ "$(q "SELECT count(*) FROM pg_extension WHERE extname = 'pg_prewarm'")" = 1 ]; then
  echo "pre-warming heap and both indexes with pg_prewarm"
  qs "SELECT pg_prewarm(:'tbl'::regclass), pg_prewarm('$NSP.$TRGM_IDX'::regclass), pg_prewarm('$NSP.$ST_IDX'::regclass)" >/dev/null
fi

qs "INSERT INTO stomata_cmp.meta VALUES
  ('server', version()), ('table', :'tbl'), ('column', :'col'),
  ('rows', (SELECT reltuples::bigint::text FROM pg_class WHERE oid = :'tbl'::regclass)),
  ('heap', pg_size_pretty(pg_relation_size(:'tbl'::regclass))),
  ('mode', '$([ "$COLD" = 1 ] && echo cold || echo warm)'), ('rounds', '$ROUNDS'), ('warmup', '$WARMUP'),
  ('seed', '$SEED'), ('threshold', '$THRESH'), ('trgm', '$TRGM_KIND'),
  ('stomata_with', nullif('$ST_WITH', '')), ('parallel', '$PARALLEL'),
  ('shared_buffers', current_setting('shared_buffers')),
  ('idx_scan_before_trgm', (SELECT idx_scan::text FROM pg_stat_user_indexes WHERE indexrelid = '$NSP.$TRGM_IDX'::regclass)),
  ('idx_scan_before_stomata', (SELECT idx_scan::text FROM pg_stat_user_indexes WHERE indexrelid = '$NSP.$ST_IDX'::regclass))"

# ---------------------------------------------------------------- schedule
# one line per sample: round qid method; shuffled per round with its own seed
QIDS=$(q "SELECT qid FROM stomata_cmp.queries ORDER BY qid")
schedule() {  # $1 = first round, $2 = last round
  local r
  for r in $(seq "$1" "$2"); do
    ms=$METHODS; [ "$r" -le 0 ] && ms="seq trgm stomata"   # counts always checked against seq
    for qid in $QIDS; do for m in $ms; do echo "$r $qid $m"; done; done |
      awk -v s=$((SEED * 1000 + r + 1000)) 'BEGIN { srand(s) } { printf "%.12f\t%s\n", rand(), $0 }' |
      sort -g -k1,1 | cut -f2-
  done
}
emit() {  # $1 round  $2 qid  $3 method  $4 seq-no
  local dl; eval "dl=\$DROP_FOR_$3"
  local cnt=false; [ "$1" -le 0 ] && cnt=true
  cat <<SQL
BEGIN;
$(set_for "$3")
${dl:+DROP INDEX $dl;}
SELECT stomata_cmp.sample($2, $cnt) AS j \\gset
ROLLBACK;
INSERT INTO stomata_cmp.samples VALUES ($1, $2, '$3', $4, :'j');
SQL
}

WARM_FIRST=$((1 - WARMUP))           # warm-up rounds are numbered <= 0
{
  echo "$SESSION_SET"
  n=0
  if [ "$COLD" = 1 ]; then last=0; else last=$ROUNDS; fi
  schedule "$WARM_FIRST" "$last" | while read -r r qid m; do n=$((n + 1)); emit "$r" "$qid" "$m" "$n"; done
} > "$OUT/samples_warm.sql"
TOTAL=$(grep -c '^BEGIN;' "$OUT/samples_warm.sql")
echo "running $TOTAL samples in one session (warm-up rounds: $WARMUP)"
qv -f "$OUT/samples_warm.sql"

if [ "$COLD" = 1 ]; then
  n=100000
  schedule 1 "$ROUNDS" > "$OUT/schedule_cold.txt"
  TOTAL=$(wc -l < "$OUT/schedule_cold.txt"); i=0
  while read -r r qid m; do
    i=$((i + 1)); n=$((n + 1))
    printf '\rcold sample %d/%d' "$i" "$TOTAL"
    sh -c "$RESTART_CMD" >/dev/null 2>&1 || die "restart command failed"
    [ -n "$DROP_CACHES_CMD" ] && { sh -c "$DROP_CACHES_CMD" >/dev/null 2>&1 || die "drop-caches command failed"; }
    for _ in $(seq 1 60); do q "SELECT 1" >/dev/null 2>&1 && break; sleep 1; done
    { echo "$SESSION_SET"; emit "$r" "$qid" "$m" "$n"; } | qv
  done < "$OUT/schedule_cold.txt"
  echo
fi

qs "SELECT pg_stat_force_next_flush()" >/dev/null 2>&1 || true
sleep 1
qs "INSERT INTO stomata_cmp.meta VALUES
  ('idx_scan_after_trgm', (SELECT idx_scan::text FROM pg_stat_user_indexes WHERE indexrelid = '$NSP.$TRGM_IDX'::regclass)),
  ('idx_scan_after_stomata', (SELECT idx_scan::text FROM pg_stat_user_indexes WHERE indexrelid = '$NSP.$ST_IDX'::regclass))"

# ---------------------------------------------------------------- analysis
qv -v "thr=$THRESH" -v "trgm_idx=$TRGM_IDX" -v "st_idx=$ST_IDX" <<'SQL'
CREATE VIEW stomata_cmp.s AS
SELECT sm.round, sm.qid, sm.method,
       (sm.j->'plan'->0->>'Execution Time')::numeric AS exec_ms,
       (sm.j->'plan'->0->>'Planning Time')::numeric  AS plan_ms,
       (sm.j->'plan'->0->'Plan'->>'Shared Hit Blocks')::bigint  AS hit,
       (sm.j->'plan'->0->'Plan'->>'Shared Read Blocks')::bigint AS rd,
       (sm.j->>'count')::bigint AS cnt,
       ARRAY(SELECT DISTINCT x #>> '{}' FROM jsonb_path_query(sm.j->'plan', 'lax $.**."Index Name"') x ORDER BY 1) AS idx_used,
       EXISTS (SELECT 1 FROM jsonb_path_query(sm.j->'plan', 'lax $.**."Node Type"') x
               WHERE x #>> '{}' = 'Seq Scan') AS seq_used
FROM stomata_cmp.samples sm;

CREATE VIEW stomata_cmp.v AS
SELECT s.*, CASE s.method
         WHEN 'seq'     THEN cardinality(s.idx_used) = 0
         WHEN 'trgm'    THEN s.idx_used = ARRAY[:'trgm_idx'] AND NOT s.seq_used
         WHEN 'stomata' THEN s.idx_used = ARRAY[:'st_idx']   AND NOT s.seq_used END AS valid
FROM stomata_cmp.s s;

-- per query and method: validity over timed rounds, timing distribution
CREATE VIEW stomata_cmp.per_method AS
SELECT qid, method,
       count(*) FILTER (WHERE round > 0) AS n,
       count(*) FILTER (WHERE round > 0 AND valid) AS n_valid,
       percentile_cont(0.5)  WITHIN GROUP (ORDER BY exec_ms) FILTER (WHERE round > 0) AS med,
       percentile_cont(0.25) WITHIN GROUP (ORDER BY exec_ms) FILTER (WHERE round > 0) AS p25,
       percentile_cont(0.75) WITHIN GROUP (ORDER BY exec_ms) FILTER (WHERE round > 0) AS p75,
       percentile_cont(0.5)  WITHIN GROUP (ORDER BY rd)      FILTER (WHERE round > 0) AS med_reads,
       max(cnt) FILTER (WHERE round <= 0) AS cnt,
       (array_agg(CASE WHEN seq_used THEN 'Seq Scan' ELSE '' END
                  || coalesce(nullif(array_to_string(idx_used, ','), ''), '')
                  ORDER BY round DESC) FILTER (WHERE NOT valid))[1] AS bad_plan
FROM stomata_cmp.v GROUP BY qid, method;

-- paired per round (only rounds where both samples are valid)
CREATE VIEW stomata_cmp.paired AS
SELECT t.qid, t.round, t.exec_ms / nullif(st.exec_ms, 0) AS ratio
FROM stomata_cmp.v t JOIN stomata_cmp.v st USING (qid, round)
WHERE t.method = 'trgm' AND st.method = 'stomata' AND t.round > 0 AND t.valid AND st.valid;

CREATE VIEW stomata_cmp.cmp AS
WITH p AS (
  SELECT qid, count(*) AS n,
         count(*) FILTER (WHERE ratio > 1) AS st_faster,
         exp(percentile_cont(0.5) WITHIN GROUP (ORDER BY ln(ratio))) AS med_ratio
  FROM stomata_cmp.paired WHERE ratio > 0 GROUP BY qid)
SELECT q.qid, q.kind, q.label, q.trgm_ok,
       sq.cnt AS seq_count, tr.cnt AS trgm_count, st.cnt AS stomata_count,
       (tr.cnt IS NOT DISTINCT FROM sq.cnt OR sq.cnt IS NULL) AND (st.cnt IS NOT DISTINCT FROM coalesce(sq.cnt, tr.cnt)) AS counts_ok,
       tr.n_valid = tr.n AS trgm_indexed, st.n_valid = st.n AS stomata_indexed,
       tr.bad_plan AS trgm_bad, st.bad_plan AS stomata_bad,
       sq.med AS seq_ms, tr.med AS trgm_ms, tr.p25 AS trgm_p25, tr.p75 AS trgm_p75,
       st.med AS stomata_ms, st.p25 AS st_p25, st.p75 AS st_p75,
       tr.med_reads AS trgm_reads, st.med_reads AS st_reads,
       p.n, p.st_faster, p.med_ratio,
       CASE WHEN tr.n_valid = tr.n AND st.n_valid = st.n
            THEN stomata_cmp.sign_p(p.st_faster::int, p.n::int) END AS p_value,
       CASE WHEN tr.n_valid < tr.n OR st.n_valid < st.n OR p.n IS NULL THEN 'excluded (index not used)'
            WHEN p.med_ratio >= :thr       AND stomata_cmp.sign_p(p.st_faster::int, p.n::int) < 0.05 THEN 'stomata faster'
            WHEN p.med_ratio <= 1.0 / :thr AND stomata_cmp.sign_p(p.st_faster::int, p.n::int) < 0.05 THEN 'pg_trgm faster'
            ELSE 'no clear difference' END AS verdict
FROM stomata_cmp.queries q
LEFT JOIN stomata_cmp.per_method sq ON sq.qid = q.qid AND sq.method = 'seq'
LEFT JOIN stomata_cmp.per_method tr ON tr.qid = q.qid AND tr.method = 'trgm'
LEFT JOIN stomata_cmp.per_method st ON st.qid = q.qid AND st.method = 'stomata'
LEFT JOIN p ON p.qid = q.qid;
SQL

{
echo "STOMATA vs pg_trgm - $(date)"
echo
"${PSQL[@]}" -P footer=off <<'SQL'
\echo '== setup'
SELECT k AS setting, v AS value FROM stomata_cmp.meta WHERE k NOT LIKE 'idx_scan%' ORDER BY k;
\echo '== index builds (median over reps)'
SELECT method, count(*) AS reps, percentile_cont(0.5) WITHIN GROUP (ORDER BY ms) AS build_ms,
       pg_size_pretty(max(bytes)) AS size
FROM stomata_cmp.builds GROUP BY method ORDER BY method;

\echo '== check 1: every sample used exactly the index under test (from its own executed plan)'
SELECT method, count(*) FILTER (WHERE round > 0) AS timed_samples,
       count(*) FILTER (WHERE round > 0 AND valid) AS used_expected_plan,
       count(DISTINCT qid) FILTER (WHERE round > 0 AND NOT valid) AS queries_not_indexed
FROM stomata_cmp.v GROUP BY method ORDER BY method;
\echo 'queries where an index could not be used (excluded from the comparison):'
SELECT qid, label, trgm_indexed, stomata_indexed,
       coalesce(trgm_bad, '') AS trgm_plan_used,
       coalesce(stomata_bad, '') AS stomata_plan_used
FROM stomata_cmp.cmp WHERE NOT (trgm_indexed AND stomata_indexed) ORDER BY qid;

\echo '== check 2: idx_scan counters (pg_stat_user_indexes) moved at least once per valid sample'
SELECT x.method, x.delta AS idx_scan_delta, x.valid AS valid_samples,
       CASE WHEN x.delta IS NULL THEN 'unknown (track_counts off?)'
            WHEN x.delta >= x.valid THEN 'ok' ELSE 'FAILED' END AS result
FROM (WITH m AS (SELECT k, v::bigint AS v FROM stomata_cmp.meta WHERE k LIKE 'idx_scan%')
      SELECT y.method,
             (SELECT v FROM m WHERE k = 'idx_scan_after_' || y.method) - (SELECT v FROM m WHERE k = 'idx_scan_before_' || y.method) AS delta,
             (SELECT count(*) FROM stomata_cmp.v WHERE method = y.method AND valid) AS valid
      FROM (VALUES ('trgm'), ('stomata')) y(method)) x;

\echo '== check 3: result counts equal the sequential scan'
SELECT count(*) AS queries, count(*) FILTER (WHERE counts_ok) AS counts_match FROM stomata_cmp.cmp;
SELECT qid, label, seq_count, trgm_count, stomata_count FROM stomata_cmp.cmp WHERE NOT counts_ok;

\echo '== check 4: cache state (median shared blocks read from outside shared_buffers per sample)'
SELECT method, percentile_cont(0.5) WITHIN GROUP (ORDER BY rd) AS median_reads, max(rd) AS max_reads
FROM stomata_cmp.v WHERE round > 0 GROUP BY method ORDER BY method;

\echo '== per query (medians of timed rounds, ms; ratio = pg_trgm / stomata, paired per round)'
SELECT qid, kind, label, trgm_ok, stomata_count AS matches,
       round(seq_ms::numeric, 2) AS seq_ms,
       round(trgm_ms::numeric, 2) AS trgm_ms, round(stomata_ms::numeric, 2) AS stomata_ms,
       round(med_ratio::numeric, 2) AS ratio,
       st_faster || '/' || n AS st_faster_rounds, round(p_value, 3) AS p, verdict
FROM stomata_cmp.cmp ORDER BY qid;

\echo '== summary (geometric mean of per-query median ratios; > 1 means stomata faster)'
WITH c AS (SELECT * FROM stomata_cmp.cmp WHERE verdict <> 'excluded (index not used)'),
g AS (
  SELECT 'all compared' AS subset, * FROM c
  UNION ALL SELECT 'excluding zero-match queries', * FROM c WHERE stomata_count > 0
  UNION ALL SELECT 'trigram-friendly patterns', * FROM c WHERE trgm_ok
  UNION ALL SELECT 'trigram-degenerate patterns', * FROM c WHERE NOT trgm_ok)
SELECT subset, count(*) AS queries,
       round(exp(avg(ln(med_ratio)))::numeric, 2) AS geomean_ratio,
       count(*) FILTER (WHERE verdict = 'stomata faster') AS stomata_faster,
       count(*) FILTER (WHERE verdict = 'pg_trgm faster') AS trgm_faster,
       count(*) FILTER (WHERE verdict = 'no clear difference') AS no_clear_difference
FROM g GROUP BY subset ORDER BY min(CASE subset WHEN 'all compared' THEN 1 WHEN 'excluding zero-match queries' THEN 2
                                               WHEN 'trigram-friendly patterns' THEN 3 ELSE 4 END);
SQL
} | tee "$REPORT"

qs "\copy (SELECT round, qid, method, exec_ms, plan_ms, hit, rd, cnt, valid, array_to_string(idx_used, ',') AS idx_used FROM stomata_cmp.v ORDER BY round, qid, method) TO '$OUT/samples.csv' CSV HEADER"
qs "SELECT jsonb_build_object('round', round, 'qid', qid, 'method', method, 'plan', j->'plan') FROM stomata_cmp.samples ORDER BY seq" > "$OUT/plans.jsonl"
qs "\copy (SELECT qid, kind, label, trgm_ok, cond FROM stomata_cmp.queries ORDER BY qid) TO '$OUT/queries.csv' CSV HEADER"
echo
echo "report: $REPORT   samples: $OUT/samples.csv   plans: $OUT/plans.jsonl"
