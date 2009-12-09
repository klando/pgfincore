/*
*  PgFincore
*  This project let you see what objects are in the FS cache memory
*  Copyright (C) 2009 Cédric Villemain
*/

/* { POSIX stuff */
#include <errno.h> /* errno */
#include <fcntl.h> /* fcntl, open */
#include <stdlib.h> /* exit, calloc, free */
#include <sys/stat.h> /* stat, fstat */
#include <sys/types.h> /* size_t, mincore */
#include <unistd.h> /* sysconf, close */
#include <sys/mman.h> /* mmap, mincore */
#define _XOPEN_SOURCE 600 /* fadvise */
#include <fcntl.h>  /* fadvise */
/* } */

/* { PostgreSQL stuff */
#include "postgres.h" /* general Postgres declarations */
#include "access/heapam.h" /* relation_open */
#include "catalog/catalog.h" /* relpath */
#include "catalog/namespace.h" /* makeRangeVarFromNameList */
#include "utils/builtins.h" /* textToQualifiedNameList */
#include "utils/rel.h" /* Relation */
#include "funcapi.h" /* SRF */
#include "catalog/pg_type.h" /* TEXTOID for tuple_desc */

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif
/* } */

typedef struct
{
  int action;				/* the action  fincore, fadvise...*/
  Relation rel;				/* the relation */
  unsigned int segcount;	/* the segment current number */
  char *relationpath;		/* the relation path */
} pgfincore_fctx;

Datum pgfincore(PG_FUNCTION_ARGS);
static Datum pgfincore_file(char *filename, FunctionCallInfo fcinfo);
static Datum pgfadv_willneed_file(char *filename, FunctionCallInfo fcinfo);
// static Datum pgfadv_dontneed_file(char *filename, FunctionCallInfo fcinfo);

/* fincore -
 */
PG_FUNCTION_INFO_V1(pgfincore);
Datum
pgfincore(PG_FUNCTION_ARGS)
{
  FuncCallContext *funcctx;
  pgfincore_fctx *fctx;
  char			pathname[MAXPGPATH];
  Datum result;
  bool isnull;

  /* stuff done only on the first call of the function */
  if (SRF_IS_FIRSTCALL())
  {
	MemoryContext oldcontext;
	Oid			  relOid = PG_GETARG_OID(0);
	text		  *forkName = PG_GETARG_TEXT_P(1);

	/* create a function context for cross-call persistence */
	funcctx = SRF_FIRSTCALL_INIT();

	/*
	 * switch to memory context appropriate for multiple function calls
	 */
	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

	/* allocate memory for user context */
	fctx = (pgfincore_fctx *) palloc(sizeof(pgfincore_fctx));

	/*
	* Use fctx to keep track of upper and lower bounds from call to call.
	* It will also be used to carry over the spare value we get from the
	* Box-Muller algorithm so that we only actually calculate a new value
	* every other call.
	*/
	fctx->rel = relation_open(relOid, AccessShareLock);
	if (fctx->rel->rd_istemp || fctx->rel->rd_islocaltemp){
		relation_close(fctx->rel, AccessShareLock);
		elog(DEBUG3, "temp table : %s", fctx->relationpath);
		pfree(fctx);
		SRF_RETURN_DONE(funcctx);
	}
	fctx->relationpath = relpath(fctx->rel->rd_node,
								 forkname_to_number(text_to_cstring(forkName)));
	fctx->action = PG_GETARG_INT32(2);;
	fctx->segcount = 0;
	funcctx->user_fctx = fctx;

	elog(DEBUG3, "1st call : %s",
		 fctx->relationpath);

	MemoryContextSwitchTo(oldcontext);
  }

  funcctx = SRF_PERCALL_SETUP();
  fctx = funcctx->user_fctx;

  if (fctx->segcount == 0)
	snprintf(pathname,
			 MAXPGPATH,
			 "%s",
			 fctx->relationpath);
  else
	snprintf(pathname,
			 MAXPGPATH,
			 "%s.%u",
			 fctx->relationpath,
			 fctx->segcount);

  elog(DEBUG2, "pathname is %s", pathname);

  switch (fctx->action) {
	case 1 : /* MINCORE */
	  result = pgfincore_file(pathname, fcinfo);
	break;
	case 2 : /* FADV_WILLNEED */
	  result = pgfadv_willneed_file(pathname, fcinfo);
	break;
// 	case 3 : /* FADV_DONTNEED */
// 	  result = pgfadv_dontneed_file(pathname, fcinfo);
// 	break;
  }
  /* do when there is no more left */
  if (DatumGetInt64(GetAttributeByName(result, "block_disk", &isnull)) == 0 || isnull) {
	relation_close(fctx->rel, AccessShareLock);
	elog(DEBUG3, "last call : %s",
		 fctx->relationpath);
	pfree(fctx);
	SRF_RETURN_DONE(funcctx);
  }

/* or send the result */
  fctx->segcount++;
  SRF_RETURN_NEXT(funcctx, result);
}

/*
 * pgfincore_file handle the mmaping, mincore process (and access file, etc.)
 */
static Datum
pgfincore_file(char *filename, FunctionCallInfo fcinfo) {
  HeapTuple	tuple;
  TupleDesc tupdesc;
  Datum		values[4];
  bool		nulls[4];
  long long int  flag=1;
  long long int  block_disk = 0;
  long long int  block_mem  = 0;
  long long int  group_mem  = 0;

  // for open file
  int fd;
  // for stat file
  struct stat st;
  // for mmap file
  void *pa = (char *)0;
  // for calloc file
  unsigned char *vec = (unsigned char *)0;

  // OS things
  size_t pageSize = sysconf(_SC_PAGESIZE);
  register size_t pageIndex;

  tupdesc = CreateTemplateTupleDesc(4, false);
  TupleDescInitEntry(tupdesc, (AttrNumber) 1, "relpath",
									  TEXTOID, -1, 0);
  TupleDescInitEntry(tupdesc, (AttrNumber) 2, "block_disk",
									  INT8OID, -1, 0);
  TupleDescInitEntry(tupdesc, (AttrNumber) 3, "block_mem",
									  INT8OID, -1, 0);
  TupleDescInitEntry(tupdesc, (AttrNumber) 4, "group_mem",
									  INT8OID, -1, 0);

  tupdesc = BlessTupleDesc(tupdesc);

/* Do the main work */
  fd = open(filename, O_RDONLY);
  if (fd == -1) {
    goto error;
  }
  if (fstat(fd, &st) == -1) {
    close(fd);
    elog(ERROR, "Can not stat object file : %s",
		 filename);
    goto error;
  }
  if (st.st_size != 0) {
	block_disk = st.st_size/pageSize;

	/* TODO We need to split mmap size to be sure (?) to be able to mmap */
	pa = mmap(NULL, st.st_size, PROT_NONE, MAP_SHARED, fd, 0);
	if (pa == MAP_FAILED) {
	  close(fd);
	  elog(ERROR, "Can not mmap object file : %s, errno = %i,%s\nThis error can happen if there is not enought space in memory to do the projection. Please mail cedric@villemain.org with '[pgfincore] ENOMEM' as subject.",
		  filename, errno, strerror(errno));
	  goto error;
	}

	vec = calloc(1, (st.st_size+pageSize-1)/pageSize);
	if ((void *)0 == vec) {
	  munmap(pa, st.st_size);
	  close(fd);
	  elog(ERROR, "Can not calloc object file : %s",
		  filename);
	  goto error;
	}

	if (mincore(pa, st.st_size, vec) != 0) {
	  free(vec);
	  munmap(pa, st.st_size);
	  close(fd);
	  elog(ERROR, "mincore(%p, %lld, %p): %s\n",
			  pa, (long long int)st.st_size, vec, strerror(errno));
	  goto error;
	}

	/* handle the results */
	for (pageIndex = 0; pageIndex <= st.st_size/pageSize; pageIndex++) {
	  // block in memory
	  if (vec[pageIndex] & 1) {
		block_mem++;
		elog (DEBUG5, "in memory blocks : %lld / %lld",
			  (long long int)pageIndex, block_disk);
		if (flag)
		  group_mem++;
		  flag = 0;
	  }
	  else
		flag=1;
	}
  }
  elog(DEBUG1, "pgfincore %s: %lld of %lld block in linux cache, %lld groups",
	   filename, block_mem,  block_disk, group_mem);

  values[0] = CStringGetTextDatum(filename);
  values[1] = Int64GetDatum(block_disk);
  values[2] = Int64GetDatum(block_mem);
  values[3] = Int64GetDatum(group_mem);

  memset(nulls, 0, sizeof(nulls));

  tuple = heap_form_tuple(tupdesc, values, nulls);

  //   free things
  free(vec);
  munmap(pa, st.st_size);
  close(fd);

  return HeapTupleGetDatum(tuple); /* return filename, block_disk, block_mem, group_mem   */

error:
  values[0] = CStringGetTextDatum(filename);
  values[1] = Int64GetDatum(false);
  values[2] = Int64GetDatum(1);
  values[3] = Int64GetDatum(2);
  memset(nulls, 0, sizeof(nulls));
  tuple = heap_form_tuple(tupdesc, values, nulls);
  return (HeapTupleGetDatum(tuple));
}

/*
 * pgfadv_willneed_file
 */
static Datum
pgfadv_willneed_file(char *filename, FunctionCallInfo fcinfo) {
  HeapTuple	tuple;
  TupleDesc tupdesc;
  Datum		values[3];
  bool		nulls[3];

  // for open file
  int fd;
  // for stat file
  struct stat st;

  // OS things
  size_t pageSize = sysconf(_SC_PAGESIZE);
//
  tupdesc = CreateTemplateTupleDesc(3, false);
  TupleDescInitEntry(tupdesc, (AttrNumber) 1, "relpath",
									  TEXTOID, -1, 0);
  TupleDescInitEntry(tupdesc, (AttrNumber) 2, "block_disk",
									  INT8OID, -1, 0);
  TupleDescInitEntry(tupdesc, (AttrNumber) 3, "block_size",
									  INT8OID, -1, 0);

  tupdesc = BlessTupleDesc(tupdesc);

/* Do the main work */
  fd = open(filename, O_RDONLY);
  if (fd == -1) {
    goto error;
  }
  if (fstat(fd, &st) == -1) {
    close(fd);
    elog(ERROR, "Can not stat object file : %s",
		 filename);
    goto error;
  }

  fdatasync(fd);
  posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
  values[0] = CStringGetTextDatum(filename);
  values[1] = Int64GetDatum(st.st_size/pageSize);
  values[2] = Int64GetDatum((long long int)pageSize);

  memset(nulls, 0, sizeof(nulls));

  tuple = heap_form_tuple(tupdesc, values, nulls);

  //   free things
  close(fd);

  return HeapTupleGetDatum(tuple);

error:
  values[0] = CStringGetTextDatum(filename);
  values[1] = Int64GetDatum(false);
  values[2] = Int64GetDatum(1);
  memset(nulls, 0, sizeof(nulls));
  tuple = heap_form_tuple(tupdesc, values, nulls);
  return (HeapTupleGetDatum(tuple));
}
