stomata 0.1.0
=============

Synopsis
--------

```sql
CREATE EXTENSION stomata;

CREATE INDEX people_name_st ON people USING stomata (name);
CREATE INDEX people_email_st ON people USING stomata (lower(email))
    WITH (cap = 4, reverse_depth = 3);

SELECT * FROM people WHERE name LIKE 'Jo%son';
SELECT * FROM people WHERE lower(email) LIKE '%@example.com';
SELECT * FROM people WHERE name ILIKE 'jo%SON';

SELECT * FROM stomata_index_info('people_name_st');
SELECT stomata_candidate_pages('people_name_st', 'Jo%son');
```

Description
-----------

`stomata` adds an index access method of the same name, and the default
operator class `stomata_text_ops` for `text`. The operator class supports two
operators: `~~` (`LIKE`, strategy 1) and `~~*` (`ILIKE`, strategy 2). `varchar` columns use it through the implicit
coercion to `text`.

The index answers "which rows may match". Keys live in one of two tiers:

- the **exact tier** (`G3`, `G4`, `H`, `P` keys at position 0, the `N`
  marker, and `G2` with `exact_bigrams`) stores sorted row TIDs, encoded as
  `block * MaxHeapTuplesPerPage + offset - 1` and delta-varbyte coded;
- the **page tier** (everything else) stores heap pages as a delta array or a
  bitmap.

When a pattern has at least one exact key the scan returns TIDs with recheck;
otherwise it returns lossy pages. Every returned tuple is rechecked by the
executor. Results are therefore always exact. The index only decides how
much is read and rechecked.

### Keys

A value `s` of `n` characters is folded (ASCII `A`-`Z` to `a`-`z`) and turned
into two strings, `F = s || '\0'` and `R = reverse(s) || '\0'`. Character `i`
belongs to segment `i / k`. The index stores these keys:

| key | stored for |
|---|---|
| `P:d:i:c` | character `c` at exact position `i < k` of `F` or `R` |
| `U:d:s:c` | character `c` somewhere in segment `s`, for the first `cap` (forward) or `reverse_depth` (reverse) segments |
| `U:F:s:\0` | the terminator's segment; always stored, it encodes `n / k` |
| `T:d:s:xy` | `x` immediately followed by `y`, both in segment `s` |
| `C:d:s:xy` | `x` ends segment `s`, `y` starts segment `s+1` (only with `cross_edges`) |
| `G1:c` .. `G4:wxyz` | grams anywhere in `F` (up to `rollup`) |
| `H:F:ab..`, `H:R:yz..` | the whole first (last) 2..`anchor_len` characters of `F` (`R`), as one key |
| `Sd:xy` | `x` at some position and `y` exactly `d` positions later, `d` = 2..`skip_depth` |
| `X.ht:a,z` | first character `a` and last character `z` of the same row |
| `X.HT:ab,yz` | first two and last two characters of the same row |
| `X.Ht:ab,z`, `X.hT:a,yz` | first two and last one (first one and last two) characters |
| `X.HL:a:l`, `X.TL:z:l` | first (last) character together with `n / k` |
| `N` | the row contains a non-ASCII byte (exact tier; used by `ILIKE`) |

`H`, `X` and `S` are *row-bound*. At page or unit granularity, an AND of
separate keys can be satisfied by different rows of the same unit; a single
key that encodes the whole conjunction cannot.

`stomata_keys(value)` lists the keys for a value, and
`stomata_pattern_keys(pattern)` lists the keys a pattern needs.

### Pattern compilation

A pattern is split at `%` into pieces (backslash escapes are honoured):
- The first piece is anchored at the start and uses forward positional keys.
- The last piece is anchored at the end and uses reverse keys.
- A pattern without `%` is anchored at both ends and also requires the
  terminator at position `n`.
- `_` is an exact displacement of one character.
- The literal runs at the anchored ends contribute `H` keys.
- Every literal run contributes rollup grams.
- Pairs of literals up to `skip_depth` apart contribute skip-grams, whether
  `_` or literals lie between them.
- Composite keys bind the head, the tail and the length.

For `ILIKE` the pattern is ASCII-folded and each non-ASCII literal becomes
`_`. The candidates are the rows matching all keys, plus the `N` rows that
match the keys that do not depend on character positions or lengths (a
non-ASCII case variant may change byte length). Pending records are tested
with `ILIKE` itself.

A pattern that yields no keys, such as `%` or `_%`, scans every page. A pattern
ending in a lone escape character does the same, so the executor still raises
PostgreSQL's usual error.

Options
-------

| option | type | default | range | effect |
|---|---|---|---|---|
| `k` | int | 3 | 1-8 | segment width |
| `cap` | int | 2 | 1-64 | forward positional depth, in segments |
| `reverse_depth` | int | 1 | 0-64 | reverse positional depth, clamped to `cap` |
| `rollup` | int | 3 | 0-4 | highest gram order stored for infix search |
| `anchor_len` | int | 4 | 0-4 | longest whole prefix/suffix key (`H`) |
| `skip_depth` | int | 4 | 0-8 | largest skip-gram gap; 0 disables skip-grams |
| `exact` | bool | on | | row-exact TID postings for the selective keys; off = page tier only |
| `exact_bigrams` | bool | off | | also put `G2` in the exact tier |
| `composite` | bool | on | | composite head/tail/length keys |
| `cross_edges` | bool | off | | cross-segment bigrams |
| `stop_threshold` | real | 0 | 0-1 | page-tier keys denser than this fraction of pages are not stored and are treated as always true |
| `pending_limit` | int | 4096 | 0-2097151 | kB of pending list that triggers a merge on insert; 0 means VACUUM or explicit merges only |

The parameters are copied into the metapage at build time. A later
`ALTER INDEX ... SET` takes effect at the next `REINDEX`.

Functions
---------

### `stomata_index_info(index regclass) RETURNS record`

Returns these columns:
- `generation` (bumped whenever the run list changes), `heap_pages`, `runs`;
- `keys`, `stopped_keys`, `blob_pages`, `blob_bytes` (summed over all runs);
- `pending_records`, `pending_pages` (including a list being merged), `index_pages`;
- the build parameters (`k`, `cap`, `rollup`, `reverse_depth`, `composite`,
  `cross_edges`, `stop_threshold`, `pending_limit`, `anchor_len`,
  `skip_depth`, `exact`, `exact_bigrams`) and `format_version` (1).

Requires `SELECT` on the table.

### `stomata_estimate(index regclass, pattern text, ilike bool = false) RETURNS record`

What the planner sees for one pattern: `tids` (row-level candidates),
`lossy_pages` (whole heap pages), `exact` (true when the candidates were
counted exactly at plan time, false for an upper bound or an estimate),
`keys`, `values_decoded` and `index_pages` (the work of the scan, which the
index cost charges), and the rows and heap pages the runs cover.
Requires `SELECT` on the table.

### `stomata_pattern_stats(index regclass, pattern text, ilike bool = false) RETURNS SETOF record`

One row per key the pattern needs: `key`, `tier` (`exact` or `page`), the
number of runs holding it, whether it is a stop-key, `entries` (rows for the
exact tier, heap pages for the page tier; lists longer than 8 kB are counted
from their first 8 kB) and payload `bytes`. Requires `SELECT` on the table.

### Setting `stomata.estimate_budget` (integer, default 50000)

Posting values the planner may decode per pattern to count its candidates
exactly. 0 uses key statistics only. Any user can set it.

### `stomata_runs(index regclass) RETURNS SETOF (run, run_id, rows, keys, pages, bytes, heap_pages)`

The sorted runs, oldest (normally largest) first: rows merged into each,
posting lists, index pages and bytes, and the heap pages its bitmaps cover.
Requires `SELECT` on the table.

### `stomata_candidate_pages(index regclass, pattern text, ilike bool = false) RETURNS bigint`

The number of heap pages a scan for `pattern` (as `ILIKE` when `ilike`) would return, including pages
from the pending list. Requires `SELECT` on the table.

### `stomata_key_stats(index regclass) RETURNS SETOF (family, tier, keys, array_lists, bitmap_lists, stop_keys, bytes)`

Dictionary size by key family, summed over all runs (a key present in several
runs counts once per run). Use it to decide which options are worth their
space. Requires `SELECT` on the table.

### `stomata_keys(value text, k int = 3, cap int = 2, rollup int = 3, reverse_depth int = 1, composite bool = true, cross_edges bool = false, anchor_len int = 4, skip_depth int = 4) RETURNS SETOF text`

The keys stored for `value` under the given parameters.

### `stomata_pattern_keys(pattern text, ...same parameters..., ilike bool = false) RETURNS SETOF text`

The keys a `LIKE` (or `ILIKE`) pattern requires. An empty set means a full scan.

### `stomata_merge_pending(index regclass) RETURNS bigint`

Folds the pending list into a new run, merging it with the newest runs that
are at most four times the size of what is being merged, and returns the
number of pending records merged. VACUUM does the same. Waits for a merge
already in progress. Requires ownership of the table.

### `stomata_compact(index regclass) RETURNS bigint`

Folds the pending list and merges every run into one. Needs free space for a
full copy of the index while it runs. Requires ownership of the table.

### `stomata_verify(index regclass) RETURNS bigint`

Scans the table with an MVCC snapshot and counts the visible tuples whose
keys are not all covered by the dictionary (by TID for exact keys, by heap
page for page keys), the pending list, or the long-value (`A`) and heap-only
(`W`) page lists. A result of `0` means the index is sound.
Requires `SELECT` on the table.

Storage and maintenance
-----------------------

| block | content |
|---|---|
| 0 | metapage: parameters, the run list (up to 16 runs), pending-list pointers |
| run: blob chain | `key -> container` entries, sorted by key; a container is a delta varbyte TID list (exact tier), a delta page list or page bitmap (page tier), or a stop marker |
| run: directory chain | the first key starting on each blob page, for lookups |
| pending chain | `(heap block, offset, value)` records appended by `INSERT`/`UPDATE`; offset 0 marks a heap-only tuple |
| frozen pending chain | the pending list a running merge is folding in (still scanned) |

Runs are immutable. All page changes use generic WAL, so the index is
crash-safe; crash recovery, including a crash in the middle of a merge, is
tested.

**Merges.** A merge first *freezes* the pending list (new inserts start a new
one), then streams the frozen records and the chosen runs into a new run with
a k-way merge, and finally switches the run list in the metapage. Only the
freeze and the switch take the metapage lock, briefly; queries and inserts
continue meanwhile. A heavyweight lock on the metapage lets one merge run at a
time. Which runs are merged follows a size-tiered policy: going from the
newest run backwards, a run is included while it is at most 4× the size of
what is already being merged (all runs for `stomata_compact()`; runs over
8 MB, or 4× `pending_limit`, are left to VACUUM when the merge was started by
an insert; the 16-run limit forces a merge). Merge memory is bounded by
`maintenance_work_mem`: pending keys beyond it are spilled as sorted
temporary runs. Builds work the same way.

**Page reuse.** When a merge replaces runs and pending pages, it marks them
deleted with the next full transaction id and puts them in the free space
map. The allocator reuses a deleted page only once that id is older than
every running snapshot (`GlobalVisCheckRemovableFullXid`), because a scan
that copied the metapage earlier may still read it. The deletion mark is a
hint and is not WAL-logged; pages whose mark was lost in a crash, and pages
written by a merge that crashed before its switch, are unreachable orphans
that the VACUUM cleanup pass marks and frees.

**Scans** copy the metapage and release it, then look each key up in every
run through its directory (cached per backend: a few bytes per blob page,
immutable per run) and read the entries they need through shared buffers.
Postings of a key in several runs are united before the usual intersection.
Pending records (frozen and active) are tested with `LIKE`/`ILIKE` itself. A
heap-only tuple (from a HOT update) cannot be reached through its own line
pointer, so its page is recorded under the `W` key and returned whole.

- **Deletes and VACUUM.** VACUUM's bulk-delete pass folds the pending list
  (skipping records of dead rows) and drops dead TIDs from every exact-tier
  list of the runs it merges; it then rewrites each other run in which at
  least a tenth of the exact-tier entries are dead (or any dead entry in a
  run under 1 MB). Page-tier postings keep stale pages, which cost only
  rechecks; `REINDEX` removes them.

**WAL and replication.** All page changes are logged with generic WAL. The
deletion mark of a retired page is an unlogged hint; a page whose mark was
lost in a crash is found unreachable by VACUUM's cleanup pass and reclaimed.
Before a deleted page is reused, a `Btree/REUSE_PAGE` record carrying the
page's deletion transaction id is logged (if `wal_level` is `replica` or
higher). Its redo does nothing but resolve recovery conflicts, exactly as for
a reused btree page: hot-standby queries whose snapshots could still see the
old page are cancelled after `max_standby_streaming_delay`, unless
`hot_standby_feedback` keeps the primary from reusing it.

`stomata.test_scan_delay_ms` (superuser, default 0) pauses every scan after it
reads the run list. It exists only for testing page reuse under concurrent
merges and must not be set in production.
- **HOT.** The access method declares itself summarising, like BRIN, so HOT
  updates remain possible.

Limitations
-----------

- `LIKE` and `ILIKE` only: no regex, `NOT LIKE` or similarity search.
- One key column; no unique, ordered, parallel or index-only scans.
- A merge started by an insert runs in that insert's statement.
- Compactions into the largest run need free space for a copy of it; the file
  keeps that space for reuse.
- On hot standbys, page reuse cancels conflicting queries like btree does,
  unless `hot_standby_feedback = on`.
- VACUUM removes dead rows by rewriting runs, which costs WAL about the size
  of the runs rewritten.
- The on-disk format may change before 1.0; such a release will require
  `REINDEX`.
- `ILIKE` over rows with non-ASCII bytes filters with fewer keys.
- Nondeterministic collations make every page a candidate.

Author
------

Sivaprasad <sivaprasad.off@gmail.com>

Copyright and License
---------------------

Copyright (c) 2026 Sivaprasad. Released under the MIT license.
