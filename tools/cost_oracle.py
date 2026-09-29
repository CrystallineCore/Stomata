#!/usr/bin/env python3
"""cost_oracle.py - how close is the planner's choice to the fastest plan?

For every query it forces each access method in turn (sequential scan,
pg_trgm GIN bitmap scan, stomata bitmap scan), with the other indexes
dropped inside a transaction that is rolled back.  For each it records the
planner's estimated total cost and the measured execution time (median of
--reps runs after a warm-up).  The oracle is the fastest method.  Because
the planner picks the path with the lowest estimated cost, the plan it
would choose among any subset of methods can be derived from the forced
estimates, and its "regret" is chosen time / oracle time.

  python3 tools/cost_oracle.py --dsn "host=/tmp port=5416 dbname=inter user=postgres" \\
      --table interactions --column username --gin ix_trgm --stomata ix_st \\
      --queries queries.txt --out results.json

queries.txt holds one WHERE clause per line (# comments allowed).
"""
import argparse, json, statistics, sys, time
import psycopg

p = argparse.ArgumentParser()
p.add_argument("--dsn", required=True)
p.add_argument("--table", required=True)
p.add_argument("--column", required=True)
p.add_argument("--gin", help="name of the pg_trgm GIN index (optional)")
p.add_argument("--stomata", required=True, help="name of the stomata index")
p.add_argument("--queries", required=True)
p.add_argument("--reps", type=int, default=5)
p.add_argument("--out", required=True)
p.add_argument("--label", default="")
p.add_argument("--parallel", action="store_true", help="allow parallel seq scans")
p.add_argument("--set", action="append", default=[], help="GUC=value applied to the session (repeatable)")
a = p.parse_args()

clauses = [l.strip() for l in open(a.queries) if l.strip() and not l.lstrip().startswith("#")]
conn = psycopg.connect(a.dsn, autocommit=True)
cur = conn.cursor()
cur.execute("SET jit = off")
if not a.parallel:
    cur.execute("SET max_parallel_workers_per_gather = 0")
for kv in a.set:
    k, v = kv.split("=", 1)
    cur.execute("SELECT set_config(%s, %s, false)", (k.strip(), v.strip()))

cur.execute("""SELECT indexrelid::regclass::text FROM pg_index
               WHERE indrelid = %s::regclass""", (a.table,))
all_indexes = [r[0] for r in cur.fetchall()]
methods = {"seq": None, "stomata": a.stomata}
if a.gin:
    methods["gin"] = a.gin


def walk(node, out):
    out.append(node)
    for c in node.get("Plans", []):
        walk(c, out)
    return out


def explain(sql, analyze, timing=False):
    opts = "ANALYZE, BUFFERS, TIMING %s, " % ("ON" if timing else "OFF") if analyze else ""
    cur.execute("EXPLAIN (%sFORMAT JSON) %s" % (opts, sql))
    return cur.fetchone()[0][0]


def run_method(sql, method):
    idx = methods[method]
    cur.execute("BEGIN")
    try:
        for i in all_indexes:
            if i != idx and i in methods.values():
                cur.execute("DROP INDEX %s" % i)
        if method == "seq":
            cur.execute("SET LOCAL enable_bitmapscan = off")
            cur.execute("SET LOCAL enable_indexscan = off")
            cur.execute("SET LOCAL enable_indexonlyscan = off")
        else:
            cur.execute("SET LOCAL enable_seqscan = off")
            cur.execute("SET LOCAL enable_indexscan = off")
            cur.execute("SET LOCAL enable_indexonlyscan = off")
        plan = explain(sql, False)
        nodes = walk(plan["Plan"], [])
        used = {n.get("Index Name") for n in nodes if n.get("Index Name")}
        seq = any(n["Node Type"] == "Seq Scan" for n in nodes)
        ok = (method == "seq" and seq and not used) or (method != "seq" and not seq and used == {idx})
        if not ok:
            return {"method": method, "applicable": False}
        rec = {"method": method, "applicable": True, "est_cost": plan["Plan"]["Total Cost"]}
        for n in nodes:
            if n["Node Type"] == "Bitmap Index Scan" and n.get("Index Name") == idx:
                rec["est_index_cost"] = rec.get("est_index_cost", 0) + n["Total Cost"]
            if n["Node Type"] in ("Bitmap Heap Scan", "Seq Scan"):
                rec["est_rows"] = n["Plan Rows"]
        explain(sql, True)                      # warm-up
        times, ptimes = [], []
        for _ in range(a.reps):
            r = explain(sql, True)
            times.append(r["Execution Time"])
            ptimes.append(r["Planning Time"])
        rec["ms"] = statistics.median(times)
        rec["ms_all"] = times
        rec["plan_ms"] = statistics.median(ptimes)
        r = explain(sql, True, timing=True)
        for n in walk(r["Plan"], []):
            if n["Node Type"] == "Bitmap Index Scan" and n.get("Index Name") == idx:
                rec["index_ms"] = rec.get("index_ms", 0) + n["Actual Total Time"]
            if n["Node Type"] == "Bitmap Heap Scan":
                rec["exact_blocks"] = n.get("Exact Heap Blocks", 0)
                rec["lossy_blocks"] = n.get("Lossy Heap Blocks", 0)
                rec["recheck_removed"] = n.get("Rows Removed by Index Recheck", 0)
                rec["heap_ms"] = n["Actual Total Time"]
            if n["Node Type"] in ("Bitmap Heap Scan", "Seq Scan"):
                rec["rows"] = n["Actual Rows"]
                rec["filter_removed"] = n.get("Rows Removed by Filter", 0)
        return rec
    finally:
        cur.execute("ROLLBACK")


results = []
t0 = time.time()
for qi, clause in enumerate(clauses):
    sql = "SELECT count(*) FROM %s WHERE %s" % (a.table, clause)
    row = {"label": a.label, "q": qi + 1, "clause": clause, "methods": {}}
    # default plan with every index present (the planner's real choice)
    plan = explain(sql, False)
    nodes = walk(plan["Plan"], [])
    row["default_indexes"] = sorted({n.get("Index Name") for n in nodes if n.get("Index Name")})
    row["default_seq"] = any(n["Node Type"] == "Seq Scan" for n in nodes)
    for m in methods:
        row["methods"][m] = run_method(sql, m)
    results.append(row)
    ms = {m: (v["ms"] if v.get("applicable") else None) for m, v in row["methods"].items()}
    print("%2d %-60s %s" % (qi + 1, clause[:60],
          "  ".join("%s=%s" % (m, "-" if t is None else "%.1f" % t) for m, t in ms.items())),
          file=sys.stderr, flush=True)

json.dump(results, open(a.out, "w"), indent=1)
print("done in %.0f s" % (time.time() - t0), file=sys.stderr)
