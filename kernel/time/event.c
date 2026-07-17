#include "time/event.h"
#include "time/timer.h"
#include "time/clock.h"
#include "time/tick_source.h"
#include "arch/x86/lapic.h"
#include "arch/x86/pit.h"
#include "arch/x86/tsc.h"
#include "arch/x86/cpu.h"
#include "proc/thread.h"
#include "proc/scheduler.h"
#include "proc/thread.h"
#include "console/console.h"

static bool s_event_mode;

/* Backend hardware delle scadenze, scelto UNA volta in
 * time_event_try_enable (LAPIC, o PIT one-shot dove il LAPIC manca —
 * Armada-class). Da li' in poi il motore non ramifica piu'
 * sull'hardware: arm/disarm passano da qui. */
static const struct tick_source *s_src;

bool time_event_mode(void)
{
    return s_event_mode;
}

/* Orologio puro: arma la PROSSIMA scadenza TEMPORALE — il minimo tra la
 * testa dell'agenda timer e la fine del quanto del thread in corso.
 * NIENTE altro. In particolare NON e' piu' il corriere dei risvegli: un
 * thread che diventa eseguibile non e' un evento temporale e non passa
 * di qui. La sua consegna e' event-driven (enqueue_home_and_kick ->
 * need_resched onorato al prossimo epilogo di uscita kernel; IPI di
 * resched verso una CPU idle su SMP). Per questo NON esiste piu' alcun
 * floor idle: non c'e' nulla da rattoppare col polling, perche' nessun
 * risveglio dipende da questa funzione. */
void time_event_refresh(void)
{
    if (!s_event_mode)
    {
        return;
    }

    uint64_t next = timer_next_deadline_ns();
    uint64_t slice = scheduler_slice_deadline_ns();
    if (slice < next)
    {
        next = slice;
    }

    if (next == UINT64_MAX)
    {
        s_src->disarm();                /* niente in agenda: silenzio */
        return;
    }
    s_src->arm_deadline_ns(next);
}

/* ISR della sorgente eventi (post-EOI, IF=0) — LAPIC o PIT one-shot,
 * identica per entrambe: drena le scadenze mature, verifica il quanto,
 * switcha nel punto consolidato, si riarma. Sul PIT i fuochi intermedi
 * del riarmo impilato NON arrivano qui (filtrati nel backend). */
static void event_fire(void)
{
    timer_on_tick();                    /* percorso vuoto: un confronto */
    scheduler_slice_check();
    scheduler_preempt_if_needed();
    time_event_refresh();
}

void time_event_try_enable(void)
{
    /* Unico prerequisito assoluto: il TSC come monotono. Le scadenze
     * sono ns assoluti su quella linea, per QUALUNQUE backend; senza
     * TSC calibrato il kernel resta sul PIT periodico a 1000 Hz —
     * l'estremo fallback, non piu' il destino di ogni macchina senza
     * LAPIC. */
    if (tsc_hz() == 0)
    {
        kprintf("[TIME] Modo eventi non disponibile (TSC non "
                "calibrato): resta il PIT periodico a %u Hz.\n", PIT_HZ);
        return;
    }

    /* Backend per capacita' hardware: LAPIC se usabile, altrimenti PIT
     * one-shot (mode 0, riarmo impilato nel backend) — la stessa
     * macchina event-driven della 1.0 sulle macchine senza APIC
     * (Armada E500: Celeron con LAPIC fuso). install() sostituisce
     * l'ISR periodico sul ramo PIT ed e' un no-op sul LAPIC. */
    s_src = tick_source_select();
    s_src->install();
    s_src->register_callback(event_fire);
    s_event_mode = true;

    /* Nessun periodico da zittire: sulle macchine con TSC il PIT non
     * ha mai generato un tick (calibrazione sincrona in stage_time).
     * Sul ramo PIT one-shot, IRQ0 e' la sorgente, gia' riprogrammata
     * da install. */

    uint32_t fl = irq_save();
    time_event_refresh();
    irq_restore(fl);

    if (s_src == &tick_source_lapic)
    {
        kprintf("[TIME] Modo eventi attivo (LAPIC %s; PIT solo "
                "calibrazione).\n",
                lapic_timer_is_tsc_deadline() ? "TSC-deadline"
                                              : "one-shot");
    }
    else
    {
        kprintf("[TIME] Modo eventi attivo (sorgente: %s).\n",
                s_src->name);
    }
}
