#include "arch/x86/tsc.h"
#include "arch/x86/pit.h"
#include "arch/x86/cpu_features.h"
#include "console/console.h"
#include "sync/atomic.h"

static uint64_t s_tsc_hz;

/* Stato del layer ns-precision (dichiarato extern in tsc.h, usato dagli
 * inline reader ad ogni sito di chiamata — vedi tsc.h). */
tsc_ns_state_t    _tsc_ns_state;
volatile uint32_t _tsc_ns_seq = 0;

/* === Verbi ============================================================== */

/* Il probe CPUID e' ora un'unica componente standard (cpu_features.c,
 * D9): prima ogni chiamante che serviva il TSC lo risondava per conto
 * suo (era anche il caso di questo file). cpu_features_init() gira
 * PRIMA di tsc_init() nella sequenza di boot (kernel.c). */
static bool cpu_has_tsc(void)
{
    return cpu_has(CPUF_TSC);
}

static uint64_t rdtsc(void)
{
    return cpu_rdtsc();
}

/* Misura i cicli TSC in una finestra SINCRONA del PIT (mode 0,
 * polling del readback: vedi pit_calibration_window). Nessun IRQ in
 * gioco: gira prima di cpu_sti e non richiede il motore periodico —
 * sulle macchine con TSC il PIT non genera mai un tick. Precisione al
 * fronte del cristallo (~838 ns su ~50 ms: ~2e-5), dominata dallo
 * skid del RDTSC come sulla 1.0. */
static uint64_t measure_hz_sync(void)
{
    const uint16_t window_ticks = 59659;    /* ~50 ms a 1.193182 MHz */
    uint64_t delta = pit_calibration_window(window_ticks);
    return delta * (uint64_t)PIT_INPUT_HZ / window_ticks;
}

/* mult/shift tali che ns ~= (cicli * mult) >> shift. Sceglie lo shift
 * piu' alto (fino a 32) per cui mult resta entro 32 bit: precisione
 * massima su ogni frequenza plausibile, da un Pentium a 60 MHz a un
 * Core-class a diversi GHz. */
static void compute_mult_shift(uint64_t hz, uint32_t *out_mult,
                               uint32_t *out_shift)
{
    if (hz < 1000000ull) hz = 1000000ull;

    uint32_t shift = 32;
    uint64_t mult;
    do
    {
        mult = (1000000000ull << shift) / hz;
        if (mult <= 0xFFFFFFFFull) break;
        shift--;
    } while (shift > 0);

    *out_mult  = (uint32_t)mult;
    *out_shift = shift;
}

/* Soglia di delta (in cicli) oltre la quale conviene riancorare per
 * IGIENE NUMERICA (delta piccoli, ancora fresca). NON e' piu' un
 * confine di correttezza: _tsc_cycles_to_ns e' esatta su tutto il
 * dominio uint64 — un delta oltre soglia converte comunque giusto. */
static uint64_t overflow_safe_threshold(uint32_t mult)
{
    if (mult == 0) return ~(uint64_t)0;
    return ((uint64_t)1 << 63) / mult;
}

/* === API ================================================================ */

void tsc_init(void)
{
    if (!cpu_has_tsc())
    {
        s_tsc_hz = 0;
        kprintf("[TSC ] Assente: clock a soli tick PIT (1 ms).\n");
        return;
    }

    s_tsc_hz = measure_hz_sync();               /* finestra 50 ms, IF=0 ok */
    kprintf("[TSC ] Calibrato: %u MHz\n",
            (uint32_t)(s_tsc_hz / 1000000ull));

    /* Layer ns-precision: singolo scrittore qui, nessun lettore ancora
     * attivo (il seq resta a 0/pari), niente danza seqlock necessaria. */
    _tsc_ns_state.hz             = s_tsc_hz;
    compute_mult_shift(s_tsc_hz, &_tsc_ns_state.mult, &_tsc_ns_state.shift);
    _tsc_ns_state.anchor_cycles  = rdtsc();
    _tsc_ns_state.anchor_ns      = 0;
    _tsc_ns_state.reanchor_after = overflow_safe_threshold(_tsc_ns_state.mult);
}

uint64_t tsc_read(void)
{
    return (s_tsc_hz != 0) ? rdtsc() : 0;
}

uint64_t tsc_hz(void)
{
    return s_tsc_hz;
}

/* === Layer ns-precision (percorso freddo) =============================== */

void _tsc_ns_reanchor(uint64_t now_cycles, uint64_t now_ns)
{
    /* Scrittore UNICO eletto. irq_save e' locale alla CPU: senza
     * elezione, piu' CPU entrerebbero qui in parallelo — e ci entrano
     * davvero insieme, perche' la soglia di riancoraggio matura ogni
     * ~2 s di delta (2^63/mult a 2-3 GHz) e al confine OGNI lettura di
     * OGNI CPU la attraversa nello stesso istante. Due writer
     * interleaved riportano il seq a PARI con le scritture ancora in
     * volo: i lettori consumano un'ancora strappata (meta' di A, meta'
     * di B). anchor_cycles strappata nel futuro = delta negativo su
     * tutte le CPU = tempo costante: in un kernel tickless nessuna
     * scadenza matura piu'. Strappi piccoli = salti/stalli di secondi.
     *
     * Elezione con xchg (trylock): il riancoraggio e' advisory, chi
     * perde salta e la sua prossima lettura usa l'ancora fresca del
     * vincitore. Dentro l'elezione, check di stantieta': un vincitore
     * appena passato puo' aver gia' spinto l'ancora OLTRE i valori di
     * questo chiamante (calcolati prima dell'elezione) — pubblicare
     * valori vecchi muoverebbe il tempo all'indietro. */
    static volatile uint32_t s_reanchor_busy;

    uint32_t fl = irq_save();
    if (atomic_xchg(&s_reanchor_busy, 1) != 0)
    {
        irq_restore(fl);                /* c'e' gia' un vincitore       */
        return;
    }

    if (now_cycles > _tsc_ns_state.anchor_cycles &&
        now_ns     > _tsc_ns_state.anchor_ns)
    {
        _tsc_ns_seq++;                  /* pari -> dispari              */
        __asm__ volatile ("" ::: "memory");
        _tsc_ns_state.anchor_cycles = now_cycles;
        _tsc_ns_state.anchor_ns     = now_ns;
        __asm__ volatile ("" ::: "memory");
        _tsc_ns_seq++;                  /* dispari -> pari              */
    }

    atomic_set(&s_reanchor_busy, 0);
    irq_restore(fl);
}

uint64_t tsc_ns_to_cycles(uint64_t ns)
{
    uint64_t hz = _tsc_ns_state.hz;
    if (hz == 0) return 0;
    return (ns * hz) / 1000000000ull;
}

#ifdef MAINDOB_SMP
/* === Sync TSC di bring-up (offset per-CPU) ==============================
 *
 * Il bring-up delle AP e' SERIALE (smp_boot_aps avvia una AP alla
 * volta e attende), quindi un solo canale condiviso basta: al piu' una
 * coppia BSP<->AP lo usa in ogni istante.
 *
 * Protocollo a round numerati (contatori, non fasi: un nuovo PING e'
 * distinguibile dal precedente senza stati intermedi):
 *   BSP, round r:  t0=RDTSC; s_ping=r; attende s_pong==r; t1=RDTSC.
 *   AP:            vede s_ping cambiare; t_ap=RDTSC; pubblica t_ap
 *                  (due meta' a 32 bit, POI s_pong=r: su TSO lo store
 *                  di s_pong e' il release che ordina le meta').
 *   Stima: al tempo AP t_ap la BSP era ~ a (t0+t1)/2 (punto medio del
 *   round-trip) -> offset = t0 + rtt/2 - t_ap. Si tiene il campione
 *   col round-trip MINIMO: e' quello con meno rumore (IRQ, contesa
 *   bus), l'errore residuo e' limitato da rtt_min/2.
 *   Chiusura: la BSP pubblica l'offset (meta' + release su s_ping =
 *   SYNC_DONE); l'AP lo ratifica nel PROPRIO slot di _tsc_cpu_off
 *   (unica scrittrice: mai un load strappato dopo la pubblicazione),
 *   risponde s_pong=DONE e la BSP azzera il canale per il prossimo AP.
 *
 * Timeout su ENTRAMBI i lati (deadline in cicli grezzi locali): una
 * controparte muta degrada a offset 0 e il boot prosegue — il sync e'
 * un miglioramento di qualita', mai un single point of failure. */

int64_t _tsc_cpu_off[MAX_CPUS];         /* BSS: BSP (slot 0) resta 0     */

#define TSC_SYNC_ROUNDS   32u
#define TSC_SYNC_DONE     0xFFFFFFFFu
#define TSC_SYNC_TIMEOUT_MS 50u

static volatile uint32_t s_sync_ping;   /* BSP -> AP: numero di round    */
static volatile uint32_t s_sync_pong;   /* AP -> BSP: round confermato   */
static volatile uint32_t s_sync_ap_lo, s_sync_ap_hi;    /* t_ap (meta')  */
static volatile uint32_t s_sync_off_lo, s_sync_off_hi;  /* offset (meta')*/

/* Attesa (bounded, cicli grezzi locali) che *word raggiunga `want`. */
static bool sync_wait_u32(volatile uint32_t *word, uint32_t want)
{
    uint64_t budget   = (s_tsc_hz / 1000ull) * TSC_SYNC_TIMEOUT_MS;
    uint64_t deadline = rdtsc() + budget;
    while (*word != want)
    {
        if (rdtsc() > deadline)
        {
            return false;
        }
        __asm__ volatile ("pause");
    }
    return true;
}

/* Lato BSP: guida i round e stima l'offset dell'AP. */
void tsc_smp_sync_bsp(uint32_t ap_slot)
{
    if (s_tsc_hz == 0 || ap_slot == 0 || ap_slot >= (uint32_t)MAX_CPUS)
    {
        return;
    }

    uint64_t best_rtt = ~(uint64_t)0;
    int64_t  best_off = 0;
    bool     ok       = true;

    uint32_t fl = irq_save();
    for (uint32_t r = 1; r <= TSC_SYNC_ROUNDS; r++)
    {
        uint64_t t0 = rdtsc();
        s_sync_ping = r;
        if (!sync_wait_u32(&s_sync_pong, r))
        {
            ok = false;
            break;
        }
        uint64_t t1   = rdtsc();
        uint64_t t_ap = ((uint64_t)s_sync_ap_hi << 32) | s_sync_ap_lo;

        uint64_t rtt = t1 - t0;
        if (rtt < best_rtt)
        {
            best_rtt = rtt;
            best_off = (int64_t)(t0 + rtt / 2) - (int64_t)t_ap;
        }
    }

    if (ok)
    {
        s_sync_off_lo = (uint32_t)((uint64_t)best_off);
        s_sync_off_hi = (uint32_t)((uint64_t)best_off >> 32);
        s_sync_ping   = TSC_SYNC_DONE;      /* release delle meta'       */
        ok = sync_wait_u32(&s_sync_pong, TSC_SYNC_DONE);
    }

    s_sync_ping = 0;                        /* canale pulito, prossimo AP */
    s_sync_pong = 0;
    irq_restore(fl);

    if (!ok)
    {
        kprintf("[TSC ] AP slot %u: sync TSC non confermato (timeout), "
                "offset 0.\n", ap_slot);
        return;
    }

    /* Log in ns con segno (il formatter e' a 32 bit: sotto i 4 s si sta
     * larghi, il tipico offset e' ~us o zero). */
    uint64_t mag_cyc = (best_off < 0) ? (uint64_t)(-best_off)
                                      : (uint64_t)best_off;
    uint32_t mag_ns  = (uint32_t)_tsc_cycles_to_ns(mag_cyc,
                                                   _tsc_ns_state.mult,
                                                   _tsc_ns_state.shift);
    kprintf("[TSC ] AP slot %u: TSC sincronizzato (offset %s%u ns, "
            "rtt min %u cicli).\n", ap_slot,
            (best_off < 0) ? "-" : "+", mag_ns, (uint32_t)best_rtt);
}

/* Lato AP: risponde ai round e ratifica l'offset nel proprio slot.
 * Chiamare PRIMA di entrare nello scheduler: dopo la ratifica nessuno
 * scrive piu' _tsc_cpu_off[ap_slot]. */
void tsc_smp_sync_ap(uint32_t ap_slot)
{
    if (s_tsc_hz == 0 || ap_slot == 0 || ap_slot >= (uint32_t)MAX_CPUS)
    {
        return;
    }

    uint32_t fl   = irq_save();
    uint32_t last = 0;

    for (;;)
    {
        /* Attende un cambiamento di s_sync_ping (nuovo round o DONE). */
        uint64_t budget   = (s_tsc_hz / 1000ull) * TSC_SYNC_TIMEOUT_MS;
        uint64_t deadline = rdtsc() + budget;
        while (s_sync_ping == last)
        {
            if (rdtsc() > deadline)
            {
                irq_restore(fl);        /* BSP muta: offset resta 0     */
                kprintf("[TSC ] slot %u: sync TSC senza BSP (timeout).\n",
                        ap_slot);
                return;
            }
            __asm__ volatile ("pause");
        }

        uint32_t ping = s_sync_ping;
        if (ping == TSC_SYNC_DONE)
        {
            /* Le meta' dell'offset sono state pubblicate PRIMA dello
             * store di DONE (release TSO): lettura sicura. */
            uint64_t off = ((uint64_t)s_sync_off_hi << 32) | s_sync_off_lo;
            _tsc_cpu_off[ap_slot] = (int64_t)off;
            s_sync_pong = TSC_SYNC_DONE;
            break;
        }

        uint64_t t_ap = rdtsc();
        s_sync_ap_lo  = (uint32_t)t_ap;
        s_sync_ap_hi  = (uint32_t)(t_ap >> 32);
        s_sync_pong   = ping;               /* release delle meta'      */
        last          = ping;
    }

    irq_restore(fl);
}
#endif /* MAINDOB_SMP */
