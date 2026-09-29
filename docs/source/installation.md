# Installation

## Requirements

- PostgreSQL 16, 17 or 18, with the server development files
  (`pg_config` and the PGXS makefiles, usually packaged as
  `postgresql-server-dev-NN` or `postgresqlNN-devel`).
- A C compiler and `make`.

The build fails with an error on PostgreSQL versions before 16.

## From source

```sh
make
make install                  # may need sudo
make installcheck             # optional: runs the regression suites
```

To build against a specific server, pass its `pg_config`:

```sh
make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config
make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config install
```

`make installcheck` needs a running server and a role that can create
databases.

## From PGXN

```sh
pgxn install stomata
```

## Enabling it in a database

```sql
CREATE EXTENSION stomata;
```

Creating the extension requires superuser privileges: it creates an access
method, and the extension is not marked as trusted. The extension is
relocatable.

## Removing it

```sql
DROP EXTENSION stomata;            -- fails while stomata indexes exist
DROP EXTENSION stomata CASCADE;    -- also drops them
```
