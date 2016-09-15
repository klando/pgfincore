
DROP FUNCTION pgsysconf_pretty();
DROP FUNCTION pgsysconf();

DROP FUNCTION pgfadvise_willneed(regclass);
DROP FUNCTION pgfadvise_dontneed(regclass);
DROP FUNCTION pgfadvise_normal(regclass);
DROP FUNCTION pgfadvise_sequential(regclass);
DROP FUNCTION pgfadvise_random(regclass);
DROP FUNCTION pgfadvise(regclass, text, int);

DROP FUNCTION pgfadvise_loader(regclass, text, int, bool, bool, varbit);
DROP FUNCTION pgfadvise_loader(regclass, int, bool, bool, varbit);

DROP FUNCTION pgfincore(regclass);
DROP FUNCTION pgfincore(regclass, bool);
DROP FUNCTION pgfincore(regclass, text, bool);
DROP FUNCTION pgfincore_drawer(varbit);
