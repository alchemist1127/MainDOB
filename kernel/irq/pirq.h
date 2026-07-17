/* MainDOB PCI IRQ routing — chipset-level interrupt routing.
 *
 * Due backend, mutuamente esclusivi per costruzione:
 *   - Intel PIIX3/PIIX4 (8086:7000/7110, QEMU 'pc', Armada E500-class):
 *     macchine PIC-mode. Espone il REWIRING (pirq_wire_device): sposta
 *     la consegna dell'INTx di un device su un'altra linea 8259.
 *   - Intel ICH-class (LPC a 0:31.0, ICH6..ICH10; QEMU q35 = ICH9,
 *     Extensa 5220 = ICH8M): macchine IOAPIC. Espone la RISOLUZIONE
 *     (pirq_resolve_gsi): INTx -> PIRQ (DxxIR via RCBA per gli
 *     integrati, swizzle per gli slot) -> GSI 16..23 fisso dell'ICH.
 *
 * Su chipset sconosciuti entrambe le API falliscono e il chiamante
 * ripiega (rewiring: fatale; risoluzione: discovery empirica). */

#ifndef MAINDOB_IRQ_PIRQ_H
#define MAINDOB_IRQ_PIRQ_H

#include "lib/types.h"

/* Rileva il bridge (PIIX o LPC ICH) e, per l'ICH, fotografa i DxxIR.
 * Chiamare una volta al boot, incondizionatamente: la detection e' pura
 * lettura e serve a ENTRAMBI i modi di consegna (PIC e IOAPIC). */
void pirq_init(void);

/* Rewire the PCI device identified by (bus, slot, func) so its interrupt
 * is delivered on 'new_line'. Reprograms the chipset PIRQ routing
 * register and updates the device's PCI Interrupt Line (offset 0x3C)
 * so subsequent readers see the correct value.
 *
 * Returns 0 on success, -1 on failure (unknown chipset, device has no
 * interrupt pin, bus != 0). Solo backend PIIX.
 *
 * WARNING: reprogramming a PIRQ register also moves any other device
 * whose INT# pin is routed to the same PIRQ. This is unavoidable at
 * chipset level — if two devices share a PIRQ (not just an IRQ number),
 * they are physically wired together and cannot be separated. */
int pirq_wire_device(uint8_t bus, uint8_t slot, uint8_t func, uint8_t new_line);

/* Risoluzione DETERMINISTICA dell'INTx di (bus, slot, func) in GSI, per
 * la consegna IOAPIC. Ritorna false se il backend ICH non c'e', il bus
 * non e' lo 0 o il device non ha pin INT#: il chiamante ripiega sulla
 * discovery empirica. Solo backend ICH. */
bool pirq_resolve_gsi(uint8_t bus, uint8_t slot, uint8_t func,
                      uint8_t *out_gsi);

#endif /* MAINDOB_IRQ_PIRQ_H */
