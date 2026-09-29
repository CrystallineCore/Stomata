# Development and testing

## Regression suites

```sh
make installcheck
```

runs the `pg_regress` suites in `test/sql/`:

| suite | covers |
|---|---|
| `01_setup` | access method, operator class, test data |
| `02_keys` | key extraction and pattern compilation |
| `03_soundness` | index results equal sequential-scan results for many patterns and option settings; `stomata_verify()` |
| `04_dml` | inserts, deletes, the pending list, VACUUM and explicit merges |
| `05_features` | planner integration, operators, options, `NULL`s, collations, error cases |
| `06_selectivity` | selective patterns return a small fraction of the table's pages |
| `07_hot` | HOT updates, compared with sequential scans across tier settings |
| `08_lsm` | runs, incremental merges, compaction, bounded-memory builds and merges, page reuse |
| `08b_cost` | the cost model's candidate estimates and plan choice |

## Tools

The scripts in `tools/` need a server that they can modify; some stop it.
Each script documents its options at the top of the file.

| tool | purpose |
|---|---|
| `stress.sh` | crash recovery (`pg_ctl -m immediate`) and concurrent activity on a scratch cluster |
| `replication.sh` | streaming replication, hot standby conflicts, `wal_consistency_checking`, point-in-time recovery (see [WAL, recovery and replication](durability.md)) |
| `crosscheck.py`, `stomata_ref.py` | compares the C key extraction with an independent Python model |
| `cost_oracle.py`, `cost_report.py` | compares planner estimates with measured times for sequential scan, pg_trgm GIN and stomata (see [Planner](planner.md)) |
| `compare_trgm.sh` | query timing against pg_trgm, one index usable per sample, plans checked, randomized order, paired comparison with a sign test |
| `crud_wal.sh` | WAL volume and time of inserts, updates, deletes and VACUUM, with no index, pg_trgm GIN and stomata |

`compare_trgm.sh` takes `ACCESS EXCLUSIVE` locks and builds indexes on the
table it is given; run it on a copy.
