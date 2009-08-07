MODULES = pgfincore
DATA_built = pgfincore.sql uninstall_pgfincore.sql
DOCS = README.pgfincore

#PG_LIBS = -lpgfincore
#SHLIBS_LINK = -lpgfincore

#PGXS := $(shell pg_config --pgxs)
PGXS = /usr/lib/postgresql/8.3/lib/pgxs/src/makefiles/pgxs.mk
include $(PGXS)
