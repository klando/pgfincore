
ALTER EXTENSION pgfincore ADD  FUNCTION pgsysconf_pretty();
ALTER EXTENSION pgfincore ADD  FUNCTION pgsysconf();

ALTER EXTENSION pgfincore ADD  FUNCTION pgfadvise_willneed(regclass);
ALTER EXTENSION pgfincore ADD  FUNCTION pgfadvise_dontneed(regclass);
ALTER EXTENSION pgfincore ADD  FUNCTION pgfadvise_normal(regclass);
ALTER EXTENSION pgfincore ADD  FUNCTION pgfadvise_sequential(regclass);
ALTER EXTENSION pgfincore ADD  FUNCTION pgfadvise_random(regclass);
ALTER EXTENSION pgfincore ADD  FUNCTION pgfadvise(regclass, text, int);

ALTER EXTENSION pgfincore ADD  FUNCTION pgfadvise_loader(regclass, text, int, bool, bool, varbit);
ALTER EXTENSION pgfincore ADD  FUNCTION pgfadvise_loader(regclass, int, bool, bool, varbit);

ALTER EXTENSION pgfincore ADD  FUNCTION pgfincore(regclass);
ALTER EXTENSION pgfincore ADD  FUNCTION pgfincore(regclass, bool);
ALTER EXTENSION pgfincore ADD  FUNCTION pgfincore(regclass, text, bool);

