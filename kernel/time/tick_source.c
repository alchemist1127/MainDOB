/* Selettore del tick source.
 *
 * Decide UNA volta fra LAPIC e PIT one-shot; dopo, il motore eventi
 * dispatcha attraverso la vtable senza piu' ramificare sull'hardware.
 * La decisione e' puramente di capacita': lapic_available() e' falso
 * sia quando la MADT non riporta un APIC sia quando il silicio l'ha
 * fuso (il revival via IA32_APIC_BASE e' gia' stato tentato da
 * lapic_init); lapic_timer_usable() copre i timer rotti o non
 * calibrabili. In entrambi i casi si scende sul PIT one-shot — la
 * stessa macchina event-driven, con grana 838 ns invece di ~10 ns.
 */

#include "time/tick_source.h"
#include "arch/x86/lapic.h"

/* === Orchestratore ====================================================== */

const struct tick_source *tick_source_select(void)
{
    if (lapic_available() && lapic_timer_usable())
    {
        return &tick_source_lapic;
    }
    return &tick_source_pit_oneshot;
}
