EXTENSION    = pgfincore
EXTVERSION   = 1.3.2

MODULES      = $(EXTENSION)
MODULEDIR    = $(EXTENSION)
DOCS         = README.md
DATA         = $(EXTENSION)--1.2--1.3.1.sql \
               $(EXTENSION)--1.3.1--1.3.2.sql \
               $(EXTENSION)--$(EXTVERSION).sql

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
