EXTENSION    = stomata
EXTVERSION   = $(shell grep default_version $(EXTENSION).control | sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

MODULE_big   = stomata
OBJS         = src/stomata.o src/keys.o src/storage.o src/lsm.o src/funcs.o src/cost.o
DATA         = $(wildcard sql/$(EXTENSION)--*.sql)
DOCS         = doc/stomata.md doc/cost_model.md

TESTS        = $(wildcard test/sql/*.sql)
REGRESS      = $(patsubst test/sql/%.sql,%,$(TESTS))
REGRESS_OPTS = --inputdir=test --outputdir=test --load-extension=$(EXTENSION) --encoding=UTF8 --no-locale

PG_CONFIG   ?= pg_config
PG_CPPFLAGS  = -Isrc
EXTRA_CLEAN  = test/results test/regression.diffs test/regression.out

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# PGXS does not track header dependencies
$(OBJS): src/stomata.h

ifeq ($(shell test $(VERSION_NUM) -lt 160000; echo $$?),0)
$(error stomata requires PostgreSQL 16 or later)
endif

dist:
	git archive --format zip --prefix=$(EXTENSION)-$(EXTVERSION)/ -o $(EXTENSION)-$(EXTVERSION).zip HEAD
