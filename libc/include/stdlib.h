#ifndef MAINDOB_LIBC_STDLIB_H
#define MAINDOB_LIBC_STDLIB_H

#include <sys/types.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);

void  abort(void);
void  exit(int code);

int   atoi(const char *s);
long  atol(const char *s);
long  strtol(const char *s, char **endp, int base);

int   abs(int x);
long  labs(long x);

#endif
