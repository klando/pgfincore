EXTENSION    = pgfincore
EXTVERSION   = 1.2

MODULES      = $(EXTENSION)
MODULEDIR    = $(EXTENSION)
DOCS         = README.md
DATA         = $(EXTENSION)--$(EXTVERSION).sql $(EXTENSION)--unpackaged--$(EXTVERSION).sql
REGRESS      = $(EXTENSION)

PG_CONFIG    = pg_config

PGXS := $(shell $(PG_CONFIG) --pgxs)

include $(PGXS)

dist:
	git archive --prefix=$(EXTENSION)-$(EXTVERSION)/ -o ../$(EXTENSION)_$(EXTVERSION).orig.tar.gz HEAD

deb:
	make clean
	make -f debian/rules debian/control
	dh clean
	make -f debian/rules orig
	debuild -us -uc -sa
