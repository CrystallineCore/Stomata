STOMATA
=======

**STO**red auto**MATA**: a segmented, bidirectional automaton index for
`LIKE` and `ILIKE` in PostgreSQL.

```sql
CREATE EXTENSION stomata;
CREATE INDEX ON users USING stomata (email);

SELECT * FROM users WHERE email LIKE 'jo%@example.com';   -- prefix + suffix
SELECT * FROM users WHERE email LIKE '%.io';              -- suffix
SELECT * FROM users WHERE email LIKE 'a___@%';            -- underscores
SELECT * FROM users WHERE email LIKE '%ab%';              -- 2-character infix
SELECT * FROM users WHERE email ILIKE 'JO%';              -- case-insensitive
```

STOMATA targets the `LIKE` patterns that trigram indexes handle poorly:
- anchored prefixes and suffixes;
- very short literals;
- `_` wildcards;
- `ESCAPE`d literal `_` and `%`.

It also matches or beats a trigram GIN index on most trigram-friendly
patterns (see [Benchmarks](#benchmarks)), at a similar size.

> **Status: 0.1.0, first release, testing.** The on-disk format may change
> before 1.0; a release that changes it will say so and require `REINDEX`.
> Please read [Limitations](#limitations) before production use.

How it works
------------

Every value is case-folded (ASCII, length preserving) and read twice, once
forwards and once backwards, each followed by a terminator `\0`. Positions are
grouped into segments of `k` characters. From these strings the index derives
*keys*:

| key | meaning |
|---|---|
| `P` | character at an exact position near either end |
| `U` | character somewhere in a segment (the `\0` segment encodes length) |
| `T` | two adjacent characters inside a segment |
| `H` | the whole first / last 2-4 characters of a row, as one key |
| `G1..G4` | 1- to 4-grams anywhere (for infixes) |
| `S2..S8` | skip-grams: `x` followed `d` characters later by `y` (for `_`) |
| `X` | composite: first/last characters and length bucket of *one row* |

Postings are stored in **two tiers**:

- **Exact tier** — the selective, row-identifying keys (`G3`, `G4`, `H`, the
  first/last character `P` keys, and `G2` with `exact_bigrams`) keep a sorted,
  delta-coded list of **row TIDs**. Intersecting them is row-exact, like a
  trigram GIN index.
- **Page tier** — all other keys keep the set of **heap pages** containing a
  row with the key, as a delta array or a bitmap, whichever is smaller. These
  are small and fast to AND together.

A pattern is compiled into the conjunction of keys that every matching string
must have. The scan reads all their postings, intersects the exact lists
smallest-first (stopping early when the result is empty), and filters the
result through the page-tier bitmaps. Surviving rows are returned as TIDs with
recheck; patterns with no exact key return lossy pages. Either way the
executor rechecks, and the soundness invariant is:

    rows(Matches(P)) ⊆ Candidates(P)

The row-bound keys (`H`, `X`) stop a page from pairing one row's prefix with
another row's suffix, and the skip-grams give `_` patterns something to filter
on.

**Storage is log-structured.** The postings live in immutable, key-sorted
*runs*. Inserts append to a pending list. A merge turns the pending list into
a new small run, and merges the newest runs with each other while each is no
more than 4× the size of what is being merged (size-tiered compaction), so
the big base run is rewritten only rarely. Scans look a key up in every run
(usually one to a few) and test pending rows exactly. Merges stream sorted
runs, so their memory use stays within `maintenance_work_mem`; builds spill
sorted runs to temporary files the same way.

**ILIKE** uses the same keys: values are already ASCII-folded, the pattern is
folded too, and non-ASCII pattern characters become `_` (their case variants
may differ in byte length only among non-ASCII rows). Rows containing any
non-ASCII byte carry an `N` marker and are added back as candidates using only
the keys that are fold-safe, so non-ASCII `ILIKE` stays correct.

The design, the proofs and the evaluation are in the STOMATA design document
(revision 2), which is shipped separately.

Installation
------------

Requires PostgreSQL 16 or later. Tested on 16, 17 and 18.

```sh
make
make install          # may need sudo
make installcheck     # optional: run the regression suite against a running server
```

Or with the PGXN client:

```sh
pgxn install stomata
```

Then, in each database:

```sql
CREATE EXTENSION stomata;
```

Usage
-----

```sql
CREATE INDEX name ON table USING stomata (column [, ...]) [WITH (option = value, ...)];
```

The index supports one `text` or `varchar` column or expression per index. It
is used for `LIKE` (`~~`) and `ILIKE` (`~~*`). Several `LIKE` conditions on the same column
are combined inside one scan. Expression indexes (for example
`lower(email)`) and partial indexes work as usual.

| option | default | meaning |
|---|---|---|
| `k` | 3 | segment width in characters (1-8) |
| `cap` | 2 | positional keys for the first `cap` segments from the start |
| `reverse_depth` | 1 | positional keys for the first segments from the end (clamped to `cap`) |
| `rollup` | 3 | order of global gram keys for infixes (0-4) |
| `anchor_len` | 4 | whole prefixes/suffixes stored as single keys, lengths 2..`anchor_len` (0 = off) |
| `skip_depth` | 4 | skip-grams for gaps 2..`skip_depth` (0 = off, max 8) |
| `exact` | on | store the selective keys as row-exact TID lists (off = pages only, about half the size) |
| `exact_bigrams` | off | also keep `G2` bigrams row-exact (sharper 2-char infixes, larger index) |
| `composite` | on | record-level composite keys |
| `cross_edges` | off | bigrams that straddle a segment boundary |
| `stop_threshold` | 0 (off) | do not store page-tier keys found on more than this fraction of pages |
| `pending_limit` | 4096 | pending-list size in kB that triggers a merge on insert (0 = VACUUM only) |

The main trade-off is `exact`:
- **`exact = on` (default):** row-exact infixes and anchors; about the size of
  a trigram index.
- **`exact = off`:** page-level only, roughly half the size, with many more
  rows to recheck for common infixes.

`rollup = 4` sharpens 4-character infixes such as `%tata%`. `skip_depth = 6`
covers `%t____t%`. Raising `cap` and `reverse_depth` sharpens long anchored
patterns, and stop-keys shrink the index further. Parameters are fixed when the index is built, so
change them with `ALTER INDEX ... SET (...)` followed by `REINDEX`.

### Functions

| function | purpose |
|---|---|
| `stomata_index_info(index)` | runs, keys, sizes, pending list, parameters |
| `stomata_runs(index)` | the runs, oldest first: rows, keys, pages, bytes |
| `stomata_key_stats(index)` | dictionary size by key family and tier (for tuning) |
| `stomata_candidate_pages(index, pattern [, ilike])` | how many heap pages a scan would return |
| `stomata_keys(value [, k, cap, rollup, reverse_depth, composite, cross_edges])` | keys stored for a value |
| `stomata_pattern_keys(pattern [, ..., ilike])` | keys a pattern requires (empty set = full scan) |
| `stomata_merge_pending(index)` | fold the pending list into a new run now (and compact similar-sized runs) |
| `stomata_compact(index)` | merge everything into a single run |
| `stomata_verify(index)` | number of visible tuples the index fails to cover; 0 means sound |
| `stomata_estimate(index, pattern [, ilike])` | the planner's view: candidate rows and lossy pages, and the scan's work |
| `stomata_pattern_stats(index, pattern [, ilike])` | each key a pattern needs, with its rows (exact tier) or heap pages (page tier) |

### Planner

The cost estimate comes from the index itself. At plan time every constant
pattern is compiled, each key is looked up (a directory probe; long lists are
counted from their first 8 kB), the page-tier bitmaps are ANDed, and the
row-level lists are intersected rarest first as long as the values decoded
stay within `stomata.estimate_budget` (default 50,000, about 0.2 ms). The
planner therefore knows the candidate count of a selective pattern exactly;
otherwise it gets an upper bound, or, when every key is common, an estimate
from the rarest key of each literal piece of the pattern. The candidates
become the index selectivity; the index cost counts the key probes, the
values the scan decodes and the TIDs it emits, in PostgreSQL's own cost units.
Results are cached per backend until the run list changes.

```sql
SELECT * FROM stomata_estimate('users_name_idx', '%son');   -- what the planner sees
SET stomata.estimate_budget = 0;                            -- statistics only
```

Patterns that are parameters (generic plans) fall back to the column
statistics.

Maintenance
-----------

- **Inserts** append `(heap TID, value)` records to a pending list, which is
  WAL-logged. Scans test pending records exactly, with the `LIKE` operator
  itself.
- **Merges** happen when the pending list reaches `pending_limit` (in the
  inserting backend), on VACUUM, or through `stomata_merge_pending()`. They
  write a new run while queries and inserts carry on; only the final switch
  of the run list takes the metapage lock, for microseconds. A merge started
  by an insert skips runs larger than 8 MB (or 4× `pending_limit`), leaving
  big compactions to VACUUM. Only one merge runs at a time per index.
- **Page reuse.** Pages of replaced runs are reused only once no running
  snapshot could still be reading them. VACUUM returns such pages to the free
  space map and reclaims pages left behind by a crash during a merge.
- **Deletes and updates.** VACUUM removes dead rows from the row-level (exact)
  tier: while it merges the pending list it drops dead rows from everything it
  rewrites anyway, and it rewrites any other run in which at least a tenth of
  the entries are dead (any run under 1 MB). Page-level postings cannot tell
  rows apart and keep stale pages; those only cost rechecks.
- **HOT updates** stay possible, because the access method is summarising
  (the same model as BRIN). A heap-only tuple cannot be reached through its
  own line pointer, so it is indexed for its whole page (a `W` page marker).

WAL, crash recovery and replication
-----------------------------------

Every page change is WAL-logged with generic WAL, so the index survives
crashes, streams to physical replicas and is restored by point-in-time
recovery like any built-in index. The one exception is deliberate: the mark
on a page that a merge has retired is a hint, like a heap hint bit, and is not
logged. If a crash loses it, the page is simply unreachable, and VACUUM
reclaims it.

A retired page is reused only when no snapshot on the primary can still read
it. Before overwriting it, stomata writes the same recovery-conflict record
that btree writes when it reuses a page (`Btree/REUSE_PAGE` in `pg_waldump`).
On a hot standby that record cancels queries that might still read the old
page, after `max_standby_streaming_delay`, exactly as for btree; with
`hot_standby_feedback = on` there is nothing to cancel.
`tools/replication.sh` checks all of this:

- **Streaming replica.** The primary runs inserts, updates, deletes, merges,
  compactions and VACUUM with `wal_consistency_checking = generic`, which
  makes the standby compare every replayed page with the primary's image.
  Standby clients meanwhile check rows nobody changes: no wrong answers, and
  the replayed index agrees with the heap.
- **Page reuse on the standby.** Standby scans are paused on purpose (a
  testing-only setting, `stomata.test_scan_delay_ms`) while the primary
  recycles pages. With the conflict record they are cancelled. Without it (a
  negative control) they return wrong answers and read corrupt pages. With
  `hot_standby_feedback` they succeed.
- **Promotion.** The promoted standby accepts writes, merges, compactions and
  VACUUM, and stays consistent.
- **Point-in-time recovery.** A base backup plus the WAL archive, recovered to
  a named restore point in the middle of the workload, gives an index that
  agrees with the heap, and keeps working after new writes.

Benchmarks
----------

These numbers are indicative only. They come from a 46-query benchmark
(sequential scan vs pg_trgm GIN vs stomata) on a 1,000,000-row table of
Faker-style usernames (16,792 heap pages), warm cache, PostgreSQL 16, with
JIT and parallel query off. Each method is forced in turn with the other
indexes dropped inside a rolled-back transaction, the plan is checked, and the
figure is the median of five runs (`tools/cost_oracle.py`). Every method
returns the same counts.

| index | size | build |
|---|---|---|
| pg_trgm (GIN) | 35 MB | 2.9 s |
| stomata | 44 MB | 10.8 s (within `maintenance_work_mem`) |

Speed-up of stomata over pg_trgm (geometric mean over queries that return
rows; > 1 means stomata is faster):

| class | queries | vs pg_trgm | vs the faster of pg_trgm and a sequential scan |
|---|---|---|---|
| trigram-friendly | 26 | 1.25× | 1.25× |
| trigram-degenerate (1-2 characters, `_`, escapes) | 10 | 4.4× | 1.1× |

On trigram-degenerate patterns pg_trgm scans its whole index, which is where
most of its losses come from; against the plan the planner would actually
pick, stomata is clearly ahead only when few rows match. With
`tools/compare_trgm.sh` (randomised order, paired rounds, sign test at
p < 0.05; measured during development with the same scan code) trigram-friendly patterns came out 1.31× faster, 11 queries
faster, none slower, 18 without a clear difference.

Selected queries (ms):

| query | matches | sequential scan | pg_trgm | stomata |
|---|---|---|---|---|
| `LIKE 'allison%'` | 1,044 | 100.0 | 2.9 | 2.0 |
| `LIKE '%kim%'` | 8,105 | 98.8 | 5.0 | 5.3 |
| `LIKE '%son%'` | 79,485 | 106.7 | 36.1 | 33.6 |
| `LIKE '%anderson%'` | 5,143 | 130.2 | 7.6 | 5.5 |
| `LIKE '%kim%son%'` | 217 | 95.7 | 2.0 | 0.9 |
| `LIKE 'a%son'` | 3,199 | 96.8 | 8.0 | 4.8 |
| `LIKE '%ez'` | 32,444 | 129.5 | 22.0 | 32.0 |
| `LIKE 'a%'` | 64,192 | 94.8 | 36.4 | 34.5 |
| `LIKE '%n'` | 152,126 | 113.6 | 447.1 | 59.1 |
| `LIKE '%mc%'` | 13,070 | 107.8 | 459.1 | 69.6 |
| `LIKE '%k_m%'` | 9,266 | 105.9 | 456.1 | 57.2 |
| `ILIKE 'ALLISON%'` | 1,044 | 186.2 | 3.3 | 2.1 |
| `LIKE '%!_%' ESCAPE '!'` | 0 | 97.5 | 443.1 | 0.03 |

**Concurrent writes** (measured during development). Inserts append to a pending list and merges build new
runs beside the old ones, so queries never wait for a merge. With 6 clients
running 5 inserts per `LIKE '%kim%'` query and `pending_limit = 256`, query
latency stayed under 46 ms at p99.9 and 80 ms at worst, while merges ran every
few seconds; folding 1,000 new rows into the index took 20 ms.

**CRUD operations** (`tools/crud_wal.sh`: a 1,000,000-row copy of the
`username` column; each operation after a `CHECKPOINT`; medians of two
runs; "index WAL" is WAL beyond the same run without any index):

| operation | pg_trgm GIN: time / index WAL | stomata: time / index WAL |
|---|---|---|
| create index | 2.8 s / 18 MB | 9.8 s / 37 MB |
| insert 100,000 rows (one statement) | 0.94 s / 70 MB | 0.67 s / 8.9 MB |
| insert 5,000 rows (one per transaction) | 1.27 s / 17 MB | 1.19 s / 0.45 MB |
| update 100,000 rows | 1.34 s / 77 MB | 1.00 s / 8.9 MB |
| update 5,000 rows (one per transaction) | 1.31 s / 3.0 MB | 0.84 s / 0.49 MB |
| delete 100,000 rows / 5,000 single rows | 0.22 s / 0.94 s, no index WAL | 0.24 s / 1.02 s, no index WAL |
| VACUUM (pending lists, dead rows) | 1.7 s / 42 MB | 5.3 s / 38 MB |
| all of the above except the build | 7.7 s / 210 MB | 10.2 s / 57 MB |
| 20 `LIKE` queries afterwards (planner's choice) | 473 ms | 492 ms |

pg_trgm with `fastupdate = off` wrote 965 MB of index WAL for the same work.
stomata writes little WAL per row because inserts only append to its pending
list, and because a `LIKE` index that is summarising leaves updates HOT; the
work is paid later, once, when VACUUM or a merge writes a run. That makes its
VACUUM the one slower operation (key extraction for the pending rows plus one
rewrite of the runs that hold dead rows). Single-transaction timings vary by
±30% between runs on this machine (commit fsyncs); in-transaction, the index
adds about 6 µs per inserted row for stomata and 5 µs for pg_trgm.

- **Where each index wins.** STOMATA leads on anchored, short, `_` and
  escaped patterns and is at parity or ahead on most 3+-character infixes.
  pg_trgm is still ahead on very unselective anchors (1-character prefixes,
  3-character suffixes matching 5-6% of rows) and `ILIKE` 3-character infixes.
- **Planner choice.** The planner picks the index without hints and falls
  back to a sequential scan for unselective patterns such as `%t%`. Measured
  with `tools/cost_oracle.py` (77 queries on two tables, `random_page_cost`
  4 and 1.1), the planner's choice between stomata and a sequential scan was
  within 1.2x of the faster one for 152 of 154 query plans, and lost 75 ms
  against 6.1 s of optimal run time (a row-count-based model: 6 mistakes,
  444 ms). With a
  trigram GIN index on the same column as well, the planner often prefers
  GIN for selective patterns even where stomata is faster, because GIN's
  estimate is lower; do not keep both.
- **Cold start.** Only a small page directory is cached per backend; entries
  are read through shared buffers.

Limitations
-----------

- **Operators:** `LIKE` and `ILIKE`. `NOT LIKE`, `LIKE ALL`, regular
  expressions and similarity operators are not indexed. Case folding is
  ASCII-only: `ILIKE` on rows with non-ASCII bytes uses fewer keys (precision
  only, never correctness).
- **Merge work lands on an insert.** When the pending list fills up, the
  inserting statement does the flush (tens to hundreds of ms). Set
  `pending_limit = 0` to leave all merging to (auto)VACUUM.
- **Full compaction needs room.** `stomata_compact()`, and VACUUM when it
  merges into the base run, write the new run before the old one is freed,
  so the file peaks at about twice the live index. The space is reused, not
  returned to the operating system; `REINDEX` shrinks the file.
- **Hot standby:** like btree, page reuse on the primary cancels standby
  queries that might still read the old pages (after
  `max_standby_streaming_delay`) unless `hot_standby_feedback` is on.
- **VACUUM rewrites runs.** Removing dead rows or merging into the base run
  writes a new run, with WAL about the size of that run. VACUUM therefore
  costs more WAL than pg_trgm's VACUUM, while ordinary writes cost far less
  (see the CRUD benchmark).
- **Dead rows in page-level postings** stay until `REINDEX` (they cost only
  rechecks).
- **Build time:** about 3–4× a trigram GIN build, but memory is bounded by
  `maintenance_work_mem`.
- **Pending list cost:** a large pending list is scanned by every query until
  it is merged.
- **Heap order:** precision depends on it. Clustered or append-ordered tables
  filter best.
- **Unsupported index shapes:** single-column only; no parallel build, no
  index-only scans, no ordering.
- **Collations:** patterns under nondeterministic collations fall back to
  scanning every page (still correct).

Testing
-------

`make installcheck` runs nine suites. They compare index results with
sequential-scan results for more than 1,000 generated patterns across 20
parameter settings. They also cover:
- inserts, updates, deletes, VACUUM and TRUNCATE;
- `ILIKE` against a case-insensitive truth table, including non-ASCII rows;
- several runs, compaction, a compacted index against a fresh build, and
  builds and merges that spill to temporary files under a 1 MB
  `maintenance_work_mem`;
- HOT chains under each tier setting;
- unlogged tables and prepared statements;
- the planner's candidate counts and plan choices, and error cases.
`tools/compare_trgm.sh` benchmarks stomata against pg_trgm on your own table
(`--help` for options). Every sample is a stored `EXPLAIN ANALYZE` plan that
must show the index under test, with the other indexes dropped inside a
rolled-back transaction; order is randomised per round and verdicts use a
paired sign test. Run it on a test copy.

`tools/cost_oracle.py` measures the planner's choices: for each query it
forces a sequential scan, the GIN index and stomata in turn (the other
indexes dropped in a rolled-back transaction), records estimated cost and
run time, and `tools/cost_report.py` reports how often the cheapest estimate
was also the fastest plan.

`tools/crosscheck.py` compares the C key extractor with the Python reference
model, and `tools/stress.sh` runs crash-recovery (including a crash in the
middle of a compaction) and concurrency checks against a scratch cluster:
inserts, updates, deletes, merges, compactions and VACUUM run together while
two clients continuously check that rows nobody touches are always found.
`tools/replication.sh` runs the streaming-replication, hot-standby and
point-in-time-recovery checks described above, and `tools/crud_wal.sh`
measures WAL and time of CRUD operations against pg_trgm. The suite, the
stress test and the replication test pass on PostgreSQL 16, 17 and 18,
including an assertion-enabled PostgreSQL 18 build.

License
-------

MIT; see [LICENSE](LICENSE).
