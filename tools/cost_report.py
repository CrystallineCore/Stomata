#!/usr/bin/env python3
"""cost_report.py - summarize cost_oracle.py results.

For each scenario (set of available methods) the planner's choice is the
method with the lowest estimated cost; the oracle is the fastest.  Reports
per-query choices, the regret (chosen / oracle time), and the time per
cost unit of each method (a well-calibrated model has similar values).
"""
import json, math, sys, statistics

files = sys.argv[1:]
rows = []
for f in files:
    rows += json.load(open(f))


def choose(r, ms):
    c = [(r["methods"][m]["est_cost"], m) for m in ms if r["methods"].get(m, {}).get("applicable")]
    return min(c)[1] if c else None


def oracle(r, ms):
    c = [(r["methods"][m]["ms"], m) for m in ms if r["methods"].get(m, {}).get("applicable")]
    return min(c)[1] if c else None


scen = [("seq+stomata", ["seq", "stomata"]), ("seq+gin", ["seq", "gin"]),
        ("seq+gin+stomata", ["seq", "gin", "stomata"])]
print("%-3s %-44s %8s %8s %8s | %9s %9s %9s | choice s+st  s+g+st" %
      ("q", "clause", "seq", "gin", "stomata", "c_seq", "c_gin", "c_st"))
for r in rows:
    m = r["methods"]
    g = lambda k, f: ("%8.1f" % m[k][f]) if m.get(k, {}).get("applicable") else "%8s" % "-"
    h = lambda k: ("%9.0f" % m[k]["est_cost"]) if m.get(k, {}).get("applicable") else "%9s" % "-"
    marks = []
    for name, ms in (scen[0], scen[2]):
        c, o = choose(r, ms), oracle(r, ms)
        if c is None:
            marks.append("   -   ")
            continue
        reg = m[c]["ms"] / m[o]["ms"]
        marks.append("%-4s%s" % (c[:4], ("  " if reg < 1.2 else " x%.1f" % reg)))
    print("%-3s %-44s %s %s %s | %s %s %s | %s" % (r["q"], r["clause"][:44], g("seq", "ms"), g("gin", "ms"),
          g("stomata", "ms"), h("seq"), h("gin"), h("stomata"), "  ".join(marks)))

print()
for name, ms in scen:
    regs, wrong, n, lost = [], 0, 0, 0.0
    for r in rows:
        c, o = choose(r, ms), oracle(r, ms)
        if c is None or not all(r["methods"].get(x, {}).get("applicable") for x in ms):
            continue
        n += 1
        t_c, t_o = r["methods"][c]["ms"], r["methods"][o]["ms"]
        regs.append(t_c / t_o)
        lost += t_c - t_o
        if t_c / t_o > 1.2:
            wrong += 1
    if n:
        gm = math.exp(sum(math.log(x) for x in regs) / n)
        print("%-16s queries %2d  costly mistakes (>1.2x) %2d  geo-mean regret %.3f  max %.2f  "
              "time lost %.0f ms of oracle %.0f ms" %
              (name, n, wrong, gm, max(regs), lost,
               sum(r["methods"][oracle(r, ms)]["ms"] for r in rows
                   if choose(r, ms) and all(r["methods"].get(x, {}).get("applicable") for x in ms))))

print("\nms per 1000 cost units (median, IQR) - calibration")
for m in ("seq", "gin", "stomata"):
    v = [r["methods"][m]["ms"] / r["methods"][m]["est_cost"] * 1000 for r in rows
         if r["methods"].get(m, {}).get("applicable")]
    if v:
        q = statistics.quantiles(v, n=4)
        print("  %-8s n=%2d  median %.3f  IQR %.3f..%.3f  min %.3f max %.3f" %
              (m, len(v), statistics.median(v), q[0], q[2], min(v), max(v)))
