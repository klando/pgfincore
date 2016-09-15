EXTENSION    = pgfincore
EXTVERSION   = 1.2
EXTCOMMENT   = examine and manage the os buffer cache

MODULES      = $(EXTENSION)
MODULEDIR    = $(EXTENSION)
DOCS         = README.md
DATA_built   = $(EXTENSION)--$(EXTVERSION).sql $(EXTENSION)--unpackaged--$(EXTVERSION).sql
REGRESS      = $(EXTENSION).ext
EXTRA_CLEAN  = $(EXTENSION).control

PG_CONFIG    = pg_config

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Build some more files for extension support:

# pgxs is included after variable definition and before targets, so the
# PostgreSQL default target is used (all:)

# build the extension--unpackaged--version.sql from uninstall_extension.sql
# this assumes that the extension was installed via sql script instead of
# CREATE EXTENSION.
# This won't upgrade from a previous version to the current one.
$(EXTENSION)--unpackaged--$(EXTVERSION).sql: uninstall_$(EXTENSION).sql
	sed 's/DROP /ALTER EXTENSION $(EXTENSION) ADD /' $< > $@

# this copy the extension.sql to extension--version.sql
$(EXTENSION)--$(EXTVERSION).sql: $(EXTENSION).sql
	cp $< $@

# this build extension.control from extension.control.in
$(EXTENSION).control: $(EXTENSION).control.in
	sed 's/EXTVERSION/$(EXTVERSION)/;s/EXTENSION/$(EXTENSION)/;s/EXTCOMMENT/$(EXTCOMMENT)/' $< > $@

dist:
	git archive --prefix=$(EXTENSION)-$(EXTVERSION)/ -o ../$(EXTENSION)_$(EXTVERSION).orig.tar.gz HEAD

deb:
	make clean
	make -f debian/rules debian/control
	dh clean
	make -f debian/rules orig
	debuild -us -uc -sa
