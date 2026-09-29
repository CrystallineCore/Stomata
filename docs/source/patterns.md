# Keys and patterns

## Values

Each indexed value is decoded into characters (UTF-8 multibyte characters
count as one character), and ASCII letters `A`-`Z` are folded to `a`-`z`.
Two strings are formed:

- `F`: the value followed by a terminator character `\0`;
- `R`: the reversed value followed by `\0`.

The terminator lets keys express "the value starts or ends here". Positions
are grouped into segments of `k` characters (default 3): position `i`
belongs to segment `i / k`.

## Key families

With default options a value produces a few dozen keys. `stomata_keys()`
lists them:

```sql
SELECT * FROM stomata_keys('hello');
```

| key | meaning |
|---|---|
| `P:d:i:c` | character `c` at position `i` of `F` or `R` (`d` = `F` or `R`), for `i < k` |
| `U:d:s:c` | character `c` somewhere in segment `s`, for the first `cap` segments of `F` and the first `reverse_depth` segments of `R` |
| `U:F:s:\0` | the segment holding the terminator: encodes the length in segments (always stored) |
| `T:d:s:xy` | `x` immediately followed by `y`, both in segment `s` (same limits as `U`) |
| `C:d:s:xy` | `x` ends segment `s` and `y` starts segment `s+1` (only with `cross_edges`) |
| `G1:c` .. `G4:wxyz` | n-grams of `F` anywhere, up to length `rollup` |
| `H:F:...`, `H:R:...` | the first (last) 2 to `anchor_len` characters of `F` (`R`) as one key |
| `Sd:xy` | `x` at some position and `y` exactly `d` positions later, `d` = 2..`skip_depth` |
| `X.*` | first and/or last one or two characters of the same value, and first or last character with the length in segments (only with `composite`) |
| `N` | the value contains a non-ASCII byte |

Two further keys are used internally and never derived from a pattern:
`A` marks heap pages holding a value longer than 2000 bytes that was
inserted after the build (such values are not indexed by content), and `W` marks heap pages holding a heap-only tuple whose chain root
could not be determined. Pages under `A` or `W` are candidates for every
scan.

`H`, `S` and `X` keys each describe a property of a single value. A
combination of separate keys can be satisfied by different rows of the same
heap page; a single key that encodes the combination cannot. This matters
for the page-level tier described below.

## Two tiers

Each key's posting list is stored in one of two tiers:

- **Row tier** (called the *exact tier* in the code and in function output):
  `G3`, `G4`, `H`, `P` keys at position 0, `N`, and `G2` when
  `exact_bigrams` is on. Postings are sorted lists of row identifiers.
- **Page tier**: all other keys. Postings are sets of heap page numbers,
  stored as a sorted list or as a bitmap, whichever is smaller.

With `exact = off`, every key is in the page tier.

The row tier holds the keys that identify few rows; the page tier holds keys
that are common but still useful to exclude pages. A page-tier key costs at
most one entry per heap page rather than one per row.

## Compiling a pattern

A pattern is compiled into the set of keys that every matching value must
contain. `stomata_pattern_keys()` shows the result:

```sql
SELECT * FROM stomata_pattern_keys('ab%yz');
```

- The pattern is split at unescaped `%` into literal pieces. Backslash
  escapes and the `ESCAPE` clause are honoured.
- The first piece, unless the pattern starts with `%`, is anchored at the
  start and produces forward positional keys (`P`, `U`, `T`) and `H` keys.
- The last piece, unless the pattern ends with `%`, is anchored at the end
  and produces reverse keys.
- A pattern without `%` is anchored at both ends and also fixes the length.
- `_` stands for exactly one character: it shifts positions but produces no
  key of its own.
- Every literal run produces n-gram keys up to `rollup`.
- Pairs of known characters up to `skip_depth` apart produce skip-gram keys,
  whatever lies between them.
- Composite keys are produced when the first or last characters are known.

A pattern that yields no keys (for example `'%'` or `'_%'`) reads every
heap page.

## Evaluating a pattern

For one conjunction of keys (one or more `LIKE`/`ILIKE` conditions on the
same index), the scan:

1. looks each key up in every run of the index; a key found in no run means
   no row can match, and the scan returns nothing;
2. intersects the row-tier lists, rarest first;
3. ANDs the page-tier sets;
4. keeps the row identifiers whose page is in the page set; if there are no
   row-tier keys, returns every page in the page set as a whole;
5. adds rows from the pending list that match the pattern (tested with
   `LIKE`/`ILIKE` itself), and the pages under `A` and `W`.

Every returned row is rechecked by the executor.

## ILIKE

For `ILIKE`, the pattern is ASCII-folded, and non-ASCII characters in it are
treated as `_` when choosing keys. PostgreSQL's `ILIKE` folds case according
to the collation, which can change non-ASCII characters in ways the ASCII
fold does not, so rows marked `N` are candidates for every `ILIKE`
condition. Results remain correct; tables with many non-ASCII values get more
rechecks for `ILIKE`.

## Collations

Key extraction works on bytes and characters. For a condition under a
nondeterministic collation, where equal strings can differ in bytes, the
condition does not restrict the scan and every page is a candidate.
