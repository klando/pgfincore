EXTENSION    = pgfincore
EXTVERSION   = 1.1.1
EXTCOMMENT   = examine and manage the os buffer cache

MODULES      = $(EXTENSION)
DOCS         = README.rst

PG_CONFIG    = pg_config
BUILD_EXTENSION = $(shell $(PG_CONFIG) --version | grep -qE "8\.|9\.0" && echo no || echo yes)

# Default (no Extension support)
DATA         = $(EXTENSION).sql uninstall_$(EXTENSION).sql
REGRESS      = $(EXTENSION)
pgf_files   := $(DOCS) $(DATA)

# we need to build with Extension support:
ifeq ($(BUILD_EXTENSION),yes)
all: $(EXTENSION)--$(EXTVERSION).sql $(EXTENSION)--unpackaged--$(EXTVERSION).sql

# this copy the extension.sql to extension--version.sql
$(EXTENSION)--$(EXTVERSION).sql: $(EXTENSION).sql
			cp $< $@

# this build the extension--unpackaged-version.sql from uninstall_extension.sql
$(EXTENSION)--unpackaged--$(EXTVERSION).sql: uninstall_$(EXTENSION).sql
			sed 's/DROP /ALTER EXTENSION $(EXTENSION) ADD /' $< > $@

# this build extension.control from extension.control.in
$(EXTENSION).control: $(EXTENSION).control.in
			sed 's/EXTVERSION/$(EXTVERSION)/;s/EXTENSION/$(EXTENSION)/;s/EXTCOMMENT/$(EXTCOMMENT)/' $< > $@

DATA        = $(EXTENSION)--unpackaged--$(EXTVERSION).sql $(EXTENSION)--$(EXTVERSION).sql
REGRESS     = $(EXTENSION).ext
EXTRA_CLEAN = $(DATA) $(EXTENSION).control
pgf_files  := $(DOCS)
endif

# Workaround for lack of good VPATH support in pgxs for extension/contrib
ifdef VPATH
pgf_files_build:= $(addprefix $(CURDIR)/, $(pgf_files))
all: $(pgf_files_build)
$(pgf_files_build): $(CURDIR)/%: $(VPATH)/%
	cp $< $@
endif # VPATH

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

deb:
	make clean
	make -f debian/rules debian/control
	dh clean
	make -f debian/rules orig
	debuild -us -uc -sa --source-option="--git-ref=master"
