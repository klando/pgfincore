/* This project let you see what objects are in the FS cache memory
*  Copyright (C) 2009 Cédric Villemain Dalibo
*  
*/
/*
#  fincore - File IN CORE: show which blocks of a file are in core
#  Copyright (C) 2007  Dave Plonka
#
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

# $Id: fincore,v 1.9 2007/05/23 21:17:52 plonka Exp $
# Dave Plonka, Apr  5 2007
*/

/* { POSIX stuff */
#include <errno.h> /* errno */
#include <fcntl.h> /* fcntl, open */
#include <stdio.h> /* perror, fprintf, stderr, printf */
#include <stdlib.h> /* exit, calloc, free */
#include <string.h> /* strerror */
#include <sys/stat.h> /* stat, fstat */
#include <sys/types.h> /* size_t */
#include <unistd.h> /* sysconf, close */
/* } */

#include "postgres.h" /* general Postgres declarations */

#include "mb/pg_wchar.h"
#include "utils/elog.h"
#include "utils/builtins.h"

#include <sys/mman.h>

#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_tablespace.h"
#include "commands/dbcommands.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/syscache.h"
#include "utils/relcache.h"
#include "utils/rel.h"
// #include "pg_config_manual.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif



Datum pgfincore_name(PG_FUNCTION_ARGS); /* Prototype */
Datum pgfincore_oid(PG_FUNCTION_ARGS); /* Prototype */
static int64 pgfincore_all(RelFileNode *rfn);
static int64 pgfincore(char *filename);

PG_FUNCTION_INFO_V1(pgfincore_name);
PG_FUNCTION_INFO_V1(pgfincore_oid);



/* fincore -
 */
Datum
pgfincore_oid(PG_FUNCTION_ARGS)
{
	Oid			relOid = PG_GETARG_OID(0);
	Relation	rel;
	int64		size = 0;

	rel = relation_open(relOid, AccessShareLock);

	size = pgfincore_all(&(rel->rd_node));

	relation_close(rel, AccessShareLock);

	PG_RETURN_INT64(size);
}
Datum pgfincore_name(PG_FUNCTION_ARGS) {
	text	   *relname = PG_GETARG_TEXT_P(0);
	RangeVar   *relrv;
	Relation	rel;
	int64		size = 0;

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	size = pgfincore_all(&(rel->rd_node));
	elog(DEBUG1, "pgfincore3 : %lu",(unsigned long)size);

	relation_close(rel, AccessShareLock);

	return size;
}

// calculate number of block in memory
static int64
pgfincore_all(RelFileNode *rfn)
{
	int64		totalsize = 0;
	int64		result    = 0;
	char	   *relationpath;
	char		pathname[MAXPGPATH];
	unsigned int segcount = 0;

	relationpath = relpath(*rfn);

	for (segcount = 0;; segcount++)
	{
		if (segcount == 0)
			snprintf(pathname, MAXPGPATH, "%s",
					 relationpath);
		else
			snprintf(pathname, MAXPGPATH, "%s.%u",
					 relationpath, segcount);

		result = pgfincore(pathname);
	elog(DEBUG1, "pgfincore : %lu",(unsigned long)result);

		if (result == -1)
		  break;
		totalsize += result;
	}
	elog(DEBUG1, "pgfincore2 : %lu",(unsigned long)totalsize);

	return (unsigned long)totalsize;
}


static int64
pgfincore(char *filename) {
  // our counter for block in memory
  int     n=0;
  // our return value TODO use array.
//   VarChar *return_pgfincore ;

  // for open file
  int fd;
  // for stat file
  struct stat st;
  // for mmap file
  void *pa = (char *)0;
  // for calloc file
  unsigned char *vec = (unsigned char *)0;

  // OS things
  off_t pa_offset;
  size_t pageSize = sysconf(_SC_PAGESIZE);
  register size_t pageIndex;

/* Do the main work */
  fd = open(filename, O_RDONLY);
  if (fd == -1)
	return -1;
//     elog(ERROR, "Can not open object file : %s", filename);

  if (fstat(fd, &st) == -1) {
    close(fd);
    elog(ERROR, "Can not stat object file : %s", filename);
  }

  pa_offset = 0 & ~(sysconf(_SC_PAGE_SIZE) - 1);

  pa = mmap(NULL, st.st_size - pa_offset, PROT_NONE, MAP_SHARED, fd, pa_offset);
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

  if (0 != mincore(pa, st.st_size, vec)) {
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
//       elog (NOTICE, "r: %lu / %lu", (unsigned long)pageIndex, (unsigned long)(st.st_size/pageSize));  /* TODO fix that /!\ (on veut concaténer tous les résultats pour le moment)*/
    }
  }

//   free things
  free(vec);
  munmap(pa, (st.st_size+pageSize-1)/pageSize);
  close(fd);

//   return_pgfincore = sprintf("pgfincore : %lu of %lu block in linux cache",(unsigned long)n, (unsigned long)(st.st_size/pageSize));

	elog(DEBUG1, "pgfincore : %lu of %lu block in linux cache",(unsigned long)n, (unsigned long)(st.st_size/pageSize));

//   PG_RETURN_VARCHAR_P(return_pgfincore);
  return (unsigned long)n;
}
