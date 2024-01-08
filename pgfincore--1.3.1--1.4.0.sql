--
-- System/PostgreSQL info
--
CREATE FUNCTION pg_page_size() RETURNS bigint
AS '$libdir/pgfincore' LANGUAGE C
IMMUTABLE PARALLEL SAFE;
COMMENT ON FUNCTION pg_page_size()
IS 'get PostgreSQL page size in bytes';

CREATE FUNCTION pg_segment_size() RETURNS int
AS '$libdir/pgfincore' LANGUAGE C
IMMUTABLE PARALLEL SAFE;
COMMENT ON FUNCTION pg_segment_size()
IS 'get max PostgreSQL segment size in blocks';

CREATE FUNCTION vm_available_pages() RETURNS bigint
AS '$libdir/pgfincore' LANGUAGE C
VOLATILE PARALLEL SAFE;
COMMENT ON FUNCTION vm_available_pages()
IS 'get number of available pages in system memory';

CREATE FUNCTION vm_page_size() RETURNS bigint
AS '$libdir/pgfincore' LANGUAGE C
IMMUTABLE PARALLEL SAFE;
COMMENT ON FUNCTION vm_page_size()
IS 'get system page size in bytes';

CREATE FUNCTION vm_physical_pages() RETURNS bigint
AS '$libdir/pgfincore' LANGUAGE C
IMMUTABLE PARALLEL SAFE;
COMMENT ON FUNCTION vm_physical_pages()
IS 'get total number of physical pages in system memory';

