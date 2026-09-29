# Changelog

## 0.1.0 (2026-09-29)

First release.

- Index access method for `LIKE` and `ILIKE` on `text` (and `varchar`),
  for PostgreSQL 16, 17 and 18.
- Keys for anchored prefixes and suffixes, `_` wildcards, short literals and
  infixes.
- Two tiers of postings: row identifiers for selective keys, heap pages
  (lists or bitmaps) for the rest.
- Log-structured storage: a pending list for inserts and up to 16 immutable
  sorted runs, merged by size. Builds and merges use bounded memory
  (`maintenance_work_mem`) and spill to temporary files.
- Summarizing access method: HOT updates remain possible.
- VACUUM removes dead rows from the row tier and makes replaced pages
  reusable once no snapshot can read them. Replaced pages are not written.
- Generic WAL for all changes: crash recovery, streaming replication, hot
  standby (with recovery conflicts on page reuse), point-in-time recovery.
  Unlogged indexes are supported.
- Cost estimation from the index's own contents, with
  `stomata.estimate_budget`.
- Functions: `stomata_index_info`, `stomata_runs`, `stomata_key_stats`,
  `stomata_candidate_pages`, `stomata_keys`, `stomata_pattern_keys`,
  `stomata_pattern_stats`, `stomata_estimate`, `stomata_merge_pending`,
  `stomata_compact`, `stomata_verify`.
- On-disk format version 1.
