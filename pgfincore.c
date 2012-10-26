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

#if PG_VERSION_NUM < 80300
#error "Unsupported postgresql version"
#endif

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif
/* } */

#define PGSYSCONF_COLS  		3
#define PGFADVISE_COLS			4
#define PGFADVISE_LOADER_COLS	5
#define PGFINCORE_COLS  		8

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

/*
 * We need to add some handler to keep the code clean
 * and support 8.3, 8.4 and 9.0
 */
#if PG_MAJOR_VERSION == 803
#if defined(HAVE_DECL_POSIX_FADVISE)
#define USE_POSIX_FADVISE
#endif
char *text_to_cstring(const text *t);
text *cstring_to_text(const char *s);
text *cstring_to_text_with_len(const char *s, int len);

char *
text_to_cstring(const text *t)
{
	/* must cast away the const, unfortunately */
	text       *tunpacked = pg_detoast_datum_packed((struct varlena *) t);
	int         len = VARSIZE_ANY_EXHDR(tunpacked);
	char       *result;

	result = (char *) palloc(len + 1);
	memcpy(result, VARDATA_ANY(tunpacked), len);
	result[len] = '\0';

	if (tunpacked != t)
		pfree(tunpacked);

	return result;
}

text *
cstring_to_text_with_len(const char *s, int len)
{
	text       *result = (text *) palloc(len + VARHDRSZ);

	SET_VARSIZE(result, len + VARHDRSZ);
	memcpy(VARDATA(result), s, len);

	return result;
}

text *
cstring_to_text(const char *s)
{
	return cstring_to_text_with_len(s, strlen(s));
}

#define CStringGetTextDatum(s) PointerGetDatum(cstring_to_text(s))
#define relpathpg(rel, forkName) \
        relpath((rel)->rd_node)

#elif PG_MAJOR_VERSION == 804 || PG_MAJOR_VERSION == 900
#define relpathpg(rel, forkName) \
        relpath((rel)->rd_node, forkname_to_number(text_to_cstring(forkName)))

#else
#define relpathpg(rel, forkName) \
        relpathbackend((rel)->rd_node, (rel)->rd_backend, (forkname_to_number(text_to_cstring(forkName))))
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

	void 		  *pa  = (char *) 0;
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

		/* TODO We need to split mmap size to be sure (?) to be able to mmap */
		pa = mmap(NULL, st.st_size, PROT_NONE, MAP_SHARED, fd, 0);
		if (pa == MAP_FAILED)
		{
			FreeFile(fp);
			elog(ERROR, "Can not mmap object file : %s, errno = %i,%s\nThis error can happen if there is not enought space in memory to do the projection. Please mail cedric@villemain.org with '[pgfincore] ENOMEM' as subject.",
			     filename, errno, strerror(errno));
			return 3;
		}

		/* Prepare our vector containing all blocks information */
		vec = calloc(1, (st.st_size+pgfncr->pageSize-1)/pgfncr->pageSize);
		if ((void *)0 == vec)
		{
			munmap(pa, st.st_size);
			FreeFile(fp);
			elog(ERROR, "Can not calloc object file : %s",
			     filename);
			return 4;
		}

		/* Affect vec with mincore */
		if (mincore(pa, st.st_size, vec) != 0)
		{
			free(vec);
			munmap(pa, st.st_size);
			FreeFile(fp);
			elog(ERROR, "mincore(%p, %lld, %p): %s\n",
			     pa, (long long int)st.st_size, vec, strerror(errno));
			return 5;
		}

		/*
		 * prepare the bit string
		 */
		bitlen = (st.st_size+pgfncr->pageSize-1)/pgfncr->pageSize;
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
			if (vec[pageIndex] & 1)
			{
				pgfncr->pages_mem++;
				*r |= x;
				elog (DEBUG5, "in memory blocks : %lld / %lld",
				      (long long int) pageIndex, (long long int) pgfncr->rel_os_pages);

				/* we flag to detect contigous blocks in the same state */
				if (flag)
					pgfncr->group_mem++;
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
	     filename, (long long int) pgfncr->pages_mem,  (long long int) pgfncr->rel_os_pages, (long long int) pgfncr->group_mem);

	/*
	 * free and close
	 */
	free(vec);
	munmap(pa, st.st_size);
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
		/* Build the result tuple. */
		tuple = heap_form_tuple(fctx->tupd, values, nulls);

        /* prepare the number of the next segment */
        fctx->segcount++;

		/* Ok, return results, and go for next call */
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
}
