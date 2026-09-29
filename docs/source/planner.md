# Planner

The index provides its own cost estimate (`amcostestimate`). PostgreSQL
compares it with the other available paths, usually a sequential scan.

## Why the index estimates candidates itself

PostgreSQL's selectivity for `LIKE` (`likesel`) estimates how many rows
*match*. The cost of an index scan depends on how many rows and pages the
index *returns as candidates*, which can be much larger (for page-level keys)
and which only the index can determine. The estimate therefore looks at the
index's contents at plan time.

## Steps

For each constant pattern, or `AND`ed group of patterns, on the index:

1. The pattern is compiled with the index's parameters, and each key is
   looked up in every run. The lookup reads the directory and the entry
   header, not the whole posting list. Lists longer than 8 kB are
   extrapolated from their first 8 kB.
2. The page-tier bitmaps are ANDed.
3. The row-tier lists are intersected, rarest first, as long as the number
   of values decoded stays within `stomata.estimate_budget` (default 50000).
   - If all lists fit, the candidate count is exact.
   - Otherwise it is an upper bound, lowered to the estimate of step 4 when
     that is smaller.
4. If even the rarest list exceeds the budget, the count is estimated: the
   pattern is split into literal pieces, each piece allows at most as many
   rows as its rarest key, and pieces are treated as independent.
5. Rows marked non-ASCII (for `ILIKE`), the pages under the `A` and `W`
   keys, and the pending records that match are added.

The resulting candidate rows and lossy pages give `indexSelectivity`. The
index cost charges key probes and list pages as page reads, decoded values
and emitted row identifiers at CPU cost, and the recheck of lossy pages.
Heap access is left to PostgreSQL's bitmap heap scan costing.

Costs use PostgreSQL's cost parameters (`random_page_cost`,
`seq_page_cost`, `cpu_operator_cost`, `cpu_index_tuple_cost`,
`cpu_tuple_cost`). They are not tuned to a particular machine.

## Caching

Key statistics and evaluated conjunctions are cached per backend and keyed
by the run list's generation, so they are discarded after a merge. Planning
a pattern for the first time in a backend reads index pages; planning it
again uses the cache.

## When the estimate is not used

- Patterns that are not constants (for example a parameter in a generic
  plan) use a statistics-based estimate that does not read the index.
- Planning without an active snapshot uses the same fallback, because
  reading runs requires a snapshot to keep their pages from being reused.

## Setting

`stomata.estimate_budget` (integer, default 50000, settable by any user) is
the number of posting values the planner may decode per pattern. `0` uses
key statistics only. Larger values make estimates of moderately selective
patterns more accurate and planning slower.

## Inspecting estimates

```sql
-- what the planner sees
SELECT * FROM stomata_estimate('name_st', '%son');

-- each key the pattern needs, with its size in each tier
SELECT * FROM stomata_pattern_stats('name_st', '%son');
```

`tools/cost_oracle.py` runs a list of queries with each access path forced
(sequential scan, a pg_trgm GIN index, stomata), records the planner's cost
and the measured time, and reports where the planner's choice differs from
the fastest path. `tools/cost_report.py` summarizes its output. The design
notes and measurements behind the model are in `doc/cost_model.md` in the
source tree.

## Using stomata together with a GIN index

If a column has both a stomata index and a pg_trgm GIN index, the planner
chooses between them from their estimates. The two estimates are produced
by different models, so the choice between the two indexes is not
necessarily the faster one.
