#! /usr/bin/perl

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

use Inline C;
use strict;
use FindBin;
use Getopt::Long;
use Pod::Usage;
use POSIX; # for sysconf

my %opt;

# { CONFIGURATION SECTION BEGIN ################################################

$opt{s} = 0;

# } CONFIGURATION SECTION END ##################################################

GetOptions('help'           => \$opt{h},
	   'man'            => \$opt{m},
	   'summary!'       => \$opt{s},
	   'justsummarize!' => \$opt{S},
	   'stdin'          => \$opt{I}) or pod2usage(2);

pod2usage(0) if ($opt{h});
pod2usage(-exitstatus => 0, -verbose => 2) if $opt{m};

pod2usage(2) if (0 == @ARGV and !$opt{I});

if ($opt{S}) {
   $opt{s} = 1;
}

my @files;
if ($opt{I}) {
   @files = grep { chomp } <STDIN>;
} else {
   @files = @ARGV;
}

my $pageSize = POSIX::sysconf(&POSIX::_SC_PAGESIZE);
print "page size: $pageSize bytes\n" if $opt{s};

my $filesProcessed = 0;
my $totalPages = 0;
foreach my $file (@files) {
   if (!stat($file)) {
      warn("$file: $!\n");
      next;
   }
   my @values = fincore($file);
   if (@values) {
      $totalPages += @values;
      printf("%s: %u incore page%s: @values\n",
             $file, scalar(@values), (1 == @values)? "" : "s") unless $opt{S};
   } else {
      print "$file: no incore pages.\n" unless $opt{S};
   }
   $filesProcessed++;
}

if ($opt{s}) {
   if ($filesProcessed) {
      printf("%.0f page%s, %sbytes in core for %u file%s; " .
             "%.2f page%s, %sbytes per file.\n",
             $totalPages, (1 == $totalPages)? "" : "s",
             scale("%.1f", $totalPages*$pageSize),
             $filesProcessed, (1 == $filesProcessed)? "" : "s",
             $totalPages/$filesProcessed,
             (1. == $totalPages/$filesProcessed)? "" : "s",
             scale("%.1f", ($totalPages*$pageSize)/$filesProcessed));
   }
}

exit;

################################################################################

sub scale($$) { # This is based somewhat on Tobi Oetiker's code in rrd_graph.c:
   my $fmt = shift;
   my $value = shift;
   my @symbols = ("a", # 10e-18 Ato
                  "f", # 10e-15 Femto
                  "p", # 10e-12 Pico
                  "n", # 10e-9  Nano
                  "u", # 10e-6  Micro
                  "m", # 10e-3  Milli
                  " ", # Base
                  "k", # 10e3   Kilo
                  "M", # 10e6   Mega
                  "G", # 10e9   Giga
                  "T", # 10e12  Terra
                  "P", # 10e15  Peta
                  "E");# 10e18  Exa

   my $symbcenter = 6;
   my $digits = (0 == $value)? 0 : floor(log($value)/log(1024));
   return sprintf(${fmt} . " %s", $value/pow(1024, $digits),
                  $symbols[$symbcenter+$digits])
}

################################################################################

__END__

=head1 NAME

fincore - File IN CORE: show which blocks of a file are in core

=head1 SYNOPSIS

fincore [options] <-stdin | file [...]>

 Options:
  -help - brief help message
  -man - full documentation
  -summary - report summary statistics for the files
  -justsummarize - just report summary statistics for the files
  -stdin - read file names from standard input

=head1 OPTIONS

=over 8

=item B<-help>

Shows usage information and exits.

=item B<-man>

Shows the manual page and exits.

=item B<-summary>

Report summary statistics for the files.

=item B<-nosummary>

Don't report summary statistics for the files.
This is the default.

=item B<-justsummarize>

Just report summary statistics for the files.  
I.e. don't show details for each file.

=item B<-nojustsummarize>

Don't just report summary statistics for the files.  
This is the default.

=item B<-stdin>

Read file names from standard input.
This is to avoid "Arg list too long" with very many files.

=back

=head1 DESCRIPTION

B<fincore> is a command that shows which pages (blocks) of a file are
in core memory.

It is particularly useful for determining the contents of the
buffer-cache.  The name means "File IN CORE" and I pronounce it
"eff in core".

=head1 EXAMPLES

 $ fincore foo.rrd
 foo.rrd: no incore pages.

 $ cat foo.rrd >/dev/null # read the whole file
 $ fincore foo.rrd
 foo.rrd: 26 incore pages: 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25

 $ ls |grep '\.rrd$' |~/perl/fincore --stdin --justsummarize
 page size: 4096 bytes
 2214049 pages, 8.4 Gbytes in core for 268994 files; 8.23 pages, 32.9 kbytes per file.

=head1 BUGS

In verbose mode, you may get an error from mincore such as "cannot
allocate memory" if the file size is zero.

Some operating systems have posix_fadvise, but it doesn't work.
For instance under Linux 2.4, you may see this error:

 posix_fadvise: Inappropriate ioctl for device

=head1 AUTHOR

Dave Plonka <plonka@cs.wisc.edu>

Copyright (C) 2007  Dave Plonka.
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

=head1 VERSION

This is fincore B<$Revision: 1.9 $>.

=head1 SEE ALSO

The B<fadvise> command.

=cut

__C__
#define PERL_INLINE /* undef this to build the C code stand-alone */

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

/* fincore -
 */
void
fincore(char *filename) {
   int fd;
   struct stat st;
   void *pa = (char *)0;
   char *vec = (char *)0;
   register size_t n = 0;
   size_t pageSize = getpagesize();
   register size_t pageIndex;
#  ifdef PERL_INLINE
   INLINE_STACK_VARS;
#  endif

#  ifdef PERL_INLINE
   INLINE_STACK_RESET;
#  endif

   fd = open(filename, 0);
   if (0 > fd) {
      perror("open");
#     ifdef PERL_INLINE
      INLINE_STACK_VOID;
#     endif
      return;
   }

   if (0 != fstat(fd, &st)) {
      perror("fstat");
      close(fd);
#     ifdef PERL_INLINE
      INLINE_STACK_VOID;
#     endif
      return;
   }

   pa = mmap((void *)0, st.st_size, PROT_NONE, MAP_SHARED, fd, 0);
   if (MAP_FAILED == pa) {
      perror("mmap");
      close(fd);
#     ifdef PERL_INLINE
      INLINE_STACK_VOID;
#     endif
      return;
   }

   /* vec = calloc(1, 1+st.st_size/pageSize); */
   vec = calloc(1, (st.st_size+pageSize-1)/pageSize);
   if ((void *)0 == vec) {
      perror("calloc");
      close(fd);
#     ifdef PERL_INLINE
      INLINE_STACK_VOID;
#     endif
      return;
   }

   if (0 != mincore(pa, st.st_size, vec)) {
      /* perror("mincore"); */
      fprintf(stderr, "mincore(%p, %lu, %p): %s\n",
              pa, (unsigned long)st.st_size, vec, strerror(errno));
      free(vec);
      close(fd);
#     ifdef PERL_INLINE
      INLINE_STACK_VOID;
#     endif
      return;
   }

   /* handle the results */
   for (pageIndex = 0; pageIndex <= st.st_size/pageSize; pageIndex++) {
      if (vec[pageIndex]&1) {
#        ifndef PERL_INLINE /* { */
         printf("%lu\n", (unsigned long)pageIndex);
#        else /* }{ */
         /* return the results on perl's stack */
         INLINE_STACK_PUSH(sv_2mortal(newSVnv(pageIndex)));
         n++;
#        endif /* } */
      }
   }

   free(vec);
   vec = (char *)0;

   munmap(pa, st.st_size);
   close(fd);

#  ifdef PERL_INLINE
   INLINE_STACK_DONE;
#  endif

#  ifdef PERL_INLINE
   INLINE_STACK_RETURN(n);
#  endif
   return;
}
