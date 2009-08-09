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


#ifdef PG_VERSION_NUM
#define PG_MAJOR_VERSION (PG_VERSION_NUM / 100)
#else
#define PG_MAJOR_VERSION 803
#endif

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif
/* } */


Datum pgfincore(PG_FUNCTION_ARGS); /* Prototype */
static int64 pgfincore_file(char *filename);

typedef struct
{
  Relation rel;				/* the relation */
  int64 segcount;	/* the segment current number */
  char *relationpath;		/* the relation path */
} pgfincore_fctx;

/* fincore -
 */
PG_FUNCTION_INFO_V1(pgfincore);
Datum pgfincore(PG_FUNCTION_ARGS)
{
  FuncCallContext *funcctx;
  MemoryContext oldcontext;
  pgfincore_fctx *fctx;

  Oid		relOid = PG_GETARG_OID(0);
  text		*forkName = PG_GETARG_TEXT_P(1);
  Relation	rel;
  char		*relationpath;
  char		pathname[MAXPGPATH];
  int64 segcount;
  int64 result    = 0;

  /* stuff done only on the first call of the function */
  if (SRF_IS_FIRSTCALL())
  {
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
	fctx->relationpath = relpath(fctx->rel->rd_node, forkname_to_number(text_to_cstring(forkName)));
	fctx->segcount = 0;
// 	elog(DEBUG2, "1st call : %s",fctx->relationpath);
	funcctx->user_fctx = fctx;

	MemoryContextSwitchTo(oldcontext);
  }

  /* stuff done on every call of the function */
  funcctx = SRF_PERCALL_SETUP();
  segcount = fctx->segcount;
  relationpath = fctx->relationpath;
  rel = fctx->rel;

  // result
  if (segcount == 0)
	snprintf(pathname, MAXPGPATH, "%s", relationpath);
  else
	snprintf(pathname, MAXPGPATH, "%s.%u", relationpath, segcount);

  result = pgfincore_file(pathname);
  elog(DEBUG2, "pgfincore : %lu",(unsigned long)result);

  /* do when there is no more left */
  if (result == -1) {
	relation_close(rel, AccessShareLock);
	SRF_RETURN_DONE(funcctx);
  }
  /* or send the result */
  else {
	fctx->segcount++;
	SRF_RETURN_NEXT(funcctx, Int64GetDatum(result));
  }
}





static int64 pgfincore_file(char *filename) {
  // our counter for block in memory
  int64     n=0;
  int64     cut=0;
  int     flag=1;

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
  fd = open(filename, O_RDONLY);
  if (fd == -1)
	return -1;

  if (fstat(fd, &st) == -1) {
    close(fd);
    elog(ERROR, "Can not stat object file : %s", filename);
  }
  if (st.st_size == 0) {
    return 0;
  } 
  pa = mmap(NULL, st.st_size, PROT_NONE, MAP_SHARED, fd, 0);
  if (pa == MAP_FAILED) {
    close(fd);
    elog(ERROR, "Can not mmap object file : %s", filename);
  }

  vec = calloc(1, (st.st_size+pageSize-1)/pageSize);
  if ((void *)0 == vec) {
    munmap(pa, (st.st_size+pageSize-1)/pageSize);
    close(fd);
    elog(ERROR, "Can not calloc object file : %s", filename);
    return -1;
  }

  if (mincore(pa, st.st_size, vec) != 0) {
    free(vec);
    munmap(pa, (st.st_size+pageSize-1)/pageSize);
    close(fd);
    elog(ERROR, "mincore(%p, %lu, %p): %s\n",
            pa, (unsigned long)st.st_size, vec, strerror(errno));
    return -1;
  }

  /* handle the results */
  for (pageIndex = 0; pageIndex <= st.st_size/pageSize; pageIndex++) {
	// block in memory
    if (vec[pageIndex] & 1) {
      n++;
      elog (DEBUG5, "r: %lu / %lu", (unsigned long)pageIndex, (unsigned long)(st.st_size/pageSize)); 
      if (flag)
		cut++;
		flag = 0;
    }
	else
	  flag=1;
  }

//   free things
  free(vec);
  munmap(pa, (st.st_size+pageSize-1)/pageSize);
  close(fd);

  elog(DEBUG1, "pgfincore %s: %lu of %lu block in linux cache, %lu groups",filename, (unsigned long)n, (unsigned long)(st.st_size/pageSize), (unsigned long)cut);

  return n;
}
