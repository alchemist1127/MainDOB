#include "time/clock.h"
#include "time/civil.h"
#include "console/console.h"

/* Questo file possiede: la scelta della sorgente monotona, l'epoca di
 * boot, la decomposizione wall-clock per SYS_GETTIME. Il monotono in
 * se' vive in arch (tsc.c / pit.c): qui c'e' solo l'orchestrazione. */

bool            _clock_tsc_ok;          /* letto inline da clock_now_ns */
#ifdef MAINDOB_SMP
volatile uint64_t _clock_last_ns __attribute__((aligned(8)));
#endif
static uint32_t s_boot_epoch_s;

void clock_init(void)
{
    _clock_tsc_ok = (tsc_hz() != 0);

    if (_clock_tsc_ok)
    {
        /* tsc_hz e' uint64 ma ogni CPU bersaglio sta sotta i 4 GHz:
         * stampa in MHz per restare nei 32 bit del formatter. */
        kprintf("[CLK]  Monotono: TSC calibrato (%u MHz)\n",
                (uint32_t)udiv64_u32(tsc_hz(), 1000000u));
    }
    else
    {
        kprintf("[CLK]  Monotono: tick PIT %u Hz (TSC non disponibile)\n",
                PIT_HZ);
    }
}

void clock_set_boot_epoch(uint32_t epoch_s)
{
    s_boot_epoch_s = epoch_s;
}

uint32_t clock_boot_epoch_s(void)
{
    return s_boot_epoch_s;
}

void clock_get_realtime(dob_realtime_t *out)
{
    if (out == NULL)
    {
        return;
    }

    /* Wall-clock: l'UNICO consumatore del monotono globale STRETTO
     * (clampato). Vedi clock.h: percorso freddo via syscall. */
    uint64_t now_ns  = clock_now_ns_global();
    uint64_t total_s = (uint64_t)s_boot_epoch_s
                     + udiv64_u32(now_ns, 1000000000U);
    uint32_t sub_ms  = umod64_u32(ns_to_ms(now_ns), 1000U);

    civil_t c;
    unix_seconds_to_civil(total_s, &c);

    out->year   = c.year;
    out->month  = c.month;
    out->day    = c.day;
    out->hour   = c.hour;
    out->minute = c.minute;
    out->second = c.second;
    out->ms     = sub_ms;
}
