/* MainDOB PCI IRQ routing — backend PIIX (rewiring, PIC mode) e
 * backend ICH (risoluzione INTx -> PIRQ -> GSI, modo IOAPIC).
 *
 * PIIX (Armada E500-class, QEMU 'pc'): il PIIX3/4 espone quattro
 * registri PIRQ a 0x60-0x63 del bridge PCI-ISA. Ogni registro decide su
 * QUALE linea 8259 viene consegnato uno dei PIRQA..D:
 *
 *   bit 0..3 : numero IRQ (0..15) di destinazione
 *   bit 7    : 1 = routing disabilitato
 *
 * Il pin INT# del device (offset config 0x3D, 1..4 = INTA..INTD) e'
 * cablato a un PIRQx dalla scheda madre; su PIIX/QEMU la mappa e'
 * rotazionale: pirq = ((slot - 1) + (pin - 1)) & 3. Queste macchine non
 * hanno IOAPIC: si resta in PIC mode, il PCI Interrupt Line e' valido e
 * il rewiring serve a risolvere i conflitti di linea (SYS_IRQ_WIRE).
 *
 * ICH (Extensa 5220 = ICH8M, QEMU q35 = ICH9, famiglia ICH6..ICH10):
 * qui c'e' l'IOAPIC e il kernel migra la consegna. In modo APIC il byte
 * Interrupt Line NON significa nulla: l'INTx e' instradato dal chipset
 * su PIRQA..PIRQH, che sull'ICH atterrano FISSI su GSI 16..23. La mappa
 * device -> PIRQ e':
 *   - device integrati (D25..D31): registri DxxIR nel chipset config
 *     space (RCBA, base a LPC config 0xF0), 4 bit per pin, valore
 *     0..7 = PIRQA..H. Default di reset 0x3210 (INTA->A .. INTD->D),
 *     usato come fallback se il BIOS ha lasciato RCBA disabilitato;
 *   - slot esterni: swizzle standard ICH su PIRQE..H,
 *     pirq = 4 + ((slot + pin - 1) & 3).
 * Con questo la risoluzione e' DETERMINISTICA: niente discovery
 * empirica (che resta l'estremo fallback per bridge sconosciuti), e
 * soprattutto niente claim sbagliati su GSI condivisi — era il 90% dei
 * sintomi USB su hardware IOAPIC (vedi 1.0 b145, che li aggirava
 * restando in PIC mode: qui c'e' la correzione definitiva che quella
 * entry prescriveva).
 *
 * Device su bus secondari: fuori scope (bridge PCI-PCI con swizzle
 * addizionale, mai incontrato dalle macchine bersaglio).
 */

#include "irq/pirq.h"
#include "arch/x86/ports.h"
#include "arch/x86/pci_cfg.h"
#include "mm/mmio_map.h"
#include "console/console.h"

/* Accessi config: SOLO via arch/x86/pci_cfg (ciclo 0xCF8/0xCFC
 * serializzato a livello di sistema). Le copie private che vivevano qui
 * sono state consolidate la' — regola D9. */

#define PIIX_PIRQ_BASE      0x60    /* PIRQA routing register */

/* Bridge PCI-ISA noti con registri PIRQ PIIX-compatibili. */
#define VENDOR_INTEL        0x8086
#define DEVICE_PIIX3        0x7000
#define DEVICE_PIIX4        0x7110

/* ICH: LPC bridge, sempre a 0:31.0, classe 06:01 (ISA bridge). */
#define ICH_LPC_SLOT        31
#define ICH_LPC_RCBA        0xF0    /* offset config: base RCBA + enable */
#define ICH_DIR_DEFAULT     0x3210  /* reset: INTA->PIRQA .. INTD->PIRQD */
#define ICH_PIRQ_GSI_BASE   16      /* PIRQA..H -> GSI 16..23, fisso    */

/* Stato PIIX */

static bool    bridge_found = false;
static uint8_t bridge_bus   = 0;
static uint8_t bridge_slot  = 0;
static uint8_t bridge_func  = 0;

/* Stato ICH: DxxIR per D25..D31, letti una volta a pirq_init. */

static bool     ich_found = false;
static uint16_t ich_dir[7];         /* [0]=D25 .. [6]=D31               */

/* Bridge detection */

static bool
is_piix_bridge(uint16_t vendor, uint16_t device)
{
    if (vendor != VENDOR_INTEL) return false;
    return (device == DEVICE_PIIX3) || (device == DEVICE_PIIX4);
}

/* Rileva il PIIX su bus 0 (QEMU 'pc': 0:1.0; la scansione regge layout
 * diversi). Solo detection: nessun side effect sul chipset. */
static bool
piix_detect(void)
{
    for (uint8_t slot = 0; slot < 32; slot++)
    {
        for (uint8_t func = 0; func < 8; func++)
        {
            uint32_t id = pci_cfg_read32(0, slot, func, 0x00);
            uint16_t vendor = (uint16_t)(id & 0xFFFF);
            uint16_t device = (uint16_t)(id >> 16);
            if (vendor == 0xFFFF) continue;

            if (is_piix_bridge(vendor, device))
            {
                bridge_found = true;
                bridge_bus   = 0;
                bridge_slot  = slot;
                bridge_func  = func;
                kprintf("[IRQ ] PIIX bridge a 0:%u.%u (%04x:%04x).\n",
                        slot, func, vendor, device);
                return true;
            }

            /* If func 0 has header type & 0x80, keep scanning funcs. */
            if (func == 0)
            {
                uint8_t header = pci_cfg_read8(0, slot, 0, 0x0E);
                if (!(header & 0x80)) break;
            }
        }
    }
    return false;
}

/* Offset RCBA dei DxxIR, indicizzati slot-25: D25IR..D31IR. Non sono
 * contigui (buco a 0x314A). 16 bit ciascuno: pin INTA..INTD nei nibble
 * 0..3, 3 bit utili per nibble (0..7 = PIRQA..H). */
static const uint16_t ich_dir_rcba_off[7] = {
    0x3150,     /* D25IR */
    0x314C,     /* D26IR */
    0x3148,     /* D27IR */
    0x3146,     /* D28IR */
    0x3144,     /* D29IR (UHCI/EHCI: il cuore dell'hotplug USB)  */
    0x3142,     /* D30IR */
    0x3140,     /* D31IR (SATA/LPC/SMBus)                        */
};

/* Rileva l'LPC ICH-class a 0:31.0 e fotografa i DxxIR. RCBA e' MMIO
 * chipset: si mappa la pagina dei registri, si leggono 7 halfword, si
 * smonta — nessuna finestra permanente. BIOS con RCBA spento (bit 0):
 * si tengono i default di reset, che sono comunque la verita' su ogni
 * macchina in cui il BIOS non ha rimescolato i PIRQ. */
static bool
ich_detect(void)
{
    uint32_t id = pci_cfg_read32(0, ICH_LPC_SLOT, 0, 0x00);
    if ((uint16_t)(id & 0xFFFF) != VENDOR_INTEL)
    {
        return false;
    }
    uint32_t cls = pci_cfg_read32(0, ICH_LPC_SLOT, 0, 0x08);
    if (((cls >> 16) & 0xFFFF) != 0x0601)       /* bridge ISA          */
    {
        return false;
    }

    for (int i = 0; i < 7; i++)
    {
        ich_dir[i] = ICH_DIR_DEFAULT;
    }

    uint32_t rcba = pci_cfg_read32(0, ICH_LPC_SLOT, 0, ICH_LPC_RCBA);
    bool from_rcba = false;
    if (rcba & 1u)
    {
        uint32_t base = rcba & 0xFFFFC000u;
        uint32_t virt = 0, pages = 0;
        /* Finestra sui soli DIR: RCBA+0x3140..0x3151. */
        volatile uint8_t *win = (volatile uint8_t *)
            mmio_map((uint64_t)base + 0x3140u, 0x20u, true, &virt, &pages);
        if (win != NULL)
        {
            for (int i = 0; i < 7; i++)
            {
                uint32_t rel = (uint32_t)ich_dir_rcba_off[i] - 0x3140u;
                ich_dir[i] = (uint16_t)(win[rel] | (win[rel + 1] << 8));
            }
            mmio_unmap(virt, pages);
            from_rcba = true;
        }
    }

    ich_found = true;
    kprintf("[IRQ ] LPC ICH a 0:31.0 (%04x:%04x): routing INTx->PIRQ->GSI "
            "deterministico (%s), D29IR=0x%04x D31IR=0x%04x.\n",
            (uint32_t)(id & 0xFFFF), (uint32_t)(id >> 16),
            from_rcba ? "RCBA" : "default di reset", ich_dir[4], ich_dir[6]);
    return true;
}

/*  *  ELCR — Edge/Level Control Register
 *
 *  PCI INTx interrupts are level-triggered (the device holds the line
 *  asserted until its status register is cleared). The 8259 PIC defaults
 *  most legacy ISA lines to edge-triggered. When a PCI device is rewired
 *  onto a line that the BIOS left in edge mode (e.g. IRQ 3, originally
 *  COM2), the PIC never sees an edge — the device asserts steady-state
 *  high, no rising transition, no interrupt delivered.
 *
 *  Fix: flip the corresponding ELCR bit to 1 (level) at rewire time.
 *  ELCR ports: 0x4D0 = IRQs 0..7, 0x4D1 = IRQs 8..15. Bit n = level mode.
 *
 *  System-reserved lines (0=timer, 1=kbd, 2=cascade, 8=RTC, 13=FPU) must
 *  remain edge-triggered. Real hardware ignores writes to these bits, but
 *  we filter explicitly for safety and clarity.
 */

#define ELCR_MASTER     0x4D0
#define ELCR_SLAVE      0x4D1

static bool
elcr_irq_is_remappable(uint8_t irq)
{
    if (irq >= 16) return false;
    if (irq == 0 || irq == 1 || irq == 2) return false;
    if (irq == 8 || irq == 13) return false;
    return true;
}

static void
elcr_set_level(uint8_t irq)
{
    if (!elcr_irq_is_remappable(irq)) return;

    uint16_t port = (irq < 8) ? ELCR_MASTER : ELCR_SLAVE;
    uint8_t  bit  = (uint8_t)(1u << (irq & 7));
    uint8_t  cur  = inb(port);

    if ((cur & bit) == 0)
    {
        outb(port, cur | bit);
        kprintf("[IRQ ] ELCR: IRQ %u set to level-triggered.\n", irq);
    }
}

/* Public API */

void
pirq_init(void)
{
    bridge_found = false;
    ich_found    = false;

    /* I due backend sono mutuamente esclusivi per costruzione: il PIIX
     * vive su macchine senza IOAPIC (PIC mode, rewiring), l'ICH su
     * macchine con IOAPIC (risoluzione). La detection li prova
     * entrambi: e' pura lettura. */
    if (piix_detect())
    {
        return;
    }
    if (ich_detect())
    {
        return;
    }

    kprintf("[IRQ ] Nessun bridge noto (PIIX/ICH): rewiring spento, "
            "risoluzione INTx solo empirica.\n");
}

int
pirq_wire_device(uint8_t bus, uint8_t slot, uint8_t func, uint8_t new_line)
{
    if (!bridge_found)
    {
        kprintf("[IRQ ] pirq_wire_device: no known bridge.\n");
        return -1;
    }
    if (bus != 0)
    {
        kprintf("[IRQ ] pirq_wire_device: secondary bus %u not supported.\n", bus);
        return -1;
    }
    if (new_line >= 16)
        return -1;

    /* Read the device's INT# pin. 0 = no interrupt, 1..4 = INTA..INTD. */
    uint8_t pin = pci_cfg_read8(bus, slot, func, 0x3D);
    if (pin == 0 || pin > 4)
    {
        kprintf("[IRQ ] pirq_wire_device: device 0:%u.%u has no INT pin.\n",
                slot, func);
        return -1;
    }

    /* PIIX rotational swizzling for bus 0. */
    uint8_t pirq_index = (uint8_t)(((slot - 1) + (pin - 1)) & 3);
    uint8_t pirq_reg   = (uint8_t)(PIIX_PIRQ_BASE + pirq_index);

    /* Write the new IRQ line into the PIIX PIRQ register. Bit 7 = 0
     * (routing enabled), bits 0..3 = IRQ number. */
    pci_cfg_write8(bridge_bus, bridge_slot, bridge_func, pirq_reg, new_line & 0x0F);

    /* Promote the destination line to level-triggered in the PIC ELCR.
     * Without this, a PCI device rewired onto a default-edge line (e.g.
     * IRQ 3) would assert INTx as a steady level and the PIC would never
     * see an edge to fire on. Idempotent: skips if already level. */
    elcr_set_level(new_line);

    /* Mirror the new line into the device's own Interrupt Line field
     * so subsequent readers (hotplug, driver re-attach) see a coherent
     * value. This field is informational — the actual routing is done
     * by the PIRQ register we just wrote. */
    pci_cfg_write8(bus, slot, func, 0x3C, new_line);

    kprintf("[IRQ ] Rewired 0:%u.%u INT%c -> PIRQ%c -> IRQ %u.\n",
            slot, func, 'A' + (pin - 1), 'A' + pirq_index, new_line);
    return 0;
}

bool
pirq_resolve_gsi(uint8_t bus, uint8_t slot, uint8_t func, uint8_t *out_gsi)
{
    if (!ich_found || bus != 0 || out_gsi == NULL)
    {
        return false;
    }

    uint8_t pin = pci_cfg_read8(bus, slot, func, 0x3D);
    if (pin == 0 || pin > 4)
    {
        return false;                   /* device senza INT#             */
    }

    uint8_t pirq;
    if (slot >= 25)
    {
        /* Device integrato: nibble del pin nel DxxIR fotografato. */
        uint16_t dir = ich_dir[slot - 25];
        pirq = (uint8_t)((dir >> ((pin - 1) * 4)) & 0x7);
    }
    else
    {
        /* Slot esterno: swizzle ICH standard su PIRQE..H. */
        pirq = (uint8_t)(4 + ((slot + pin - 1) & 3));
    }

    *out_gsi = (uint8_t)(ICH_PIRQ_GSI_BASE + pirq);
    return true;
}
