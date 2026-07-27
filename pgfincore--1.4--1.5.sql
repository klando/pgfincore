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
-- Rewrite pgsysconf() as SQL wrapper over vm_* functions
--
CREATE OR REPLACE FUNCTION
pgsysconf(OUT os_page_size   bigint,
          OUT os_pages_free  bigint,
          OUT os_total_pages bigint)
RETURNS record
AS $pgsysconf$
SELECT
  vm_page_size()       AS os_page_size
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

--
-- Rewrite pgsysconf_pretty() as SQL wrapper
--
CREATE OR REPLACE FUNCTION
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
