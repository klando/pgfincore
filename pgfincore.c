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


#include "postgres.h"

#include "mb/pg_wchar.h"
#include "utils/elog.h"
#include "utils/builtins.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

Datum
pgfincore(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pgfincore);

/* fincore -
 */
Datum
pgfincore(PG_FUNCTION_ARGS) {
  char    *filename     = (char *)PG_GETARG_CSTRING(0);
  int     filename_size = VARSIZE(filename) - VARHDRSZ;
  VarChar *return_pgfincore;
  char    *buffer_in,*output;




   int fd;
   struct stat st;
   void *pa = (char *)0;
   char *vec = (char *)0;
   register size_t n = 0;
   size_t pageSize = getpagesize();
   register size_t pageIndex;

   fd = open(filename, 0);
   if (0 > fd) {
      elog(ERROR, "Can not open object file : %s", filename);
      return 0;
   }

   if (0 != fstat(fd, &st)) {
      elog(ERROR, "Can not stat object file : %s", filename);
      close(fd);
      return 0;
   }

   pa = mmap((void *)0, st.st_size, PROT_NONE, MAP_SHARED, fd, 0);
   if (MAP_FAILED == pa) {
      elog(ERROR, "Can not mmap object file : %s", filename);
      close(fd);
      return 0;
   }

   /* vec = calloc(1, 1+st.st_size/pageSize); */
   vec = calloc(1, (st.st_size+pageSize-1)/pageSize);
   if ((void *)0 == vec) {
      elog(ERROR, "Can not calloc object file : %s", filename);
      close(fd);
      return 0;
   }

   if (0 != mincore(pa, st.st_size, vec)) {
      /* perror("mincore"); */
      elog(ERROR, "mincore(%p, %lu, %p): %s\n",
              pa, (unsigned long)st.st_size, vec, strerror(errno));
      free(vec);
      close(fd);
      return 0;
   }

   /* handle the results */
   for (pageIndex = 0; pageIndex <= st.st_size/pageSize; pageIndex++) {
      if (vec[pageIndex]&1) {
         elog (NOTICE, "r: %lu", (unsigned long)pageIndex);  /* TODO fix that /!\ (on veut concaténer tous les résultats pour le moment)*/
      }
   }

   free(vec);
   vec = (char *)0;

   munmap(pa, st.st_size);
   close(fd);



  /* convert string to VarChar for result */
  return_pgfincore = DatumGetVarCharP(
                  DirectFunctionCall2(
                    varcharin,
                    CStringGetDatum(output),
                    Int32GetDatum(strlen(output) + VARHDRSZ)
                  )
                );

#ifdef PGFINCORE_DEBUG
  elog(NOTICE, "pgfincore : %s", return_pgfincore);
#endif /*PGFINCORE_DEBUG*/
  PG_RETURN_VARCHAR_P(return_pgfincore);
}


/*   END   */

  void    *h ;

  pg_verifymbstr(VARDATA(str_in), str_in_size, false);

  buffer_in = DatumGetCString(
                DirectFunctionCall1(textout,
                  PointerGetDatum(str_in)
                )
              );
  PG_FREE_IF_COPY(str_in, 0);

  h = textcat_Init( "/usr/share/postgresql/8.2/contrib/textcat_conf.txt" );
  if (!h) {
    elog(ERROR, "can not open config file : /usr/share/postgresql/8.2/contrib/textcat_conf.txt or one of the fingerprints.");
    PG_RETURN_VARCHAR_P("unable to open one file");
  }

}

