/* Stato per-CPU. g_cpus[0] e' la boot CPU; gli altri slot si popolano
 * man mano che le AP salgono online. Azzerato in BSS, lo stato iniziale
 * corretto: gdt_init() puo' usare this_cpu() prima di ogni init SMP. */

#include "proc/percpu.h"

percpu_t g_cpus[MAX_CPUS];

/* Guardia di layout: `self` deve stare all'offset 0 perche' il fast
 * path di this_cpu() legge [gs:0] e si aspetta il self-pointer li'. Se
 * la struct venisse riordinata la build fallisce qui, rumorosamente,
 * invece di produrre un this_cpu() silenziosamente sbagliato. */
typedef char percpu_self_at_offset0[
    __builtin_offsetof(percpu_t, self) == 0 ? 1 : -1];

#ifdef MAINDOB_SMP

#include "acpi/acpi.h"
#include "arch/x86/lapic.h"
#include "sync/atomic.h"

/* Local-APIC id (0..255) -> slot in g_cpus[]. Costruita da
 * percpu_smp_init(). PERCPU_SLOT_INVALID marca un id che l'init NON ha
 * mappato (piu' CPU di MAX_CPUS, o un id che ACPI non ha mai riportato).
 * Uno zero-BSS alieherebbe silenziosamente ogni id ignoto allo slot 0
 * (il blocco della BSP): la init riempie percio' la tabella con la
 * sentinella per prima, e ogni lookup tratta >= MAX_CPUS come "ignoto". */
static uint8_t g_apicid_to_idx[256];

/* Finche' resta false, gira solo la BSP e il LAPIC potrebbe non essere
 * mappato: percpu_current() non deve consultare l'APIC e ritorna
 * semplicemente il blocco della BSP. */
static bool g_smp_active = false;

/* Diagnostica: quante volte this_cpu() ha mancato il fast path %gs ed
 * e' ricaduta qui. Con il GS per-CPU gia' caricato negli entry-point la
 * BSP non dovrebbe mai ricadere; l'AP lo fa una volta sola (dentro
 * ap_main, prima di avere il proprio GDT). Una crescita guidata dalla
 * BSP segnala un contesto kernel con GS != GDT_SEL_PERCPU. */
static volatile uint32_t g_percpu_fallbacks;

void percpu_smp_init(void)
{
    uint32_t bsp_apic = lapic_get_id() & 0xFFu;

    for (uint32_t i = 0; i < 256; i++)
    {
        g_apicid_to_idx[i] = PERCPU_SLOT_INVALID;
    }

    /* La BSP e' sempre slot 0: gdt_init() ha gia' preparato g_cpus[0].tss
     * per lei via this_cpu() == &g_cpus[0] molto prima di questo punto,
     * quindi deve restare slot 0. */
    g_apicid_to_idx[bsp_apic] = 0;
    g_cpus[0].self      = &g_cpus[0];
    g_cpus[0].cpu_index = 0;
    g_cpus[0].apic_id   = (uint8_t)bsp_apic;

    uint32_t slot  = 1;
    uint32_t total = acpi_cpu_count();
    for (uint32_t i = 0; i < total && slot < MAX_CPUS; i++)
    {
        acpi_cpu_info_t c;
        if (!acpi_cpu_get(i, &c) || !c.enabled || c.apic_id == bsp_apic)
        {
            continue;
        }
        g_apicid_to_idx[c.apic_id] = (uint8_t)slot;
        g_cpus[slot].self      = &g_cpus[slot];
        g_cpus[slot].cpu_index = slot;
        g_cpus[slot].apic_id   = c.apic_id;
        slot++;
    }

    g_smp_active = true;
}

percpu_t *percpu_current(void)
{
    atomic_inc(&g_percpu_fallbacks);
    if (!g_smp_active)
    {
        return &g_cpus[0];
    }
    uint8_t s = g_apicid_to_idx[lapic_get_id() & 0xFFu];
    /* Difensivo: uno slot non mappato qui non dovrebbe mai capitare
     * (smp_boot_aps non manda mai il SIPI a una CPU senza slot), ma non
     * si indicizza mai oltre g_cpus. */
    if (s >= (uint8_t)MAX_CPUS)
    {
        return &g_cpus[0];
    }
    return &g_cpus[s];
}

uint32_t percpu_fallback_count(void)
{
    return atomic_read(&g_percpu_fallbacks);
}

uint32_t percpu_slot_of_apic(uint8_t apic_id)
{
    return g_apicid_to_idx[apic_id];
}

#endif /* MAINDOB_SMP */
