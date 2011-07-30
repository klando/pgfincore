ifndef VPATH
SRCDIR = .
else
SRCDIR = $(VPATH)
endif

EXTENSION    = pgfincore
EXTVERSION   = $(shell grep default_version $(SRCDIR)/$(EXTENSION).control | \
               sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

MODULES      = $(EXTENSION)
DATA         = pgfincore.sql uninstall_pgfincore.sql
DOCS         = README.rst

ifndef PG_CONFIG
PG_CONFIG    = pg_config
endif

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

PKGNAME = $(EXTENSION)
PKGVERS = $(shell dpkg-parsechangelog | awk -F '[:-]' '/^Version:/ { print substr($$2, 2) }')

deb:
	PKGVERS=$(PKGVERS) make -f debian/rules debian/control
	dh clean
	PKGVERS=$(PKGVERS) make -f debian/rules orig
	debuild -us -uc -sa
