with my_table as (
  select oid
       , relfilenode
       , relname
  from pg_class
  where relname = 'pgbench_accounts'
)
, t as (
  select generate_series(1, relpages) as g
  from my_table
  join pg_class using (relname)
)
, buf as (
  select relblocknumber * 2 as bn -- Pgfincore use filesystem block size
       , usagecount as c
       , isdirty as d
  from my_table
  join pg_buffercache using (relfilenode)
  where relforknumber = 0
)
, pgf as (
  select (row_number() over (partition by c)) - 1 as bn -- pascal vs C
  	   , c
	   , NULL as d
  from (select unnest(
                      string_to_array(
			               (pgfincore(my_table.oid, true)).databit::text, NULL
			          )
               ) as c 
        from my_table ) g
)
, fb as (
   select pgf.bn as file_block_number
       	, buf.c as pgcache
      	, buf.d as pgdirty
        , pgf.c as oscache
        , pgf.d as osdirty
  from buf 
  right join pgf using (bn)
  order by 1, 2, 3
),
res as (
  select *
  from fb
)
select row_to_json(res) -- use "res" CTE if no JSON datatype (pg < 9.2)
from res;
