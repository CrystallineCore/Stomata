"""Cross-check the C key extractor against the Python reference model.

Usage: python3 tools/crosscheck.py "host=/tmp port=5416 dbname=smoke user=postgres"
Requires psycopg (pip install psycopg[binary]) or falls back to psql.
"""
import json, random, subprocess, sys
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import stomata_ref as ref

TERM = "\0"
LEAN = lambda key: (key[0] == "C") or (key[0] in "PUTC" and key[1] == "R" and
                                        ((key[2] // 3) if key[0] == "P" else key[2]) >= 1)


def render(key):
    t = lambda s: s.replace(TERM, "\\0")
    tag = key[0]
    if tag in ("P", "U"):
        return f"{tag}:{key[1]}:{key[2]}:{t(key[3])}"
    if tag in ("T", "C"):
        return f"{tag}:{key[1]}:{key[2]}:{t(key[3] + key[4])}"
    if tag == "G1":
        return "G1:" + t(key[1])
    if tag == "G2":
        return "G2:" + t(key[1] + key[2])
    if tag == "G3":
        return "G3:" + t(key[1])
    if tag == "X":
        sub = key[1]
        if sub == "ht":
            return f"X.ht:{t(key[2])},{t(key[3])}"
        if sub == "HT":
            return f"X.HT:{t(key[2])},{t(key[3])}"
        return f"X.{sub}:{t(key[2])}:{key[3]}"
    raise ValueError(key)


def ref_keys(s):
    ix = ref.Stomata(k=3, rollup=3, cap=2, drop=LEAN, composite=True)
    return {render(k) for k in ix.keys_for(s) if not LEAN(k)}


def main(conninfo):
    rng = random.Random(1)
    alpha = list("abcdeABC_%\\ .xyz") + ["é", "日", "ß"]
    strings = [""] + ["".join(rng.choice(alpha) for _ in range(rng.randint(1, 40))) for _ in range(3000)]
    sql = ("SELECT json_agg(json_build_object('s', s, 'k', (SELECT json_agg(x) FROM stomata_keys(s) x))) "
           "FROM json_array_elements_text(%s::json) s")
    payload = json.dumps(strings)
    q = sql.replace("%s", "$$" + payload + "$$")
    out = subprocess.run(["psql", conninfo, "-XAtc", q], capture_output=True, text=True, check=True).stdout
    rows = json.loads(out)
    bad = 0
    # key families the reference model does not implement
    extra = ("H:", "S2:", "S3:", "S4:", "S5:", "S6:", "S7:", "S8:", "X.Ht:", "X.hT:", "G4:", "N")
    for r in rows:
        c = {k for k in (r["k"] or []) if not k.startswith(extra)}
        p = ref_keys(r["s"])
        if c != p:
            bad += 1
            if bad <= 3:
                print("MISMATCH", repr(r["s"]), "C-only:", sorted(c - p)[:5], "py-only:", sorted(p - c)[:5])
    print(f"compared {len(rows)} strings, mismatches: {bad}")
    return bad


if __name__ == "__main__":
    sys.exit(1 if main(sys.argv[1]) else 0)
