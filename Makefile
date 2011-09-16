EXTENSION    = pgfincore
EXTVERSION   = 1.1

MODULES      = $(EXTENSION)
DATA         = pgfincore.sql uninstall_pgfincore.sql
DOCS         = README.rst

PG_CONFIG    = pg_config

PG91         = $(shell $(PG_CONFIG) --version | grep -qE "8\.|9\.0" && echo no || echo yes)

ifeq ($(PG91),yes)
all: pgfincore--$(EXTVERSION).sql

pgfincore--$(EXTVERSION).sql: pgfincore.sql
	cp $< $@

DATA        = pgfincore--unpackaged--$(EXTVERSION).sql pgfincore--$(EXTVERSION).sql
EXTRA_CLEAN = $(EXTENSION)--$(EXTVERSION).sql
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

deb:
	make -f debian/rules debian/control
	dh clean
	make -f debian/rules orig
	debuild -us -uc -sa
