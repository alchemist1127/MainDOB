#include "arch/x86/pit.h"
#include "arch/x86/irq.h"
#include "arch/x86/ports.h"
#include "console/console.h"
#include "time/timer.h"
#include "time/tick_source.h"
#include "proc/scheduler.h"
#include "krt/entropy.h"
#include "arch/x86/tsc.h"
#include "arch/x86/cpu.h"

#define PIT_CH0_DATA 0x40
#define PIT_CMD      0x43

static volatile uint64_t s_ticks;

/* === Verbi ============================================================== */

static void program_channel0(uint32_t hz)
{
    uint32_t divisor = PIT_INPUT_HZ / hz;

    outb(PIT_CMD, 0x36);                        /* ch0, lo/hi, mode 3     */
    outb(PIT_CH0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0_DATA, (uint8_t)(divisor >> 8));
}

/* === One-shot (mode 0): backend tick source ============================
 *
 * Tutto questo blocco serve SOLO quando il LAPIC non c'e' (Armada-class)
 * e il kernel entra comunque in modo eventi: canale 0 riprogrammato per
 * SCADENZA, non per periodo. Mode 0 ("interrupt on terminal count"): la
 * scrittura del byte alto carica e avvia il conteggio; a zero, OUT sale
 * e IRQ0 spara una volta.
 *
 * Contatore a 16 bit su cristallo 1.193182 MHz -> orizzonte massimo di
 * un singolo armamento 65535/1193182 s ~= 54.925 ms. Scadenze piu'
 * lontane: l'armamento viene troncato al tetto e l'ISR RIARMA con il
 * delta residuo (riarmo impilato) finche' la scadenza vera non e'
 * raggiunta — solo allora invoca la callback. Il chiamante non vede
 * alcuna differenza rispetto al LAPIC.
 *
 * Concorrenza: stato letto/scritto solo con IF=0 (l'arm arriva da
 * time_event_refresh sotto irq_save o dall'ISR stesso; l'ISR gira con
 * IF=0 fino all'iret). Niente lock. Il backend e' globale (un solo
 * 8254): senza LAPIC non ci sono AP, quindi UP garantito.
 */

#define PIT_CMD_CH0_LOHI_MODE0   0x30      /* ch0, lo/hi, mode 0, binario */
#define PIT_ONESHOT_MAX_COUNT    0xFFFFu
#define PIT_ONESHOT_MAX_DELTA_NS 54925450ull

/* Scadenza assoluta programmata e flag di armamento. Con s_os_armed
 * falso, s_os_target_ns non ha significato. */
static volatile uint64_t s_os_target_ns;
static volatile bool     s_os_armed;

/* Callback del motore eventi; invocata dall'ISR solo a scadenza REALE.
 * NULL finche' non registrata (ISR muto). */
static tick_source_cb_t  s_os_cb;

/* Riprogramma il canale 0 in mode 0 con il conteggio dato. IF=0
 * obbligatorio nel chiamante: fra i due outb il contatore e' mezzo
 * caricato, e un altro sito di armamento interposto lo corromperebbe. */
static void os_program_count(uint32_t count)
{
    outb(PIT_CMD, PIT_CMD_CH0_LOHI_MODE0);
    outb(PIT_CH0_DATA, (uint8_t)(count & 0xFF));
    outb(PIT_CH0_DATA, (uint8_t)((count >> 8) & 0xFF));
}

/* Finestra di calibrazione SINCRONA (semantica 1.0): mode 0 con il
 * conteggio dato, poi polling del readback finche' OUT sale — nessun
 * IRQ, nessun tick contato, gira anche a IF=0. Ritorna il delta TSC
 * misurato sui fronti esatti della finestra: lo snapshot t0 avviene
 * fra byte basso e alto (il conteggio parte alla scrittura del byte
 * alto), t1 al fronte OUT. E' la primitiva della calibrazione TSC e
 * dell'attesa di boot senza TSC (con delta ignorato). Mode 0 e' usato
 * apposta: e' l'unico in cui OUT sale ESATTAMENTE al conteggio zero,
 * quindi il readback da' un'attesa allineata al fronte. */
uint64_t pit_calibration_window(uint16_t delay_ticks)
{
    if (delay_ticks == 0) delay_ticks = 1;

    outb(PIT_CMD, PIT_CMD_CH0_LOHI_MODE0);
    outb(PIT_CH0_DATA, (uint8_t)(delay_ticks & 0xFF));

    /* RDTSC CRUDO (cpu_rdtsc), non la lettura ancorata di tsc.h: la
     * finestra gira PRIMA di tsc_init — l'ancora non esiste ancora e
     * tsc_read restituirebbe zero (la regressione "Calibrato: 0 MHz",
     * che trascinava anche la calibrazione del timer LAPIC e il
     * fallback periodico su macchine col TSC sano). */
    uint64_t t0 = cpu_rdtsc();
    outb(PIT_CH0_DATA, (uint8_t)((delay_ticks >> 8) & 0xFF));

    for (;;)
    {
        outb(PIT_CMD, 0xE2);            /* readback: latch status ch0 */
        if (inb(PIT_CH0_DATA) & 0x80)   /* bit 7 = livello OUT        */
            break;
    }
    return cpu_rdtsc() - t0;
}

/* ns -> conteggio PIT. L'ingresso e' gia' troncato al tetto, quindi il
 * prodotto sta in 46 bit; la divisione usa udiv64_u32 (due divl, come
 * ovunque nel kernel a 32 bit). Conteggio 0 in mode 0 significa 65536
 * — l'opposto di "subito": si forza 1 tick. */
static uint32_t os_ns_to_count(uint64_t delta_ns)
{
    uint64_t count = udiv64_u32(delta_ns * (uint64_t)PIT_INPUT_HZ,
                                1000000000u);
    if (count == 0) return 1;
    if (count > PIT_ONESHOT_MAX_COUNT) return PIT_ONESHOT_MAX_COUNT;
    return (uint32_t)count;
}

/* Arma il residuo fino a s_os_target_ns. Scadenza gia' passata: 1 tick
 * cosi' l'ISR spara il prima possibile. Da chiamare solo armati. */
static void os_arm_remaining(uint64_t now_ns)
{
    if (s_os_target_ns <= now_ns)
    {
        os_program_count(1);
        return;
    }
    uint64_t delta_ns = s_os_target_ns - now_ns;
    if (delta_ns > PIT_ONESHOT_MAX_DELTA_NS)
        delta_ns = PIT_ONESHOT_MAX_DELTA_NS;
    os_program_count(os_ns_to_count(delta_ns));
}

/* ISR IRQ0 in modo one-shot. Fuoco intermedio del riarmo impilato:
 * riprogramma il prossimo tratto e resta armato, SENZA callback. Fuoco
 * a scadenza reale: si disarma PRIMA di invocare la callback, cosi' un
 * riarmo dentro la callback (time_event_refresh) trova stato pulito.
 * Fuoco spurio (residuo dopo un disarm): ignorato. */
static void os_isr(isr_regs_t *regs UNUSED)
{
    entropy_add((uint32_t)tsc_read());  /* stessa dieta del periodico */

    if (!s_os_armed)
        return;

    uint64_t now_ns = tsc_now_ns();
    if (s_os_target_ns > now_ns)
    {
        os_arm_remaining(now_ns);
        return;
    }

    s_os_armed = false;
    if (s_os_cb != NULL)
        s_os_cb();
}

static void on_tick(isr_regs_t *regs UNUSED)
{
    s_ticks++;
    entropy_add((uint32_t)tsc_read());  /* jitter: costo di un rdtsc      */
    timer_on_tick();                    /* scadenze (deferral attivo)     */
    scheduler_tick();                   /* contabilita' del quanto        */
    /* Lo switch eventuale avviene post-EOI (hook del dispatcher IRQ). */
}

/* === Orchestratore ====================================================== */

/* Motore PERIODICO: solo estremo fallback senza TSC calibrabile
 * (deciso una volta in stage_time_online). Sulle macchine con TSC il
 * PIT non genera MAI un tick: calibrazione sincrona (finestra sopra) e
 * poi, in modo eventi, o il LAPIC o il one-shot qui sotto. */
void pit_init(void)
{
    program_channel0(PIT_HZ);
    irq_register_handler(0, on_tick);

    kprintf("[PIT ] Fallback periodico a %u Hz (TSC assente).\n", PIT_HZ);
}

uint64_t pit_ticks(void)
{
    return s_ticks;
}

uint64_t pit_uptime_ms(void)
{
    return s_ticks * (1000u / PIT_HZ);
}

/* === Orchestratore: backend tick source (one-shot) ======================
 *
 * Superficie raggiungibile SOLO via time/tick_source.h: i verbi os_*
 * della sezione in alto fanno il lavoro, qui c'e' l'orchestrazione e
 * l'export della vtable. */

static void ts_pit_install(void)
{
    s_os_armed     = false;
    s_os_target_ns = 0;
    s_os_cb        = NULL;

    /* Parcheggio al conteggio massimo: nessun fuoco utile finche' il
     * motore eventi non arma una scadenza vera. irq_register_handler
     * SOSTITUISCE on_tick e smaschera IRQ0 — da qui il periodico e'
     * morto, s_ticks e pit_uptime_ms si congelano (come col LAPIC:
     * in modo eventi il monotono e' il TSC). */
    os_program_count(PIT_ONESHOT_MAX_COUNT);
    irq_register_handler(0, os_isr);

    kprintf("[PIT ] One-shot (mode 0) installato su IRQ0: grana 838 ns, "
            "orizzonte singolo %u ns.\n",
            (uint32_t)PIT_ONESHOT_MAX_DELTA_NS);
}

static void ts_pit_arm(uint64_t deadline_ns)
{
    /* Pubblica il bersaglio prima di toccare il ferro: l'ISR legge solo
     * con IF=0 e il chiamante e' gia' sotto irq_save. */
    s_os_target_ns = deadline_ns;
    s_os_armed     = true;
    os_arm_remaining(tsc_now_ns());
}

static void ts_pit_disarm(void)
{
    /* Mode 0 non ha uno "stop" vero: si azzera il flag e si parcheggia
     * al massimo. L'eventuale fuoco residuo trova !s_os_armed e tace. */
    s_os_armed     = false;
    s_os_target_ns = 0;
    os_program_count(PIT_ONESHOT_MAX_COUNT);
}

static void ts_pit_register(tick_source_cb_t cb)
{
    s_os_cb = cb;
}

const struct tick_source tick_source_pit_oneshot =
{
    .install           = ts_pit_install,
    .arm_deadline_ns   = ts_pit_arm,
    .disarm            = ts_pit_disarm,
    .register_callback = ts_pit_register,
    .name              = "PIT one-shot (mode 0)",
    .max_arm_delta_ns  = PIT_ONESHOT_MAX_DELTA_NS,
};
