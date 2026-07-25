EXTENSION    = pgfincore

MODULES      = $(EXTENSION)
MODULEDIR    = $(EXTENSION)
DOCS         = README.md
DATA         = $(EXTENSION)--1.2--1.3.1.sql \
               $(EXTENSION)--1.3.1--1.4.sql \
               $(EXTENSION)--1.4.sql \
               $(EXTENSION)--1.4--2.0.0.sql

REGRESS      = $(EXTENSION)

PG_CONFIG    = pg_config

PGXS := $(shell $(PG_CONFIG) --pgxs)

include $(PGXS)
