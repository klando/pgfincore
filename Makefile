EXTENSION    = pgfincore
MODULES      = $(EXTENSION)
MODULEDIR    = $(EXTENSION)
DOCS         = README.md
DATA         = pgfincore--1.2.sql \
               pgfincore--1.2--1.3.1.sql

REGRESS      = $(EXTENSION)

PG_CONFIG    = pg_config

PGXS := $(shell $(PG_CONFIG) --pgxs)

include $(PGXS)

EXTVERSION   = 1.3.1
dist:
	git archive --prefix=$(EXTENSION)-$(EXTVERSION)/ -o ../$(EXTENSION)_$(EXTVERSION).orig.tar.gz HEAD

deb:
	make clean
	pg_buildext updatecontrol
	make -f debian/rules debian/control
	dh clean
	make dist
	dpkg-buildpackage -us -uc
