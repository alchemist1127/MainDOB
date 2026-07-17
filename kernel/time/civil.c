#include "time/civil.h"

/* Una sola tabella giorni-mese per entrambi i versi della conversione:
 * composizione e decomposizione non possono divergere. */

static const uint8_t  days_in_month[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};
static const uint16_t days_before_month[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

/* Giorni del mese `m` (1..12) nell'anno `year`, febbraio incluso. */
static uint32_t month_days(uint32_t year, uint32_t m)
{
    if (m == 2 && is_leap_year(year))
    {
        return 29;
    }
    return days_in_month[m - 1];
}

uint64_t civil_to_unix_seconds(const civil_t *c)
{
    if (c == NULL || c->year < 1970 || c->month < 1 || c->month > 12
        || c->day < 1)
    {
        return 0;
    }

    uint32_t days = 0;

    /* Anni pieni dal 1970. */
    for (uint32_t y = 1970; y < c->year; y++)
    {
        days += is_leap_year(y) ? 366 : 365;
    }

    /* Giorni prima di questo mese, in quest'anno. */
    days += days_before_month[c->month - 1];
    if (c->month > 2 && is_leap_year(c->year))
    {
        days += 1;
    }

    /* Giorni dentro il mese. */
    days += c->day - 1;

    return (uint64_t)days * 86400ULL
         + (uint64_t)c->hour * 3600ULL
         + (uint64_t)c->minute * 60ULL
         + (uint64_t)c->second;
}

void unix_seconds_to_civil(uint64_t total, civil_t *out)
{
    if (out == NULL)
    {
        return;
    }

    uint32_t s_of_day = umod64_u32(total, 86400U);
    uint64_t days     = udiv64_u32(total, 86400U);

    out->second = s_of_day % 60;
    s_of_day   /= 60;
    out->minute = s_of_day % 60;
    out->hour   = s_of_day / 60;

    /* Anno: cammina dal 1970 sottraendo anni pieni. */
    uint32_t year = 1970;
    for (;;)
    {
        uint32_t yd = is_leap_year(year) ? 366 : 365;
        if (days < yd)
        {
            break;
        }
        days -= yd;
        year++;
    }
    out->year = year;

    /* Mese dentro l'anno — stessa tabella della composizione. */
    uint32_t month = 1;
    for (uint32_t m = 1; m <= 12; m++)
    {
        uint32_t md = month_days(year, m);
        if (days < md)
        {
            month = m;
            break;
        }
        days -= md;
    }
    out->month = month;
    out->day   = (uint32_t)days + 1;
}
