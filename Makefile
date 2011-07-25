EXTENSION    = pgfincore

EXTVERSION   = $(shell grep default_version $(EXTENSION).control | \
               sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

MODULES      = $(EXTENSION)
DATA         = $(filter-out $(wildcard sql/*--*.sql),$(wildcard sql/*.sql))
DOCS         = doc/README.$(EXTENSION).rst
# TESTS        = $(wildcard test/sql/*.sql)
# REGRESS      = $(patsubst test/sql/%.sql,%,$(TESTS))
# REGRESS_OPTS = --inputdir=test --load-language=plpgsql

ifndef PG_CONFIG
PG_CONFIG    = pg_config
endif

PG91         = $(shell $(PG_CONFIG) --version | grep -qE "8\.|9\.0" && echo no || echo yes)

ifeq ($(PG91),yes)
all: sql/$(EXTENSION)--$(EXTVERSION).sql

sql/$(EXTENSION)--$(EXTVERSION).sql: sql/$(EXTENSION).sql
	cp $< $@

DATA = $(wildcard sql/*--*.sql) sql/$(EXTENSION)--$(EXTVERSION).sql
EXTRA_CLEAN = sql/$(EXTENSION)--$(EXTVERSION).sql
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

PKGNAME = $(EXTENSION)
PKGVERS = $(shell dpkg-parsechangelog | awk -F '[:-]' '/^Version:/ { print substr($$2, 2) }')

DEBDIR = /tmp/$(PKGNAME)
EXPORT = $(DEBDIR)/export/$(PKGNAME)-$(PKGVERS)
ORIG   = $(DEBDIR)/export/$(PKGNAME)_$(PKGVERS).orig.tar.gz
ARCHIVE= $(DEBDIR)/export/$(PKGNAME)-$(PKGVERS).tar.gz
DEBEXTS= {gz,changes,build,dsc}

deb:	# working copy from where to make the .orig archive
	rm -rf $(DEBDIR)
	mkdir -p $(DEBDIR)/$(PKGNAME)-$(PKGVERS)
	mkdir -p $(EXPORT)/sql $(EXPORT)/doc
	cp Makefile  $(EXPORT)/
	cp sql/*  $(EXPORT)/sql
	cp $(DOCS)  $(EXPORT)/doc
	cp $(MODULES).c  $(EXPORT)/
	rsync -Ca debian $(EXPORT)/

	# prepare the .orig without the debian/ packaging stuff
	rsync -Ca $(EXPORT) $(DEBDIR)
	rm -rf $(DEBDIR)/$(PKGNAME)-$(PKGVERS)/debian
	(cd $(DEBDIR) && tar czf $(ORIG) $(PKGNAME)-$(PKGVERS))

	# have a copy of the $$ORIG file named $$ARCHIVE for non-debian packagers
	cp $(ORIG) $(ARCHIVE)

	# build the debian package and copy them to ..
	(cd $(EXPORT) && make -f debian/rules debian/control && debuild -us -uc)

	cp $(EXPORT)/debian/control debian
	find $(DEBDIR)/export -maxdepth 1 -type f -name "*$(PGGVERS)*" -exec cp {} .. \;

.PHONY: deb
