# Architecture

## Storage

The index is log-structured: inserts append to a pending list,
and merges turn the pending list into immutable sorted runs.

### Pages

| block | content |
|---|---|
| 0 | metapage |
| run: blob chain | `key -> container` entries, sorted by key |
| run: directory chain | the first key on each blob page, used for lookups |
| pending chain | unsorted records appended by `INSERT` and `UPDATE` |
| frozen pending chain | the pending list that a running merge is folding in |

Every page stores, in its special space, the id of the chain it belongs to
(a run or a pending list). This id is used for page reuse; see
[Concurrency and page reuse](#concurrency-and-page-reuse).

#### Metapage

Block 0 holds:

- the build parameters and the on-disk format version (1);
- the run list, oldest first, at most 16 runs;
- pointers to the active and the frozen pending chains, and their sizes;
- a generation number, incremented whenever the run list changes;
- chain-id bookkeeping: the next chain id, the chain being written by a
  merge, and a ring of up to 64 retired chains with their retirement
  transaction ids, plus a horizon covering older retirements.

#### Runs

A run holds one entry per key, sorted by key. The entry's container is one
of:

- a delta-coded varbyte list of row identifiers (row tier);
- a delta-coded varbyte list of heap page numbers, or a bitmap over heap
  pages, whichever is smaller (page tier);
- a stop marker, for a page-tier key present on more than `stop_threshold`
  of the heap pages; a stop key is treated as present on every page.

Row identifiers are encoded as `block * MaxHeapTuplesPerPage + offset - 1`
so that they sort in heap order.

A run is never modified after it is written. It is replaced as a whole by a
merge.

#### Pending list

Each insert appends a record `(heap block, offset, value)` to the pending
list. Offset 0 marks a heap-only tuple whose page must be returned whole.
Values longer than 2000 bytes are not stored; when the record is merged, its
heap page is recorded under the `A` key, so the page is a candidate for every
scan. A build reads values from the table directly and indexes them
regardless of length.

Scans test pending records with `LIKE` or `ILIKE` directly, so they cost a
comparison per record rather than a key lookup.

### Merges

A merge:

1. **freezes** the pending list: new inserts start a new list, and the frozen
   one remains visible to scans;
2. chooses the runs to merge (below) and claims a chain id for the new run;
3. **collects** the keys of the frozen records in memory, spilling sorted
   temporary files when `maintenance_work_mem` is exceeded;
4. streams the collected keys and the chosen runs into the new run with a
   k-way merge, holding one posting list in memory at a time;
5. **switches** the run list in the metapage, in the same record that retires
   the replaced chains;
6. offers the replaced pages to the free space map.

Only the freeze and the switch take the metapage buffer lock, briefly. A
heavyweight lock on the metapage allows one merge at a time. Scans and
inserts continue while a merge runs.

#### Choosing runs

Runs are chosen by size, from the newest backwards. A run is included while
its size is at most 4 times the total already being merged. In addition:

- `stomata_compact()` merges all runs;
- a merge started by an insert does not include runs larger than
  max(8 MB, 4 × `pending_limit`); those are left to VACUUM or an explicit
  merge;
- if the run list is full (16 runs), the next run is included regardless.

### Build

`CREATE INDEX` and `REINDEX` scan the table, collect the keys with the same
collector (bounded by `maintenance_work_mem`, spilling to temporary files),
and write one run. The build is single-process.

## Concurrency and page reuse

### Locks

| operation | locks taken on the index |
|---|---|
| scan | metapage buffer, shared, only while copying the metapage |
| insert | metapage and pending-list tail buffers, exclusive, while appending one record |
| merge | heavyweight lock on the metapage for the whole merge (one merge at a time); metapage buffer, exclusive, for the freeze and the switch |
| merge started by an insert | as above, but skipped if another merge holds the heavyweight lock |
| VACUUM page pass | heavyweight lock on the metapage |

Every insert updates the metapage, so concurrent inserts into one index are
serialized for the duration of an append.

### Scans

A scan copies the metapage and releases it. It then reads the runs and
pending lists that the copy points to, without holding a lock. Runs are
never modified, so the copy stays consistent. A merge may replace those runs
while the scan runs; the replaced pages must stay readable until the scan
ends. This is what the page reuse rules guarantee.

Run directories (the first key of each blob page) are cached per backend,
keyed by run. A cached directory is valid for as long as its run exists,
since runs are immutable.

### Retiring chains

Each run and each pending list is a chain of pages with its own chain id,
stored on every page of the chain. When a merge switches the run list, the
same metapage update records the replaced chains as retired, together with
the next full transaction id. The retired pages themselves are not written.

The metapage keeps the most recent 64 retirements. When the ring is full,
the oldest entry is folded into a single horizon value; since retirement ids
only increase, the horizon covers every chain that has left the ring.

### Reusing a page

When the allocator considers a page from the free space map, it classifies
it from the page's chain id and the current metapage:

- **in use** if the chain is live (a run, a pending list, or the chain being
  written by a merge), or if the chain id is newer than the metapage copy
  being consulted;
- **retired** if the chain's retirement id is not yet older than every
  snapshot that could still read it
  (`GlobalVisCheckRemovableFullXid`);
- **reusable** otherwise, or if the page is new.

Only reusable pages are taken. Retired pages are set aside and returned to
the free space map after the allocation, so they can be considered again
later.

### Failed merges

A merge that fails or crashes leaves its partly written chain recorded as
"being written". The next merge, or the next VACUUM, retires it. Its pages
then become reusable under the same rules.

### VACUUM page pass

After merging, VACUUM retires any chain left by a failed merge, then reads
every index page and records reusable and retired pages in the free space
map, including pages orphaned by a crash. Whether a retired page can
actually be reused is decided again by the allocator when it is taken.
VACUUM reports reusable pages as free, and retired pages as deleted but not
yet free.
