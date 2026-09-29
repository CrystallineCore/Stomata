# SQL reference

## Access method and operator class

| object | description |
|---|---|
| access method `stomata` | index access method, type `INDEX` |
| operator class `stomata_text_ops` | default for `text`; strategy 1 `~~ (text, text)`, strategy 2 `~~* (text, text)` |

Access method properties: one key column, no unique indexes, no ordered
scans, no index-only scans, no parallel scans, no parallel build, bitmap
scans only, summarizing (HOT updates remain possible).

## Index options

Set with `CREATE INDEX ... WITH (...)`. They are copied into the metapage at
build time; `ALTER INDEX ... SET` takes effect at the next `REINDEX`.

| option | type | default | range | description |
|---|---|---|---|---|
| `k` | int | 3 | 1-8 | segment width in characters |
| `cap` | int | 2 | 1-64 | segments from the start with positional keys |
| `reverse_depth` | int | 1 | 0-64 | segments from the end with positional keys; limited to `cap` |
| `rollup` | int | 3 | 0-4 | longest n-gram stored |
| `anchor_len` | int | 4 | 0-4 | longest whole prefix/suffix key (`H`); values below 2 disable `H` keys |
| `skip_depth` | int | 4 | 0-8 | largest skip-gram distance; 0 disables skip-grams |
| `exact` | bool | on | | keep selective keys in the row tier; off keeps only page-tier postings |
| `exact_bigrams` | bool | off | | put `G2` keys in the row tier |
| `composite` | bool | on | | store `X` keys |
| `cross_edges` | bool | off | | store `C` keys |
| `stop_threshold` | real | 0 | 0-1 | page-tier keys present on more than this fraction of heap pages are stored as stop keys; 0 disables |
| `pending_limit` | int | 4096 | 0-2097151 | pending-list size in kB that makes an insert start a merge; 0 disables |

## Settings

| setting | type | default | who can set | description |
|---|---|---|---|---|
| `stomata.estimate_budget` | int | 50000 | any user | posting values the planner may decode per pattern; 0 uses key statistics only |
| `stomata.test_scan_delay_ms` | int (ms) | 0 | superuser | pauses every scan after it reads the run list; for testing only |

## Functions

Functions that take an index raise an error if it is not a stomata index.
Unless stated otherwise, they require `SELECT` privilege on the table.

### stomata_index_info

```
stomata_index_info(index regclass) RETURNS record
```

| column | description |
|---|---|
| `generation` | incremented whenever the run list changes |
| `heap_pages` | heap pages covered by the runs |
| `runs` | number of runs |
| `keys`, `stopped_keys` | posting lists and stop keys, summed over runs |
| `blob_pages`, `blob_bytes` | size of the runs' key data |
| `pending_records`, `pending_pages` | pending list, including one being merged |
| `index_pages` | pages in the index file |
| `k` ... `exact_bigrams` | build parameters |
| `format_version` | on-disk format (1) |

### stomata_runs

```
stomata_runs(index regclass)
  RETURNS SETOF (run int, run_id bigint, rows bigint, keys bigint,
                 pages bigint, bytes bigint, heap_pages bigint)
```

One row per run, oldest first: rows merged into it, posting lists, index
pages, bytes of key data, and heap pages covered.

### stomata_key_stats

```
stomata_key_stats(index regclass)
  RETURNS SETOF (family text, tier text, keys bigint, array_lists bigint,
                 bitmap_lists bigint, stop_keys bigint, bytes bigint)
```

Index content by key family and tier, summed over runs. A key present in
several runs is counted once per run.

### stomata_candidate_pages

```
stomata_candidate_pages(index regclass, pattern text, ilike bool DEFAULT false)
  RETURNS bigint
```

Number of heap pages a scan for `pattern` would return, including pages
from the pending list. With `ilike`, the pattern is treated as `ILIKE`.

### stomata_estimate

```
stomata_estimate(index regclass, pattern text, ilike bool DEFAULT false)
  RETURNS record
```

| column | description |
|---|---|
| `tids` | row-level candidates |
| `lossy_pages` | whole heap pages returned |
| `exact` | true if the candidates were counted exactly |
| `keys` | keys looked up |
| `values_decoded`, `index_pages` | work of the scan, as charged by the cost model |
| `rows`, `heap_pages` | rows and heap pages covered by the runs |

### stomata_pattern_stats

```
stomata_pattern_stats(index regclass, pattern text, ilike bool DEFAULT false)
  RETURNS SETOF (key text, tier text, runs int, stopped bool,
                 entries bigint, bytes bigint)
```

One row per key the pattern needs: its tier (`exact` or `page`), the number
of runs holding it, whether it is a stop key, its entries (rows in the row
tier, heap pages in the page tier) and bytes. Lists longer than 8 kB are
counted from their first 8 kB.

### stomata_keys

```
stomata_keys(value text, k int DEFAULT 3, cap int DEFAULT 2, rollup int DEFAULT 3,
             reverse_depth int DEFAULT 1, composite bool DEFAULT true,
             cross_edges bool DEFAULT false, anchor_len int DEFAULT 4,
             skip_depth int DEFAULT 4)
  RETURNS SETOF text
```

Keys stored for `value` with the given parameters. Does not read an index.

### stomata_pattern_keys

```
stomata_pattern_keys(pattern text, k int DEFAULT 3, cap int DEFAULT 2,
                     rollup int DEFAULT 3, reverse_depth int DEFAULT 1,
                     composite bool DEFAULT true, cross_edges bool DEFAULT false,
                     anchor_len int DEFAULT 4, skip_depth int DEFAULT 4,
                     ilike bool DEFAULT false)
  RETURNS SETOF text
```

Keys a pattern requires with the given parameters. An empty result means the
pattern reads every page. Does not read an index.

### stomata_merge_pending

```
stomata_merge_pending(index regclass) RETURNS bigint
```

Folds the pending list into a new run and merges runs by the size rule.
Waits for a merge in progress. Returns the number of pending records merged.
Requires ownership of the table.

### stomata_compact

```
stomata_compact(index regclass) RETURNS bigint
```

Folds the pending list and merges all runs into one. Needs free space for a
copy of the index. Returns the number of pending records merged. Requires
ownership of the table.

### stomata_verify

```
stomata_verify(index regclass) RETURNS bigint
```

Scans the table with an MVCC snapshot and returns the number of visible rows
whose keys are not all covered by the index (runs, pending list, or the `A`
and `W` page lists). `0` means every visible row is covered.
