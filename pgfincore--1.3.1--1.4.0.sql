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

CREATE FUNCTION
vm_relation_cachestat(
  IN relation regclass
, IN fork_name text default 'main'
, IN "offset" bigint default null
, IN "length" bigint default null
, IN "range" bigint default null
, OUT block_start bigint
, OUT block_count bigint
, OUT nr_pages bigint
, OUT nr_cache bigint
, OUT nr_dirty bigint
, OUT nr_writeback bigint
, OUT nr_evicted bigint
, OUT nr_recently_evicted bigint
)
RETURNS SETOF record
AS '$libdir/pgfincore' LANGUAGE C
VOLATILE PARALLEL SAFE;

COMMENT ON FUNCTION vm_relation_cachestat(regclass, text, bigint, bigint, bigint)
IS 'Returns linux cachestat information:
 - block_start is the starting block number for the current tuple
 - block_count is the number of blocks analyzed for the current tuple
 - nr_cache is Number of cached pages
 - nr_dirty is Number of dirty pages
 - nr_writeback is Number of pages marked for writeback
 - nr_evicted is Number of pages evicted from the cache
 - nr_recently_evicted is Number of pages recently evicted from the cache
/*
 * A page is recently evicted if its last eviction was recent enough that its
 * reentry to the cache would indicate that it is actively being used by the
 * system, and that there is memory pressure on the system.
 */
- define block_num as NULL to inspect the full relation
- define nblocks as 0 or NULL to inspect only the segment of the block_num up to its end.';

