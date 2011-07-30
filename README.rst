===========
 PgFincore
===========

--------------------------------------------------------------
 A set of functions to manage pages in memory from PostgreSQL
--------------------------------------------------------------

DESCRIPTION
===========

With PostgreSQL, each Table or Index is splitted in segments of (usually) 1GB,
and each segment is splitted in pages in memory then in blocks for the
filesystem.

Those functions let you know which and how many disk block from a relation are
in the page cache of the operating system. It can provide the result as a VarBit
and can be stored in a table. Then using this table, it is possible to restore
the page cache state for each block of the relation, even in another node,
thanks to Streaming Replication.

Other functions are used to set a *POSIX_FADVISE* flag on the entire relation
(each segment). The more usefull are probably *WILLNEED* and *DONTNEED* which
push and pop blocks of each segments of a relation from page cache,
respectively.

Each functions are call with at least a table name or an index name (or oid)
as a parameter and walk each segment of the relation.

DOWNLOAD
========

You can grab the latest code with git:: 

   git clone git://git.postgresql.org/git/pgfincore.git
   or
   git://github.com/klando/pgfincore.git

And the project is on pgfoundry : http://pgfoundry.org/projects/pgfincore

INSTALL
=======

From source code:: 

  make clean
  make
  su
  make install

For PostgreSQL >= 9.1, log in your database and::

  mydb=# CREATE EXTENSION pgfincore;

For other release, create the functions from the sql script (it should be in
your contrib directory)::

  psql mydb -f pgfincore.sql

PgFincore is also shipped with Debian scripts to build your own package::

  aptitude install debhelper postgresql-server-dev-all postgresql-server-dev-9.1
  # or postgresql-server-dev-8.4|postgresql-server-dev-9.0
  make deb
  dpkg -i ../postgresql-9.1-pgfincore_1.0-1_amd64.deb

And if you are a *RPM* user, see: http://pgrpms.org/

EXAMPLES
========

Here are some examples of usage. If you want more details go to Documentation_

Get current state of a relation
-------------------------------

May be useful::

   cedric=# select * from pgfincore('pgbench_accounts');
         relpath       | segment | os_page_size | rel_os_pages | pages_mem | group_mem | os_pages_free | databit 
   --------------------+---------+--------------+--------------+-----------+-----------+---------------+---------
    base/11874/16447   |       0 |         4096 |       262144 |    262144 |         1 |         81016 | 
    base/11874/16447.1 |       1 |         4096 |        65726 |     65726 |         1 |         81016 | 
   (2 rows)
   
   Time: 31.563 ms

Load a table or an index in OS Page Buffer
------------------------------------------

You may want to try to keep a table or an index into the OS Page Cache, or
preload a table before your well know big query is executed (reducing the query
time).

To do so, just execute the following query::

   cedric=# select * from pgfadvise_willneed('pgbench_accounts');
         relpath       | os_page_size | rel_os_pages | os_pages_free 
   --------------------+--------------+--------------+---------------
    base/11874/16447   |         4096 |       262144 |        169138
    base/11874/16447.1 |         4096 |        65726 |        103352
   (2 rows)
    
   Time: 4462,936 ms

  * The column *os_page_size* report that page size is 4KB.
  * The column *rel_os_pages* is the number of pages of the specified file.
  * The column *os_pages_free* is the number of free pages in memory (for caching).

Snapshot and Restore the OS Page Buffer state of a table or an index (or more)
------------------------------------------------------------------------------

You may want to restore a table or an index into the OS Page Cache as it was
while you did the snapshot. For example if you have to reboot your server, then
when PostgreSQL start up the first queries might be slower because neither
PostgreSQL or the OS have pages in their respective cache about the relations
involved in those first queries.

Executing a snapshot and a restore is very simple::

   -- Snapshot
   cedric=# create table pgfincore_snapshot as
   cedric-#   select 'pgbench_accounts'::text as relname,*,now() as date_snapshot
   cedric-#   from pgfincore('pgbench_accounts',true);
   
   -- Restore
   cedric=# select * from pgfadvise_loader('pgbench_accounts', 0, true, true,
                          (select databit from  pgfincore_snapshot
                           where relname='pgbench_accounts' and segment = 0));
        relpath      | os_page_size | os_pages_free | pages_loaded | pages_unloaded 
   ------------------+--------------+---------------+--------------+----------------
    base/11874/16447 |         4096 |         80867 |       262144 |              0
   (1 row)
   
   Time: 35.349 ms

 * The column *pages_loaded* report how many pages have been read to memory
   (they may have already been in memoy)
 * The column *pages_unloaded* report how many pages have been removed from
   memory (they may not have already been in memoy);

SYNOPSIS
========

::

   pgsysconf(OUT os_page_size bigint, OUT os_pages_free bigint,
             OUT os_total_pages bigint)
     RETURNS record
    
   pgsysconf_pretty(OUT os_page_size text, OUT os_pages_free text,
                    OUT os_total_pages text)
     RETURNS record

   pgfadvise(IN relname regclass, IN fork text, IN action int,
             OUT relpath text, OUT os_page_size bigint,
             OUT rel_os_pages bigint, OUT os_pages_free bigint)
     RETURNS setof record

   pgfadvise_willneed(IN relname regclass,
                      OUT relpath text, OUT os_page_size bigint,
                      OUT rel_os_pages bigint, OUT os_pages_free bigint)
     RETURNS setof record

   pgfadvise_dontneed(IN relname regclass,
                      OUT relpath text, OUT os_page_size bigint,
                      OUT rel_os_pages bigint, OUT os_pages_free bigint)
     RETURNS setof record

   pgfadvise_normal(IN relname regclass,
                    OUT relpath text, OUT os_page_size bigint,
                    OUT rel_os_pages bigint, OUT os_pages_free bigint)
     RETURNS setof record

   pgfadvise_sequential(IN relname regclass,
                        OUT relpath text, OUT os_page_size bigint,
                        OUT rel_os_pages bigint, OUT os_pages_free bigint)
     RETURNS setof record

   pgfadvise_random(IN relname regclass,
                    OUT relpath text, OUT os_page_size bigint,
                    OUT rel_os_pages bigint, OUT os_pages_free bigint)
     RETURNS setof record

   pgfadvise_loader(IN relname regclass, IN fork text, IN segment int,
                    IN load bool, IN unload bool, IN databit varbit,
                    OUT relpath text, OUT os_page_size bigint,
                    OUT os_pages_free bigint, OUT pages_loaded bigint,
                    OUT pages_unloaded bigint)
     RETURNS setof record

   pgfadvise_loader(IN relname regclass, IN segment int,
                    IN load bool, IN unload bool, IN databit varbit,
                    OUT relpath text, OUT os_page_size bigint,
                    OUT os_pages_free bigint, OUT pages_loaded bigint,
                    OUT pages_unloaded bigint)
     RETURNS setof record

   pgfincore(IN relname regclass, IN fork text, IN getdatabit bool,
             OUT relpath text, OUT segment int, OUT os_page_size bigint,
             OUT rel_os_pages bigint, OUT pages_mem bigint,
             OUT group_mem bigint, OUT os_pages_free bigint,
             OUT databit      varbit)
     RETURNS setof record

   pgfincore(IN relname regclass, IN getdatabit bool,
             OUT relpath text, OUT segment int, OUT os_page_size bigint,
             OUT rel_os_pages bigint, OUT pages_mem bigint,
             OUT group_mem bigint, OUT os_pages_free bigint,
             OUT databit      varbit)
     RETURNS setof record

   pgfincore(IN relname regclass,
             OUT relpath text, OUT segment int, OUT os_page_size bigint,
             OUT rel_os_pages bigint, OUT pages_mem bigint,
             OUT group_mem bigint, OUT os_pages_free bigint,
             OUT databit      varbit)
     RETURNS setof record

DOCUMENTATION
=============

pgsysconf
---------

This function output size of OS blocks, number of free page in the OS Page Buffer.

::

   cedric=# select * from pgsysconf();
    os_page_size | os_pages_free | os_total_pages 
   --------------+---------------+----------------
            4096 |         80431 |        4094174

pgsysconf_pretty
----------------

The same as above, but with pretty output.

::

   cedric=# select * from pgsysconf_pretty();
    os_page_size | os_pages_free | os_total_pages 
   --------------+---------------+----------------
    4096 bytes   | 314 MB        | 16 GB

pgfadvise_WILLNEED
------------------

This function set *WILLNEED* flag on the current relation. It means that the
Operating Sytem will try to load as much pages as possible of the relation.
Main idea is to preload files on server startup, perhaps using cache hit/miss
ratio or most required relations/indexes.

::

   cedric=# select * from pgfadvise_willneed('pgbench_accounts');
         relpath       | os_page_size | rel_os_pages | os_pages_free 
   --------------------+--------------+--------------+---------------
    base/11874/16447   |         4096 |       262144 |         80650
    base/11874/16447.1 |         4096 |        65726 |         80650

pgfadvise_DONTNEED
------------------

This function set *DONTNEED* flag on the current relation. It means that the
Operating System will first unload pages of the file if it need to free some
memory. Main idea is to unload files when they are not usefull anymore (instead
of perhaps more interesting pages)

::

   cedric=# select * from pgfadvise_dontneed('pgbench_accounts');
         relpath       | os_page_size | rel_os_pages | os_pages_free
   --------------------+--------------+--------------+---------------
    base/11874/16447   |         4096 |       262144 |        342071
    base/11874/16447.1 |         4096 |        65726 |        408103


pgfadvise_NORMAL
----------------

This function set *NORMAL* flag on the current relation.

pgfadvise_SEQUENTIAL
--------------------

This function set *SEQUENTIAL* flag on the current relation.

pgfadvise_RANDOM
----------------

This function set *RANDOM* flag on the current relation.

pgfadvise_loader
----------------

This function allow to interact directly with the Page Cache.
It can be used to load and/or unload page from memory based on a varbit
representing the map of the pages to load/unload accordingly.

Work with relation pgbench_accounts, segment 0, arbitrary varbit map::

   -- Loading and Unloading
   cedric=# select * from pgfadvise_loader('pgbench_accounts', 0, true, true, B'111000');
        relpath      | os_page_size | os_pages_free | pages_loaded | pages_unloaded 
   ------------------+--------------+---------------+--------------+----------------
    base/11874/16447 |         4096 |        408376 |            3 |              3

   -- Loading
   cedric=# select * from pgfadvise_loader('pgbench_accounts', 0, true, false, B'111000');
        relpath      | os_page_size | os_pages_free | pages_loaded | pages_unloaded 
   ------------------+--------------+---------------+--------------+----------------
    base/11874/16447 |         4096 |        408370 |            3 |              0

   -- Unloading
   cedric=# select * from pgfadvise_loader('pgbench_accounts', 0, false, true, B'111000');
        relpath      | os_page_size | os_pages_free | pages_loaded | pages_unloaded 
   ------------------+--------------+---------------+--------------+----------------
    base/11874/16447 |         4096 |        408370 |            0 |              3

pgfincore
---------

This function provide information about the file system cache (page cache). 

::

   cedric=# select * from pgfincore('pgbench_accounts');
         relpath       | segment | os_page_size | rel_os_pages | pages_mem | group_mem | os_pages_free | databit 
   --------------------+---------+--------------+--------------+-----------+-----------+---------------+---------
    base/11874/16447   |       0 |         4096 |       262144 |         3 |         1 |        408444 | 
    base/11874/16447.1 |       1 |         4096 |        65726 |         0 |         0 |        408444 | 

For the specified relation it returns:

  * relpath : the relation path 
  * segment : the segment number analyzed 
  * os_page_size : the size of one page
  * rel_os_pages : the total number of pages of the relation
  * pages_mem : the total number of relation's pages in page cache.
    (not the shared buffers from PostgreSQL but the OS cache)
  * group_mem : the number of groups of adjacent pages_mem
  * os_page_free : the number of free page in the OS page cache
  * databit : the varbit map of the file, because of its size it is useless to output
    Use pgfincore('pgbench_accounts',true) to activate it.

DEBUG
=====

You can debug the PgFincore with the following error level: *DEBUG1* and
*DEBUG5*.

For example::

   set client_min_messages TO debug1; -- debug5 is only usefull to trace each block

SEE ALSO
========

2ndQuadrant, PostgreSQL Expertise, developement, training and 24x7 support:

  http://2ndQuadrant.fr


