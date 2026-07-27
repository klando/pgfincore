--
-- pgfincore 1.5 install script
--

--
-- SYSCONF
--
CREATE FUNCTION pg_page_size() RETURNS bigint
AS '$libdir/pgfincore' LANGUAGE C STABLE;
COMMENT ON FUNCTION pg_page_size()
IS 'Returns PostgreSQL page size in bytes';

CREATE FUNCTION pg_segment_size() RETURNS bigint
AS '$libdir/pgfincore' LANGUAGE C STABLE;
COMMENT ON FUNCTION pg_segment_size()
IS 'Returns PostgreSQL segment size in blocks';

CREATE FUNCTION vm_available_pages() RETURNS bigint
AS '$libdir/pgfincore' LANGUAGE C VOLATILE;
COMMENT ON FUNCTION vm_available_pages()
IS 'Returns current number of free pages in system memory';

CREATE FUNCTION vm_page_size() RETURNS bigint
AS '$libdir/pgfincore' LANGUAGE C STABLE;
COMMENT ON FUNCTION vm_page_size()
IS 'Returns system page size in bytes';

CREATE FUNCTION vm_physical_pages() RETURNS bigint
AS '$libdir/pgfincore' LANGUAGE C STABLE;
COMMENT ON FUNCTION vm_physical_pages()
IS 'Returns number of pages in system memory';

--
-- SYSCONF — legacy wrappers (SQL over vm_* functions)
--
CREATE FUNCTION
pgsysconf(OUT os_page_size   bigint,
          OUT os_pages_free  bigint,
          OUT os_total_pages bigint)
RETURNS record
AS $pgsysconf$
SELECT
  vm_page_size()    AS os_page_size
, vm_available_pages() AS os_pages_free
, vm_physical_pages()  AS os_total_pages
$pgsysconf$
LANGUAGE SQL VOLATILE PARALLEL SAFE;

COMMENT ON FUNCTION pgsysconf()
IS 'DEPRECATED - use vm_page_size(), vm_available_pages(), vm_physical_pages() instead.
Get system configuration information at run time:
 - os_page_size is _SC_PAGESIZE
 - os_pages_free is _SC_AVPHYS_PAGES
 - os_total_pages is _SC_PHYS_PAGES

man 3 sysconf for details';

CREATE FUNCTION
pgsysconf_pretty(OUT os_page_size   text,
                 OUT os_pages_free  text,
                 OUT os_total_pages text)
RETURNS record
AS $pgsysconf_pretty$
SELECT
  pg_size_pretty(os_page_size)                  AS os_page_size
, pg_size_pretty(os_pages_free * os_page_size)  AS os_pages_free
, pg_size_pretty(os_total_pages * os_page_size) AS os_total_pages
FROM pgsysconf()
$pgsysconf_pretty$
LANGUAGE SQL VOLATILE PARALLEL SAFE;

COMMENT ON FUNCTION pgsysconf_pretty()
IS 'DEPRECATED - use vm_page_size(), vm_available_pages(), vm_physical_pages() with pg_size_pretty() instead.
pgsysconf() with human readable output';

--
-- PGFADVISE
--
CREATE FUNCTION
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

CREATE FUNCTION
pgfadvise_willneed(IN regclass,
				   OUT relpath text,
				   OUT os_page_size bigint,
				   OUT rel_os_pages bigint,
				   OUT os_pages_free bigint)
RETURNS setof record
AS 'SELECT pgfadvise($1, ''main'', 10)'
LANGUAGE SQL;

CREATE FUNCTION
pgfadvise_dontneed(IN regclass,
				   OUT relpath text,
				   OUT os_page_size bigint,
				   OUT rel_os_pages bigint,
				   OUT os_pages_free bigint)
RETURNS setof record
AS 'SELECT pgfadvise($1, ''main'', 20)'
LANGUAGE SQL;

CREATE FUNCTION
pgfadvise_normal(IN regclass,
				 OUT relpath text,
				 OUT os_page_size bigint,
				 OUT rel_os_pages bigint,
				 OUT os_pages_free bigint)
RETURNS setof record
AS 'SELECT pgfadvise($1, ''main'', 30)'
LANGUAGE SQL;

CREATE FUNCTION
pgfadvise_sequential(IN regclass,
					 OUT relpath text,
					 OUT os_page_size bigint,
					 OUT rel_os_pages bigint,
					 OUT os_pages_free bigint)
RETURNS setof record
AS 'SELECT pgfadvise($1, ''main'', 40)'
LANGUAGE SQL;

CREATE FUNCTION
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
CREATE FUNCTION
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

CREATE FUNCTION
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
CREATE FUNCTION
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

CREATE FUNCTION
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

CREATE FUNCTION
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

CREATE FUNCTION
pgfincore_drawer(IN varbit,
		  OUT drawer cstring)
RETURNS cstring
AS '$libdir/pgfincore'
LANGUAGE C;

COMMENT ON FUNCTION pgfincore_drawer(varbit)
IS 'A naive drawing function to visualize page cache per object';
