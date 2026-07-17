#ifndef MAINDOB_TIME_CLOCK_H
#define MAINDOB_TIME_CLOCK_H

/* Orologio canonico del kernel.
 *
 * Due unita', entrambe uint64:
 *   ns monotoni   — nanosecondi dal boot. Timestamp autorevole per
 *                   deadline dello scheduler, armo timer, sleep,
 *                   ritorni di syscall.
 *   ns wall-clock — boot_epoch + monotono, calcolato a richiesta per
 *                   SYS_GETTIME. Zero I/O dopo il boot.
 *
 * Sorgente monotona: clock_init() sceglie una volta —
 *   TSC calibrato (tsc_hz != 0)  -> tsc_now_ns(), precisione ns;
 *   TSC assente/inaffidabile     -> tick PIT a 1000 Hz, risoluzione
 *                                   1 ms, comunque monotono.
 * La scelta e' un flag letto inline: un branch previsto, niente
 * puntatori a funzione sul percorso caldo. */

#include "lib/types.h"
#include "time/units.h"
#include "arch/x86/tsc.h"
#include "arch/x86/pit.h"

/* Decomposizione del tempo reale per SYS_GETTIME. Il layout E' l'ABI
 * userspace (7 uint32, in quest'ordine): non cambiare. */
typedef struct
{
    uint32_t year;
    uint32_t month;
    uint32_t day;
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
    uint32_t ms;
} dob_realtime_t;

/* Inizializza il monotono: sceglie la sorgente (TSC o PIT) e la
 * annuncia. Precondizioni: pit_init() e tsc_init() gia' eseguite. */
void clock_init(void);

/* Consegna dell'epoca di boot (secondi Unix), chiamata da rtc_init().
 * Puo' arrivare prima o dopo clock_init(): i due stati sono
 * indipendenti. */
void clock_set_boot_epoch(uint32_t epoch_s);

/* Epoca di boot in secondi Unix (0 se CMOS illeggibile). */
uint32_t clock_boot_epoch_s(void);

/* Percorso caldo: nanosecondi monotoni dal boot. */
extern bool _clock_tsc_ok;

#ifdef MAINDOB_SMP
/* Clamp monotono GLOBALE — SOLO percorso freddo (wall-clock).
 *
 * Storia: fino alla build 114 OGNI clock_now_ns passava di qui: un
 * lock cmpxchg8b su una singola cache line condivisa, a ogni lettura
 * del clock da ogni CPU — e il clock si legge in switch_to, slice
 * check, time_event_refresh, timer. La linea rimbalzava di continuo
 * tra i core: terza radice del multicore-piu'-lento.
 *
 * Oggi lo sfasamento tra i TSC e' corretto ALLA FONTE: al bring-up di
 * ogni AP un handshake ping-pong misura l'offset del suo TSC rispetto
 * alla BSP (tsc_smp_sync_*, arch/x86/tsc.c) e ogni lettura lo applica
 * per-CPU. Il residuo e' l'errore dell'handshake (ordine di grandezza:
 * il round-trip minimo, frazioni di us). Le logiche a deadline
 * (heap timer, quanto, sleep) fanno solo CONFRONTI ora>=scadenza:
 * uno skew cosi' piccolo sposta il fuoco di frazioni di us, mai
 * underflow ne' stallo — e l'aritmetica trascorso=ora-inizio e'
 * per-thread, quindi per-CPU (thread pinnati, mai migrati).
 *
 * Il clamp resta per l'unico consumatore che promette monotonia
 * STRETTA globale osservabile: il wall-clock di SYS_GETTIME
 * (clock_get_realtime) — due processi su core diversi non devono mai
 * vedere il tempo civile arretrare, nemmeno di un ns. Percorso
 * syscall: freddo per definizione.
 * cmpxchg8b: il target i686 (P6) lo garantisce. */
extern volatile uint64_t _clock_last_ns;

static inline uint64_t _clock_monotonic_clamp(uint64_t ns)
{
    uint64_t last = _clock_last_ns;
    for (;;)
    {
        if (ns <= last)
        {
            return last;                /* qualcuno e' gia' piu' avanti */
        }
        uint64_t prev;
        __asm__ volatile ("lock cmpxchg8b %1"
                          : "=A"(prev), "+m"(_clock_last_ns)
                          : "b"((uint32_t)ns), "c"((uint32_t)(ns >> 32)),
                            "0"(last)
                          : "cc");
        if (prev == last)
        {
            return ns;                  /* fronte avanzato da noi        */
        }
        last = prev;                    /* race: rivaluta col nuovo last */
    }
}
#endif

/* Percorso CALDO: monotono per-CPU (TSC gia' corretto dall'offset di
 * sync, vedi sopra). Nessun accesso a stato condiviso in scrittura. */
static inline uint64_t clock_now_ns(void)
{
    uint64_t raw;
    if (likely(_clock_tsc_ok))
    {
        raw = tsc_now_ns();
    }
    else
    {
        raw = MS_TO_NS(pit_uptime_ms());
    }
    return raw;
}

/* Percorso FREDDO: monotono globale STRETTO (mai all'indietro su
 * nessuna coppia di CPU). Solo per il tempo user-visible. */
static inline uint64_t clock_now_ns_global(void)
{
#ifdef MAINDOB_SMP
    return _clock_monotonic_clamp(clock_now_ns());
#else
    return clock_now_ns();
#endif
}

/* Comodita': via reciprocal-multiply di units.h (una mul+shift al
 * posto della coppia di divl — ~5x su silicio classe Pentium). */
static inline uint64_t clock_now_us(void) { return ns_to_us(clock_now_ns()); }
static inline uint64_t clock_now_ms(void) { return ns_to_ms(clock_now_ns()); }

/* Wall-clock: scompone adesso in anno/.../ms. Pura aritmetica su
 * epoca + monotono — niente CMOS, niente I/O. */
void clock_get_realtime(dob_realtime_t *out);

#endif /* MAINDOB_TIME_CLOCK_H */
