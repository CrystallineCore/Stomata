# Maintenance

## Inserts and the pending list

`INSERT` and `UPDATE` append a record to the pending list. When the pending
list exceeds `pending_limit` (kB, default 4096), the inserting statement
starts a merge itself. That merge:

- is skipped if another merge is already running;
- does not include runs larger than max(8 MB, 4 × `pending_limit`).

With `pending_limit = 0`, inserts never merge; the pending list is folded
only by VACUUM (including autovacuum) or by an explicit merge.

Scans read the whole pending list and test each record with `LIKE`/`ILIKE`,
so a long pending list makes every scan slower.

## VACUUM

VACUUM on a table with a stomata index:

1. folds the pending list into a new run, skipping records of dead rows;
2. finds the oldest run in which at least a tenth of the row-tier entries
   are dead (estimated from a sample), or which contains any dead entry and
   is at most 1 MB; that run and every newer run are rewritten without the
   dead row identifiers;
3. retires the replaced chains, and runs the page pass that makes pages
   reusable (see [Concurrency and page reuse](architecture.md#concurrency-and-page-reuse)).

With `exact = off` there are no row-tier entries, and step 2 is skipped.

Page-tier postings are not cleaned: a page that no longer holds a row with a
given key stays in that key's list. This causes extra rechecks, not wrong
results. `REINDEX` removes such entries.

Rewriting runs writes WAL about the size of the runs rewritten.

## Explicit merges

```sql
SELECT stomata_merge_pending('name_st');
SELECT stomata_compact('name_st');
```

`stomata_merge_pending()` folds the pending list and merges runs by the same
size rule VACUUM uses. It waits for a merge already in progress.

`stomata_compact()` folds the pending list and merges all runs into one.
It needs free space for a complete copy of the index while it runs. The
pages of the old runs are reused afterwards, but the file is not truncated.

Both require ownership of the table.

## Index size

The index file does not shrink. Pages freed by merges are reused for later
runs and pending pages. To return space to the operating system, use
`REINDEX` (or `REINDEX CONCURRENTLY`).

## HOT updates

stomata declares itself a summarizing access method, as BRIN does. HOT
updates therefore remain possible on an indexed column, and stomata is
called for each heap-only tuple. A heap-only tuple cannot be reached through
its own line pointer, so it is indexed under the root line pointer of its
HOT chain, as `CREATE INDEX` does. If the root cannot be determined, its
heap page is recorded under the `W` key and returned by every scan.

Because the root identifier keeps the keys of earlier versions in the
chain, those versions' keys remain in the index until the row is removed or
the index is rebuilt. This causes extra rechecks, not wrong results.

## Monitoring

```sql
-- pending list size, number of runs, total pages
SELECT pending_records, pending_pages, runs, index_pages
FROM stomata_index_info('name_st');

-- size of each run
SELECT * FROM stomata_runs('name_st');

-- consistency check: number of visible rows the index does not cover
SELECT stomata_verify('name_st');
```

`stomata_verify()` reads the whole table and index; it is meant for testing
and for checking an index after an incident, not for routine monitoring.

VACUUM reports, for each stomata index, the pages it made reusable (free)
and the pages still readable by some snapshot (deleted).
