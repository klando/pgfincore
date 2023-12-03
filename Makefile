EXTENSION    = pgfincore
EXTVERSION   = 2.0.0

MODULES      = $(EXTENSION)
MODULEDIR    = $(EXTENSION)
DOCS         = README.md
DATA         = $(wildcard *.sql)

REGRESS      = $(EXTENSION)

PG_CONFIG    = pg_config

PGXS := $(shell $(PG_CONFIG) --pgxs)

include $(PGXS)

dist:
	git archive --prefix=$(EXTENSION)-$(EXTVERSION)/ -o ../$(EXTENSION)_$(EXTVERSION).orig.tar.gz HEAD

deb:
	make clean
	pg_buildext updatecontrol
	make -f debian/rules debian/control
	dh clean
	make dist
	dpkg-buildpackage -us -uc
