/*
*  PgFincore
*  This project let you see and mainpulate objects in the FS page cache
*  Copyright (C) 2009-2011 Cédric Villemain
*/

/* POSIX stuff */
#define _XOPEN_SOURCE 600 /* fadvise */

#include <fcntl.h>  /* fadvise */
#include <stdlib.h> /* exit, calloc, free */
#include <sys/stat.h> /* stat, fstat */
#include <sys/types.h> /* size_t, mincore */
#include <sys/mman.h> /* mmap, mincore */
#include <unistd.h> /* sysconf, close */
/* } */

/* PostgreSQL stuff */
#include "postgres.h" /* general Postgres declarations */

#include "access/heapam.h" /* relation_open */
#include "catalog/catalog.h" /* relpath */
#include "catalog/namespace.h" /* makeRangeVarFromNameList */
#include "catalog/pg_type.h" /* TEXTOID for tuple_desc */
#include "funcapi.h" /* SRF */
#include "utils/builtins.h" /* textToQualifiedNameList */
#include "utils/rel.h" /* Relation */
#include "utils/varbit.h" /* bitstring datatype */
#include "storage/fd.h"
#include "access/htup_details.h" /* heap_form_tuple */
#include "common/relpath.h" /* relpathbackend */

#ifdef PG_VERSION_NUM
#if PG_VERSION_NUM < 100000
#error "Unsupported postgresql version"
#endif
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
/*
 * pgfadvise_fctx structure is needed
 * to keep track of relation path, segment number, ...
 */
typedef struct
{
	int				advice;			/* the posix_fadvise advice */
	TupleDesc		tupd;			/* the tuple descriptor */
	Relation		rel;			/* the relation */
	unsigned int	segcount;		/* the segment current number */
	char 			*relationpath;	/* the relation path */
} pgfadvise_fctx;

/*
 * pgfadvise structure is needed
 * to return values
 */
typedef struct
{
	size_t			pageSize;	/* os page size */
	size_t			pagesFree;	/* free page cache */
	size_t			filesize;	/* the filesize */
} pgfadviseStruct;

/*
 * pgfloader structure is needed
 * to return values
 */
typedef struct
{
	size_t	pageSize;		/* os page size */
	size_t	pagesFree;		/* free page cache */
	size_t	pagesLoaded;	/* pages loaded */
	size_t	pagesUnloaded;	/* pages unloaded  */
} pgfloaderStruct;

/*
 * pgfincore_fctx structure is needed
 * to keep track of relation path, segment number, ...
 */
typedef struct
{
	bool			getvector;		/* output varbit data ? */
	TupleDesc		tupd;			/* the tuple descriptor */
	Relation 		rel;			/* the relation */
	unsigned int	segcount;		/* the segment current number */
	char			*relationpath;	/* the relation path */
} pgfincore_fctx;

/*
 * pgfadvise_loader_struct structure is needed
 * to keep track of relation path, segment number, ...
 */
typedef struct
{
	size_t	pageSize;		/* os page size */
	size_t	pagesFree;		/* free page cache */
	size_t	rel_os_pages;
	size_t	pages_mem;
	size_t	group_mem;
	size_t	pages_dirty;
	size_t	group_dirty;
	VarBit	*databit;
} pgfincoreStruct;

Datum pgsysconf(PG_FUNCTION_ARGS);

Datum 		pgfadvise(PG_FUNCTION_ARGS);
static int	pgfadvise_file(char *filename, int advice, pgfadviseStruct *pgfdv);

Datum		pgfadvise_loader(PG_FUNCTION_ARGS);
static int	pgfadvise_loader_file(char *filename,
								  bool willneed, bool dontneed,
								  VarBit *databit,
								  pgfloaderStruct *pgfloader);

Datum		pgfincore(PG_FUNCTION_ARGS);
static int	pgfincore_file(char *filename, pgfincoreStruct *pgfncr);

Datum		pgfincore_drawer(PG_FUNCTION_ARGS);

#if PG_VERSION_NUM < 160000
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
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	Datum		values[PGSYSCONF_COLS];
	bool		nulls[PGSYSCONF_COLS];

	/* initialize nulls array to build the tuple */
	memset(nulls, 0, sizeof(nulls));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "pgsysconf: return type must be a row type");

	/* Page size */
	values[0] = Int64GetDatum(sysconf(_SC_PAGESIZE));

	/* free page in memory */
	values[1] = Int64GetDatum(sysconf(_SC_AVPHYS_PAGES));

	/* total memory */
	values[2] = Int64GetDatum(sysconf(_SC_PHYS_PAGES));

	/* Build and return the result tuple. */
	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM( HeapTupleGetDatum(tuple) );
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
	FILE	*fp;
	int	fd;
	struct stat st;
	int	    adviceFlag;

	/*
	 * OS Page size and Free pages
	 */
	pgfdv->pageSize	= sysconf(_SC_PAGESIZE);

	/*
	 * Fopen and fstat file
	 * fd will be provided to posix_fadvise
	 * if there is no file, just return 1, it is expected to leave the SRF
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
pgfadvise_file(char *filename, int advice, pgfadviseStruct	*pgfdv)
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
	pgfadvise_fctx  *fctx;

	/* our structure use to return values */
	pgfadviseStruct	*pgfdv;

	/* our return value, 0 for success */
	int 			result;

	/* The file we are working on */
	char			filename[MAXPGPATH];

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;

		Oid			  relOid    = PG_GETARG_OID(0);
		text		  *forkName = PG_GETARG_TEXT_P(1);
		int			  advice	= PG_GETARG_INT32(2);

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
		// TODO use try_relation_open instead ?
		fctx->rel = relation_open(relOid, AccessShareLock);

		/* we get the common part of the filename of each segment of a relation */
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
	 * If we are still looking the first segment
	 * relationpath should not be suffixed
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
	* When we have work with all segments of the current relation
	* We exit from the SRF
	* Else we build and return the tuple for this segment
	*/
	if (result)
	{
		elog(DEBUG1, "pgfadvise: closing %s", fctx->relationpath);
		relation_close(fctx->rel, AccessShareLock);
		pfree(fctx);
		SRF_RETURN_DONE(funcctx);
	}
	else {
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
		values[0] = CStringGetTextDatum( filename );
		/* os page size */
		values[1] = Int64GetDatum( (int64) pgfdv->pageSize );
		/* number of pages used by segment */
		values[2] = Int64GetDatum( (int64) ((pgfdv->filesize+pgfdv->pageSize-1)/pgfdv->pageSize) );
		/* free page cache */
		values[3] = Int64GetDatum( (int64) pgfdv->pagesFree );
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
	bits8	*sp;
	int		bitlen;
	bits8	x;
	int		i, k;

	/*
	 * We use the AllocateFile(2) provided by PostgreSQL.  We're going to
	 * close it ourselves even if PostgreSQL close it anyway at transaction
	 * end.
	 */
	FILE	*fp;
	int	fd;
	struct stat st;

	/*
	 * OS things : Page size
	 */
	pgfloader->pageSize = sysconf(_SC_PAGESIZE);

	/*
	 * we count the action we perform
	 * both are theorical : we don't know if the page was or not in memory
	 * when we call posix_fadvise
	 */
	pgfloader->pagesLoaded		= 0;
	pgfloader->pagesUnloaded	= 0;

	/*
	 * Fopen and fstat file
	 * fd will be provided to posix_fadvise
	 * if there is no file, just return 1, it is expected to leave the SRF
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
		/*  Is this bit set ? */
		for (k = 0; k < BITS_PER_BYTE; k++)
		{
			if (IS_HIGHBIT_SET(x))
			{
				if (willneed)
				{
					(void) posix_fadvise(fd,
					                     ((i+k) * pgfloader->pageSize),
					                     pgfloader->pageSize,
					                     POSIX_FADV_WILLNEED);
					pgfloader->pagesLoaded++;
				}
			}
			else if (dontneed)
			{
				(void) posix_fadvise(fd,
				                     ((i+k) * pgfloader->pageSize),
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
	Oid       relOid        = PG_GETARG_OID(0);
	text      *forkName     = PG_GETARG_TEXT_P(1);
	int       segmentNumber = PG_GETARG_INT32(2);
	bool      willneed      = PG_GETARG_BOOL(3);
	bool      dontneed      = PG_GETARG_BOOL(4);
	VarBit    *databit;

	/* our structure use to return values */
	pgfloaderStruct	*pgfloader;

	Relation  rel;
	char      *relationpath;
	char      filename[MAXPGPATH];

	/* our return value, 0 for success */
	int 			result;

	/*
	 * Postgresql stuff to return a tuple
	 */
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	Datum		values[PGFADVISE_LOADER_COLS];
	bool		nulls[PGFADVISE_LOADER_COLS];

	if (PG_ARGISNULL(5))
		elog(ERROR, "pgfadvise_loader: databit argument shouldn't be NULL");

        databit		= PG_GETARG_VARBIT_P(5);

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
	 * If we are looking the first segment,
	 * relationpath should not be suffixed
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
	 * We don't need the relation anymore
	 * the only purpose was to get a consistent filename
	 * (if file disappear, an error is logged)
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
	values[0] = CStringGetTextDatum( filename );
	/* os page size */
	values[1] = Int64GetDatum( pgfloader->pageSize );
	/* free page cache */
	values[2] = Int64GetDatum( pgfloader->pagesFree );
	/* pages loaded */
	values[3] = Int64GetDatum( pgfloader->pagesLoaded );
	/* pages unloaded  */
	values[4] = Int64GetDatum( pgfloader->pagesUnloaded );

	/* Build and return the result tuple. */
	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM( HeapTupleGetDatum(tuple) );
}

/*
 * pgfincore_file handle the mmaping, mincore process (and access file, etc.)
 */
static int
pgfincore_file(char *filename, pgfincoreStruct *pgfncr)
{
	int		flag=1;
	int		flag_dirty=1;

	int		len, bitlen;
	bits8	*r;
	bits8	x = 0;
	register int64 pageIndex;


	/*
	 * We use the AllocateFile(2) provided by PostgreSQL.  We're going to
	 * close it ourselves even if PostgreSQL close it anyway at transaction
	 * end.
	 */
	FILE	*fp;
	int	fd;
	struct stat st;

#ifndef HAVE_FINCORE
	void 		  *pa  = (char *) 0;
#endif
	unsigned char *vec = (unsigned char *) 0;

	/*
	 * OS Page size
	 */
	pgfncr->pageSize  = sysconf(_SC_PAGESIZE);

	/*
	 * Initialize counters
	 */
	pgfncr->pages_mem		= 0;
	pgfncr->group_mem		= 0;
	pgfncr->pages_dirty		= 0;
	pgfncr->group_dirty		= 0;
	pgfncr->rel_os_pages	= 0;

	/*
	 * Fopen and fstat file
	 * fd will be provided to posix_fadvise
	 * if there is no file, just return 1, it is expected to leave the SRF
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
	* if file ok
	* then process
	*/
	if (st.st_size != 0)
	{
		/* number of pages in the current file */
		pgfncr->rel_os_pages = (st.st_size+pgfncr->pageSize-1)/pgfncr->pageSize;

#ifndef HAVE_FINCORE
		pa = mmap(NULL, st.st_size, PROT_NONE, MAP_SHARED, fd, 0);
		if (pa == MAP_FAILED)
		{
			int	save_errno = errno;
			FreeFile(fp);
			elog(ERROR, "Can not mmap object file : %s, errno = %i,%s\nThis error can happen if there is not enought space in memory to do the projection. Please mail cedric@villemain.org with '[pgfincore] ENOMEM' as subject.",
			     filename, save_errno, strerror(save_errno));
			return 3;
		}
#endif

		/* Prepare our vector containing all blocks information */
		vec = calloc(1, (st.st_size+pgfncr->pageSize-1)/pgfncr->pageSize);
		if ((void *)0 == vec)
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
			int save_errno = errno;
			munmap(pa, st.st_size);
			elog(ERROR, "mincore(%p, %lld, %p): %s\n",
			     pa, (long long int)st.st_size, vec, strerror(save_errno));
#else
		/* Affect vec with fincore */
		if (fincore(fd, 0, st.st_size, vec) != 0)
		{
			int save_errno = errno;
			elog(ERROR, "fincore(%u, 0, %lld, %p): %s\n",
			     fd, (long long int)st.st_size, vec, strerror(save_errno));
#endif
			free(vec);
			FreeFile(fp);
			return 5;
		}

		/*
		 * prepare the bit string
		 */
		bitlen = FINCORE_BITS * ((st.st_size+pgfncr->pageSize-1)/pgfncr->pageSize);
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
			// block in memory
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
						/* we flag to detect contigous blocks in the same state */
						if (flag_dirty)
							pgfncr->group_dirty++;
						flag_dirty = 0;
					}
					else
						flag_dirty = 1;
				}
				elog (DEBUG5, "in memory blocks : %lld / %lld",
				      (long long int) pageIndex, (long long int) pgfncr->rel_os_pages);

				/* we flag to detect contigous blocks in the same state */
				if (flag)
					pgfncr->group_mem++;
				flag = 0;
			}
			else
				flag=1;


			x >>= FINCORE_BITS;
			if (x == 0)
			{
				x = HIGHBIT;
				r++;
			}
		}
	}
	elog(DEBUG1, "pgfincore %s: %lld of %lld block in linux cache, %lld groups",
	     filename, (long long int) pgfncr->pages_mem,  (long long int) pgfncr->rel_os_pages, (long long int) pgfncr->group_mem);

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
	pgfincore_fctx  *fctx;

	/* our structure use to return values */
	pgfincoreStruct	*pgfncr;

	/* our return value, 0 for success */
	int 			result;

	/* The file we are working on */
	char			filename[MAXPGPATH];

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;

		Oid		relOid    = PG_GETARG_OID(0);
		text	*forkName = PG_GETARG_TEXT_P(1);
		bool	getvector = PG_GETARG_BOOL(2);

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
		// TODO use try_relation_open instead ?
		fctx->rel = relation_open(relOid, AccessShareLock);

		/* we get the common part of the filename of each segment of a relation */
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
	 * If we are still looking the first segment
	 * relationpath should not be suffixed
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
	* When we have work with all segment of the current relation, test success
	* We exit from the SRF
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
			nulls[7]  = true;
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
        char *result,
             *r;
	int  len,i,k;
	VarBit *databit;
	bits8 *sp;
	bits8 x;

	if (PG_ARGISNULL(0))
		elog(ERROR, "pgfincore_drawer: databit argument shouldn't be NULL");

        databit	= PG_GETARG_VARBIT_P(0);

	len =  VARBITLEN(databit);
	result = (char *) palloc((len/FINCORE_BITS) + 1);
	sp = VARBITS(databit);
	r = result;

	for (i = 0; i <= len - BITS_PER_BYTE; i += BITS_PER_BYTE, sp++)
	{
		x = *sp;
		/*  Is this bit set ? */
		for (k = 0; k < (BITS_PER_BYTE/FINCORE_BITS); k++)
		{
		  char out = ' ';
			if (IS_HIGHBIT_SET(x))
			  out = '.' ;
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
		for (k = i; k < (len/FINCORE_BITS); k++)
		{
		        char out = ' ';
			if (IS_HIGHBIT_SET(x))
			  out = '.' ;
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

#include "miscadmin.h"			/* GetUserId() */
#include "utils/acl.h"			/* AclResult */
#include "utils/lsyscache.h"	/* get_rel_name */
#if PG_VERSION_NUM >= 150000
#include "utils/wait_event.h"	/* pgstat_report_wait_end */
#endif
#include "storage/bufmgr.h"     /* RelationGetNumberOfBlocksInFork */

#include "pgfincore.h"

/*
 * sysconf and PostgreSQL informations
 */
PG_FUNCTION_INFO_V1(pg_page_size);
PG_FUNCTION_INFO_V1(pg_segment_size);
PG_FUNCTION_INFO_V1(vm_available_pages);
PG_FUNCTION_INFO_V1(vm_page_size);
PG_FUNCTION_INFO_V1(vm_physical_pages);
PG_FUNCTION_INFO_V1(vm_relation_cachestat);

#define VM_RELATION_CACHESTAT_COLS	8
/* cachestat syscall is linux only */
#if defined(__linux__)
#define MAY_HAVE_CACHESTAT
#	ifndef __NR_cachestat
#		if defined(__alpha__)
#		define __NR_cachestat 561
#		else
#		define __NR_cachestat 451
#		endif
#	endif
#endif

typedef struct
{
	uint64  off;
	uint64  len;
} cachestat_range;

typedef struct
{
	/* Number of cached pages */
	uint64	nr_cache;
	/* Number of dirty pages */
	uint64	nr_dirty;
	/* Number of pages marked for writeback. */
	uint64	nr_writeback;
	/* Number of pages evicted from the cache. */
	uint64	nr_evicted;
	/*
	 * Number of recently evicted pages. A page is recently evicted if its
	 * last eviction was recent enough that its reentry to the cache would
	 * indicate that it is actively being used by the system, and that there
	 * is memory pressure on the system.
	 */
	uint64	nr_recently_evicted;
} cachestat;

typedef struct
{
	int64	offset;
	int64	length;
	int64	range;
	int		flags;
#define OFFSET_IS_MAGIC	0x01
#define LENGTH_IS_MAGIC	0x02
} blockParams;

typedef struct
{
	size_t	off;
	size_t	len;
	int		flags;
} fileParams;

typedef void *(FileSyscallFunction)(unsigned int, fileParams);

static dlist_head *RelationSyscall(FileSyscallFunction *FileSyscall,
								   Relation rel, ForkNumber forkNumber,
								   blockParams *bp);

static long SegmentSyscall(FileSyscallFunction *FileSyscall,
						   dlist_head *statList, SMgrRelation reln,
						   ForkNumber forkNum, BlockNumber segno,
						   blockParams bp);

static void *FileCachestat(unsigned int fd, fileParams fp);

static long sys_cachestat(unsigned int fd, cachestat_range *cstat_range,
						  void *stat, unsigned int flags);

/* PostgreSQL Page size */
static inline size_t pg_PageSize()
{
	return BLCKSZ;
}
Datum
pg_page_size(PG_FUNCTION_ARGS)
{
	PG_RETURN_UINT64(pg_PageSize());
}

/* PostgreSQL Segment size */
Datum
pg_segment_size(PG_FUNCTION_ARGS)
{
	PG_RETURN_UINT32(pg_SegmentSize());
}

/* System Page size */
static size_t     _sc_pagesize    = 0;
static inline size_t vm_PageSize()
{
	if (_sc_pagesize == 0)
		_sc_pagesize = sysconf(_SC_PAGESIZE);
	return ((size_t) _sc_pagesize);
}
Datum
vm_page_size(PG_FUNCTION_ARGS)
{
	PG_RETURN_UINT64(vm_PageSize());
}

/* System number of available pages */
static inline size_t vm_AvPhysPages()
{
	return (size_t) sysconf(_SC_AVPHYS_PAGES);
}
Datum
vm_available_pages(PG_FUNCTION_ARGS)
{
	PG_RETURN_UINT64(vm_AvPhysPages());
}

/* System number of physical pages */
static size_t 		_sc_phys_pages  = 0;
static inline size_t vm_PhysPages()
{
	if (_sc_phys_pages == 0)
		_sc_phys_pages = sysconf(_SC_PHYS_PAGES);
	return _sc_phys_pages;
}
Datum
vm_physical_pages(PG_FUNCTION_ARGS)
{
	PG_RETURN_UINT64(vm_PhysPages());
}

typedef struct
{
	BlockNumber	offset;
	BlockNumber	range;
	void		*stat;
	dlist_node	node;
} statTuple;

/*
 * Implement linux 6.5 cachestat
 * vm_relation_cachestat returns number of pages as viewed by the system.
 * math must be done to get the sizing in postgresql pages instead.
 */
Datum
vm_relation_cachestat(PG_FUNCTION_ARGS)
{
	ReturnSetInfo	*rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	Oid				relOid;
	text			*forkName;
	Relation		rel;
	blockParams		*relBlockParams	= palloc(sizeof(blockParams));
	dlist_head		*statList	= (dlist_head *) palloc(sizeof(dlist_head));
	dlist_iter		iter;

	/* Basic sanity checking. */
	relOid					= pgArgRelation(fcinfo, 0);
	forkName				= pgArgForkName(fcinfo, 1);
	/*
	 * Validate block numbers, handle nulls.
	 * Correct values will be checked later.
	 */
	relBlockParams->offset	= pgArgBlockOffset(fcinfo, 2);
	relBlockParams->length	= pgArgBlockLength(fcinfo, 3);
	relBlockParams->range	= pgArgBlockRange(fcinfo, 4);

	InitMaterializedSRF(fcinfo, 0);

	/* Open relation and check privileges. */
	rel						= relation_open(relOid, AccessShareLock);
	checkAcl(relOid, rel);
	checkForkExists(relOid, rel, forkName);

	/* Get the stats */
	statList = RelationSyscall(FileCachestat, rel,
							   forkname_to_number(text_to_cstring(forkName)),
							   relBlockParams);
	pfree(forkName);
	pfree(relBlockParams);

	/* Close relation, release lock. */
	relation_close(rel, AccessShareLock);

	dlist_reverse_foreach(iter, statList)
	{
		Datum		values[VM_RELATION_CACHESTAT_COLS];
		bool		nulls[VM_RELATION_CACHESTAT_COLS] = {0};
		int			col = 0;
		statTuple	*tuple = dlist_container(statTuple, node, iter.cur);
		cachestat	*cstat = (cachestat *) tuple->stat;

		/* blockOff */
		values[col++] = Int64GetDatum(tuple->offset);
		/* blockLen */
		values[col++] = Int64GetDatum(tuple->range);
		/* total number of pages examinated */
		values[col++] = Int64GetDatum(tuple->range * BLCKSZ / vm_PageSize());
		/* Number of cached pages */
		values[col++] = Int64GetDatum(cstat->nr_cache);
		/* Number of dirty pages */
		values[col++] = Int64GetDatum(cstat->nr_dirty);
		/* Number of pages marked for writeback. */
		values[col++] = Int64GetDatum(cstat->nr_writeback);
		/* Number of pages evicted from the cache. */
		values[col++] = Int64GetDatum(cstat->nr_evicted);
		/*
		* Number of recently evicted pages. A page is recently evicted if its
		* last eviction was recent enough that its reentry to the cache would
		* indicate that it is actively being used by the system, and that there
		* is memory pressure on the system.
		*/
		values[col++] = Int64GetDatum(cstat->nr_recently_evicted);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		pfree(cstat);
		pfree(tuple);
	}
	pfree(statList);
	return (Datum) 0;
}

/*
 * Inline functions for Relation* functions
 */
static inline BlockNumber GetSegnoOfBlock(BlockNumber blockNum)
{
	return blockNum / RELSEG_SIZE;
}

static inline BlockNumber GetSeekEndOrSeekNBlocks(BlockNumber blockNum, BlockNumber nblocks)
{
	if (GetSegnoOfBlock(blockNum) != GetSegnoOfBlock(blockNum + nblocks - 1))
		nblocks = RELSEG_SIZE - (blockNum % ((BlockNumber) RELSEG_SIZE));

	return nblocks;
}

static inline void setBlockParamsOK(int64 relblocks, blockParams *bp)
{
	bp->flags = 0;
	/* full relation ? */
	if (bp->offset < 0)
	{
		bp->flags	= OFFSET_IS_MAGIC | LENGTH_IS_MAGIC;
		bp->offset	= 0;
		bp->length	= relblocks;
	}
	/* start too far ? */
	if (bp->offset > relblocks)
	{
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("start block %lu is greater than number of blocks in relation (%lu)",
					   bp->offset, relblocks),
				 errhint("setting start block to %lu", relblocks -1)));
		bp->offset = relblocks - 1;
	}
	/* only up to the end of segment maching bp.offset ? */
	if (bp->length <= 0)
	{
		bp->flags	|= LENGTH_IS_MAGIC;
		/*
		 * if offset and relblocks are not the same segment then we go from
		 * offset to end of a full segment, otherwise stop earlier.
		 */
		if ((bp->offset / RELSEG_SIZE) == (relblocks / RELSEG_SIZE))
			bp->length = relblocks - bp->offset;
		else
			bp->length = RELSEG_SIZE - (bp->offset % ((BlockNumber) RELSEG_SIZE));
	}
	/* end too far ? */
	if (bp->offset + bp->length > relblocks + 1)
	{
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("number of blocks (%lu) will go after end of relation (%lu)",
					   bp->length, relblocks),
				errhint("setting number of blocks to %lu",
						relblocks - bp->offset)));
		bp->length = relblocks - bp->offset;
	}
}

/*
 * RelationSyscall executes a syscall on all or part of a relation.
 * The function takes care of each relevant segment.
 *
 * In order to get a call of the full relation:
 * pass blockNum=InvalidBlockNumber
 * (in this case length is ignored)
 * And in order to execute from blockNun to the end of 'its' segment:
 * pass length=InvalidBlockNumber or 0
 * It is a bit annoying from user point of view as offset is used to find the
 * relevant segment: calling on all "segment 2" requires the user to pass
 * the offset matching with the first page of the "segment 2":
 *  - segment N start block = (N * RELSEG_SIZE)
 *  - with default build it is: segment N start block is N * 131072
 *  - segment 0 start block is 131072 * 0
 *  - segment 1 start block is 131072 * 1
 *  - segment 2 start block is 131072 * 2 ...
 *
 * An alternative solution is to use SegmentCachestat() which works on a single
 * segment.
 */
static dlist_head *
RelationSyscall(FileSyscallFunction FileSyscall, Relation rel,
				ForkNumber forkNum, blockParams *relbp)
{
	blockParams		bp;
	dlist_head		*statList = (dlist_head *) palloc(sizeof(dlist_head));
	SMgrRelation	reln = RelationGetSmgr(rel);
	/*
	 * we need the relblocks to loop over the segments
	 * this is convenient to use "range" lookup
	 * an alternative solution consists in looping
	 * until OpenTransientFile() returns false...
	 */
	BlockNumber		relblocks = RelationGetNumberOfBlocksInFork(rel, forkNum);

	Assert((io_direct_flags & IO_DIRECT_DATA) == 0);
	/*
	 * now check and adjust values if needed
	 * (can't go over end of file, start after end, etc.)
	 * blockParams will be updated to safe values matching the *request*.
	 * this is distinct from blockParams which is really pass down to
	 * SegmentSyscall
	 */
	setBlockParamsOK(relblocks, relbp);

	bp.flags	= relbp->flags;
	bp.range	= relbp->range;

	dlist_init(statList);

	/*
	 * Issue as few requests as possible; have to split at segment boundaries
	 * though, since those are actually separate files.
	 */
	while (relbp->length > 0)
	{
		long		rc;
		BlockNumber	segment = GetSegnoOfBlock(relbp->offset);

		bp.length	= GetSeekEndOrSeekNBlocks(relbp->offset, relbp->length);
		bp.offset	= relbp->offset % pg_SegmentSize();

		CHECK_FOR_INTERRUPTS();
		rc = SegmentSyscall(FileSyscall, statList, reln, forkNum, segment, bp);
		/* ERROR out if any error */
		if (rc)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("SegmentSyscall returns: %m")));

		relbp->length	-= bp.length;
		relbp->offset	+= bp.length;
	}
	return statList;
}

/*
 * SegmentSyscall calls the syscall on a relation's segment. Caller is
 * responsible for checking input values (offset/length) as they should match
 * segment range.
 *
 * Pay attention, blockOff is the starting block in the current segment, not
 * in the "relation".
 *
 * if flags & LENGTH_IS_MAGIC and length <= range
 * then we pass len=0 to the syscall
 */
static long
SegmentSyscall(FileSyscallFunction FileSyscall, dlist_head *statList,
			   SMgrRelation reln, ForkNumber forkNum, BlockNumber segno,
			   blockParams bp)
{
	int			fd;
	long		rc = 0;
	int			nbytes		= BLCKSZ * bp.length;
	fileParams	fp;
	fp.flags = bp.flags;

	Assert((io_direct_flags & IO_DIRECT_DATA) == 0);

	pgstat_report_wait_start(PG_WAIT_IO);
	fd = getFileDescriptor(reln, forkNum, segno);

	/*
	 * here is the most convenient place to loop on smaller calls to collect
	 * details because we have the file open already.
	 * len and range are in bytes.
	 */
	fp.off		= BLCKSZ * bp.offset;
	fp.len		= BLCKSZ * bp.range;
	while (nbytes > (BLCKSZ * bp.range))
	{
		void		*stat;
		statTuple	*stuple = (statTuple *) palloc0(sizeof(statTuple));

#ifdef DEBUG
		elog(DEBUG1, "segno: %d offset: %lu length: %lu", segno, fp.off, fp.len);
#endif
		stat = FileSyscall(fd, fp);

		/*
		 * add block offset and len to the stuple
		 * it's always BLCKSZ aligned.
		 */
		stuple->offset	= (segno * pg_SegmentSize()) + (fp.off / BLCKSZ);
		stuple->range	= bp.range;
		stuple->stat	= stat;

		dlist_push_head(statList, &stuple->node);

		nbytes	-= BLCKSZ * bp.range;
		fp.off	+= BLCKSZ * bp.range;
	}
	/*
	 * tail loop for remaining blocks OR for special "full file"
	 */
	if (nbytes > 0)
	{
		void		*stat;
		statTuple	*stuple = (statTuple *) palloc0(sizeof(statTuple));

		if (fp.flags & LENGTH_IS_MAGIC)
			fp.len = 0;
		else
			fp.len = nbytes;
#ifdef DEBUG
		elog(DEBUG1, "segno: %d offset: %lu length: %lu", segno, fp.off, fp.len);
#endif
		stat = FileSyscall(fd, fp);

		/*
		 * add block offset and len to the stuple
		 * it's always BLCKSZ aligned.
		 */
		stuple->offset	= (segno * pg_SegmentSize()) + (fp.off / BLCKSZ);
		stuple->range	= nbytes / BLCKSZ;
		stuple->stat	= stat;

		dlist_push_head(statList, &stuple->node);
	}
	closeFileDescriptor(fd, segno);
	pgstat_report_wait_end();
	return rc;
}

/*
 * FileCachestat collects kernel cache stats of all or a portion of a file.
 */
static void *
FileCachestat(unsigned int fd, fileParams fp)
{
	cachestat		*cstat;
#if defined(MAY_HAVE_CACHESTAT)
	long			rc;
	cachestat_range	cstat_range = {(uint64) fp.off, (uint64) fp.len};
	cstat = (cachestat *) palloc0(sizeof(cachestat));

	rc = sys_cachestat(fd, &cstat_range, cstat, 0);
	if (rc == -1)
	{
		if (errno == ENOSYS)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("sys_cachestat is not available: %m"),
					 errhint("linux 6.5 minimum is required!")));
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("sys_cachestat returns: %m")));
	}
#else
	ereport(WARNING,
			errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cachestat is supported only on linux"),
					errhint("see functions based on mincore instead on others UNIX-like systems"));
#endif  /* MAY_HAVE_CACHESTAT */

	return cstat;
}

/*
 * Implement linux 6.5 sys_cachestat
 *
 * `off` and `len` must be non-negative integers. If `len` > 0,
 * the queried range is [`off`, `off` + `len`]. If `len` == 0,
 * we will query in the range from `off` to the end of the file.
 * flags (the last param) is unused and must be 0, it's distinct from fp.flags!
 */
static long
sys_cachestat(unsigned int fd, cachestat_range *cstat_range, void *stat,
			  unsigned int flags)
{
	return syscall(__NR_cachestat, fd, cstat_range, (cachestat *) stat, flags);
}
