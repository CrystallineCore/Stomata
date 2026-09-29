#!/usr/bin/env bash
# crud_wal.sh - WAL volume and time of CRUD operations: stomata vs pg_trgm.
#
# For each method (no index, pg_trgm GIN with and without fastupdate,
# stomata) a fresh copy of one text column is made, the index is built, and
# the same operations run in the same order.  Every operation starts after a
# CHECKPOINT (so every method pays the same first-touch full-page images) and
# is measured by WAL position and wall time.  "index WAL" is the method's WAL
# minus the no-index run: what the index itself costs.  Deferred index work
# (pending lists) is included by the final VACUUM of each run.
#
#   tools/crud_wal.sh --table interactions --column username [options]
#
# Options:
#   --dsn STR          libpq connection string (default: PG* environment)
#   --rows N           rows copied from the source (default: all)
#   --batch N          rows per bulk INSERT / UPDATE / DELETE (default 100000)
#   --singles N        single-row transactions per operation (default 5000)
#   --methods LIST     default "none trgm trgm_nofastupdate stomata"
#   --reps N           repetitions of the whole run; medians reported (default 1)
#   --out FILE         report file (default ./crud_wal_<timestamp>.txt)
#
# Needs a quiet server (WAL is measured cluster-wide) and permission to run
# CHECKPOINT (superuser or pg_checkpoint).  Creates and drops the table
# stomata_crud and the schema stomata_crud_results.
set -euo pipefail

DSN=""; TABLE=""; COL=""; ROWS=""; BATCH=100000; SINGLES=5000
METHODS="none trgm trgm_nofastupdate stomata"; REPS=1; OUT=""
die() { echo "error: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
  case "$1" in
    --dsn) DSN=$2; shift 2;;
    --table) TABLE=$2; shift 2;;
    --column) COL=$2; shift 2;;
    --rows) ROWS=$2; shift 2;;
    --batch) BATCH=$2; shift 2;;
    --singles) SINGLES=$2; shift 2;;
    --methods) METHODS=$2; shift 2;;
    --reps) REPS=$2; shift 2;;
    --out) OUT=$2; shift 2;;
    -h|--help) sed -n '2,26p' "$0"; exit 0;;
    *) die "unknown option $1";;
  esac
done
[ -n "$TABLE" ] && [ -n "$COL" ] || die "--table and --column are required"
OUT=${OUT:-./crud_wal_$(date +%Y%m%d_%H%M%S).txt}
PSQL=(psql -X -q -v ON_ERROR_STOP=1)
[ -n "$DSN" ] && PSQL+=(-d "$DSN")
q() { "${PSQL[@]}" -At "$@"; }

q -c "CREATE EXTENSION IF NOT EXISTS pg_trgm" -c "CREATE EXTENSION IF NOT EXISTS stomata"
q <<'SQL'
DROP SCHEMA IF EXISTS stomata_crud_results CASCADE;
CREATE SCHEMA stomata_crud_results;
CREATE TABLE stomata_crud_results.r (rep int, method text, seq int, op text, ms numeric, wal bigint);
CREATE TABLE stomata_crud_results.size (rep int, method text, stage text, bytes bigint);
SQL
LIMIT=""; [ -n "$ROWS" ] && LIMIT="LIMIT $ROWS"

index_ddl() {
  case "$1" in
    none) echo "";;
    trgm) echo "CREATE INDEX stomata_crud_idx ON stomata_crud USING gin (s gin_trgm_ops)";;
    trgm_nofastupdate) echo "CREATE INDEX stomata_crud_idx ON stomata_crud USING gin (s gin_trgm_ops) WITH (fastupdate = off)";;
    stomata) echo "CREATE INDEX stomata_crud_idx ON stomata_crud USING stomata (s)";;
    *) die "unknown method $1";;
  esac
}

# run one measured operation: $1 rep, $2 method, $3 seq, $4 label, $5 SQL
measure() {
  "${PSQL[@]}" -v rep="$1" -v method="$2" -v seq="$3" -v op="$4" <<SQL
CHECKPOINT;
SELECT pg_current_wal_lsn() AS l0, clock_timestamp() AS t0 \gset
\o /dev/null
$5
\o
SELECT round((extract(epoch FROM clock_timestamp() - :'t0'::timestamptz) * 1000)::numeric, 1) AS ms,
       pg_current_wal_lsn() - :'l0'::pg_lsn AS wal \gset
INSERT INTO stomata_crud_results.r VALUES (:rep, :'method', :seq, :'op', :ms, :wal);
SQL
}

size() {
  q -c "INSERT INTO stomata_crud_results.size SELECT $1, '$2', '$3',
        coalesce(pg_relation_size(to_regclass('stomata_crud_idx')), 0)"
}

for rep in $(seq 1 "$REPS"); do
  for m in $METHODS; do
    echo "rep $rep: $m"
    q -c "DROP TABLE IF EXISTS stomata_crud" \
      -c "CREATE TABLE stomata_crud (id bigserial PRIMARY KEY, s text) WITH (autovacuum_enabled = off)" \
      -c "INSERT INTO stomata_crud (s) SELECT $COL::text FROM $TABLE WHERE $COL IS NOT NULL $LIMIT" \
      -c "VACUUM (ANALYZE) stomata_crud"
    ddl="$(index_ddl "$m")"
    measure "$rep" "$m" 1 "create index" "${ddl:-SELECT 1};"
    size "$rep" "$m" "built"
    measure "$rep" "$m" 2 "insert: bulk ($BATCH rows)" \
      "INSERT INTO stomata_crud (s) SELECT s || 'n' FROM stomata_crud ORDER BY id LIMIT $BATCH;"
    measure "$rep" "$m" 3 "insert: $SINGLES single-row txns" \
      "DO \$\$ DECLARE r record; BEGIN FOR r IN SELECT s FROM stomata_crud ORDER BY id LIMIT $SINGLES OFFSET $BATCH LOOP INSERT INTO stomata_crud (s) VALUES (r.s || 'i'); COMMIT; END LOOP; END \$\$;"
    measure "$rep" "$m" 4 "update: bulk ($BATCH rows)" \
      "UPDATE stomata_crud SET s = s || 'u' WHERE id % 10 = 3 AND id <= (SELECT min(id) FROM stomata_crud) + $BATCH * 10;"
    measure "$rep" "$m" 5 "update: $SINGLES single-row txns" \
      "DO \$\$ DECLARE i bigint; BEGIN FOR i IN SELECT id FROM stomata_crud WHERE id % 10 = 5 ORDER BY id LIMIT $SINGLES LOOP UPDATE stomata_crud SET s = s || 'v' WHERE id = i; COMMIT; END LOOP; END \$\$;"
    measure "$rep" "$m" 6 "delete: bulk ($BATCH rows)" \
      "DELETE FROM stomata_crud WHERE id % 10 = 7 AND id <= (SELECT min(id) FROM stomata_crud) + $BATCH * 10;"
    measure "$rep" "$m" 7 "delete: $SINGLES single-row txns" \
      "DO \$\$ DECLARE i bigint; BEGIN FOR i IN SELECT id FROM stomata_crud WHERE id % 10 = 9 ORDER BY id LIMIT $SINGLES LOOP DELETE FROM stomata_crud WHERE id = i; COMMIT; END LOOP; END \$\$;"
    size "$rep" "$m" "before vacuum"
    measure "$rep" "$m" 8 "vacuum (index cleanup, pending lists)" "VACUUM stomata_crud;"
    size "$rep" "$m" "after vacuum"
    measure "$rep" "$m" 9 "read: 20 LIKE queries" "DO \$\$ DECLARE p text; n bigint; BEGIN
      FOR p IN SELECT unnest(ARRAY['%kim%','%son','a%','%ez','%mc%','%an%er%','%ell%','j%s%n','%k_m%','brandy%',
                                   '%22','%1%2%','_llison%','%anderson%','%zzzqx%','allison%','%so_','%8%','kim%','%mcdon%'])
      LOOP EXECUTE format('SELECT count(*) FROM stomata_crud WHERE s LIKE %L', p) INTO n; END LOOP; END \$\$;"
  done
done
q -c "DROP TABLE IF EXISTS stomata_crud"

{
echo "CRUD WAL and time - $(date)"
q -c "SELECT version()"
echo
"${PSQL[@]}" -P footer=off <<SQL
\\echo '== per operation (median over reps): time in ms, WAL in kB; index WAL = WAL minus the no-index run'
WITH m AS (
  SELECT method, seq, op, percentile_cont(0.5) WITHIN GROUP (ORDER BY ms) AS ms,
         percentile_cont(0.5) WITHIN GROUP (ORDER BY wal) AS wal
  FROM stomata_crud_results.r GROUP BY method, seq, op),
b AS (SELECT seq, wal AS base_wal, ms AS base_ms FROM m WHERE method = 'none')
SELECT m.op, m.method, round(m.ms::numeric, 1) AS ms, round((m.wal / 1024)::numeric) AS wal_kb,
       CASE WHEN m.method = 'none' THEN NULL ELSE round(((m.wal - b.base_wal) / 1024)::numeric) END AS index_wal_kb
FROM m LEFT JOIN b USING (seq)
ORDER BY m.seq, array_position(ARRAY['none','trgm','trgm_nofastupdate','stomata'], m.method);

\\echo '== totals over all operations except create index and read (median over reps)'
WITH t AS (
  SELECT rep, method, sum(ms) AS ms, sum(wal) AS wal FROM stomata_crud_results.r
  WHERE seq BETWEEN 2 AND 8 GROUP BY rep, method),
m AS (SELECT method, percentile_cont(0.5) WITHIN GROUP (ORDER BY ms) AS ms,
             percentile_cont(0.5) WITHIN GROUP (ORDER BY wal) AS wal FROM t GROUP BY method)
SELECT method, round(ms::numeric) AS ms, pg_size_pretty(wal::bigint) AS wal,
       pg_size_pretty((wal - (SELECT wal FROM m WHERE method = 'none'))::bigint) AS index_wal
FROM m ORDER BY array_position(ARRAY['none','trgm','trgm_nofastupdate','stomata'], method);

\\echo '== index size'
SELECT method, stage, pg_size_pretty(percentile_cont(0.5) WITHIN GROUP (ORDER BY bytes)::bigint) AS size
FROM stomata_crud_results.size WHERE method <> 'none'
GROUP BY method, stage ORDER BY method, min(CASE stage WHEN 'built' THEN 1 WHEN 'before vacuum' THEN 2 ELSE 3 END);
SQL
} | tee "$OUT"
q -c "DROP SCHEMA stomata_crud_results CASCADE"
echo "report: $OUT"
