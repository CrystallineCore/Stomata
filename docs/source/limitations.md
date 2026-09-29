# Limitations

## Functionality

- Only `LIKE` and `ILIKE` (`~~`, `~~*`). Not supported: `NOT LIKE`,
  `LIKE ALL`, regular expressions, `SIMILAR TO`, similarity operators,
  nearest-neighbour ordering.
- One key column; no unique, ordered, index-only or parallel scans; no
  parallel build.
- Bitmap scans only. Every candidate is rechecked by the executor.
- Folding for `ILIKE` covers ASCII letters only. Rows with non-ASCII bytes
  are candidates for every `ILIKE` condition, filtered only by `LIKE`
  conditions in the same scan.
- Under a nondeterministic collation, a condition does not restrict the scan.
- Values longer than 2000 bytes that are inserted after the index is built
  are not indexed by content; their heap pages are candidates for every scan
  until the next `REINDEX`. Values present at build time are indexed
  regardless of length.
- Patterns without literal characters (`'%'`, `'_%'`) read every page.

## Operation

- Concurrent inserts into one index serialize on the metapage for the
  duration of each append.
- A merge started by an insert runs inside that insert's statement.
- Scans read the whole pending list and test each record with `LIKE`, so a
  large pending list slows every scan.
- VACUUM removes dead rows by rewriting runs, which writes WAL about the size
  of the runs rewritten.
- Page-tier postings keep pages whose matching rows were deleted or updated;
  they cost rechecks until `REINDEX`.
- The index file does not shrink without `REINDEX`. `stomata_compact()`
  needs free space for a copy of the index.
- On a hot standby, page reuse on the primary cancels conflicting queries,
  as for btree, unless `hot_standby_feedback` is on.
- Planning a pattern for the first time in a backend reads index pages.
- Creating the extension requires superuser privileges; it is not available
  on services that allow only an approved list of extensions unless it is on
  that list.

## Maturity

- 0.1.0 is the first release. It has not had production use.
- The on-disk format may change before 1.0. A release that changes it will
  require `REINDEX`; `stomata_index_info()` reports the format version.
- Performance depends on the data, the patterns and the options. Measure on
  your own data before relying on it; `tools/compare_trgm.sh` and
  `tools/crud_wal.sh` are provided for that.
