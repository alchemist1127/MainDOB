#ifndef MAINDOB_TIME_CIVIL_H
#define MAINDOB_TIME_CIVIL_H

/* Calendario civile: conversione bidirezionale secondi Unix <->
 * (anno, mese, giorno, ora, minuto, secondo). Funzioni pure, fuori dai
 * percorsi caldi (boot epoch, SYS_GETTIME). Range: 1970..~2104. */

#include "lib/types.h"

/* Tempo civile scomposto. Layout uguale a dob_realtime_t (ABI
 * userspace di SYS_GETTIME) ma tipo separato: cambi all'ABI non
 * devono propagarsi ai chiamanti interni. */
typedef struct
{
    uint32_t year;
    uint32_t month;     /* 1..12 */
    uint32_t day;       /* 1..31 */
    uint32_t hour;      /* 0..23 */
    uint32_t minute;    /* 0..59 */
    uint32_t second;    /* 0..59 */
} civil_t;

/* Predicato bisestile gregoriano. */
static inline bool is_leap_year(uint32_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

/* civile -> secondi Unix. 0 su input non valido (anno < 1970,
 * mese/giorno fuori range). */
uint64_t civil_to_unix_seconds(const civil_t *c);

/* secondi Unix -> civile. */
void unix_seconds_to_civil(uint64_t total, civil_t *out);

#endif /* MAINDOB_TIME_CIVIL_H */
