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
-- tests vm_cachestat
--
-- Test bad parameters
select * from vm_cachestat(NULL, NULL, -1, -1);
select * from vm_cachestat('test', NULL, -1, -1);
select * from vm_cachestat('test', 'vm', -1, -1);
select * from vm_cachestat('test', 'main', -1, -1);
select * from vm_cachestat('test', 'main', NULL, -1);
select * from vm_cachestat('test', 'main', 10, NULL);
select * from vm_cachestat('test', 'main', 0, 10);
-- Working cases
select from pgfadvise_loader('test', 0, true, true, B'1111');
select (pgfincore('test')).pages_mem, (vm_cachestat('test', 'main', NULL, NULL)).nr_cache;
select from pgfadvise_loader('test', 0, true, true, B'1000');
select (pgfincore('test')).pages_mem, (vm_cachestat('test', 'main', NULL, NULL)).nr_cache;
select from pgfadvise_loader('test', 0, true, true, B'0000');
select (pgfincore('test')).pages_mem, (vm_cachestat('test', 'main', NULL, NULL)).nr_cache;

--
-- tests vm_fadvise
--
-- Test bad parameters
select vm_fadvise(NULL, NULL, -1, -1, 'badflag');
select vm_fadvise('test', NULL, -1, -1, 'badflag');
select vm_fadvise('test', 'vm', -1, -1, 'badflag');
select vm_fadvise('test', 'main', -1, -1, 'badflag');
select vm_fadvise('test', 'main', NULL, -1, 'badflag');
select vm_fadvise('test', 'main', 10, NULL, 'badflag');
select vm_fadvise('test', 'main', 0, 10, 'badflag');
select vm_fadvise('test', 'main', 0, 2, 'POSIX_FADV_NOREUSE');
-- Working cases
select vm_fadvise('test', 'main', 0, 2, 'POSIX_FADV_NORMAL');
select vm_fadvise('test', 'main', 0, 2, 'POSIX_FADV_SEQUENTIAL');
select vm_fadvise('test', 'main', 0, 2, 'POSIX_FADV_RANDOM');
select vm_fadvise('test', 'main', 0, 2, 'POSIX_FADV_DONTNEED');
select (pgfincore('test')).pages_mem;
select vm_fadvise('test', 'main', 0, 2, 'POSIX_FADV_WILLNEED');
select (pgfincore('test')).pages_mem;
