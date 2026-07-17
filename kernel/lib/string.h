#ifndef MAINDOB_LIB_STRING_H
#define MAINDOB_LIB_STRING_H

#include "lib/types.h"

/* Componente standard mem/str del kernel 1.1: UNICA implementazione,
 * riusata da ogni sottosistema (heap, paging, IPC, console, ELF).
 * Regola Dob: niente copie locali di memcpy nei moduli. */

void   *memset(void *dst, int val, size_t n);
void   *memcpy(void *dst, const void *src, size_t n);
void   *memmove(void *dst, const void *src, size_t n);
int     memcmp(const void *a, const void *b, size_t n);

size_t  strlen(const char *s);
size_t  strnlen(const char *s, size_t maxlen);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
char   *strcpy(char *dst, const char *src);
char   *strncpy(char *dst, const char *src, size_t n);
char   *strstr(const char *haystack, const char *needle);

/* Copia troncante SEMPRE terminata (aggiunta 1.1): ritorna strlen(src),
 * cosi' `ret >= size` segnala il troncamento. Preferirla a strncpy nei
 * percorsi kernel: strncpy non termina se src riempie il buffer. */
size_t  strlcpy(char *dst, const char *src, size_t size);

#endif /* MAINDOB_LIB_STRING_H */
