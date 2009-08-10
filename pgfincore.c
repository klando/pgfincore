/*
*  PgFincore
*  This project let you see what objects are in the FS cache memory
*  Copyright (C) 2009 Cédric Villemain Dalibo
*
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 2 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software
#  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*
*
*/

/* { POSIX stuff */
#include <errno.h> /* errno */
#include <fcntl.h> /* fcntl, open */
#include <stdlib.h> /* exit, calloc, free */
#include <sys/stat.h> /* stat, fstat */
#include <sys/types.h> /* size_t, mincore */
#include <unistd.h> /* sysconf, close */
#include <sys/mman.h> /* mmap, mincore */
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


#ifdef PG_VERSION_NUM
#define PG_MAJOR_VERSION (PG_VERSION_NUM / 100)
#else
#define PG_MAJOR_VERSION 803
#endif

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif
/* } */



typedef struct
{
  Relation rel;				/* the relation */
  unsigned int segcount;	/* the segment current number */
  char *relationpath;		/* the relation path */
} pgfincore_fctx;

typedef struct
{
  int64		block_mem;		/* number of blocks in memory */
  int64		block_disk;		/* size of file in blocks */
  int64		group_mem;		/* number of group of adjacent blocks in memory */
} pgfincore_info;

Datum pgfincore(PG_FUNCTION_ARGS); 
static pgfincore_info pgfincore_file(char *filename);

/* fincore -
 */
PG_FUNCTION_INFO_V1(pgfincore);
Datum pgfincore(PG_FUNCTION_ARGS)
{
  FuncCallContext *funcctx;
  pgfincore_fctx *fctx;
  pgfincore_info *info;
  char			pathname[MAXPGPATH];

  /* stuff done only on the first call of the function */
  if (SRF_IS_FIRSTCALL())
  {
	TupleDesc     tupdesc;
	MemoryContext oldcontext;
	Oid		relOid = PG_GETARG_OID(0);
	text	*forkName = PG_GETARG_TEXT_P(1);

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
	fctx->relationpath = relpath(fctx->rel->rd_node,
								 forkname_to_number(text_to_cstring(forkName)));
	fctx->segcount = 0;
	funcctx->user_fctx = fctx;
	
	tupdesc = CreateTemplateTupleDesc(2, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "relpath",
										TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "block_mem",
										INT8OID, -1, 0);

	funcctx->tuple_desc = BlessTupleDesc(tupdesc);

	elog(DEBUG3, "1st call : %s",fctx->relationpath);

	MemoryContextSwitchTo(oldcontext);
  }

  funcctx = SRF_PERCALL_SETUP();

  if (fctx->segcount == 0)
	snprintf(pathname, MAXPGPATH, "%s", fctx->relationpath);
  else
	snprintf(pathname, MAXPGPATH, "%s.%u", fctx->relationpath, fctx->segcount);

  elog(DEBUG2, "pathname is %s", pathname);

  info = (pgfincore_info *) palloc(sizeof(pgfincore_info));
  *info = pgfincore_file(pathname);

  elog(DEBUG2, "got result = %lu", (unsigned long)info->block_mem); // TODO fix the %lu

  /* do when there is no more left */
  if (info->block_disk == -1) {
	relation_close(fctx->rel, AccessShareLock);
	elog(DEBUG3, "last call : %s", fctx->relationpath);
	pfree(fctx);
	SRF_RETURN_DONE(funcctx);
  }
  /* or send the result */
  else {
	HeapTuple		tuple;
	Datum			values[2];
	bool			nulls[2];

	fctx->segcount++;
	values[0] = CStringGetTextDatum(pathname);
	values[1] = Int64GetDatum(info->block_mem);
	memset(nulls, 0, sizeof(nulls));
	tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
	elog(DEBUG1, "file %s contain %i block in linux cache memory", pathname, info->block_mem);
	SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
  }
}

static pgfincore_info pgfincore_file(char *filename) {
  // our counter for block in memory
  int64     n=0;
  int64     nmem=0;
  int       flag=1;
  pgfincore_info *info;

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

/* Do the main work */
  info = (pgfincore_info *) palloc(sizeof(pgfincore_info));
  fd = open(filename, O_RDONLY);
  if (fd == -1) {
	info->block_disk = -1;
    return *info;
  }
  if (fstat(fd, &st) == -1) {
    close(fd);
    elog(ERROR, "Can not stat object file : %s", filename);
  }
  if (st.st_size == 0) {
	info->block_disk = 0;
	info->block_mem = 0;
	info->group_mem = 0;
    return *info;
  }

  /* TODO We need to split mmap size to be sure (?) to be able to mmap */
  pa = mmap(NULL, st.st_size, PROT_NONE, MAP_SHARED, fd, 0);
  if (pa == MAP_FAILED) {
    close(fd);
    elog(ERROR, 
		 "Can not mmap object file : %s, errno = %i,%s",
		 filename,
		 errno,
		 strerror(errno));
  }

  vec = calloc(1, (st.st_size+pageSize-1)/pageSize);
  if ((void *)0 == vec) {
    munmap(pa, st.st_size);
    close(fd);
    elog(ERROR, "Can not calloc object file : %s", filename);
	info->block_disk = -1;
    return *info;
  }

  if (mincore(pa, st.st_size, vec) != 0) {
    free(vec);
    munmap(pa, st.st_size);
    close(fd);
    elog(ERROR, "mincore(%p, %lu, %p): %s\n",
            pa, (unsigned long)st.st_size, vec, strerror(errno));
	info->block_disk = -1;
    return *info;
  }

  /* handle the results */
  for (pageIndex = 0; pageIndex <= st.st_size/pageSize; pageIndex++) {
	// block in memory
    if (vec[pageIndex] & 1) {
      n++;
      elog (DEBUG5, "in memory blocks : %lu / %lu", (unsigned long)pageIndex, (unsigned long)(st.st_size/pageSize)); 
      if (flag)
		nmem++;
		flag = 0;
    }
	else
	  flag=1;
  }
  info->block_disk = st.st_size/pageSize;
  info->block_mem = n;
  info->group_mem = nmem;

  //   free things
  free(vec);
  munmap(pa, st.st_size);
  close(fd);

  elog(DEBUG1, "pgfincore %s: %lu of %lu block in linux cache, %lu groups",filename, (unsigned long)n, (unsigned long)(st.st_size/pageSize), (unsigned long)info->group_mem);

  return *info; /* return block_disk, block_mem, group_mem   */
}
