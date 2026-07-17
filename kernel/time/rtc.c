#include "time/rtc.h"
#include "time/civil.h"
#include "time/clock.h"
#include "console/console.h"
#include "arch/x86/ports.h"

/* Lettura coerente del chip: attesa UIP, doppia lettura confermata,
 * decodifica BCD/12h/secolo. */

#define CMOS_INDEX  0x70
#define CMOS_DATA   0x71

#define REG_SECOND     0x00
#define REG_MINUTE     0x02
#define REG_HOUR       0x04
#define REG_DAY        0x07
#define REG_MONTH      0x08
#define REG_YEAR       0x09
#define REG_STATUS_A   0x0A
#define REG_STATUS_B   0x0B
#define REG_CENTURY    0x32     /* definito da ACPI; non sempre presente */

#define STAT_A_UIP     0x80
#define STAT_B_24H     0x02
#define STAT_B_BIN     0x04

/* Campi grezzi come escono dal chip, prima di ogni decodifica. */
typedef struct
{
    uint8_t second, minute, hour, day, month, year, century;
} raw_fields_t;

/* === Verbi ============================================================= */

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_INDEX, reg & 0x7F);       /* bit 7 = 0: NMI resta attivo */
    return inb(CMOS_DATA);
}

static inline uint8_t bcd_to_bin(uint8_t v)
{
    return (uint8_t)(((v >> 4) * 10) + (v & 0x0F));
}

/* Attende la fine di un eventuale update-in-progress. Attesa limitata:
 * la finestra e' ~1 ms una volta al secondo, e siamo a boot-time. */
static void wait_update_done(void)
{
    for (int spin = 0; spin < 100000; spin++)
    {
        if (!(cmos_read(REG_STATUS_A) & STAT_A_UIP))
        {
            return;
        }
    }
}

static void read_raw_fields(raw_fields_t *f)
{
    f->second  = cmos_read(REG_SECOND);
    f->minute  = cmos_read(REG_MINUTE);
    f->hour    = cmos_read(REG_HOUR);
    f->day     = cmos_read(REG_DAY);
    f->month   = cmos_read(REG_MONTH);
    f->year    = cmos_read(REG_YEAR);
    f->century = cmos_read(REG_CENTURY);
}

static bool fields_equal(const raw_fields_t *a, const raw_fields_t *b)
{
    return a->second == b->second && a->minute == b->minute
        && a->hour == b->hour && a->day == b->day
        && a->month == b->month && a->year == b->year;
}

/* Decodifica BCD/binario, 12/24 ore, ricostruzione del secolo. */
static void decode_fields(const raw_fields_t *f, uint8_t status_b,
                          rtc_time_t *out)
{
    bool is_bin = (status_b & STAT_B_BIN) != 0;
    bool is_24h = (status_b & STAT_B_24H) != 0;

    uint8_t hour = f->hour;
    uint8_t pm = 0;
    if (!is_24h && (hour & 0x80))
    {
        hour &= 0x7F;
        pm = 1;
    }

    uint8_t second  = f->second;
    uint8_t minute  = f->minute;
    uint8_t day     = f->day;
    uint8_t month   = f->month;
    uint8_t year2   = f->year;
    uint8_t century = f->century;
    if (!is_bin)
    {
        second  = bcd_to_bin(second);
        minute  = bcd_to_bin(minute);
        hour    = bcd_to_bin(hour);
        day     = bcd_to_bin(day);
        month   = bcd_to_bin(month);
        year2   = bcd_to_bin(year2);
        century = bcd_to_bin(century);
    }

    if (!is_24h)
    {
        if (hour == 12)                 /* 12 -> 0 (mezzanotte)... */
        {
            hour = 0;
        }
        if (pm)                         /* ...poi +12 se PM        */
        {
            hour = (uint8_t)(hour + 12);
        }
    }

    /* Anno: registro a 2 cifre + secolo da 0x32 se plausibile, altrimenti
     * euristica Y2K: 70..99 -> 1900, 0..69 -> 2000. */
    uint16_t full_year;
    if (century >= 19 && century <= 21)
    {
        full_year = (uint16_t)(century * 100 + year2);
    }
    else
    {
        full_year = (uint16_t)((year2 >= 70) ? (1900 + year2)
                                             : (2000 + year2));
    }

    out->second = second;
    out->minute = minute;
    out->hour   = hour;
    out->day    = day;
    out->month  = month;
    out->year   = full_year;
}

/* === API =============================================================== */

bool rtc_read_time(rtc_time_t *out)
{
    if (out == NULL)
    {
        return false;
    }

    wait_update_done();
    uint8_t status_b = cmos_read(REG_STATUS_B);

    /* Due letture uguali di fila = coerenti. Tentativi limitati. */
    for (int retry = 0; retry < 8; retry++)
    {
        raw_fields_t a, b;
        read_raw_fields(&a);
        read_raw_fields(&b);

        if (fields_equal(&a, &b))
        {
            decode_fields(&a, status_b, out);
            return true;
        }
    }
    return false;                       /* chip incastrato */
}

void rtc_init(void)
{
    rtc_time_t t;
    if (!rtc_read_time(&t))
    {
        clock_set_boot_epoch(0);
        kprintf("[RTC]  CMOS illeggibile: wall-clock dall'epoca Unix\n");
        return;
    }

    civil_t c = {
        .year   = t.year,
        .month  = t.month,
        .day    = t.day,
        .hour   = t.hour,
        .minute = t.minute,
        .second = t.second,
    };
    uint32_t epoch_s = (uint32_t)civil_to_unix_seconds(&c);

    /* ANCORAGGIO: il wall-clock e' epoch + monotono (clock_get_realtime),
     * quindi l'epoch deve valere "ora CMOS meno l'uptime GIA' trascorso"
     * alla lettura — rtc_init gira dopo calibrazione TSC e splash, ~3 s
     * di monotono accumulato: senza sottrazione il sistema viveva ~3 s
     * nel futuro, per sempre (bug latente dell'1.0, reso visibile dalla
     * splash dell'1.1). */
    /* Guardia sull'assurdo: se il CMOS restituisce un istante piu'
     * piccolo dell'uptime gia' trascorso (batteria morta, data 1970),
     * la sottrazione andrebbe in underflow a ~2106. Meglio un orologio
     * sballato di 3 s che uno sballato di 80 anni. */
    uint32_t up_s = (uint32_t)udiv64_u32(clock_now_ns(), 1000000000u);
    if (epoch_s < up_s)
    {
        up_s = 0;
    }
    clock_set_boot_epoch(epoch_s - up_s);

    kprintf("[RTC]  Boot epoch: %u (%04u-%02u-%02u %02u:%02u:%02u UTC, "
            "-%us di boot)\n",
            epoch_s - up_s, (uint32_t)t.year, (uint32_t)t.month,
            (uint32_t)t.day, (uint32_t)t.hour, (uint32_t)t.minute,
            (uint32_t)t.second, up_s);
}
