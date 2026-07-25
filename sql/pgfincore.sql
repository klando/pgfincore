CREATE EXTENSION pgfincore;

--
-- test SYSCONF
--
select from pgsysconf();
select from pgsysconf_pretty();

--
-- make a temp table to use below
--
CREATE TEMP TABLE test AS SELECT generate_series(1,256) as a;

--
-- this is not perfect testing but it is hard to predict what the OS will do
-- for *sure*
--

--
-- test fadvise_loader
--
select from pgfadvise_loader('test', 0, true, true, B'1010');
select from pgfadvise_loader('test', 0, true, false, B'1010');
select from pgfadvise_loader('test', 0, false, true, B'1010');
select from pgfadvise_loader('test', 0, false, false, B'1010');
-- must not fail on empty databit input
select from pgfadvise_loader('test', 0, false, false, B'');
-- ERROR on NULL databit input
select from pgfadvise_loader('test', 0, false, false, NULL);

--
-- test pgfincore
--
select from pgfincore('test', true);
select from pgfincore('test');

--
-- test DONTNEED, WILLNEED
--
select from pgfadvise_willneed('test');
select from pgfadvise_dontneed('test');

--
-- test PGFADVISE flags
--
select from pgfadvise_sequential('test');
select from pgfadvise_random('test');
select from pgfadvise_normal('test');

--
-- tests drawers
--
select NULL || pgfincore_drawer(databit) from pgfincore('test','main',true);

--
-- System/PostgreSQL info
--
-- assume most systems have same defaults
-- it's possible to add another output file
-- to manage larger PostgreSQL and or system page size.
SELECT pg_page_size();
SELECT pg_segment_size();
SELECT vm_page_size();
-- no output for the following:
SELECT FROM vm_physical_pages();
SELECT FROM vm_available_pages();

