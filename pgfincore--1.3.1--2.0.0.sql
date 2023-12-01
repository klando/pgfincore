CREATE FUNCTION
pg_sysconf_size(
  OUT pg_page_size    bigint
, OUT sys_pages_size  bigint
, OUT sys_total_pages bigint
)
RETURNS record
AS '$libdir/pgfincore'
LANGUAGE C IMMUTABLE;

COMMENT ON FUNCTION pg_sysconf_size()
IS 'Get system information:
 - pg_page_size is PostgreSQL page size
 - sys_pages_size is system page size
 - sys_total_pages is total memory pages';

CREATE FUNCTION
vm_free_pages(
  OUT sys_pages_free    bigint
)
RETURNS bigint
AS '$libdir/pgfincore'
LANGUAGE C STABLE;

COMMENT ON FUNCTION vm_free_pages()
IS 'Get system information:
 - sys_pages_free is current number of free pages in system memory';

REPLACE FUNCTION
pgsysconf(OUT os_page_size   bigint,
          OUT os_pages_free  bigint,
          OUT os_total_pages bigint)
RETURNS record
AS $pgsysconf$
SELECT
  p.sys_pages_size as os_page_size
, v.sys_pages_free as os_pages_free
, p.sys_total_pages as os_total_pages
FROM pg_sysconf_size() p, vm_free_pages() v
$pgsysconf$
LANGUAGE SQL STABLE;

COMMENT ON FUNCTION pgsysconf()
IS 'pgsysconf is DEPRECATED, use pg_sysconf_size and vm_free_pages instead.';

COMMENT ON FUNCTION pgsysconf_pretty()
IS 'pgsysconf_pretty is DEPRECATED, use pg_sysconf_size and vm_free_pages with pg_size_pretty';
