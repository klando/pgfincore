EXTENSION    = pgfincore
EXTVERSION   = 1.1

MODULES      = $(EXTENSION)
DATA         = $(EXTENSION).sql uninstall_$(EXTENSION).sql
DOCS         = README.rst
REGRESS      = $(EXTENSION)

PG_CONFIG    = pg_config
BUILD_EXTENSION = $(shell $(PG_CONFIG) --version | grep -qE "8\.|9\.0" && echo no || echo yes)

ifeq ($(BUILD_EXTENSION),yes)
all: $(EXTENSION)--$(EXTVERSION).sql

$(EXTENSION)--$(EXTVERSION).sql: $(EXTENSION).sql
	cp $< $@

DATA        = $(EXTENSION)--unpackaged--$(EXTVERSION).sql $(EXTENSION)--$(EXTVERSION).sql
REGRESS     = $(EXTENSION).ext
EXTRA_CLEAN = $(EXTENSION)--$(EXTVERSION).sql
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

deb:
	make clean
	make -f debian/rules debian/control
	dh clean
	make -f debian/rules orig
	debuild -us -uc -sa
