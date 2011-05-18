/*
*  PgFincore
*  This project let you see and mainpulate objects in the FS page cache
*  Copyright (C) 2009-2011 Cédric Villemain
*/

/* { POSIX stuff */
#define _XOPEN_SOURCE 600 /* fadvise */

#include <stdlib.h> /* exit, calloc, free */
#include <sys/stat.h> /* stat, fstat */
#include <sys/types.h> /* size_t, mincore */
#include <unistd.h> /* sysconf, close */
#include <sys/mman.h> /* mmap, mincore */
#include <fcntl.h>  /* fadvise */
/* } */

/* { PostgreSQL stuff */
#include "postgres.h" /* general Postgres declarations */
#include "access/heapam.h" /* relation_open */
#include "catalog/catalog.h" /* relpath */
#include "catalog/namespace.h" /* makeRangeVarFromNameList */
#include "utils/builtins.h" /* textToQualifiedNameList */
#include "utils/rel.h" /* Relation */
#include "utils/varbit.h" /* bitstring datatype */
#include "funcapi.h" /* SRF */
#include "catalog/pg_type.h" /* TEXTOID for tuple_desc */
#include "storage/fd.h"

#ifdef PG_VERSION_NUM
#define PG_MAJOR_VERSION (PG_VERSION_NUM / 100)
#else
#error "Unknown postgresql version"
#endif

#if PG_MAJOR_VERSION != 804 && PG_MAJOR_VERSION != 900 && PG_MAJOR_VERSION != 901
#error "Unsupported postgresql version"
#endif

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif
/* } */

#define PGSYSCONF_COLS  		3
#define PGFADVISE_COLS			4
#define PGFADVISE_LOADER_COLS	5
#define PGFINCORE_COLS  		7

#define PGF_WILLNEED	10
#define PGF_DONTNEED	20
#define PGF_NORMAL		30
#define PGF_SEQUENTIAL	40
#define PGF_RANDOM		50

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
	int64			rel_os_pages;
	int64			pages_mem;
	int64			group_mem;
	VarBit			*databit;
	Relation 		rel;			/* the relation */
	unsigned int	segcount;		/* the segment current number */
	char			*relationpath;	/* the relation path */
} pgfincore_fctx;


Datum pgsysconf(PG_FUNCTION_ARGS);

Datum 		pgfadvise(PG_FUNCTION_ARGS);
static int	pgfadvise_file(char *filename, int advice, pgfadviseStruct *pgfdv);

Datum		pgfadvise_loader(PG_FUNCTION_ARGS);
static int	pgfadvise_loader_file(char *filename,
								  bool willneed, bool dontneed,
								  VarBit *databit,
								  pgfloaderStruct *pgfloader);

Datum		pgfincore(PG_FUNCTION_ARGS);
static int	pgfincore_file(char *filename, pgfincore_fctx *fctx);

/*
 * We need to add some handler to keep the code clean
 * and support 8.4 and 9.0
 * XXX: and 8.3 ?!
 */
#if PG_MAJOR_VERSION == 804 || PG_MAJOR_VERSION == 900

static char *relpathperm(RelFileNode rnode, ForkNumber forknum);
static bool RelationUsesTempNamespace(Relation relation);

static char *relpathperm(RelFileNode rnode, ForkNumber forknum)
{
	return relpath(rnode, forknum);
}
static bool RelationUsesTempNamespace(Relation relation)
{
	return (relation->rd_istemp || relation->rd_islocaltemp);
}

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

/*
 * pgfadvise_file
 */
static int
pgfadvise_file(char *filename, int advice, pgfadviseStruct	*pgfdv)
{
	/*
	 * We work directly with the file
	 * we don't use the postgresql file handler
	 */
	struct stat	st;
	int			fd;
	int			adviceFlag;

	/*
	 * OS Page size and Free pages
	 */
	pgfdv->pageSize	= sysconf(_SC_PAGESIZE);

	/*
	 * Open and fstat file
	 * fd will be provided to posix_fadvise
	 * if there is no file, just return 1, it is expected to leave the SRF
	 */
	fd = open(filename, O_RDONLY);
	if (fd == -1)
		return 1;
	if (fstat(fd, &st) == -1)
	{
		close(fd);
		elog(ERROR, "pgfadvise: Can not stat object file : %s", filename);
		return 2;
	}

	/*
	 * the file size is used in the SRF to output the number of pages used by
	 * the segment
	 */
	pgfdv->filesize = st.st_size;
	elog(DEBUG1, "pgfadvise: working on %s of %li bytes",
		 filename, (long int) pgfdv->filesize);

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
	close(fd);

	/*
	 * OS things : Pages free
	 */
	pgfdv->pagesFree = sysconf(_SC_AVPHYS_PAGES);

	return 0;
}

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

		/*
		 * Because temp tables are not in the same directory, we fail
		 * XXX: can be fixed
		 */
		if (RelationUsesTempNamespace(fctx->rel))
		{
			relation_close(fctx->rel, AccessShareLock);
			elog(NOTICE,
			     "pgfadvise: does not work with temporary tables.");
			pfree(fctx);
			SRF_RETURN_DONE(funcctx);
		}

		/* we get the common part of the filename of each segment of a relation */
		fctx->relationpath = relpathperm(fctx->rel->rd_node,
		                                 forkname_to_number( text_to_cstring(forkName) ));

		/* Here we keep track of current action in all calls */
		fctx->advice = advice;

		/* segcount is used to get the next segment of the current relation */
		fctx->segcount = 0;

		/* And finally we keep track of our initialization */
		elog(DEBUG1, "pgfadvise: init done for %s", fctx->relationpath);
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
	 * We work directly with the file
	 * we don't use the postgresql file handler
	 */
	struct stat	st;
	int			fd;

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
	 * Open and fstat file
	 * fd will be provided to posix_fadvise
	 * if there is no file, just return 1, it is expected to leave the SRF
	 */
	fd = open(filename, O_RDONLY);
	if (fd == -1)
		return 1;
	if (fstat(fd, &st) == -1)
	{
		close(fd);
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
	close(fd);

	/*
	 * OS things : Pages free
	 */
	pgfloader->pagesFree = sysconf(_SC_AVPHYS_PAGES);

	return 0;
}

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
	VarBit    *databit		= PG_GETARG_VARBIT_P(5);

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

	/* initialize nulls array to build the tuple */
	memset(nulls, 0, sizeof(nulls));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* open the current relation in accessShareLock */
	rel = relation_open(relOid, AccessShareLock);

	/*
	 * Because temp tables are not in the same directory, we fail
	 * XXX: can be fixed
	 */
	if (RelationUsesTempNamespace(rel))
	{
		relation_close(rel, AccessShareLock);
		elog(NOTICE,
		     "pgfincore does not work with temporary tables.");
		PG_RETURN_VOID();
	}

	/* we get the common part of the filename of each segment of a relation */
	relationpath = relpathperm(rel->rd_node,
	                           forkname_to_number(text_to_cstring(forkName))
	                          );
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
pgfincore_file(char *filename, pgfincore_fctx *fctx)
{
	int		flag=1;

	int		len, slen, bitlen;
	bits8	*r;
	bits8	x = 0;
	register int64 pageIndex;

	/*
	 * We work directly with the file
	 * we don't use the postgresql file handler
	 */
	struct stat	  st;
	int			  fd;
	void 		  *pa  = (char *) 0;
	unsigned char *vec = (unsigned char *) 0;

	/*
	 * OS Page size
	 */
	int64 pageSize  = sysconf(_SC_PAGESIZE);

	/*
	 * Initialize counters
	 */
	fctx->pages_mem = 0;
	fctx->group_mem = 0;
	fctx->pages_mem = 0;

	/*
	* Open, fstat file
	*/
	fd = open(filename, O_RDONLY);
	if (fd == -1)
		return 1;

	if (fstat(fd, &st) == -1)
	{
		close(fd);
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
		fctx->rel_os_pages = (st.st_size+pageSize-1)/pageSize;

		/* TODO We need to split mmap size to be sure (?) to be able to mmap */
		pa = mmap(NULL, st.st_size, PROT_NONE, MAP_SHARED, fd, 0);
		if (pa == MAP_FAILED)
		{
			close(fd);
			elog(ERROR, "Can not mmap object file : %s, errno = %i,%s\nThis error can happen if there is not enought space in memory to do the projection. Please mail cedric@villemain.org with '[pgfincore] ENOMEM' as subject.",
			     filename, errno, strerror(errno));
			return 3;
		}

		/* Prepare our vector containing all blocks information */
		vec = calloc(1, (st.st_size+pageSize-1)/pageSize);
		if ((void *)0 == vec)
		{
			munmap(pa, st.st_size);
			close(fd);
			elog(ERROR, "Can not calloc object file : %s",
			     filename);
			return 4;
		}

		/* Affect vec with mincore */
		if (mincore(pa, st.st_size, vec) != 0)
		{
			free(vec);
			munmap(pa, st.st_size);
			close(fd);
			elog(ERROR, "mincore(%p, %lld, %p): %s\n",
			     pa, (int64)st.st_size, vec, strerror(errno));
			return 5;
		}

		/*
		 * prepare the bit string
		 */
		slen = st.st_size/pageSize;
		bitlen = slen;
		len = VARBITTOTALLEN(bitlen);
		/*
		 * set to 0 so that *r is always initialised and string is zero-padded
		 * XXX: do we need to free that ?
		 */
		fctx->databit = (VarBit *) palloc0(len);
		SET_VARSIZE(fctx->databit, len);
		VARBITLEN(fctx->databit) = Min(bitlen, bitlen);

		r = VARBITS(fctx->databit);
		x = HIGHBIT;

		/* handle the results */
		for (pageIndex = 0; pageIndex <= fctx->rel_os_pages; pageIndex++)
		{
			// block in memory
			if (vec[pageIndex] & 1)
			{
				fctx->pages_mem++;
				*r |= x;
				elog (DEBUG5, "in memory blocks : %lld / %lld",
				      pageIndex, fctx->rel_os_pages);

				/* we flag to detect contigous blocks in the same state */
				if (flag)
					fctx->group_mem++;
				flag = 0;
			}
			else
				flag=1;

			x >>= 1;
			if (x == 0)
			{
				x = HIGHBIT;
				r++;
			}
		}
	}
	elog(DEBUG1, "pgfincore %s: %lld of %lld block in linux cache, %lld groups",
	     filename, fctx->pages_mem,  fctx->rel_os_pages, fctx->group_mem);

	/*
	 * free and close
	 */
	free(vec);
	munmap(pa, st.st_size);
	close(fd);
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
	FuncCallContext *funcctx;
	pgfincore_fctx  *fctx;

	int 			result;
	char			filename[MAXPGPATH];

	/*
	 * OS Page size and Free pages
	 */
	int64 pageSize	= sysconf(_SC_PAGESIZE);
	int64 pagesFree	= sysconf(_SC_AVPHYS_PAGES);

	/*
	 * Postgresql stuff to return a tuple
	 */
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	Datum		values[PGFINCORE_COLS];
	bool		nulls[PGFINCORE_COLS];
	tupdesc = CreateTemplateTupleDesc(PGFINCORE_COLS, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "relpath",		TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "os_page_size",	INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "rel_os_pages",	INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "pages_mem",	INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "group_mem",	INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "os_pages_free",INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 7, "data",		  VARBITOID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		Oid			  relOid    = PG_GETARG_OID(0);
		text		  *forkName = PG_GETARG_TEXT_P(1);

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* allocate memory for user context */
		fctx = (pgfincore_fctx *) palloc(sizeof(pgfincore_fctx));

		/* open the current relation, accessShareLock */
		// TODO use try_relation_open instead ?
		fctx->rel = relation_open(relOid, AccessShareLock);

		/*
		 * Because temp tables are not in the same directory, we fail
		 * XXX: can be fixed
		 */
		if (RelationUsesTempNamespace(fctx->rel))
		{
			relation_close(fctx->rel, AccessShareLock);
			elog(NOTICE,
			     "pgfincore does not work with temporary tables.");
			pfree(fctx);
			SRF_RETURN_DONE(funcctx);
		}

		/* we get the common part of the filename of each segment of a relation */
		fctx->relationpath = relpathperm(fctx->rel->rd_node,
		                                 forkname_to_number( text_to_cstring(forkName) ));

		/* segcount is used to get the next segment of the current relation */
		fctx->segcount = 0;

		/* And finally we keep track of our initialization */
		elog(DEBUG1, "pgfincore: init done for %s", fctx->relationpath);
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

	result = pgfincore_file(filename, fctx);

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

	values[0] = CStringGetTextDatum(filename);
	values[1] = Int64GetDatum(pageSize);
	values[2] = Int64GetDatum(fctx->rel_os_pages);
	values[3] = Int64GetDatum(fctx->pages_mem);
	values[4] = Int64GetDatum(fctx->group_mem);
	values[5] = Int64GetDatum( pagesFree );
	values[6] = VarBitPGetDatum(fctx->databit);
	memset(nulls, 0, sizeof(nulls));
	tuple = heap_form_tuple(tupdesc, values, nulls);

	/* prepare the number of the next segment */
	fctx->segcount++;

	/* Ok, return results, and go for next call */
	SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
}
