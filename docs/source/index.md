# stomata

stomata is a PostgreSQL extension that adds an index access method for the
`LIKE` and `ILIKE` operators on `text` and `varchar` columns.

```sql
CREATE EXTENSION stomata;
CREATE INDEX users_email_st ON users USING stomata (email);

SELECT * FROM users WHERE email LIKE 'jo%@example.com';
SELECT * FROM users WHERE email LIKE '%.io';
SELECT * FROM users WHERE email LIKE 'a___@%';
SELECT * FROM users WHERE email ILIKE 'JO%';
```

## What it does

The index returns candidate rows for a pattern; the executor rechecks every
candidate against the original condition, so query results are always the
same as without the index. The index decides how many rows and pages are
read and rechecked.

It stores keys derived from each value: characters at positions near the
start and the end, short n-grams anywhere in the value, whole prefixes and
suffixes up to four characters, character pairs a fixed distance apart, and
keys that combine the first and last characters of the same row. This lets
it restrict anchored patterns (`'abc%'`, `'%xyz'`, `'ab%yz'`), patterns
containing `_`, and short literals, in addition to infixes (`'%abc%'`).

Selective keys point to rows; the others point to heap pages. When a
pattern has at least one row-level key, the scan returns rows; otherwise it
returns whole pages, all of whose rows are rechecked.

## Status

Version 0.1.0 is the first release and is intended for testing. The on-disk
format may change before 1.0; a release that changes it will require
`REINDEX`. See [Limitations](limitations.md).

| | |
|---|---|
| PostgreSQL | 16, 17, 18 |
| Operators | `~~` (`LIKE`), `~~*` (`ILIKE`) |
| Column types | `text`, `varchar` (through the implicit cast to `text`) |
| License | MIT |

```{toctree}
:maxdepth: 2
:caption: User guide

installation
usage
maintenance
limitations
```

```{toctree}
:maxdepth: 2
:caption: Internals

patterns
architecture
planner
durability
```

```{toctree}
:maxdepth: 2
:caption: Reference

api
development
changelog
```
