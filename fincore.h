//#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#define __NR_fincore 316

//#define HAVE_FINCORE 0

int fincore(unsigned int fd, loff_t start, loff_t len, unsigned char * vec);

int fincore(unsigned int fd, loff_t start, loff_t len, unsigned char * vec)
{
  return syscall(__NR_fincore, fd, start, len, vec);
}
