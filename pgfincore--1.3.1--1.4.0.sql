CREATE FUNCTION pg_page_size() RETURNS bigint
AS '$libdir/pgfincore' LANGUAGE C STABLE;
COMMENT ON FUNCTION pg_page_size()
IS 'Returns PostgreSQL page size in bytes';

CREATE FUNCTION pg_segment_size() RETURNS bigint
AS '$libdir/pgfincore' LANGUAGE C STABLE;
COMMENT ON FUNCTION pg_segment_size()
IS 'Returns PostgreSQL segment size in blocks';

CREATE FUNCTION vm_available_pages() RETURNS bigint
AS '$libdir/pgfincore' LANGUAGE C STABLE;
COMMENT ON FUNCTION vm_available_pages()
IS 'Returns current number of free pages in system memory';

CREATE FUNCTION vm_page_size() RETURNS bigint
AS '$libdir/pgfincore' LANGUAGE C STABLE;
COMMENT ON FUNCTION vm_page_size()
IS 'Returns system page size in bytes';

CREATE FUNCTION vm_physical_pages(OUT sys_pages_free bigint)
AS '$libdir/pgfincore' LANGUAGE C STABLE;
COMMENT ON FUNCTION vm_physical_pages()
IS 'Returns number of pages in system memory';
