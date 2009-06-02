MODULES = pgfincore
DATA_built = pgfincore.sql
DATA = uninstall_pgfincore.sql
DOCS = README.pgfincore
REGRESS = pgfincore

PG_LIBS = -lpgfincore
SHLIBS_LINK = -lpgfincore
# PG_CPPFLAGS = -lpgfincore

PGXS := $(shell pg_config --pgxs)
include $(PGXS)
