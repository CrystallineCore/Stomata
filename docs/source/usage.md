# Usage

## Creating an index

```sql
CREATE INDEX name_st ON people USING stomata (name);
```

The default operator class `stomata_text_ops` is used automatically for
`text`; `varchar` columns use it through the implicit cast to `text`.

Expression and partial indexes work as with other access methods:

```sql
CREATE INDEX email_lower_st ON people USING stomata (lower(email));
CREATE INDEX active_name_st ON people USING stomata (name) WHERE active;
```

`CREATE INDEX CONCURRENTLY`, `REINDEX` and `REINDEX CONCURRENTLY` are
supported. An index can be created on an unlogged table.

The index has one key column. `NULL` values are not indexed.

## Queries that can use the index

| condition | index used |
|---|---|
| `col LIKE 'pattern'`, `col ~~ 'pattern'` | yes |
| `col ILIKE 'pattern'`, `col ~~* 'pattern'` | yes |
| `col LIKE 'pattern' ESCAPE '!'` | yes |
| `col LIKE ANY (ARRAY[...])` | yes, one index scan per element |
| several `LIKE` conditions combined with `AND` / `OR` | yes (one scan, or a `BitmapOr`) |
| `col LIKE $1` in a prepared statement | yes |
| `col NOT LIKE ...`, `col LIKE ALL (...)` | no |
| regular expressions, similarity operators | no |

Scans are bitmap scans: the plan shows a `Bitmap Index Scan` under a
`Bitmap Heap Scan` with a `Recheck Cond`. The index does not return rows in
any order and does not support index-only scans.

Whether the planner uses the index is a cost decision; see
[Planner](planner.md). For patterns that match most of the table, such as
`'%a%'` or `'%'`, a sequential scan is normally chosen.

## What each kind of pattern needs

Every pattern is turned into a set of keys that any matching value must
contain (see [Keys and patterns](patterns.md)). `stomata_pattern_keys()` shows
them:

```sql
SELECT * FROM stomata_pattern_keys('jo%son');
```

In general:

- literals of three or more characters, anchored prefixes and suffixes, and
  combinations of several literal pieces produce row-level keys;
- a single character, two-character infixes, and characters separated only
  by `_` produce page-level keys, so whole pages are returned and rechecked;
- a pattern without literal characters produces no keys (`'%'`, `'_%'`) or
  only length information (`'____'`), and reads every page or nearly every
  page.

`stomata_estimate()` shows what the planner expects a pattern to return:

```sql
SELECT tids, lossy_pages, exact FROM stomata_estimate('name_st', 'jo%son');
```

## ILIKE and collations

`ILIKE` folds ASCII letters only. Rows containing non-ASCII bytes are marked
in the index. For an `ILIKE` condition, every marked row is a candidate
(filtered only by `LIKE` conditions in the same scan), because its case
variants may not match the ASCII-folded keys. No matching row is missed; the
cost is extra rechecks on tables with many non-ASCII values. Non-ASCII
characters in an `ILIKE` pattern are treated as `_` when choosing keys.

Patterns under a nondeterministic collation cannot be reasoned about byte by
byte; such a condition does not restrict the scan, and every page is a
candidate.

## Options

Options are set with `WITH (...)` and stored in the index when it is built:

```sql
CREATE INDEX code_st ON items USING stomata (code) WITH (cap = 4, skip_depth = 6);
```

| option | default | effect |
|---|---|---|
| `k` | 3 | segment width in characters (1-8) |
| `cap` | 2 | segments from the start that get positional keys (1-64) |
| `reverse_depth` | 1 | segments from the end that get positional keys (0-64, at most `cap`) |
| `rollup` | 3 | longest n-gram stored for infix search (0-4) |
| `anchor_len` | 4 | longest whole prefix/suffix stored as one key (0, or 2-4) |
| `skip_depth` | 4 | largest gap of skip-gram keys (0-8; 0 disables them) |
| `exact` | on | keep selective keys at row level; off stores pages only |
| `exact_bigrams` | off | keep two-character grams at row level too |
| `composite` | on | store keys combining first/last characters and length |
| `cross_edges` | off | store character pairs that straddle segment boundaries |
| `stop_threshold` | 0 | do not store page-level keys present on more than this fraction of pages (0 disables) |
| `pending_limit` | 4096 | pending list size in kB that triggers a merge during an insert (0: only VACUUM and explicit merges) |

Changing an option with `ALTER INDEX ... SET (...)` takes effect only after
`REINDEX`, because the index keeps using the parameters it was built with.

Larger `cap`, `reverse_depth`, `rollup` and `skip_depth` values make more
patterns row-level or more selective and make the index larger.
`stomata_key_stats()` shows how much space each key family takes.

## Diagnostic queries

```sql
-- structure and parameters
SELECT * FROM stomata_index_info('name_st');
SELECT * FROM stomata_runs('name_st');

-- how many heap pages a pattern would read
SELECT stomata_candidate_pages('name_st', '%son');

-- index content by key family
SELECT * FROM stomata_key_stats('name_st') ORDER BY bytes DESC;

-- consistency check (0 means every visible row is covered)
SELECT stomata_verify('name_st');
```

All functions are described in the [SQL reference](api.md).
