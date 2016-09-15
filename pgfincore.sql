--
-- SYSCONF
--
CREATE OR REPLACE FUNCTION
pgsysconf(OUT os_page_size   bigint,
          OUT os_pages_free  bigint,
          OUT os_total_pages bigint)
RETURNS record
AS '$libdir/pgfincore'
LANGUAGE C;

COMMENT ON FUNCTION pgsysconf()
IS 'Get system configuration information at run time:
 - os_page_size is _SC_PAGESIZE 
 - os_pages_free is _SC_AVPHYS_PAGES
 - os_total_pages is _SC_PHYS_PAGES

man 3 sysconf for details';


CREATE OR REPLACE FUNCTION
pgsysconf_pretty(OUT os_page_size   text,
                 OUT os_pages_free  text,
                 OUT os_total_pages text)
RETURNS record
AS '
select pg_size_pretty(os_page_size)                  as os_page_size,
       pg_size_pretty(os_pages_free * os_page_size)  as os_pages_free,
       pg_size_pretty(os_total_pages * os_page_size) as os_total_pages
from pgsysconf()'
LANGUAGE SQL;

COMMENT ON FUNCTION pgsysconf_pretty()
IS 'Pgsysconf() with human readable output';

--
-- PGFADVISE
--
CREATE OR REPLACE FUNCTION
pgfadvise(IN regclass, IN text, IN int,
		  OUT relpath text,
		  OUT os_page_size bigint,
		  OUT rel_os_pages bigint,
		  OUT os_pages_free bigint)
RETURNS setof record
AS '$libdir/pgfincore'
LANGUAGE C;

COMMENT ON FUNCTION pgfadvise(regclass, text, int)
IS 'Predeclare an access pattern for file data';

CREATE OR REPLACE FUNCTION
pgfadvise_willneed(IN regclass,
				   OUT relpath text,
				   OUT os_page_size bigint,
				   OUT rel_os_pages bigint,
				   OUT os_pages_free bigint)
RETURNS setof record
AS 'SELECT pgfadvise($1, ''main'', 10)'
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION
pgfadvise_dontneed(IN regclass,
				   OUT relpath text,
				   OUT os_page_size bigint,
				   OUT rel_os_pages bigint,
				   OUT os_pages_free bigint)
RETURNS setof record
AS 'SELECT pgfadvise($1, ''main'', 20)'
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION
pgfadvise_normal(IN regclass,
				 OUT relpath text,
				 OUT os_page_size bigint,
				 OUT rel_os_pages bigint,
				 OUT os_pages_free bigint)
RETURNS setof record
AS 'SELECT pgfadvise($1, ''main'', 30)'
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION
pgfadvise_sequential(IN regclass,
					 OUT relpath text,
					 OUT os_page_size bigint,
					 OUT rel_os_pages bigint,
					 OUT os_pages_free bigint)
RETURNS setof record
AS 'SELECT pgfadvise($1, ''main'', 40)'
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION
pgfadvise_random(IN regclass,
				 OUT relpath text,
				 OUT os_page_size bigint,
				 OUT rel_os_pages bigint,
				 OUT os_pages_free bigint)
RETURNS setof record
AS 'SELECT pgfadvise($1, ''main'', 50)'
LANGUAGE SQL;

--
-- PGFADVISE_LOADER
--
CREATE OR REPLACE FUNCTION
pgfadvise_loader(IN regclass, IN text, IN int, IN bool, IN bool, IN varbit,
				 OUT relpath text,
				 OUT os_page_size bigint,
				 OUT os_pages_free bigint,
				 OUT pages_loaded bigint,
				 OUT pages_unloaded bigint)
RETURNS setof record
AS '$libdir/pgfincore'
LANGUAGE C;

COMMENT ON FUNCTION pgfadvise_loader(regclass, text, int, bool, bool, varbit)
IS 'Restore cache from the snapshot, options to load/unload each block to/from cache';


CREATE OR REPLACE FUNCTION
pgfadvise_loader(IN regclass, IN int, IN bool, IN bool, IN varbit,
				 OUT relpath text,
				 OUT os_page_size bigint,
				 OUT os_pages_free bigint,
				 OUT pages_loaded bigint,
				 OUT pages_unloaded bigint)
RETURNS setof record
AS 'SELECT pgfadvise_loader($1, ''main'', $2, $3, $4, $5)'
LANGUAGE SQL;

--
-- PGFINCORE
--
CREATE OR REPLACE FUNCTION
pgfincore(IN regclass, IN text, IN bool,
		  OUT relpath text,
		  OUT segment int,
		  OUT os_page_size bigint,
		  OUT rel_os_pages bigint,
		  OUT pages_mem bigint,
		  OUT group_mem bigint,
		  OUT os_pages_free bigint,
		  OUT databit      varbit,
		  OUT pages_dirty bigint,
		  OUT group_dirty bigint)
RETURNS setof record
AS '$libdir/pgfincore'
LANGUAGE C;

COMMENT ON FUNCTION pgfincore(regclass, text, bool)
IS 'Utility to inspect and get a snapshot of the system cache';

CREATE OR REPLACE FUNCTION
pgfincore(IN regclass, IN bool,
		  OUT relpath text,
		  OUT segment int,
		  OUT os_page_size bigint,
		  OUT rel_os_pages bigint,
		  OUT pages_mem bigint,
		  OUT group_mem bigint,
		  OUT os_pages_free bigint,
		  OUT databit      varbit,
		  OUT pages_dirty bigint,
		  OUT group_dirty bigint)
RETURNS setof record
AS 'SELECT * from pgfincore($1, ''main'', $2)'
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION
pgfincore(IN regclass,
		  OUT relpath text,
		  OUT segment int,
		  OUT os_page_size bigint,
		  OUT rel_os_pages bigint,
		  OUT pages_mem bigint,
		  OUT group_mem bigint,
		  OUT os_pages_free bigint,
		  OUT databit      varbit,
		  OUT pages_dirty bigint,
		  OUT group_dirty bigint)
RETURNS setof record
AS 'SELECT * from pgfincore($1, ''main'', false)'
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION
pgfincore_drawer(IN varbit,
		  OUT drawer cstring)
RETURNS cstring
AS '$libdir/pgfincore'
LANGUAGE C;

COMMENT ON FUNCTION pgfincore_drawer(varbit)
IS 'A naive drawing function to visualize page cache per object';
