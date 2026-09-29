STOMATA cost model
==================

This note records how the cost model was designed and measured. The
design came from comparing the planner's estimates with measured run times for
the sequential scan, a pg_trgm GIN index, and stomata.

Method
------

`tools/cost_oracle.py` runs each query three times, once with each access
path forced: a sequential scan, the GIN index, and stomata. For the index
paths the other indexes are dropped inside a transaction that is rolled back.
For each path it records the planner's total cost and the median of five
`EXPLAIN ANALYZE` runs, after a warm-up. The "oracle" is the fastest path.

Since the planner picks the path with the lowest estimated cost, its choice
among any set of available paths follows from the recorded estimates. A
*mistake* is a choice more than 1.2x slower than the oracle.

The setup:

- Two tables of 1,000,000 rows:
  - `interactions.username`: Faker-style user names, random heap order, 42 queries;
  - `urls.url`: synthetic URLs with date paths clustered in heap order, 5% non-ASCII rows, 36 queries.
- PostgreSQL 16, warm cache, JIT and parallel query off unless stated.
- `random_page_cost` 4 and 1.1.

What the comparison showed
--------------------------

1. **PostgreSQL's own estimates are not uniformly accurate.**
   - Sequential scans took 3.6 µs per cost unit on the user-name table and
     6.0 µs on the URL table. The planner charges one `cpu_operator_cost` per
     `LIKE` call whatever the string length, and `ILIKE` is about twice as slow
     as `LIKE` at the same charge.
   - GIN ranged from 0.02 to 81 µs per cost unit. For selective patterns
     it charges about 100 units for 3 ms of work. It prices heap access
     with the column-statistics row estimate, not with the trigram
     candidates it will actually recheck.
   - Bitmap heap scans over cached pages are overcharged at
     `random_page_cost = 4`. This affects GIN and stomata equally.

   The stomata estimate therefore has to be *consistent with PostgreSQL's
   conventions*: pages at `random_page_cost` / `seq_page_cost`, work at
   `cpu_operator_cost`, heap access left to `cost_bitmap_heap_scan`.
   Tuning it to wall-clock time on one machine would not carry over.

2. **The number the planner lacks is the candidate count.** An earlier model
   used the row estimate of `likesel` (doubled for row-level keys). That estimate was:
   - 99 rows for `'allison%'` (actual 1,044);
   - 99 rows for `'%1%2%'`, which returns 15,000 lossy pages.

   Only the index knows how many rows and pages a pattern's keys select.

3. **Keys of one literal are strongly correlated.** 340 random literals were
   sampled from the two tables. Their candidate counts were compared with
   estimates built from the per-key counts:

   | estimator | mean \|log10 error\| | estimates off by >10x |
   |---|---|---|
   | independence (product of key fractions) | 1.56 | most |
   | rarest key only | 0.93 | 125 of 340 |
   | exponential backoff (s1·s2^½·s3^¼…) | 0.79 | 117 |
   | rarest key, lists intersected up to 50,000 values | 0.061 | 8 |
   | the same, up to 100,000 values | 0.030 | 4 |

   No statistic combining per-key counts came close to counting. So the
   planner counts, within a budget.

4. **Index work is dominated by decoding.** A least-squares fit of index scan
   time over about 140 measured scans gave:
   - 3.8 ns per posting value decoded;
   - 26 ns per TID emitted (sort + `tbm_add_tuples`);
   - 19 µs per key probe;
   - 15 ns per lossy page.

   Against the sequential-scan calibration (one cost unit ≈ 4.5 µs), these are
   about ¼ `cpu_operator_cost`, one `cpu_index_tuple_cost`, and one random
   page.

The model
---------

For each constant pattern (`LIKE`, `ILIKE`, `= ANY` arrays, `AND`ed clauses):

1. Compile the pattern with the index's parameters. Look up each key in every
   run: a directory probe, with the count read in place. Lists longer than
   8 kB are extrapolated from their first 8 kB.
2. AND the page-tier bitmaps exactly (npages/8 bytes each).
3. Intersect the row-level lists rarest first, the way the scan does, while
   the values decoded stay within `stomata.estimate_budget` (default 50,000).
   The result is filtered by the page bitmap.
   - If every list fits in the budget, the candidate count is exact.
   - Otherwise it is an upper bound, because lists left out can only remove
     candidates. It is lowered to the statistical estimate below when that is
     smaller.
4. If even the rarest list exceeds the budget, estimate instead:
   - split the pattern into literal pieces (`'a%son'` → `'a%'`, `'%son'`);
   - a piece allows at most as many rows as its rarest key;
   - pieces are independent.
5. Add:
   - rows marked non-ASCII (`ILIKE`);
   - the pages of the `A` and `W` pseudo keys;
   - pending records, which the scan tests with `LIKE`, so they count as
     matches only.

`indexSelectivity` is the fraction of the table the bitmap will hold: exact
TIDs plus lossy pages × rows per page. The index cost is:

```
probes × random_page_cost + further list pages × seq_page_cost
+ values decoded × ¼ cpu_operator_cost
+ page-tier keys × npages/64 × cpu_operator_cost
+ TIDs × cpu_index_tuple_cost + lossy pages × cpu_operator_cost
+ lossy pages × rows per page × cpu_tuple_cost
```

The last term covers a bitmap heap scan over lossy pages: it measured about
1.35x a sequential scan of the same pages, which `cost_bitmap_heap_scan`
does not model.

Further adjustments:
- Page reads are capped at the index size.
- They are discounted for repeated scans with `index_pages_fetched`, as in
  `genericcostestimate`.
- Counts are absolute heap rows and pages, which is also correct for a
  partial index.

Per-key statistics and evaluated conjunctions are cached per backend and keyed
by the run list's generation. The first planning of a new pattern in a backend
costs a median of 0.6 ms (0.35 ms with a budget of 0). Repeated planning adds
nothing measurable. Patterns that are not constants (generic plans) use the
previous, statistics-based estimate. So does planning without an active
snapshot, since reading runs needs one: the snapshot keeps the pages of
runs retired by a concurrent merge from being reused.

Results
-------

Choice between stomata and a sequential scan (the usual deployment), across
both tables, 77 queries each at two `random_page_cost` settings:

| | row-estimate model (earlier) | this model |
|---|---|---|
| plans >1.2x slower than the fastest | 6 of 154 (up to 2.3x) | 2 of 154 (1.2x, 1.4x) |
| time lost against the oracle | 444 ms of 6.1 s | 75 ms of 6.1 s |
| with parallel sequential scans allowed (`random_page_cost` 4) | 9 of 77, 329 ms lost | 6 of 77, 126 ms lost |

The two remaining mistakes are the same query,
`'https://www.example.com/news/2016/%'`. The scan decodes 18.7 million posting
values, the estimate prices that correctly, and the two plans are within
1.2-1.4x of each other.

With a GIN index on the same column as well, the planner still prefers GIN
for most selective patterns, including ones where stomata is 1.2-14x faster
(a few milliseconds each): GIN's estimate for those is lower. That is GIN's estimate,
not stomata's, and it is why keeping both indexes on one column is not
recommended.

Next steps suggested by the data
--------------------------------

- **Stop decoding hopeless lists.** Long anchored literals (`https://…`) make
  the scan decode lists of a million TIDs to trim a few thousand candidates.
  The estimate now prices this correctly, but the scan itself would be much
  faster if it skipped lists far longer than the candidates left, and let the
  recheck handle the rest.
- **Tell PostgreSQL what `LIKE` costs.** Raising the cost of `textlike` and
  `texticlike` (`ALTER FUNCTION ... COST`) would correct the per-row cost
  that every plan underestimates, for sequential scans and rechecks alike.
  This was not measured here.
