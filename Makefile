MODULES = pgfincore
DATA_built = pgfincore.sql uninstall_pgfincore.sql
DOCS = README.pgfincore

PGXS := $(shell pg_config --pgxs)
include $(PGXS)
