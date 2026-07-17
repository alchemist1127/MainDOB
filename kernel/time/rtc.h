#ifndef MAINDOB_TIME_RTC_H
#define MAINDOB_TIME_RTC_H

/* RTC CMOS (MC146818, porte 0x70/0x71) — sorgente del wall-clock.
 * Qui vive solo l'hardware: rtc_init() legge il chip una volta al boot
 * e consegna l'epoca a clock_set_boot_epoch(); dopo, zero I/O (il
 * tempo reale e' epoca + monotono). Niente IRQ8. */

#include "lib/types.h"

typedef struct
{
    uint16_t year;        /* a 4 cifre, es. 2026 */
    uint8_t  month;       /* 1..12 */
    uint8_t  day;         /* 1..31 */
    uint8_t  hour;        /* 0..23 */
    uint8_t  minute;      /* 0..59 */
    uint8_t  second;      /* 0..59 */
} rtc_time_t;

/* Legge l'ora dal CMOS. Gestisce BCD/binario e 12/24 ore (status B).
 * false se il chip non da' due letture stabili (chip incastrato). */
bool rtc_read_time(rtc_time_t *out);

/* Boot: legge il CMOS una volta, calcola l'epoca Unix e la consegna a
 * clock_set_boot_epoch(). Su CMOS illeggibile il wall-clock parte
 * dall'epoca Unix (epoca 0) — il monotono non e' toccato. */
void rtc_init(void);

#endif /* MAINDOB_TIME_RTC_H */
