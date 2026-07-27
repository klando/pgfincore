EXTENSION    = pgfincore
MODULES      = pgfincore
MODULEDIR    = pgfincore
DOCS         = README.md
DATA         = pgfincore--1.2.2--1.4.sql \
               pgfincore--1.2.3--1.4.sql \
               pgfincore--1.2.4--1.4.sql \
               pgfincore--1.3.1--1.4.sql \
               pgfincore--1.3--1.4.sql \
               pgfincore--1.4.sql \
               pgfincore--1.4--1.5.sql \
               pgfincore--1.5.sql

REGRESS      = upgrade \
               pgfincore

PG_CONFIG    = pg_config

PGXS := $(shell $(PG_CONFIG) --pgxs)

include $(PGXS)
