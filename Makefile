ifndef VPATH
SRCDIR = .
else
SRCDIR = $(VPATH)
endif

EXTENSION    = pgfincore
EXTVERSION   = $(shell grep default_version $(SRCDIR)/$(EXTENSION).control | \
               sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

MODULES      = $(EXTENSION)
DATA         = $(filter-out $(wildcard sql/*--*.sql),$(wildcard sql/*.sql))
DOCS         = doc/README.$(EXTENSION).rst

PG_CONFIG    = pg_config

PG91         = $(shell $(PG_CONFIG) --version | grep -qE "8\.|9\.0" && echo no || echo yes)

ifeq ($(PG91),yes)
all: $(EXTENSION)--$(EXTVERSION).sql

$(EXTENSION)--$(EXTVERSION).sql: $(SRCDIR)/sql/$(EXTENSION).sql
	cp $< $@

DATA = $(wildcard sql/*--*.sql) sql/$(EXTENSION)--$(EXTVERSION).sql
EXTRA_CLEAN = sql/$(EXTENSION)--$(EXTVERSION).sql
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

PKGNAME = $(EXTENSION)
PKGVERS = $(shell dpkg-parsechangelog | awk -F '[:-]' '/^Version:/ { print substr($$2, 2) }')

deb:
	dh clean
	PKGVERS=$(PKGVERS) make -f debian/rules orig
	debuild -us -uc -sa
