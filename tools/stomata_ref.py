"""
STOMATA -- STOred autoMATA index: reference implementation (proof of concept).

A record-aware, segmented, bidirectional positional n-gram index for SQL LIKE.

Representation
--------------
Each indexed string s (length n) is case-folded per code point and terminated:
    forward  F = fold(s)          + '\\0'     (terminator at position n)
    reverse  R = reverse(fold(s)) + '\\0'     (terminator at position n)
Positions are split into segments of k:  seg(i) = i // k.

For each direction d in {F, R} the index stores record bitmaps for
    P(d, i, c)       exact character at position i, for i < head   (START = P(d,0,c))
    U(d, s, c)       character c occurs somewhere in segment s      (node state)
    T(d, s, x, y)    x immediately followed by y, both inside segment s   (intra)
    C(d, s, x, y)    x is the last char of segment s, y the first of s+1  (cross)
plus optional direction-free rollups for infix search
    G1(c), G2(x, y)  character / bigram present anywhere.

Bitmaps are Python ints (bit r set <=> record r), standing in for the TID
posting lists GIN would store.

Correctness invariant:   TrueMatches(P)  is a subset of  Candidates(P).
Every key a query emits is *implied* by any string that matches the pattern,
so the AND/OR evaluation can only over-approximate. Recheck removes the rest.
"""
from __future__ import annotations

import re
from collections import defaultdict
from dataclasses import dataclass, field

TERM = "\0"


# --------------------------------------------------------------------------
# Case folding: per code point and length preserving (never changes positions)
# --------------------------------------------------------------------------
def fold_char(c: str) -> str:
    lc = c.lower()
    return lc if len(lc) == 1 else c          # e.g. 'I-dot' (U+0130).lower() has 2 code points


def fold(s: str) -> str:
    return "".join(fold_char(c) for c in s)


# --------------------------------------------------------------------------
# Index
# --------------------------------------------------------------------------
class Stomata:
    def __init__(self, k: int = 3, head: int | None = None, rollup: int = 3,
                 cap: int | None = None, drop=None, gran: int = 1, composite: bool = False):
        self.composite = composite  # record-level conjunction keys (head x tail x length)
        self.drop = drop          # predicate: key families deliberately not stored (query treats as TRUE)
        self.gran = gran          # rows per posting unit (1 = row-level, >1 = page/block-level)
        self.stop: set = set()    # dense keys removed after build (query treats as TRUE)
        self.k = k
        self.head = k if head is None else max(1, head)   # exact offsets for the first `head` positions
        self.rollup = rollup      # max order of global rollup grams (0 = none, 2 = G1+G2, 3 = +G3)
        self.cap = cap            # positional keys only for segments < cap (per direction); None = all
        self.head = min(self.head, k * cap) if cap else self.head
        self.post: dict[tuple, int] = defaultdict(int)
        self.n = 0
        self.max_len = 0

    # ---- build -----------------------------------------------------------
    def keys_for(self, s: str) -> set[tuple]:
        k, head = self.k, self.head
        lim = self.cap * k if self.cap else None          # first position NOT covered
        f = fold(s)
        keys: set[tuple] = set()
        for d, t in (("F", f + TERM), ("R", f[::-1] + TERM)):
            tt = t if lim is None else t[:lim]
            for i, c in enumerate(tt):
                keys.add(("U", d, i // k, c))
                if i < head:
                    keys.add(("P", d, i, c))
            for i in range(len(tt) - 1):
                x, y = tt[i], tt[i + 1]
                si, sj = i // k, (i + 1) // k
                keys.add(("T", d, si, x, y) if si == sj else ("C", d, si, x, y))
        if lim is not None and len(f) >= lim:             # length key survives the cap:
            keys.add(("U", "F", len(f) // k, TERM))       # terminator segment = length / k
        t = f + TERM
        if self.rollup >= 1:
            keys.update(("G1", c) for c in t)
        if self.rollup >= 2:
            keys.update(("G2", t[i], t[i + 1]) for i in range(len(t) - 1))
        if self.rollup >= 3:
            keys.update(("G3", t[i:i + 3]) for i in range(len(t) - 2))
        if self.composite and f:
            L = len(f) // k                                 # terminator segment (length bucket)
            keys.add(("X", "ht", f[0], f[-1]))
            keys.add(("X", "HL", f[0], L))
            keys.add(("X", "TL", f[-1], L))
            if len(f) >= 2:
                keys.add(("X", "HT", f[:2], f[-2:]))
        return keys

    def add(self, s: str) -> int:
        rid = self.n
        bit = 1 << (rid // self.gran)
        drop = self.drop
        for key in self.keys_for(s):
            if drop is None or not drop(key):
                self.post[key] |= bit
        self.n += 1
        self.max_len = max(self.max_len, len(s))
        return rid

    def build(self, strings) -> "Stomata":
        for s in strings:
            self.add(s)
        return self

    @property
    def units(self) -> int:
        return (self.n + self.gran - 1) // self.gran

    def stop_dense(self, frac: float) -> "Stomata":
        """Remove posting lists covering more than `frac` of units; remember them as stop-keys."""
        lim = frac * self.units
        for key in [k for k, bm in self.post.items() if bm.bit_count() > lim]:
            self.stop.add(key); del self.post[key]
        return self

    def is_true(self, key) -> bool:
        return key in self.stop or (self.drop is not None and self.drop(key))

    # ---- stats -----------------------------------------------------------
    def stats(self) -> dict:
        by_kind = defaultdict(lambda: [0, 0])
        for key, bm in self.post.items():
            by_kind[key[0]][0] += 1
            by_kind[key[0]][1] += bm.bit_count()
        return {
            "keys": len(self.post),
            "postings": sum(v[1] for v in by_kind.values()),
            "by_kind": {k: tuple(v) for k, v in sorted(by_kind.items())},
        }

    # ---- query -----------------------------------------------------------
    def search(self, pattern: str, infix: str = "segmented", escape: str = "\\"):
        plan = compile_like(pattern, self, infix=infix, escape=escape)
        return plan.evaluate(self)


# --------------------------------------------------------------------------
# Pattern compiler: LIKE pattern -> Stomata query plan (AND of terms,
# each term a key or an OR over alternative AND-groups)
# --------------------------------------------------------------------------
LIT, ONE, ANY = "L", "_", "%"


def parse_like(pattern: str, escape: str = "\\") -> list[tuple]:
    items, i = [], 0
    while i < len(pattern):
        c = pattern[i]
        if escape and c == escape and i + 1 < len(pattern):
            items.append((LIT, fold_char(pattern[i + 1]))); i += 2; continue
        if c == "%":
            if not items or items[-1][0] != ANY:
                items.append((ANY,))
        elif c == "_":
            items.append((ONE,))
        else:
            items.append((LIT, fold_char(c)))
        i += 1
    return items


def split_pieces(items):
    pieces, cur = [], []
    for it in items:
        if it[0] == ANY:
            pieces.append(cur); cur = []
        else:
            cur.append(it)
    pieces.append(cur)
    return pieces            # len(pieces)-1 == number of '%'


def pos_keys(idx: Stomata, d: str, piece, start: int) -> list[tuple]:
    """Keys implied by `piece` occupying positions start.. in direction d."""
    k, head = idx.k, idx.head
    lim = idx.cap * k if idx.cap else float("inf")
    lits = [(start + j, it[1]) for j, it in enumerate(piece) if it[0] == LIT
            and (start + j < lim or (d == "F" and it[1] == TERM))]
    keys = []
    for p, c in lits:
        keys.append(("P", d, p, c) if p < head else ("U", d, p // k, c))
    for (p, x), (q, y) in zip(lits, lits[1:]):
        if q == p + 1 and q < lim:
            keys.append(("T", d, p // k, x, y) if p // k == q // k else ("C", d, p // k, x, y))
    return keys


@dataclass
class Plan:
    must: list = field(default_factory=list)          # keys that must all be present
    alts: list = field(default_factory=list)          # list of OR-groups: [ [keys...], [keys...] ]
    full_scan: bool = False

    def evaluate(self, idx: Stomata):
        cache, stats = {}, {"probes": 0, "postings_read": 0, "bitmap_ops": 0}

        def get(key):
            if key not in cache:
                if idx.is_true(key):
                    cache[key] = allbits
                    return allbits
                bm = idx.post.get(key, 0)
                cache[key] = bm
                stats["probes"] += 1
                stats["postings_read"] += bm.bit_count()
            return cache[key]

        allbits = (1 << idx.units) - 1
        if self.full_scan:
            return allbits, stats | {"full_scan": True}
        acc = allbits
        for key in self.must:
            acc &= get(key); stats["bitmap_ops"] += 1
            if not acc:
                return 0, stats | {"full_scan": False}
        for group in self.alts:
            orbm = 0
            for conj in group:
                bm = acc
                for key in conj:
                    bm &= get(key); stats["bitmap_ops"] += 1
                    if not bm:
                        break
                orbm |= bm; stats["bitmap_ops"] += 1
            acc &= orbm
            if not acc:
                break
        return acc, stats | {"full_scan": False}


def compile_like(pattern: str, idx: Stomata, infix: str = "segmented", escape: str = "\\") -> Plan:
    items = parse_like(pattern, escape)
    pieces = split_pieces(items)
    exact = len(pieces) == 1                       # no '%': anchored at both ends, length known
    head, tail = pieces[0], pieces[-1]
    middles = pieces[1:-1]
    plan = Plan()

    if exact:
        whole = head + [(LIT, TERM)]               # terminator is a real state at position n
        plan.must += pos_keys(idx, "F", whole, 0)
        plan.must += pos_keys(idx, "R", list(reversed(head)) + [(LIT, TERM)], 0)
    else:
        if head:                                   # anchored prefix  -> forward automaton from START
            plan.must += pos_keys(idx, "F", head, 0)
        if tail:                                   # anchored suffix  -> reverse automaton from START
            plan.must += pos_keys(idx, "R", list(reversed(tail)), 0)

    if idx.composite:
        plan.must += composite_keys(idx, head, tail, exact)

    min_start = len(head)
    for piece in middles:                          # floating pieces (infix)
        if not any(it[0] == LIT for it in piece):
            continue
        if idx.rollup:
            plan.must += rollup_keys(piece, idx.rollup)
        if infix == "segmented" or not idx.rollup:
            plan.alts.append(alignments(idx, piece, min_start))
        min_start += len(piece)                    # later pieces start after earlier ones

    plan.must = list(dict.fromkeys(plan.must))
    if not plan.must and not plan.alts:
        plan.full_scan = True
    return plan


def composite_keys(idx: Stomata, head, tail, exact: bool) -> list[tuple]:
    """Record-level conjunctions. At page granularity a plain AND can pair the prefix of one
    row with the suffix of another row on the same page; these keys bind them to one row."""
    def run(items):
        out = []
        for it in items:
            if it[0] != LIT: break
            out.append(it[1])
        return "".join(out)
    h = run(head)
    t = run(list(reversed(tail)))[::-1] if tail else ""
    keys = []
    if h and t:
        keys.append(("X", "ht", h[0], t[-1]))
        if len(h) >= 2 and len(t) >= 2:
            keys.append(("X", "HT", h[:2], t[-2:]))
    if exact and head:
        L = len(head) // idx.k
        if h: keys.append(("X", "HL", h[0], L))
        if t: keys.append(("X", "TL", t[-1], L))
    return keys


def rollup_keys(piece, order: int) -> list[tuple]:
    """Global (position-free) keys: highest-order gram available for each literal run."""
    keys, run = [], []
    for it in piece + [(ONE,)]:                     # sentinel flushes the last run
        if it[0] == LIT:
            run.append(it[1]); continue
        if run:
            w = "".join(run); g = min(order, len(w))
            if g == 1:   keys += [("G1", c) for c in w]
            elif g == 2: keys += [("G2", w[i], w[i + 1]) for i in range(len(w) - 1)]
            else:        keys += [("G3", w[i:i + 3]) for i in range(len(w) - 2)]
        run = []
    return keys


def alignments(idx: Stomata, piece, min_start: int) -> list[list[tuple]]:
    """Enumerate every start position the floating piece can take; positions that
    produce the same key set (same phase, same segment) are deduplicated."""
    seen, out = set(), []
    last = idx.max_len - len(piece)
    for p in range(min_start, last + 1):
        ks = tuple(pos_keys(idx, "F", piece, p))
        if ks not in seen:
            seen.add(ks); out.append(list(ks))
    return out


# --------------------------------------------------------------------------
# Ground truth: SQL LIKE semantics (the executor's recheck)
# --------------------------------------------------------------------------
def like_regex(pattern: str, escape: str = "\\") -> re.Pattern:
    out, i = [], 0
    while i < len(pattern):
        c = pattern[i]
        if escape and c == escape and i + 1 < len(pattern):
            out.append(re.escape(pattern[i + 1])); i += 2; continue
        out.append(".*" if c == "%" else "." if c == "_" else re.escape(c))
        i += 1
    return re.compile("".join(out), re.S)


def like(s: str, pattern: str) -> bool:
    return like_regex(pattern).fullmatch(s) is not None


# --------------------------------------------------------------------------
# Baseline: simplified model of pg_trgm (GIN, LIKE support)
# --------------------------------------------------------------------------
class Trigram:
    """Words = maximal alphanumeric runs, lower-cased, padded '  w ' (pg_trgm style).
    LIKE extraction: literal word-runs of the pattern; left padding only when the run
    is not preceded by a wildcard, right padding only when not followed by one."""

    def __init__(self):
        self.post = defaultdict(int); self.n = 0

    @staticmethod
    def _trgms(padded):
        return {padded[i:i + 3] for i in range(len(padded) - 2)}

    def add(self, s):
        bit = 1 << self.n
        for w in re.findall(r"[^\W_]+", fold(s)):
            for t in self._trgms("  " + w + " "):
                self.post[t] |= bit
        self.n += 1

    def build(self, strings):
        for s in strings:
            self.add(s)
        return self

    def stats(self):
        return {"keys": len(self.post), "postings": sum(b.bit_count() for b in self.post.values())}

    def query_trgms(self, pattern, escape="\\"):
        # tokens: ('w', ch) word char, ('s',) separator literal, ('m',) wildcard meta
        toks, i = [], 0
        while i < len(pattern):
            c = pattern[i]
            if escape and c == escape and i + 1 < len(pattern):
                c2 = fold_char(pattern[i + 1]); i += 2
                toks.append(("w", c2) if c2.isalnum() else ("s",)); continue
            if c in "%_":
                toks.append(("m",))
            else:
                c = fold_char(c); toks.append(("w", c) if c.isalnum() else ("s",))
            i += 1
        out, j = set(), 0
        while j < len(toks):
            if toks[j][0] != "w":
                j += 1; continue
            st = j
            while j < len(toks) and toks[j][0] == "w":
                j += 1
            word = "".join(t[1] for t in toks[st:j])
            lpad = "  " if (st == 0 or toks[st - 1][0] == "s") else ""
            rpad = " " if (j == len(toks) or toks[j][0] == "s") else ""
            out |= self._trgms(lpad + word + rpad)
        return out

    def search(self, pattern):
        ts = self.query_trgms(pattern)
        stats = {"probes": len(ts), "postings_read": 0, "full_scan": not ts}
        acc = (1 << self.n) - 1
        for t in ts:
            bm = self.post.get(t, 0)
            stats["postings_read"] += bm.bit_count()
            acc &= bm
        return acc, stats
