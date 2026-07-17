#ifndef MAINDOB_LIB_FORMAT_H
#define MAINDOB_LIB_FORMAT_H

#include "lib/types.h"

/* Motore di formattazione condiviso del kernel 1.1.
 *
 * Nell'1.0 il formatter viveva dentro lib/printf.c, fuso col driver
 * VGA: qui e' un componente standard a se' — puro, senza I/O, senza
 * lock — riusato da chiunque debba produrre testo:
 *   console/kprintf   (sink = VGA + seriale)
 *   snprintf          (sink = buffer)
 *   kpanic            (sink = emissione diretta, senza lock)
 *
 * Specificatori (superset 1:1 dell'1.0): %d %i %u %x %X %p %s %c %%,
 * flag '0' e larghezza minima (es. %08x, %03u). */

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)

/* Sink: riceve un carattere alla volta piu' un contesto opaco. */
typedef void (*putchar_fn)(char c, void *ctx);

/* Formatta `fmt`+args nel sink. Nessun lock, nessun I/O: la mutua
 * esclusione e' responsabilita' del chiamante. */
void format_output(putchar_fn put, void *ctx, const char *fmt, va_list args);

/* Formattazione su buffer, sempre NUL-terminata (se size > 0).
 * Ritorna i caratteri che SAREBBERO stati scritti (stile C99):
 * ret >= size segnala il troncamento. */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif /* MAINDOB_LIB_FORMAT_H */
