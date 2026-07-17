#ifndef MAINDOB_ARCH_TSC_H
#define MAINDOB_ARCH_TSC_H

#include "lib/types.h"
#include "arch/x86/cpu.h"
#include "proc/percpu.h"    /* MAX_CPUS + this_cpu: offset TSC per-CPU */

/* TSC calibrato contro il PIT al boot. Su ferro senza TSC affidabile
 * (o CPUID senza il flag) tsc_hz() ritorna 0 e il clock ripiega sui
 * soli tick del PIT (risoluzione 1 ms) senza altre conseguenze.
 *
 * === Layer ns-precision (milestone SMP, osservato dal kernel 1.0) ===
 * tsc_now_ns()/tsc_snapshot() aggiungono una lettura a precisione di
 * nanosecondo, protetta da seqlock (l'ancora a 64 bit non e' atomica su
 * i386): ns = ancora_ns + ((cicli - ancora_cicli) * mult) >> shift.
 * mult/shift sono immutabili dopo tsc_init(); solo l'ancora si aggiorna,
 * col riancoraggio a evento (mai periodico) quando il delta supera la
 * soglia di igiene numerica. La conversione cicli->ns e' ESATTA su
 * tutto il dominio (vedi _tsc_cycles_to_ns): il riancoraggio tiene i
 * delta piccoli, ma un delta arbitrariamente grande — sistema fermo in
 * hlt per ore ad agenda vuota — converte comunque al ns giusto.
 *
 * Questo layer e' cio' che serve a arch/x86/lapic.c per calibrare e
 * armare il timer one-shot/TSC-deadline per-core: e' additivo, non
 * sostituisce time/clock.c (che resta sul tick PIT + rifinitura TSC
 * fino a quando il tick source diventa per-core LAPIC). */

void     tsc_init(void);
uint64_t tsc_read(void);
uint64_t tsc_hz(void);

typedef struct
{
    uint64_t hz;
    uint64_t anchor_cycles;
    uint64_t anchor_ns;
    uint64_t reanchor_after;
    uint32_t mult;
    uint32_t shift;
} tsc_ns_state_t;

extern tsc_ns_state_t     _tsc_ns_state;
extern volatile uint32_t  _tsc_ns_seq;

#ifdef MAINDOB_SMP
/* Offset per-CPU (in cicli) applicato a ogni RDTSC: allinea il TSC di
 * ogni AP a quello della BSP (riferimento, offset 0). Misurato una
 * volta dall'handshake di bring-up (tsc_smp_sync_*): dopo, le letture
 * cross-CPU divergono al piu' dell'errore dell'handshake (~ meta' del
 * round-trip minimo), abbastanza da rendere superfluo qualunque clamp
 * globale sul percorso caldo (vedi time/clock.h).
 *
 * Disciplina di scrittura: _tsc_cpu_off[slot] e' scritto SOLO dall'AP
 * proprietaria, UNA volta, dentro tsc_smp_sync_ap, PRIMA che quell'AP
 * entri nello scheduler — dopo la pubblicazione nessuno scrive piu':
 * il load a 64 bit non atomico dell'i386 non puo' mai strappare. */
extern int64_t _tsc_cpu_off[MAX_CPUS];

/* Handshake di sincronizzazione TSC al bring-up (bring-up SERIALE: un
 * solo AP alla volta usa il canale condiviso). La BSP guida i round
 * ping-pong e stima l'offset dal campione a round-trip minimo; l'AP
 * risponde e ratifica l'offset nel proprio slot. Entrambe degradano
 * con timeout (offset 0) se la controparte non risponde: mai un boot
 * bloccato per un sync mancato. */
void tsc_smp_sync_bsp(uint32_t ap_slot);
void tsc_smp_sync_ap(uint32_t ap_slot);
#endif

void _tsc_ns_reanchor(uint64_t now_cycles, uint64_t now_ns);

/* RDTSC gia' corretto dall'offset per-CPU: e' il "cicli" canonico di
 * tutto il layer ns. Su UP l'offset non esiste: RDTSC nudo. */
static inline uint64_t _tsc_local_cycles(void)
{
#ifdef MAINDOB_SMP
    return cpu_rdtsc() + (uint64_t)_tsc_cpu_off[this_cpu()->cpu_index];
#else
    return cpu_rdtsc();
#endif
}

/* Riconversione a cicli GREZZI di questa CPU. L'unico consumatore
 * legittimo e' chi parla con hardware che confronta il TSC nudo —
 * oggi solo la scrittura dell'MSR IA32_TSC_DEADLINE (lapic_arm_tscd).
 * Contratto: TUTTE le interfacce in cicli del kernel viaggiano
 * CORRETTE; la conversione a grezzo avviene una sola volta, al filo
 * dell'hardware. */
static inline uint64_t tsc_cycles_to_raw(uint64_t corrected)
{
#ifdef MAINDOB_SMP
    return corrected - (uint64_t)_tsc_cpu_off[this_cpu()->cpu_index];
#else
    return corrected;
#endif
}

/* Conversione cicli -> ns ESATTA su tutto il dominio uint64 del delta.
 *
 * Il prodotto pieno delta * mult e' a 96 bit: (hi*mult) * 2^32 + lo*mult.
 * La vecchia forma lo accumulava in un uint64 e troncava per
 * delta >= 2^64/mult (secondi di quiete a frequenze GHz): il primo
 * lettore dopo un lungo silenzio otteneva ns spazzatura e il
 * riancoraggio la pubblicava — perdita permanente di tempo monotono.
 *
 * Forma esatta senza mai materializzare i 96 bit: con shift <= 32
 * (invariante di compute_mult_shift, che parte da 32 e solo decrementa)
 *
 *     (delta * mult) >> shift
 *   = ((hi*mult) * 2^32 + lo*mult) >> shift
 *   = (hi*mult) << (32 - shift)  +  (lo*mult) >> shift
 *
 * L'uguaglianza e' esatta: il termine alto e' multiplo di 2^shift,
 * quindi i suoi bit non interagiscono con i bit bassi scartati dal
 * termine basso — nessun riporto perso. Il risultato sta in 64 bit per
 * ogni uptime < 2^64 ns (~584 anni), lo stesso confine dichiarato da
 * time/units.h per l'intero kernel.
 *
 * Conseguenza architetturale: il riancoraggio (vedi _tsc_ns_read) NON
 * e' piu' un requisito di correttezza ma solo igiene numerica — tiene
 * i delta piccoli e l'ancora fresca. Un sistema fermo in hlt per ore
 * ad agenda vuota si sveglia con il tempo GIUSTO. */
static inline uint64_t _tsc_cycles_to_ns(uint64_t delta, uint32_t mult,
                                         uint32_t shift)
{
    uint64_t lo_prod = (uint64_t)(uint32_t)delta         * mult;
    uint64_t hi_prod = (uint64_t)(uint32_t)(delta >> 32) * mult;
    return (lo_prod >> shift) + (hi_prod << (32u - shift));
}

static inline void _tsc_ns_read(uint64_t *out_cycles, uint64_t *out_ns)
{
    const uint32_t mult  = _tsc_ns_state.mult;
    const uint32_t shift = _tsc_ns_state.shift;

    uint32_t s1, s2;
    uint64_t cycles, anchor_cycles, anchor_ns;

    /* for(;;) + break esplicito, NON do-while + continue: in un
     * do-while, `continue` salta DIRETTAMENTE alla valutazione della
     * condizione finale, non in cima al corpo. Col seqlock dispari
     * (scrittore in corso) quel salto avrebbe valutato `s1 != s2` con
     * s2 mai assegnata in questo giro — lettura di variabile non
     * inizializzata. Qui `continue` rientra dalla cima del for, quindi
     * s1/s2 sono sempre entrambe fresche prima di qualunque confronto. */
    for (;;)
    {
        s1 = _tsc_ns_seq;
        if (unlikely(s1 & 1u))
        {
            __asm__ volatile ("pause");
            continue;
        }
        __asm__ volatile ("" ::: "memory");

        cycles        = _tsc_local_cycles();
        anchor_cycles = _tsc_ns_state.anchor_cycles;
        anchor_ns     = _tsc_ns_state.anchor_ns;

        __asm__ volatile ("" ::: "memory");
        s2 = _tsc_ns_seq;
        if (s1 == s2)
        {
            break;
        }
    }

    uint64_t delta = cycles - anchor_cycles;

    /* Guard SMP: se questa CPU ha il TSC (corretto) INDIETRO rispetto a
     * quello che ha scritto l'ancora, `cycles - anchor_cycles`
     * underflowa e wrappa verso 2^64: il prodotto tronca, e il
     * riancoraggio qui sotto avrebbe riscritto l'ancora con quella
     * spazzatura, teletrasportando il clock. Delta negativo = "il tempo
     * per questa CPU non e' ancora arrivato all'ancora": si risponde
     * l'ancora stessa e NON si rianca mai da qui. Dopo il sync di
     * bring-up il caso e' confinato al residuo dell'handshake
     * (frazioni di us di tempo fermo, mai all'indietro su questa CPU). */
    if (unlikely((int64_t)delta < 0))
    {
        delta = 0;
    }

    uint64_t ns    = anchor_ns + _tsc_cycles_to_ns(delta, mult, shift);

    *out_cycles = cycles;
    *out_ns     = ns;

    if (unlikely(delta != 0 && delta >= _tsc_ns_state.reanchor_after))
    {
        _tsc_ns_reanchor(cycles, ns);
    }
}

typedef struct
{
    uint64_t cycles;
    uint64_t ns;
} tsc_snapshot_t;

/* Nanosecondi monotoni dal boot, precisione TSC. Valida SOLO dopo che
 * tsc_init() ha calibrato con successo (tsc_hz() != 0); su TSC assente
 * ritorna comunque una progressione coerente ancorata a 0 (mult/shift
 * restano 0 -> ns non avanza: i chiamanti SMP-only, es. lapic.c, sono
 * raggiungibili solo quando cpu_has(CPUF_APIC) e quindi TSC e' gia'
 * garantito dal floor di cpu_features_init). */
static inline uint64_t tsc_now_ns(void)
{
    uint64_t cycles, ns;
    _tsc_ns_read(&cycles, &ns);
    (void)cycles;
    return ns;
}

static inline void tsc_snapshot(tsc_snapshot_t *out)
{
    _tsc_ns_read(&out->cycles, &out->ns);
}

/* Attesa attiva sub-microsecondo, solo RDTSC. Per temporizzazioni driver
 * e ritardi di init — qualunque attesa >= 100 us dovrebbe armare un
 * timer dello scheduler invece di girare qui. PAUSE nello spin aiuta il
 * branch predictor ed evita di saturare il bus. */
static inline void tsc_busy_wait_ns(uint64_t ns)
{
    if (ns == 0) return;
    uint64_t target = tsc_now_ns() + ns;
    while (tsc_now_ns() < target)
    {
        __asm__ volatile ("pause");
    }
}

/* Converte una durata in ns in cicli TSC grezzi. Usato da
 * lapic_arm_at_ns() sul percorso TSC-deadline. */
uint64_t tsc_ns_to_cycles(uint64_t ns);

#endif
