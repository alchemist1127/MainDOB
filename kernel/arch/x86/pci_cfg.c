#include "arch/x86/pci_cfg.h"
#include "arch/x86/ports.h"
#include "sync/spinlock.h"

/* Vedi pci_cfg.h per il razionale: un solo possessore, un solo lock.
 * irqsave: il costo e' due porte I/O per ciclo, la sezione critica e'
 * minuscola e non chiama nulla — nessun ordine di lock da rispettare
 * (foglia del grafo). */

#define PCI_CONFIG_ADDR 0x0CF8
#define PCI_CONFIG_DATA 0x0CFC

static spinlock_t s_lock = SPINLOCK_INIT;

/* === Verbi (chiamante tiene il lock) ==================================== */

static inline uint32_t cfg_addr(uint8_t bus, uint8_t slot, uint8_t func,
                                uint32_t offset)
{
    return (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) | (offset & 0xFCu);
}

static uint32_t cycle_read32(uint8_t bus, uint8_t slot, uint8_t func,
                             uint32_t offset)
{
    outl(PCI_CONFIG_ADDR, cfg_addr(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

static void cycle_write32(uint8_t bus, uint8_t slot, uint8_t func,
                          uint32_t offset, uint32_t value)
{
    outl(PCI_CONFIG_ADDR, cfg_addr(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

/* === API ================================================================ */

uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func,
                        uint32_t offset)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_lock);
    uint32_t v  = cycle_read32(bus, slot, func, offset);
    spinlock_release_irqrestore(&s_lock, fl);
    return v;
}

void pci_cfg_write32(uint8_t bus, uint8_t slot, uint8_t func,
                     uint32_t offset, uint32_t value)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_lock);
    cycle_write32(bus, slot, func, offset, value);
    spinlock_release_irqrestore(&s_lock, fl);
}

uint8_t pci_cfg_read8(uint8_t bus, uint8_t slot, uint8_t func,
                      uint8_t offset)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_lock);
    uint32_t w  = cycle_read32(bus, slot, func, offset & 0xFCu);
    spinlock_release_irqrestore(&s_lock, fl);
    return (uint8_t)(w >> ((offset & 0x03u) * 8));
}

void pci_cfg_write8(uint8_t bus, uint8_t slot, uint8_t func,
                    uint8_t offset, uint8_t value)
{
    /* Read-modify-write del dword contenitore SOTTO il lock: la scrittura
     * a byte via porta 0xCFC+off funziona sul ferro, ma il ciclo resta
     * comunque due accessi (addr, data) e va serializzato come gli altri.
     * Il RMW esplicito evita anche i chipset schizzinosi sui sub-word. */
    uint32_t fl = spinlock_acquire_irqsave(&s_lock);
    uint32_t w  = cycle_read32(bus, slot, func, offset & 0xFCu);
    uint32_t sh = (offset & 0x03u) * 8;
    w = (w & ~(0xFFu << sh)) | ((uint32_t)value << sh);
    cycle_write32(bus, slot, func, offset & 0xFCu, w);
    spinlock_release_irqrestore(&s_lock, fl);
}
