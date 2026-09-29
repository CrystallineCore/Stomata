# WAL, recovery and replication

## WAL

Every change to an index page is logged with PostgreSQL's generic WAL
(`GenericXLog`). stomata has no resource manager of its own, so no server
module is needed to replay its WAL.

- An insert logs the change to the pending-list page and the metapage.
- Pages of a new run (from a build or a merge) are logged as full page
  images, so a merge writes WAL roughly the size of the run it writes.
- Retiring chains changes only the metapage. Replaced pages are never
  written, so no hint-bit page images are produced for them when data
  checksums or `wal_log_hints` are enabled.

## Crash recovery

A crash at any point leaves the index consistent after replay:

- the metapage switch is a single WAL record, so after recovery the run list
  is either the old one or the new one;
- a merge interrupted by a crash leaves a partly written chain recorded as
  "being written"; the next merge or VACUUM retires it and its pages are
  reused;
- pages not reachable from the metapage are found by VACUUM's page pass.

An unlogged index has an init fork holding only a metapage and is reset to
it after a crash, as with other access methods.

## Streaming replication and hot standby

The index is replicated through WAL like any other relation, and can be
scanned on a hot standby.

A standby query may be reading pages of a run that the primary has already
replaced. Before reusing such a page, the primary logs a
`Btree/REUSE_PAGE` record carrying the retirement transaction id of the
page's chain (when `wal_level` is `replica` or higher). On the standby this
record only resolves recovery conflicts, exactly as for a reused btree page:

- with `hot_standby_feedback = off`, standby queries whose snapshots could
  still read the page are cancelled after `max_standby_streaming_delay`;
- with `hot_standby_feedback = on`, the primary does not reuse the page
  while such queries run.

The record appears in `pg_waldump` output as a btree record even though it
refers to a stomata index.

## Point-in-time recovery

Base backups and WAL archives include stomata indexes like any other index.
Recovery to a target time or restore point replays generic WAL records.

## How this is tested

`tools/replication.sh` sets up a primary with WAL archiving and
`wal_consistency_checking = 'generic'`, a streaming standby with
`hot_standby_feedback` off, and a base backup. While the primary runs
inserts, updates, deletes, merges, compactions and VACUUM, clients on the
standby repeatedly query rows that the workload does not modify; a wrong
answer fails the run, and queries cancelled by recovery conflicts are
counted. The run is repeated with `hot_standby_feedback` on, where no errors
are allowed. It then checks that no inconsistent page was reported and
performs a point-in-time recovery to a named restore point.

`tools/stress.sh` stops a scratch server with `pg_ctl -m immediate` during
concurrent activity and checks the index after recovery.
