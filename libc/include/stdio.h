#ifndef MAINDOB_LIBC_STDIO_H
#define MAINDOB_LIBC_STDIO_H

#include <sys/types.h>

/* File descriptor constants */
#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

/* Formatted output */
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t max, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Simple output */
int puts(const char *s);

/* Raw I/O */
ssize_t write(int fd, const void *buf, size_t count);
ssize_t read(int fd, void *buf, size_t count);

#endif /* MAINDOB_LIBC_STDIO_H */
