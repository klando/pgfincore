/*
*  PgFincore
*  This project let you see and mainpulate objects in the FS page cache
*  Copyright (C) 2009-2011 Cédric Villemain
*/

/* POSIX stuff */
#define _XOPEN_SOURCE 600		/* fadvise */

#include <fcntl.h>				/* fadvise */
#include <stdlib.h>				/* exit, calloc, free */
#include <sys/stat.h>			/* stat, fstat */
#include <sys/types.h>			/* size_t, mincore */
#include <sys/mman.h>			/* mmap, mincore, cachestat */
#include <unistd.h>				/* sysconf, close */
/* } */

/* PostgreSQL stuff */
#include "postgres.h"			/* general Postgres declarations */

#include "access/heapam.h"		/* relation_open */
#include "catalog/catalog.h"	/* relpath */
#include "catalog/namespace.h"	/* makeRangeVarFromNameList */
#include "catalog/pg_type.h"	/* TEXTOID for tuple_desc */
#include "funcapi.h"			/* SRF */
#include "miscadmin.h"			/* GetUserId() */
#include "utils/acl.h"			/* AclResult */
#include "utils/builtins.h"		/* textToQualifiedNameList */
#include "utils/lsyscache.h"	/* get_rel_name */
#if PG_MAJOR_VERSION < 1600
#include "pgstat.h"				/* pgstat_report_wait_end */
#else
#include "utils/wait_event.h"	/* pgstat_report_wait_end */
#endif
#include "utils/rel.h"			/* Relation */
#include "utils/varbit.h"		/* bitstring datatype */
#include "storage/bufmgr.h"	/* RelationGetNumberOfBlocksInFork */
#include "storage/fd.h"			/* FileAcces */
#include "access/htup_details.h"	/* heap_form_tuple */
#include "common/relpath.h"		/* relpathbackend */
#ifdef PG_VERSION_NUM
#define PG_MAJOR_VERSION (PG_VERSION_NUM / 100)
#else
#error "Unknown postgresql version"
#endif

#if PG_VERSION_NUM < 90300
#error "Unsupported postgresql version"
#endif

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

#define PGSYSCONF_COLS  		3
#define PGFADVISE_COLS			4
#define PGFADVISE_LOADER_COLS	5
#define PGFINCORE_COLS  		10

#define PGF_WILLNEED	10
#define PGF_DONTNEED	20
#define PGF_NORMAL		30
#define PGF_SEQUENTIAL	40
#define PGF_RANDOM		50

#define FINCORE_PRESENT 0x1
#define FINCORE_DIRTY   0x2
#ifndef HAVE_FINCORE
#define FINCORE_BITS    1
#else
#define FINCORE_BITS    2
#endif

/* CACHESTAT */
#define ALLOW_CACHESTAT
#define VM_CACHESTAT_COLS	7

#if defined(__linux__)
/* cachestat syscall is linux only */
#define MAY_HAVE_CACHESTAT	1
#ifndef __NR_cachestat
#if defined(__alpha__)
#define __NR_cachestat 561
#else
#define __NR_cachestat 451
#endif							/* __alpha__ */
#endif							/* __NR_cachestat */
struct cachestat_range
{
	__u64		off;
	__u64		len;
};

struct cachestat
{
	/* Number of cached pages */
	__u64		nr_cache;
	/* Number of dirty pages */
	__u64		nr_dirty;
	/* Number of pages marked for writeback. */
	__u64		nr_writeback;
	/* Number of pages evicted from the cache. */
	__u64		nr_evicted;

	/*
	 * Number of recently evicted pages. A page is recently evicted if its
	 * last eviction was recent enough that its reentry to the cache would
	 * indicate that it is actively being used by the system, and that there
	 * is memory pressure on the system.
	 */
	__u64		nr_recently_evicted;
};
#endif /* __linux__ */

static char	*_mdfd_segpath(SMgrRelation reln, ForkNumber forkNum,
						   BlockNumber segno);

PG_FUNCTION_INFO_V1(vm_cachestat);
Datum		vm_cachestat(PG_FUNCTION_ARGS);
static long	RelationCachestat(Relation rel, ForkNumber forkNumber,
							  BlockNumber blockNum, BlockNumber nblocks,
							  struct cachestat *cstatsum);
static long	SegmentCachestat(SMgrRelation reln, ForkNumber forkNum,
							 BlockNumber segno, BlockNumber blockNum,
							 BlockNumber nblocks, struct cachestat *cstat);
static long	FileCachestat(unsigned int fd, struct cachestat_range *cstat_range,
						  struct cachestat *cstat, uint32 wait_event_info);
static long	sys_cachestat(unsigned int fd, struct cachestat_range *cstat_range,
						  struct cachestat *cstat, unsigned int flags);

/*
 * pgfadvise_fctx structure is needed
 * to keep track of relation path, segment number, ...
 */
typedef struct
{
	int			advice;			/* the posix_fadvise advice */
	TupleDesc	tupd;			/* the tuple descriptor */
	Relation	rel;			/* the relation */
	unsigned int segcount;		/* the segment current number */
	char	   *relationpath;	/* the relation path */
} pgfadvise_fctx;

/*
 * pgfadvise structure is needed
 * to return values
 */
typedef struct
{
	size_t		pageSize;		/* os page size */
	size_t		pagesFree;		/* free page cache */
	size_t		filesize;		/* the filesize */
} pgfadviseStruct;

/*
 * pgfloader structure is needed
 * to return values
 */
typedef struct
{
	size_t		pageSize;		/* os page size */
	size_t		pagesFree;		/* free page cache */
	size_t		pagesLoaded;	/* pages loaded */
	size_t		pagesUnloaded;	/* pages unloaded  */
} pgfloaderStruct;

/*
 * pgfincore_fctx structure is needed
 * to keep track of relation path, segment number, ...
 */
typedef struct
{
	bool		getvector;		/* output varbit data ? */
	TupleDesc	tupd;			/* the tuple descriptor */
	Relation	rel;			/* the relation */
	unsigned int segcount;		/* the segment current number */
	char	   *relationpath;	/* the relation path */
} pgfincore_fctx;

/*
 * pgfadvise_loader_struct structure is needed
 * to keep track of relation path, segment number, ...
 */
typedef struct
{
	size_t		pageSize;		/* os page size */
	size_t		pagesFree;		/* free page cache */
	size_t		rel_os_pages;
	size_t		pages_mem;
	size_t		group_mem;
	size_t		pages_dirty;
	size_t		group_dirty;
	VarBit	   *databit;
} pgfincoreStruct;

Datum		pgsysconf(PG_FUNCTION_ARGS);

Datum		pgfadvise(PG_FUNCTION_ARGS);
static int	pgfadvise_file(char *filename, int advice, pgfadviseStruct *pgfdv);

Datum		pgfadvise_loader(PG_FUNCTION_ARGS);
static int	pgfadvise_loader_file(char *filename,
								  bool willneed, bool dontneed,
								  VarBit *databit,
								  pgfloaderStruct *pgfloader);

Datum		pgfincore(PG_FUNCTION_ARGS);
static int	pgfincore_file(char *filename, pgfincoreStruct *pgfncr);

Datum		pgfincore_drawer(PG_FUNCTION_ARGS);

#if PG_MAJOR_VERSION < 1600
#define relpathpg(rel, forkName) \
        relpathbackend((rel)->rd_node, (rel)->rd_backend, (forkname_to_number(text_to_cstring(forkName))))
#else
#define relpathpg(rel, forkName) \
        relpathbackend((rel)->rd_locator, (rel)->rd_backend, (forkname_to_number(text_to_cstring(forkName))))
#endif

/*
 * pgsysconf
 * just output the actual system value for
 * _SC_PAGESIZE     --> Page Size
 * _SC_AVPHYS_PAGES --> Free page in memory
 * _SC_PHYS_PAGES   --> Total memory
 *
 */
PG_FUNCTION_INFO_V1(pgsysconf);
Datum
pgsysconf(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[PGSYSCONF_COLS] = {0};
	bool		nulls[PGSYSCONF_COLS] = {0};
	int			col = 0;

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				errcode(ERRCODE_DATATYPE_MISMATCH),
				errmsg("return type must be a row type"));

	/* Page size */
	values[col++] = Int64GetDatum((off_t) sysconf(_SC_PAGESIZE));
	/* free page in memory */
	values[col++] = Int64GetDatum((off_t) sysconf(_SC_AVPHYS_PAGES));
	/* total memory */
	values[col++] = Int64GetDatum((off_t) sysconf(_SC_PHYS_PAGES));

	/* Build and return the result tuple. */
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

#if defined(USE_POSIX_FADVISE)
/*
 * pgfadvise_file
 */
static int
pgfadvise_file(char *filename, int advice, pgfadviseStruct *pgfdv)
{
	/*
	 * We use the AllocateFile(2) provided by PostgreSQL.  We're going to
	 * close it ourselves even if PostgreSQL close it anyway at transaction
	 * end.
	 */
	FILE	   *fp;
	int			fd;
	struct stat st;
	int			adviceFlag;

	/*
	 * OS Page size and Free pages
	 */
	pgfdv->pageSize = sysconf(_SC_PAGESIZE);

	/*
	 * Fopen and fstat file fd will be provided to posix_fadvise if there is
	 * no file, just return 1, it is expected to leave the SRF
	 */
	fp = AllocateFile(filename, "rb");
	if (fp == NULL)
		return 1;

	fd = fileno(fp);
	if (fstat(fd, &st) == -1)
	{
		FreeFile(fp);
		elog(ERROR, "pgfadvise: Can not stat object file : %s", filename);
		return 2;
	}

	/*
	 * the file size is used in the SRF to output the number of pages used by
	 * the segment
	 */
	pgfdv->filesize = st.st_size;
	elog(DEBUG1, "pgfadvise: working on %s of %lld bytes",
		 filename, (long long int) pgfdv->filesize);

	/* FADVISE_WILLNEED */
	if (advice == PGF_WILLNEED)
	{
		adviceFlag = POSIX_FADV_WILLNEED;
		elog(DEBUG1, "pgfadvise: setting advice POSIX_FADV_WILLNEED");
	}
	/* FADVISE_DONTNEED */
	else if (advice == PGF_DONTNEED)
	{
		adviceFlag = POSIX_FADV_DONTNEED;
		elog(DEBUG1, "pgfadvise: setting advice POSIX_FADV_DONTNEED");

	}
	/* POSIX_FADV_NORMAL */
	else if (advice == PGF_NORMAL)
	{
		adviceFlag = POSIX_FADV_NORMAL;
		elog(DEBUG1, "pgfadvise: setting advice POSIX_FADV_NORMAL");

	}
	/* POSIX_FADV_SEQUENTIAL */
	else if (advice == PGF_SEQUENTIAL)
	{
		adviceFlag = POSIX_FADV_SEQUENTIAL;
		elog(DEBUG1, "pgfadvise: setting advice POSIX_FADV_SEQUENTIAL");

	}
	/* POSIX_FADV_RANDOM */
	else if (advice == PGF_RANDOM)
	{
		adviceFlag = POSIX_FADV_RANDOM;
		elog(DEBUG1, "pgfadvise: setting advice POSIX_FADV_RANDOM");

	}
	else
	{
		elog(ERROR, "pgfadvise: invalid advice: %d", advice);
		return 2;
	}

	/*
	 * Call posix_fadvise with the relevant advice on the file descriptor
	 */
	posix_fadvise(fd, 0, 0, adviceFlag);

	/* close the file */
	FreeFile(fp);

	/*
	 * OS things : Pages free
	 */
	pgfdv->pagesFree = sysconf(_SC_AVPHYS_PAGES);

	return 0;
}
#else
static int
pgfadvise_file(char *filename, int advice, pgfadviseStruct *pgfdv)
{
	elog(ERROR, "POSIX_FADVISE UNSUPPORTED on your platform");
	return 9;
}
#endif

/*
 * pgfadvise is a function that handle the process to have a sharelock
 * on the relation and to walk the segments.
 * for each segment it call the posix_fadvise with the required flag
 * parameter
 */
PG_FUNCTION_INFO_V1(pgfadvise);
Datum
pgfadvise(PG_FUNCTION_ARGS)
{
	/* SRF Stuff */
	FuncCallContext *funcctx;
	pgfadvise_fctx *fctx;

	/* our structure use to return values */
	pgfadviseStruct *pgfdv;

	/* our return value, 0 for success */
	int			result;

	/* The file we are working on */
	char		filename[MAXPGPATH];

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;

		Oid			relOid = PG_GETARG_OID(0);
		text	   *forkName = PG_GETARG_TEXT_P(1);
		int			advice = PG_GETARG_INT32(2);

		/*
		 * Postgresql stuff to return a tuple
		 */
		TupleDesc	tupdesc;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* allocate memory for user context */
		fctx = (pgfadvise_fctx *) palloc(sizeof(pgfadvise_fctx));

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "pgfadvise: return type must be a row type");

		/* provide the tuple descriptor to the fonction structure */
		fctx->tupd = tupdesc;

		/* open the current relation, accessShareLock */
		/* TODO use try_relation_open instead ? */
		fctx->rel = relation_open(relOid, AccessShareLock);

		/*
		 * we get the common part of the filename of each segment of a
		 * relation
		 */
		fctx->relationpath = relpathpg(fctx->rel, forkName);

		/* Here we keep track of current action in all calls */
		fctx->advice = advice;

		/* segcount is used to get the next segment of the current relation */
		fctx->segcount = 0;

		/* And finally we keep track of our initialization */
		elog(DEBUG1, "pgfadvise: init done for %s, in fork %s",
			 fctx->relationpath, text_to_cstring(forkName));
		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	/* After the first call, we recover our context */
	funcctx = SRF_PERCALL_SETUP();
	fctx = funcctx->user_fctx;

	/*
	 * If we are still looking the first segment relationpath should not be
	 * suffixed
	 */
	if (fctx->segcount == 0)
		snprintf(filename,
				 MAXPGPATH,
				 "%s",
				 fctx->relationpath);
	else
		snprintf(filename,
				 MAXPGPATH,
				 "%s.%u",
				 fctx->relationpath,
				 fctx->segcount);

	elog(DEBUG1, "pgfadvise: about to work with %s, current advice : %d",
		 filename, fctx->advice);

	/*
	 * Call posix_fadvise with the advice, returning the structure
	 */
	pgfdv = (pgfadviseStruct *) palloc(sizeof(pgfadviseStruct));
	result = pgfadvise_file(filename, fctx->advice, pgfdv);

	/*
	 * When we have work with all segments of the current relation We exit
	 * from the SRF Else we build and return the tuple for this segment
	 */
	if (result)
	{
		elog(DEBUG1, "pgfadvise: closing %s", fctx->relationpath);
		relation_close(fctx->rel, AccessShareLock);
		pfree(fctx);
		SRF_RETURN_DONE(funcctx);
	}
	else
	{
		/*
		 * Postgresql stuff to return a tuple
		 */
		HeapTuple	tuple;
		Datum		values[PGFADVISE_COLS];
		bool		nulls[PGFADVISE_COLS];

		/* initialize nulls array to build the tuple */
		memset(nulls, 0, sizeof(nulls));

		/* prepare the number of the next segment */
		fctx->segcount++;

		/* Filename */
		values[0] = CStringGetTextDatum(filename);
		/* os page size */
		values[1] = Int64GetDatum((int64) pgfdv->pageSize);
		/* number of pages used by segment */
		values[2] = Int64GetDatum((int64) ((pgfdv->filesize + pgfdv->pageSize - 1) / pgfdv->pageSize));
		/* free page cache */
		values[3] = Int64GetDatum((int64) pgfdv->pagesFree);
		/* Build the result tuple. */
		tuple = heap_form_tuple(fctx->tupd, values, nulls);

		/* Ok, return results, and go for next call */
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
}

#if defined(USE_POSIX_FADVISE)
/*
 * pgfadvise_file
 */
static int
pgfadvise_loader_file(char *filename,
					  bool willneed, bool dontneed, VarBit *databit,
					  pgfloaderStruct *pgfloader)
{
	bits8	   *sp;
	int			bitlen;
	bits8		x;
	int			i,
				k;

	/*
	 * We use the AllocateFile(2) provided by PostgreSQL.  We're going to
	 * close it ourselves even if PostgreSQL close it anyway at transaction
	 * end.
	 */
	FILE	   *fp;
	int			fd;
	struct stat st;

	/*
	 * OS things : Page size
	 */
	pgfloader->pageSize = sysconf(_SC_PAGESIZE);

	/*
	 * we count the action we perform both are theorical : we don't know if
	 * the page was or not in memory when we call posix_fadvise
	 */
	pgfloader->pagesLoaded = 0;
	pgfloader->pagesUnloaded = 0;

	/*
	 * Fopen and fstat file fd will be provided to posix_fadvise if there is
	 * no file, just return 1, it is expected to leave the SRF
	 */
	fp = AllocateFile(filename, "rb");
	if (fp == NULL)
		return 1;

	fd = fileno(fp);
	if (fstat(fd, &st) == -1)
	{
		FreeFile(fp);
		elog(ERROR, "pgfadvise_loader: Can not stat object file: %s", filename);
		return 2;
	}

	elog(DEBUG1, "pgfadvise_loader: working on %s", filename);

	bitlen = VARBITLEN(databit);
	sp = VARBITS(databit);
	for (i = 0; i < bitlen - BITS_PER_BYTE; i += BITS_PER_BYTE, sp++)
	{
		x = *sp;
		/* Is this bit set ? */
		for (k = 0; k < BITS_PER_BYTE; k++)
		{
			if (IS_HIGHBIT_SET(x))
			{
				if (willneed)
				{
					(void) posix_fadvise(fd,
										 ((i + k) * pgfloader->pageSize),
										 pgfloader->pageSize,
										 POSIX_FADV_WILLNEED);
					pgfloader->pagesLoaded++;
				}
			}
			else if (dontneed)
			{
				(void) posix_fadvise(fd,
									 ((i + k) * pgfloader->pageSize),
									 pgfloader->pageSize,
									 POSIX_FADV_DONTNEED);
				pgfloader->pagesUnloaded++;
			}

			x <<= 1;
		}
	}

	/*
	 * XXX this copy/paste of code to finnish to walk the bits is not pretty
	 */
	if (i < bitlen)
	{
		/* print the last partial byte */
		x = *sp;
		for (k = i; k < bitlen; k++)
		{
			if (IS_HIGHBIT_SET(x))
			{
				if (willneed)
				{
					(void) posix_fadvise(fd,
										 (k * pgfloader->pageSize),
										 pgfloader->pageSize,
										 POSIX_FADV_WILLNEED);
					pgfloader->pagesLoaded++;
				}
			}
			else if (dontneed)
			{
				(void) posix_fadvise(fd,
									 (k * pgfloader->pageSize),
									 pgfloader->pageSize,
									 POSIX_FADV_DONTNEED);
				pgfloader->pagesUnloaded++;
			}
			x <<= 1;
		}
	}
	FreeFile(fp);

	/*
	 * OS things : Pages free
	 */
	pgfloader->pagesFree = sysconf(_SC_AVPHYS_PAGES);

	return 0;
}
#else
static int
pgfadvise_loader_file(char *filename,
					  bool willneed, bool dontneed, VarBit *databit,
					  pgfloaderStruct *pgfloader)
{
	elog(ERROR, "POSIX_FADVISE UNSUPPORTED on your platform");
	return 9;
}
#endif

/*
*
* pgfadv_loader to handle work with varbit map of buffer cache.
* it is actually used for loading/unloading block to/from buffer cache
*
*/
PG_FUNCTION_INFO_V1(pgfadvise_loader);
Datum
pgfadvise_loader(PG_FUNCTION_ARGS)
{
	Oid			relOid = PG_GETARG_OID(0);
	text	   *forkName = PG_GETARG_TEXT_P(1);
	int			segmentNumber = PG_GETARG_INT32(2);
	bool		willneed = PG_GETARG_BOOL(3);
	bool		dontneed = PG_GETARG_BOOL(4);
	VarBit	   *databit;

	/* our structure use to return values */
	pgfloaderStruct *pgfloader;

	Relation	rel;
	char	   *relationpath;
	char		filename[MAXPGPATH];

	/* our return value, 0 for success */
	int			result;

	/*
	 * Postgresql stuff to return a tuple
	 */
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	Datum		values[PGFADVISE_LOADER_COLS];
	bool		nulls[PGFADVISE_LOADER_COLS];

	if (PG_ARGISNULL(5))
		elog(ERROR, "pgfadvise_loader: databit argument shouldn't be NULL");

	databit = PG_GETARG_VARBIT_P(5);

	/* initialize nulls array to build the tuple */
	memset(nulls, 0, sizeof(nulls));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* open the current relation in accessShareLock */
	rel = relation_open(relOid, AccessShareLock);

	/* we get the common part of the filename of each segment of a relation */
	relationpath = relpathpg(rel, forkName);

	/*
	 * If we are looking the first segment, relationpath should not be
	 * suffixed
	 */
	if (segmentNumber == 0)
		snprintf(filename,
				 MAXPGPATH,
				 "%s",
				 relationpath);
	else
		snprintf(filename,
				 MAXPGPATH,
				 "%s.%u",
				 relationpath,
				 (int) segmentNumber);

	/*
	 * We don't need the relation anymore the only purpose was to get a
	 * consistent filename (if file disappear, an error is logged)
	 */
	relation_close(rel, AccessShareLock);

	/*
	 * Call pgfadvise_loader with the varbit
	 */
	pgfloader = (pgfloaderStruct *) palloc(sizeof(pgfloaderStruct));
	result = pgfadvise_loader_file(filename,
								   willneed, dontneed, databit,
								   pgfloader);
	if (result != 0)
		elog(ERROR, "Can't read file %s, fork(%s)",
			 filename, text_to_cstring(forkName));
	/* Filename */
	values[0] = CStringGetTextDatum(filename);
	/* os page size */
	values[1] = Int64GetDatum(pgfloader->pageSize);
	/* free page cache */
	values[2] = Int64GetDatum(pgfloader->pagesFree);
	/* pages loaded */
	values[3] = Int64GetDatum(pgfloader->pagesLoaded);
	/* pages unloaded  */
	values[4] = Int64GetDatum(pgfloader->pagesUnloaded);

	/* Build and return the result tuple. */
	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * pgfincore_file handle the mmaping, mincore process (and access file, etc.)
 */
static int
pgfincore_file(char *filename, pgfincoreStruct *pgfncr)
{
	int			flag = 1;
	int			flag_dirty = 1;

	int			len,
				bitlen;
	bits8	   *r;
	bits8		x = 0;
	register int64 pageIndex;


	/*
	 * We use the AllocateFile(2) provided by PostgreSQL.  We're going to
	 * close it ourselves even if PostgreSQL close it anyway at transaction
	 * end.
	 */
	FILE	   *fp;
	int			fd;
	struct stat st;

#ifndef HAVE_FINCORE
	void	   *pa = (char *) 0;
#endif
	unsigned char *vec = (unsigned char *) 0;

	/*
	 * OS Page size
	 */
	pgfncr->pageSize = sysconf(_SC_PAGESIZE);

	/*
	 * Initialize counters
	 */
	pgfncr->pages_mem = 0;
	pgfncr->group_mem = 0;
	pgfncr->pages_dirty = 0;
	pgfncr->group_dirty = 0;
	pgfncr->rel_os_pages = 0;

	/*
	 * Fopen and fstat file fd will be provided to posix_fadvise if there is
	 * no file, just return 1, it is expected to leave the SRF
	 */
	fp = AllocateFile(filename, "rb");
	if (fp == NULL)
		return 1;

	fd = fileno(fp);

	if (fstat(fd, &st) == -1)
	{
		FreeFile(fp);
		elog(ERROR, "Can not stat object file : %s",
			 filename);
		return 2;
	}

	/*
	 * if file ok then process
	 */
	if (st.st_size != 0)
	{
		/* number of pages in the current file */
		pgfncr->rel_os_pages = (st.st_size + pgfncr->pageSize - 1) / pgfncr->pageSize;

#ifndef HAVE_FINCORE
		pa = mmap(NULL, st.st_size, PROT_NONE, MAP_SHARED, fd, 0);
		if (pa == MAP_FAILED)
		{
			int			save_errno = errno;

			FreeFile(fp);
			elog(ERROR, "Can not mmap object file : %s, errno = %i,%s\nThis error can happen if there is not enought space in memory to do the projection. Please mail cedric@villemain.org with '[pgfincore] ENOMEM' as subject.",
				 filename, save_errno, strerror(save_errno));
			return 3;
		}
#endif

		/* Prepare our vector containing all blocks information */
		vec = calloc(1, (st.st_size + pgfncr->pageSize - 1) / pgfncr->pageSize);
		if ((void *) 0 == vec)
		{
#ifndef HAVE_FINCORE
			munmap(pa, st.st_size);
#endif
			FreeFile(fp);
			elog(ERROR, "Can not calloc object file : %s",
				 filename);
			return 4;
		}

#ifndef HAVE_FINCORE
		/* Affect vec with mincore */
		if (mincore(pa, st.st_size, vec) != 0)
		{
			int			save_errno = errno;

			munmap(pa, st.st_size);
			elog(ERROR, "mincore(%p, %lld, %p): %s\n",
				 pa, (long long int) st.st_size, vec, strerror(save_errno));
#else
		/* Affect vec with fincore */
		if (fincore(fd, 0, st.st_size, vec) != 0)
		{
			int			save_errno = errno;

			elog(ERROR, "fincore(%u, 0, %lld, %p): %s\n",
				 fd, (long long int) st.st_size, vec, strerror(save_errno));
#endif
			free(vec);
			FreeFile(fp);
			return 5;
		}

		/*
		 * prepare the bit string
		 */
		bitlen = FINCORE_BITS * ((st.st_size + pgfncr->pageSize - 1) / pgfncr->pageSize);
		len = VARBITTOTALLEN(bitlen);

		/*
		 * set to 0 so that *r is always initialised and string is zero-padded
		 * XXX: do we need to free that ?
		 */
		pgfncr->databit = (VarBit *) palloc0(len);
		SET_VARSIZE(pgfncr->databit, len);
		VARBITLEN(pgfncr->databit) = bitlen;

		r = VARBITS(pgfncr->databit);
		x = HIGHBIT;

		/* handle the results */
		for (pageIndex = 0; pageIndex <= pgfncr->rel_os_pages; pageIndex++)
		{
			/* block in memory */
			if (vec[pageIndex] & FINCORE_PRESENT)
			{
				pgfncr->pages_mem++;
				*r |= x;
				if (FINCORE_BITS > 1)
				{
					if (vec[pageIndex] & FINCORE_DIRTY)
					{
						pgfncr->pages_dirty++;
						*r |= (x >> 1);

						/*
						 * we flag to detect contigous blocks in the same
						 * state
						 */
						if (flag_dirty)
							pgfncr->group_dirty++;
						flag_dirty = 0;
					}
					else
						flag_dirty = 1;
				}
				elog(DEBUG5, "in memory blocks : %lld / %lld",
					 (long long int) pageIndex, (long long int) pgfncr->rel_os_pages);

				/* we flag to detect contigous blocks in the same state */
				if (flag)
					pgfncr->group_mem++;
				flag = 0;
			}
			else
				flag = 1;


			x >>= FINCORE_BITS;
			if (x == 0)
			{
				x = HIGHBIT;
				r++;
			}
		}
	}
	elog(DEBUG1, "pgfincore %s: %lld of %lld block in linux cache, %lld groups",
		 filename, (long long int) pgfncr->pages_mem, (long long int) pgfncr->rel_os_pages, (long long int) pgfncr->group_mem);

	/*
	 * free and close
	 */
	free(vec);
#ifndef HAVE_FINCORE
	munmap(pa, st.st_size);
#endif
	FreeFile(fp);

	/*
	 * OS things : Pages free
	 */
	pgfncr->pagesFree = sysconf(_SC_AVPHYS_PAGES);

	return 0;
}

/*
 * pgfincore is a function that handle the process to have a sharelock
 * on the relation and to walk the segments.
 * for each segment it call the appropriate function depending on 'action'
 * parameter
 */
PG_FUNCTION_INFO_V1(pgfincore);
Datum
pgfincore(PG_FUNCTION_ARGS)
{
	/* SRF Stuff */
	FuncCallContext *funcctx;
	pgfincore_fctx *fctx;

	/* our structure use to return values */
	pgfincoreStruct *pgfncr;

	/* our return value, 0 for success */
	int			result;

	/* The file we are working on */
	char		filename[MAXPGPATH];

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;

		Oid			relOid = PG_GETARG_OID(0);
		text	   *forkName = PG_GETARG_TEXT_P(1);
		bool		getvector = PG_GETARG_BOOL(2);

		/*
		 * Postgresql stuff to return a tuple
		 */
		TupleDesc	tupdesc;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* allocate memory for user context */
		fctx = (pgfincore_fctx *) palloc(sizeof(pgfincore_fctx));

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "pgfadvise: return type must be a row type");

		/* provide the tuple descriptor to the fonction structure */
		fctx->tupd = tupdesc;

		/* are we going to grab and output the varbit data (can be large) */
		fctx->getvector = getvector;

		/* open the current relation, accessShareLock */
		/* TODO use try_relation_open instead ? */
		fctx->rel = relation_open(relOid, AccessShareLock);

		/*
		 * we get the common part of the filename of each segment of a
		 * relation
		 */
		fctx->relationpath = relpathpg(fctx->rel, forkName);

		/* segcount is used to get the next segment of the current relation */
		fctx->segcount = 0;

		/* And finally we keep track of our initialization */
		elog(DEBUG1, "pgfincore: init done for %s, in fork %s",
			 fctx->relationpath, text_to_cstring(forkName));
		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	/* After the first call, we recover our context */
	funcctx = SRF_PERCALL_SETUP();
	fctx = funcctx->user_fctx;

	/*
	 * If we are still looking the first segment relationpath should not be
	 * suffixed
	 */
	if (fctx->segcount == 0)
		snprintf(filename,
				 MAXPGPATH,
				 "%s",
				 fctx->relationpath);
	else
		snprintf(filename,
				 MAXPGPATH,
				 "%s.%u",
				 fctx->relationpath,
				 fctx->segcount);

	elog(DEBUG1, "pgfincore: about to work with %s", filename);

	/*
	 * Call pgfincore with the advice, returning the structure
	 */
	pgfncr = (pgfincoreStruct *) palloc(sizeof(pgfincoreStruct));
	result = pgfincore_file(filename, pgfncr);

	/*
	 * When we have work with all segment of the current relation, test
	 * success We exit from the SRF
	 */
	if (result)
	{
		elog(DEBUG1, "pgfincore: closing %s", fctx->relationpath);
		relation_close(fctx->rel, AccessShareLock);
		pfree(fctx);
		SRF_RETURN_DONE(funcctx);
	}
	else
	{
		/*
		 * Postgresql stuff to return a tuple
		 */
		HeapTuple	tuple;
		Datum		values[PGFINCORE_COLS];
		bool		nulls[PGFINCORE_COLS];

		/* initialize nulls array to build the tuple */
		memset(nulls, 0, sizeof(nulls));

		/* Filename */
		values[0] = CStringGetTextDatum(filename);
		/* Segment Number */
		values[1] = Int32GetDatum(fctx->segcount);
		/* os page size */
		values[2] = Int64GetDatum(pgfncr->pageSize);
		/* number of pages used by segment */
		values[3] = Int64GetDatum(pgfncr->rel_os_pages);
		/* number of pages in OS cache */
		values[4] = Int64GetDatum(pgfncr->pages_mem);
		/* number of group of contigous page in os cache */
		values[5] = Int64GetDatum(pgfncr->group_mem);
		/* free page cache */
		values[6] = Int64GetDatum(pgfncr->pagesFree);
		/* the map of the file with bit set for in os cache page */
		if (fctx->getvector && pgfncr->rel_os_pages)
		{
			values[7] = VarBitPGetDatum(pgfncr->databit);
		}
		else
		{
			nulls[7] = true;
			values[7] = (Datum) NULL;
		}
		/* number of pages dirty in OS cache */
		values[8] = Int64GetDatum(pgfncr->pages_dirty);
		/* number of group of contigous dirty pages in os cache */
		values[9] = Int64GetDatum(pgfncr->group_dirty);
		/* Build the result tuple. */
		tuple = heap_form_tuple(fctx->tupd, values, nulls);

		/* prepare the number of the next segment */
		fctx->segcount++;

		/* Ok, return results, and go for next call */
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
}

/*
 * pgfincore_drawer A very naive renderer. (for testing)
 */
PG_FUNCTION_INFO_V1(pgfincore_drawer);
Datum
pgfincore_drawer(PG_FUNCTION_ARGS)
{
	char	   *result,
			   *r;
	int			len,
				i,
				k;
	VarBit	   *databit;
	bits8	   *sp;
	bits8		x;

	if (PG_ARGISNULL(0))
		elog(ERROR, "pgfincore_drawer: databit argument shouldn't be NULL");

	databit = PG_GETARG_VARBIT_P(0);

	len = VARBITLEN(databit);
	result = (char *) palloc((len / FINCORE_BITS) + 1);
	sp = VARBITS(databit);
	r = result;

	for (i = 0; i <= len - BITS_PER_BYTE; i += BITS_PER_BYTE, sp++)
	{
		x = *sp;
		/* Is this bit set ? */
		for (k = 0; k < (BITS_PER_BYTE / FINCORE_BITS); k++)
		{
			char		out = ' ';

			if (IS_HIGHBIT_SET(x))
				out = '.';
			x <<= 1;
			if (FINCORE_BITS > 1)
			{
				if (IS_HIGHBIT_SET(x))
					out = '*';
				x <<= 1;
			}
			*r++ = out;
		}
	}
	if (i < len)
	{
		/* print the last partial byte */
		x = *sp;
		for (k = i; k < (len / FINCORE_BITS); k++)
		{
			char		out = ' ';

			if (IS_HIGHBIT_SET(x))
				out = '.';
			x <<= 1;
			if (FINCORE_BITS > 1)
			{
				if (IS_HIGHBIT_SET(x))
					out = '*';
				x <<= 1;
			}
			*r++ = out;
		}
	}

	*r = '\0';
	PG_RETURN_CSTRING(result);
}

/*
 * Return the filename for the specified segment of the relation. The
 * returned string is palloc'd.
 *
 * IMPORTED from PostgreSQL source-code as-is
 */
static char *
_mdfd_segpath(SMgrRelation reln, ForkNumber forkNum, BlockNumber segno)
{
	char	   *path,
			   *fullpath;

#if PG_MAJOR_VERSION < 1600
	path = relpath(reln->smgr_rnode, forkNum);
#else
	path = relpath(reln->smgr_rlocator, forkNum);
#endif

	if (segno > 0)
	{
		fullpath = psprintf("%s.%u", path, segno);
		pfree(path);
	}
	else
		fullpath = path;

	return fullpath;
}

/*
 * Implement linux 6.5 cachestat
 * vm_cachestat returns number of pages as viewed by the system
 * math must be done to get the sizing in postgresql pages instead.
 * XXX: make it work with windows
 */
Datum
vm_cachestat(PG_FUNCTION_ARGS)
{
	AclResult	aclresult;
	int			blockNum;
	Oid			relOid;
	text	   *forkName;
	ForkNumber	forkNumber;
	char	   *forkString;
	int			nblocks;
	Relation	rel;
	struct cachestat cstatsum = {0};
	long		ret;

	TupleDesc	tupdesc;
	int			col = 0;
	Datum		values[VM_CACHESTAT_COLS] = {0};
	bool		nulls[VM_CACHESTAT_COLS] = {0};

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				errcode(ERRCODE_DATATYPE_MISMATCH),
				errmsg("return type must be a row type"),
				errhint("please report a bug if you get this message: function is not correctly defined"));

	/* Basic sanity checking. */
	if (PG_ARGISNULL(0))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("relation cannot be NULL"),
				errhint("check parameters"));
	relOid = PG_GETARG_OID(0);

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("relation fork cannot be NULL"),
				errhint("check parameters"));
	forkName = PG_GETARG_TEXT_PP(1);
	forkString = text_to_cstring(forkName);
	forkNumber = forkname_to_number(forkString);

	/* Open relation and check privileges. */
	rel = relation_open(relOid, AccessShareLock);
	aclresult = pg_class_aclcheck(relOid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(rel->rd_rel->relkind),
					   get_rel_name(relOid));

	/* Check that the fork exists. */
	if (!smgrexists(RelationGetSmgr(rel), forkNumber))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("fork \"%s\" does not exist for the relation \"%s\"",
					   forkString, get_rel_name(relOid)),
				errhint("check parameters"));

	/* Validate block numbers, or handle nulls. */
	if (PG_ARGISNULL(2))
		blockNum = InvalidBlockNumber;
	else
	{
		blockNum = PG_GETARG_INT64(2);
		if (blockNum < 0)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("starting block number cannot be negative"),
					errhint("check parameters")
					);
	}
	if (PG_ARGISNULL(3))
		nblocks = 0;
	else
	{
		nblocks = PG_GETARG_INT64(3);
		if (nblocks < 0)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("number of blocks cannot be negative"),
					errhint("check parameters"));
	}

	/* Get the stats */
	ret = RelationCachestat(rel, forkNumber, blockNum, nblocks, &cstatsum);

	/* Close relation, release lock. */
	relation_close(rel, AccessShareLock);

	/* ERROR out if any error */
	if (ret)
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("sys_cachestat returns: %m"),
				errhint("please report a bug if you get this message"));

	/* system page size */
	values[col++] = Int64GetDatum((off_t) sysconf(_SC_PAGESIZE));
	/* Number of cached pages */
	values[col++] = Int64GetDatum(cstatsum.nr_cache);
	/* Number of dirty pages */
	values[col++] = Int64GetDatum(cstatsum.nr_dirty);
	/* Number of pages marked for writeback. */
	values[col++] = Int64GetDatum(cstatsum.nr_writeback);
	/* Number of pages evicted from the cache. */
	values[col++] = Int64GetDatum(cstatsum.nr_evicted);

	/*
	 * Number of recently evicted pages. A page is recently evicted if its
	 * last eviction was recent enough that its reentry to the cache would
	 * indicate that it is actively being used by the system, and that there
	 * is memory pressure on the system.
	 */
	values[col++] = Int64GetDatum(cstatsum.nr_recently_evicted);
	/* postgres page size */
	values[col++] = Int64GetDatum((off_t) BLCKSZ);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * RelationCachestat collects kernel cache stats of all or part of a relation.
 * The function takes care to each relevant segment:
 * The underlying sys_cachestat() allows to collect the stats of all or a region
 * of a file.
 * In order to get stats of the full relation:
 * pass blockNum=InvalidBlockNumber
 * It is a bit annoying from user point of view as blockNum is used to find the
 * relevant segment: getting stats of all "segment 2" requires the user to pass
 * the blockNum matching with the first page of the "segment 2"!
 * An alternative solution is to use SegmentCachestat() which works on a single
 * segment.
 */
static long
RelationCachestat(Relation rel, ForkNumber forkNum, BlockNumber blockNum,
				  BlockNumber nblocks, struct cachestat *cstatsum)
{
	SMgrRelation reln = RelationGetSmgr(rel);
	BlockNumber blocks = 0;
	BlockNumber relblocks = RelationGetNumberOfBlocksInFork(rel, forkNum);

	Assert((io_direct_flags & IO_DIRECT_DATA) == 0);

	/*
	 * If stats of the full relation, we need to pass {0,0} to cachestat for
	 * each segment.
	 */
	if (blockNum == InvalidBlockNumber)
	{
		blockNum = 0;
		nblocks = 0;
		blocks = relblocks;
	}

	if (nblocks == InvalidBlockNumber)
		nblocks = 0;

	if (blockNum > relblocks)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("starting block (%u) cannot be greater than number of blocks in relation (%u)",
					   blockNum, relblocks),
				errhint("check parameters"));

	if (blockNum + nblocks > relblocks)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("starting block (%u) and number of blocks (%u) cannot be greater than number of blocks in relation (%u)",
					   blockNum, nblocks, relblocks),
				errhint("check parameters"));

	if (nblocks)
		blocks = nblocks;

	/*
	 * Issue stat requests in as few requests as possible; have to split at
	 * segment boundaries though, since those are actually separate files.
	 */
	while (blocks > 0)
	{
		struct cachestat cstat = {0};
		BlockNumber nstat = blocks;
		long		rc;
		int			seglen = 0;
		int			segnum_start = blockNum / RELSEG_SIZE;
		int			segnum_end = (blockNum + blocks - 1) / RELSEG_SIZE;

		if (segnum_start != segnum_end)
			nstat = RELSEG_SIZE - (blockNum % ((BlockNumber) RELSEG_SIZE));

		if (nblocks > 0)
			seglen = nstat;

		CHECK_FOR_INTERRUPTS();
		rc = SegmentCachestat(reln, forkNum, segnum_start, blockNum, seglen, &cstat);
		if (rc)
			return rc;

		/*
		 * Here we sum the results for the relation. We might collect details
		 * but it does not cost much to execute this functions per segment by
		 * providing the relevant "start block".
		 */
		cstatsum->nr_cache += cstat.nr_cache;
		cstatsum->nr_dirty += cstat.nr_dirty;
		cstatsum->nr_writeback += cstat.nr_writeback;
		cstatsum->nr_evicted += cstat.nr_evicted;
		cstatsum->nr_recently_evicted += cstat.nr_recently_evicted;

		blocks -= nstat;
		blockNum += nstat;
	}

	elog(DEBUG1, "sum:\t\tnr_cache:%llu, nr_dirty:%llu, nr_writeback:%llu, nr_evicted:%llu, nr_recently_evicted:%llu",
		 cstatsum->nr_cache, cstatsum->nr_dirty, cstatsum->nr_writeback,
		 cstatsum->nr_evicted, cstatsum->nr_recently_evicted);

	return 0;
}

/*
 * SegmentCachestat collects kernel cache stats of a relation's segment.
 * Pay attention, blockNum is the starting block in the current segment, not
 * in the "relation".
 * Contrary to RelationCachestat(), SegmentCachestat() respects linux
 * sys_cachestat() inputs value (i.e. pass {0,0} as (blockNum,nblocks) to get
 * stats of the whole file.
 */
static long
SegmentCachestat(SMgrRelation reln, ForkNumber forkNum, BlockNumber segno,
				 BlockNumber blockNum, BlockNumber nblocks,
				 struct cachestat *cstat)
{
	int			fd;
	long		rc;
	char	   *fullpath;
	struct cachestat_range cstat_range = {
		(off_t) BLCKSZ * blockNum,
		(off_t) BLCKSZ * nblocks
	};

	Assert(segno >= 0);
	Assert(blockNum >= 0);
	Assert(nblocks >= 0);
	Assert((io_direct_flags & IO_DIRECT_DATA) == 0);

	fullpath = _mdfd_segpath(reln, forkNum, segno);
	fd = OpenTransientFile(fullpath, O_RDONLY | PG_BINARY);
	pfree(fullpath);
	if (fd < 0)
	{
		if (errno != ENOENT)
			ereport(ERROR, (errcode_for_file_access(),
						  errmsg("could not read segment %u", segno),
						  errhint("please report a bug if you get this message")));
		return false;
	}

	rc = FileCachestat(fd, &cstat_range, cstat, PG_WAIT_EXTENSION);

	if (CloseTransientFile(fd) != 0)
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not close segment %u", segno),
				 errhint("please report a bug if you get this message")));

	if (rc)
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("FileCachestat returns: %m"),
				errhint("please report a bug if you get this message"));

	elog(DEBUG1, "segment %d:\tnr_cache:%llu, nr_dirty:%llu, nr_writeback:%llu, nr_evicted:%llu, nr_recently_evicted:%llu",
		 segno, cstat->nr_cache, cstat->nr_dirty, cstat->nr_writeback,
		 cstat->nr_evicted, cstat->nr_recently_evicted);

	return rc;
}

/*
 * FileCachestat collects kernel cache stats of all or a portion of a file.
 */
static long
FileCachestat(unsigned int fd, struct cachestat_range *cstat_range,
			  struct cachestat *cstat, uint32 wait_event_info)
{
	long		rc = -1;
#if defined(MAY_HAVE_CACHESTAT)
	elog(DEBUG1, "range: off: %llu bytes, len: %llu bytes",
		 cstat_range->off, cstat_range->len);

	pgstat_report_wait_start(wait_event_info);
	rc = sys_cachestat(fd, cstat_range, cstat, 0);
	pgstat_report_wait_end();
	if (rc == -1)
	{
		if (errno == ENOSYS)
			ereport(ERROR,
					errcode(ERRCODE_INTERNAL_ERROR),
					errmsg("sys_cachestat is not available: %m"),
					errhint("linux 6.5 minimum is required!"));
		else
			ereport(ERROR,
					errcode(ERRCODE_INTERNAL_ERROR),
					errmsg("sys_cachestat returns: %m"),
					errhint("please report a bug if you get this message"));
	}
#else
	ereport(WARNING,
			errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("cachestat is supported only on linux"),
			errhint("see vm_mincore instead on others UNIX-like systems"));
#endif	/* MAY_HAVE_CACHESTAT */
	return rc;
}

/*
 * Implement linux 6.5 sys_cachestat
 */
static long
sys_cachestat(unsigned int fd,
			  struct cachestat_range *cstat_range,
			  struct cachestat *cstat, unsigned int flags)
{
	long		rc = -1;
#if defined(ALLOW_CACHESTAT)
	rc = syscall(__NR_cachestat, fd, cstat_range, cstat, flags);
#else
	ereport(ERROR,
			errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("cannot execute sys_cachestat: ALLOW_CACHESTAT is not defined"),
			errhint("build pgfincore with ALLOW_CACHESTAT defined to get access to the feature"));
#endif	/* ALLOW_CACHESTAT */
	return rc;
}
