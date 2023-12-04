CREATE OR REPLACE FUNCTION
vm_cachestat(
  IN relation regclass
, IN fork_name text
, IN block_num bigint
, IN nblocks bigint
, OUT vm_pagesize bigint
, OUT nr_cache bigint
, OUT nr_dirty bigint
, OUT nr_writeback bigint
, OUT nr_evicted bigint
, OUT nr_recently_evicted bigint
, OUT pg_pagesize bigint
)
RETURNS record
AS '$libdir/pgfincore'
LANGUAGE C;

CREATE OR REPLACE FUNCTION
vm_cachestat(
  IN relation regclass
, IN block_num bigint
, IN nblocks bigint
, OUT vm_pagesize bigint
, OUT nr_cache bigint
, OUT nr_dirty bigint
, OUT nr_writeback bigint
, OUT nr_evicted bigint
, OUT nr_recently_evicted bigint
, OUT pg_pagesize bigint
)
RETURNS record
AS $vm_cachestat$
SELECT * FROM vm_cachestat($1,'main',$2,$3)
$vm_cachestat$
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION
vm_cachestat(
  IN relation regclass
, OUT vm_pagesize bigint
, OUT nr_cache bigint
, OUT nr_dirty bigint
, OUT nr_writeback bigint
, OUT nr_evicted bigint
, OUT nr_recently_evicted bigint
, OUT pg_pagesize bigint
)
RETURNS record
AS $vm_cachestat$
SELECT * FROM vm_cachestat($1,'main',NULL,NULL)
$vm_cachestat$
LANGUAGE SQL;
