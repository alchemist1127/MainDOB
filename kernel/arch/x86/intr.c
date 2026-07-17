#include "arch/x86/intr.h"
#include "arch/x86/irq.h"
#include "arch/x86/ioapic.h"
#include "arch/x86/lapic.h"
#include "arch/x86/isr.h"
#include "arch/x86/idt.h"
#include "arch/x86/gdt.h"
#include "acpi/acpi.h"
#include "proc/percpu.h"
#include "console/console.h"

/* Controller di interrupt unificato.
 *
 * Astrae la CONSEGNA degli IRQ dispositivo: finche' l'IOAPIC non e'
 * migrato, le linee legacy passano dall'8259 esattamente come prima;
 * dopo la migrazione ogni linea 0..15 e ogni GSI PCI (>=16) arriva
 * dall'IOAPIC, con EOI al LAPIC. I vettori allocabili (0x50..0xDF) —
 * IOAPIC-routed su vettore fresco, o MSI che scrive dritto al LAPIC —
 * hanno SEMPRE EOI al LAPIC, indipendentemente dal backend legacy.
 *
 * Un vettore appartiene a un PID (proprieta' event-driven: liberata
 * alla morte del processo, mai spazzata da un timer). Lo steering
 * per-CPU ripunta la voce di redirezione IOAPIC sul core home del
 * driver: l'ISR — e la sveglia IPC che scatena — gira li', senza un
 * hop IPI+inbox cross-core a ogni interrupt. */

#define BOOT_LAPIC_ID 0

/* Handler per vettore: uno solo albero, indicizzato per vettore (linea
 * legacy L -> vettore IRQ_BASE_VECTOR+L; IOAPIC/MSI -> vettore
 * allocato). */
static isr_handler_t g_vector_handler[256];

/* Proprieta' dei vettori allocabili 0x50..0xDF (0 = libero). */
static pid_t g_vector_owner[256];

static bool g_ioapic_active;

/* --- Stub dei vettori allocabili (generati in isr_stubs.asm) --------- */

/* Finestra GSI 0x30..0x3F: identita' vettore = IRQ_BASE_VECTOR + gsi
 * per i GSI PCI 16..31 armati da intr_switch_to_ioapic (cosi'
 * vector-32 recupera il GSI come indice di irq_lines[]). L'ALLOCATORE
 * (MSI e simili) parte comunque da VEC_STUB_FIRST: questa finestra e'
 * riservata all'identita' e non viene mai distribuita. Senza i suoi
 * stub, il primo INTx di un GSI armato arrivava su un gate ASSENTE:
 * #GP(IDT, vettore) sul processo di turno — l'ata ammazzato da
 * innocente — e IRQ AHCI mai consegnati (scritture MBR in timeout). */
#define VEC_GSI_FIRST  0x30u
#define VEC_STUB_FIRST 0x50u
#define VEC_STUB_LAST  0xDFu

#define DECL_VSTUB(n) extern void isr_stub_##n(void);
#define VSTUB(n) [n] = (uint32_t)isr_stub_##n,

/* Tabella indirizzi per i vettori 48..223 (0x30..0xDF). Nomi DECIMALI:
 * %assign di nasm memorizza numeri e li rende in base 10, quindi gli
 * stub generati si chiamano isr_stub_80..isr_stub_223. Gli indici
 * fuori range restano 0 e non vengono mai usati. */
DECL_VSTUB(48) DECL_VSTUB(49) DECL_VSTUB(50) DECL_VSTUB(51) DECL_VSTUB(52) DECL_VSTUB(53)
DECL_VSTUB(54) DECL_VSTUB(55) DECL_VSTUB(56) DECL_VSTUB(57) DECL_VSTUB(58) DECL_VSTUB(59)
DECL_VSTUB(60) DECL_VSTUB(61) DECL_VSTUB(62) DECL_VSTUB(63) DECL_VSTUB(64) DECL_VSTUB(65)
DECL_VSTUB(66) DECL_VSTUB(67) DECL_VSTUB(68) DECL_VSTUB(69) DECL_VSTUB(70) DECL_VSTUB(71)
DECL_VSTUB(72) DECL_VSTUB(73) DECL_VSTUB(74) DECL_VSTUB(75) DECL_VSTUB(76) DECL_VSTUB(77)
DECL_VSTUB(78) DECL_VSTUB(79)
DECL_VSTUB(80) DECL_VSTUB(81) DECL_VSTUB(82) DECL_VSTUB(83) DECL_VSTUB(84) DECL_VSTUB(85)
DECL_VSTUB(86) DECL_VSTUB(87) DECL_VSTUB(88) DECL_VSTUB(89) DECL_VSTUB(90) DECL_VSTUB(91)
DECL_VSTUB(92) DECL_VSTUB(93) DECL_VSTUB(94) DECL_VSTUB(95) DECL_VSTUB(96) DECL_VSTUB(97)
DECL_VSTUB(98) DECL_VSTUB(99) DECL_VSTUB(100) DECL_VSTUB(101) DECL_VSTUB(102) DECL_VSTUB(103)
DECL_VSTUB(104) DECL_VSTUB(105) DECL_VSTUB(106) DECL_VSTUB(107) DECL_VSTUB(108) DECL_VSTUB(109)
DECL_VSTUB(110) DECL_VSTUB(111) DECL_VSTUB(112) DECL_VSTUB(113) DECL_VSTUB(114) DECL_VSTUB(115)
DECL_VSTUB(116) DECL_VSTUB(117) DECL_VSTUB(118) DECL_VSTUB(119) DECL_VSTUB(120) DECL_VSTUB(121)
DECL_VSTUB(122) DECL_VSTUB(123) DECL_VSTUB(124) DECL_VSTUB(125) DECL_VSTUB(126) DECL_VSTUB(127)
DECL_VSTUB(128) DECL_VSTUB(129) DECL_VSTUB(130) DECL_VSTUB(131) DECL_VSTUB(132) DECL_VSTUB(133)
DECL_VSTUB(134) DECL_VSTUB(135) DECL_VSTUB(136) DECL_VSTUB(137) DECL_VSTUB(138) DECL_VSTUB(139)
DECL_VSTUB(140) DECL_VSTUB(141) DECL_VSTUB(142) DECL_VSTUB(143) DECL_VSTUB(144) DECL_VSTUB(145)
DECL_VSTUB(146) DECL_VSTUB(147) DECL_VSTUB(148) DECL_VSTUB(149) DECL_VSTUB(150) DECL_VSTUB(151)
DECL_VSTUB(152) DECL_VSTUB(153) DECL_VSTUB(154) DECL_VSTUB(155) DECL_VSTUB(156) DECL_VSTUB(157)
DECL_VSTUB(158) DECL_VSTUB(159) DECL_VSTUB(160) DECL_VSTUB(161) DECL_VSTUB(162) DECL_VSTUB(163)
DECL_VSTUB(164) DECL_VSTUB(165) DECL_VSTUB(166) DECL_VSTUB(167) DECL_VSTUB(168) DECL_VSTUB(169)
DECL_VSTUB(170) DECL_VSTUB(171) DECL_VSTUB(172) DECL_VSTUB(173) DECL_VSTUB(174) DECL_VSTUB(175)
DECL_VSTUB(176) DECL_VSTUB(177) DECL_VSTUB(178) DECL_VSTUB(179) DECL_VSTUB(180) DECL_VSTUB(181)
DECL_VSTUB(182) DECL_VSTUB(183) DECL_VSTUB(184) DECL_VSTUB(185) DECL_VSTUB(186) DECL_VSTUB(187)
DECL_VSTUB(188) DECL_VSTUB(189) DECL_VSTUB(190) DECL_VSTUB(191) DECL_VSTUB(192) DECL_VSTUB(193)
DECL_VSTUB(194) DECL_VSTUB(195) DECL_VSTUB(196) DECL_VSTUB(197) DECL_VSTUB(198) DECL_VSTUB(199)
DECL_VSTUB(200) DECL_VSTUB(201) DECL_VSTUB(202) DECL_VSTUB(203) DECL_VSTUB(204) DECL_VSTUB(205)
DECL_VSTUB(206) DECL_VSTUB(207) DECL_VSTUB(208) DECL_VSTUB(209) DECL_VSTUB(210) DECL_VSTUB(211)
DECL_VSTUB(212) DECL_VSTUB(213) DECL_VSTUB(214) DECL_VSTUB(215) DECL_VSTUB(216) DECL_VSTUB(217)
DECL_VSTUB(218) DECL_VSTUB(219) DECL_VSTUB(220) DECL_VSTUB(221) DECL_VSTUB(222) DECL_VSTUB(223)

static const uint32_t g_vstub[256] =
{
    VSTUB(48) VSTUB(49) VSTUB(50) VSTUB(51) VSTUB(52) VSTUB(53)
    VSTUB(54) VSTUB(55) VSTUB(56) VSTUB(57) VSTUB(58) VSTUB(59)
    VSTUB(60) VSTUB(61) VSTUB(62) VSTUB(63) VSTUB(64) VSTUB(65)
    VSTUB(66) VSTUB(67) VSTUB(68) VSTUB(69) VSTUB(70) VSTUB(71)
    VSTUB(72) VSTUB(73) VSTUB(74) VSTUB(75) VSTUB(76) VSTUB(77)
    VSTUB(78) VSTUB(79)
    VSTUB(80) VSTUB(81) VSTUB(82) VSTUB(83) VSTUB(84) VSTUB(85)
    VSTUB(86) VSTUB(87) VSTUB(88) VSTUB(89) VSTUB(90) VSTUB(91)
    VSTUB(92) VSTUB(93) VSTUB(94) VSTUB(95) VSTUB(96) VSTUB(97)
    VSTUB(98) VSTUB(99) VSTUB(100) VSTUB(101) VSTUB(102) VSTUB(103)
    VSTUB(104) VSTUB(105) VSTUB(106) VSTUB(107) VSTUB(108) VSTUB(109)
    VSTUB(110) VSTUB(111) VSTUB(112) VSTUB(113) VSTUB(114) VSTUB(115)
    VSTUB(116) VSTUB(117) VSTUB(118) VSTUB(119) VSTUB(120) VSTUB(121)
    VSTUB(122) VSTUB(123) VSTUB(124) VSTUB(125) VSTUB(126) VSTUB(127)
    VSTUB(128) VSTUB(129) VSTUB(130) VSTUB(131) VSTUB(132) VSTUB(133)
    VSTUB(134) VSTUB(135) VSTUB(136) VSTUB(137) VSTUB(138) VSTUB(139)
    VSTUB(140) VSTUB(141) VSTUB(142) VSTUB(143) VSTUB(144) VSTUB(145)
    VSTUB(146) VSTUB(147) VSTUB(148) VSTUB(149) VSTUB(150) VSTUB(151)
    VSTUB(152) VSTUB(153) VSTUB(154) VSTUB(155) VSTUB(156) VSTUB(157)
    VSTUB(158) VSTUB(159) VSTUB(160) VSTUB(161) VSTUB(162) VSTUB(163)
    VSTUB(164) VSTUB(165) VSTUB(166) VSTUB(167) VSTUB(168) VSTUB(169)
    VSTUB(170) VSTUB(171) VSTUB(172) VSTUB(173) VSTUB(174) VSTUB(175)
    VSTUB(176) VSTUB(177) VSTUB(178) VSTUB(179) VSTUB(180) VSTUB(181)
    VSTUB(182) VSTUB(183) VSTUB(184) VSTUB(185) VSTUB(186) VSTUB(187)
    VSTUB(188) VSTUB(189) VSTUB(190) VSTUB(191) VSTUB(192) VSTUB(193)
    VSTUB(194) VSTUB(195) VSTUB(196) VSTUB(197) VSTUB(198) VSTUB(199)
    VSTUB(200) VSTUB(201) VSTUB(202) VSTUB(203) VSTUB(204) VSTUB(205)
    VSTUB(206) VSTUB(207) VSTUB(208) VSTUB(209) VSTUB(210) VSTUB(211)
    VSTUB(212) VSTUB(213) VSTUB(214) VSTUB(215) VSTUB(216) VSTUB(217)
    VSTUB(218) VSTUB(219) VSTUB(220) VSTUB(221) VSTUB(222) VSTUB(223)
};

/* --- Stato ------------------------------------------------------------- */

bool intr_ioapic_delivery_active(void)
{
    return g_ioapic_active;
}

/* --- Binding vettore/handler ------------------------------------------ */

void intr_set_vector_handler(uint8_t vector, isr_handler_t handler)
{
    g_vector_handler[vector] = handler;
    isr_register_handler(vector, intr_dispatch);

    /* Gate IDT verso lo stub del vettore allocabile: le linee legacy
     * (0x20..0x2F) hanno gia' il gate da isr_init e non stanno in
     * g_vstub. */
    if (vector >= VEC_GSI_FIRST && vector <= VEC_STUB_LAST &&
        g_vstub[vector] != 0)
    {
        idt_set_gate(vector, g_vstub[vector], GDT_SEL_KCODE,
                     IDT_FLAG_INT_KERNEL);
    }
}

void intr_clear_vector(uint8_t vector)
{
    g_vector_handler[vector] = NULL;
}

/* --- Allocazione vettori (range IOAPIC/MSI) --------------------------- */

int32_t intr_alloc_vector(pid_t owner)
{
    if (owner == 0)
    {
        return -1;
    }
    for (uint32_t v = VEC_STUB_FIRST; v <= VEC_STUB_LAST; v++)
    {
        if (v == 0x80u)
        {
            continue;                   /* riservato al gate syscall      */
        }
        if (g_vector_owner[v] == 0)
        {
            g_vector_owner[v] = owner;
            return (int32_t)v;
        }
    }
    return -1;                          /* range esaurito                 */
}

void intr_free_vector(uint8_t vector)
{
    if (vector < VEC_STUB_FIRST || vector > VEC_STUB_LAST)
    {
        return;
    }
    g_vector_owner[vector]   = 0;
    g_vector_handler[vector] = NULL;
}

pid_t intr_vector_owner(uint8_t vector)
{
    return g_vector_owner[vector];
}

void intr_release_for_pid(pid_t pid)
{
    if (pid == 0)
    {
        return;
    }
    for (uint32_t v = VEC_STUB_FIRST; v <= VEC_STUB_LAST; v++)
    {
        if (g_vector_owner[v] == pid)
        {
            g_vector_owner[v]   = 0;
            g_vector_handler[v] = NULL;
        }
    }
}

/* --- Dispatcher unificato --------------------------------------------- *
 *
 * Handler dei vettori device (legacy migrati + allocabili). EOI PRIMA
 * dell'handler: un handler puo' context-switchare via IPC fast path e
 * non tornare da questo frame, quindi il controller va ack'ato prima o
 * bloccherebbe interrupt di pari/minore priorita'. Il bersaglio EOI
 * dipende dalla consegna: allocabili -> sempre LAPIC; legacy -> LAPIC
 * se migrato, altrimenti nulla (il layer irq.c dell'8259 fa il suo EOI
 * quando NON siamo migrati — vedi nota sotto). */
void intr_dispatch(isr_regs_t *regs)
{
    uint32_t vector = regs->vector;

    if (vector >= VEC_STUB_FIRST && vector <= VEC_STUB_LAST)
    {
        lapic_eoi();
    }
    else if (g_ioapic_active)
    {
        lapic_eoi();                    /* linea legacy migrata all'IOAPIC */
    }
    /* else: linea legacy su PIC — l'EOI 8259 lo fa irq.c, che resta il
     * dispatcher registrato sui vettori 0x20..0x2F finche' non migriamo. */

    isr_handler_t h = g_vector_handler[vector];
    if (h != NULL)
    {
        h(regs);
    }

    /* Stessa invariante di irq_dispatch: dopo EOI + handler, rivaluta la
     * preemption pendente. Senza, un IRQ di dispositivo che sveglia un
     * thread (disco->driver, input->GUI via ipc_notify->scheduler_unblock)
     * alza need_resched ma NON switcha: il thread resta READY finche' non
     * arriva un altro interrupt -> freeze su CPU idle (timer di quanto
     * disarmato), latenze e consegne perse altrove. Il hook e' auto-guardato
     * (need_resched / crit_depth / in_timer_drain), quindi e' un no-op quando
     * non serve. */
    irq_run_post_eoi_hook();
}

/* --- Attributi GSI (unica sorgente di verita') ------------------------ */

static uint16_t intr_gsi_attributes(uint8_t unit, uint32_t *gsi_out)
{
    if (unit < 16)
    {
        uint16_t flags = 0;
        uint32_t gsi   = acpi_resolve_gsi(unit, &flags);
        if (gsi_out != NULL)
        {
            *gsi_out = gsi;
        }
        return flags;
    }
    if (gsi_out != NULL)
    {
        *gsi_out = unit;
    }
    return (uint16_t)(ACPI_MADT_POLARITY_LOW | ACPI_MADT_TRIGGER_LEVEL);
}

static bool gsi_claimed_by_legacy(uint32_t gsi)
{
    for (uint8_t line = 0; line < 16; line++)
    {
        if (acpi_resolve_gsi(line, NULL) == gsi)
        {
            return true;
        }
    }
    return false;
}

/* --- Mask/unmask di linea attraverso il controller attivo ------------- */

void intr_line_mask(uint8_t line)
{
    if (g_ioapic_active)
    {
        ioapic_mask_gsi(acpi_resolve_gsi(line, NULL));
    }
    else
    {
        irq_mask(line);
    }
}

void intr_line_unmask(uint8_t line)
{
    if (g_ioapic_active)
    {
        ioapic_unmask_gsi(acpi_resolve_gsi(line, NULL));
    }
    else
    {
        irq_unmask(line);
    }
}

/* Steering: ripunta la voce IOAPIC di una linea su una CPU (per
 * cpu_index -> suo LAPIC id). No-op sotto PIC e sul core stesso in UP:
 * comportamento monoprocessore invariato. La voce e' programmata
 * MASCHERATA; il chiamante riapre dopo (sys_irq_register lo fa). */
void intr_route_line_to_cpu(uint8_t line, uint32_t cpu_index)
{
    if (!g_ioapic_active || cpu_index >= (uint32_t)MAX_CPUS)
    {
        return;
    }
    uint32_t gsi   = 0;
    uint16_t flags = intr_gsi_attributes(line, &gsi);
    uint8_t  vector = (uint8_t)(IRQ_BASE_VECTOR + line);
    ioapic_route_gsi(gsi, vector, g_cpus[cpu_index].apic_id, flags);
}

/* --- Migrazione PIC -> IOAPIC ----------------------------------------- */

bool intr_switch_to_ioapic(void)
{
    if (g_ioapic_active)
    {
        return true;
    }
    if (!ioapic_available())
    {
        return false;
    }

    /* Una voce di redirezione per ogni linea legacy: il vettore resta
     * IRQ_BASE_VECTOR+line, quindi ogni handler gia' registrato resta
     * valido — cambia solo la consegna. GSI risolto via MADT (override
     * IRQ0->GSI2 incluso). Programmate mascherate; riaperte sotto. */
    for (uint8_t line = 0; line < 16; line++)
    {
        uint16_t flags = 0;
        uint32_t gsi = acpi_resolve_gsi(line, &flags);
        ioapic_route_gsi(gsi, (uint8_t)(IRQ_BASE_VECTOR + line),
                         BOOT_LAPIC_ID, flags);
    }

    /* GSI PCI/platform (>=16): l'8259 non li instradava mai (il BIOS
     * faceva INTx->PIRQ->linea). In modo APIC non c'e' quel servizio:
     * il kernel programma ogni ingresso IOAPIC sopra le 16 linee su
     * vettore IRQ_BASE_VECTOR+gsi (cosi' `vector-32` recupera il GSI
     * come indice linea), consegnato alla BSP, LEVEL+LOW per INTx.
     * Mascherate: un GSI si apre solo quando un driver lo rivendica. */
    uint32_t pci_gsi_armed = 0;
    for (uint32_t gsi = 16; gsi < 32; gsi++)
    {
        if (gsi_claimed_by_legacy(gsi))
        {
            continue;
        }
        if (ioapic_route_gsi(gsi, (uint8_t)(IRQ_BASE_VECTOR + gsi),
                             BOOT_LAPIC_ID,
                             (uint16_t)(ACPI_MADT_POLARITY_LOW |
                                        ACPI_MADT_TRIGGER_LEVEL)) == 0)
        {
            pci_gsi_armed++;
        }
    }

    /* Maschera l'8259 del tutto: da qui l'IOAPIC possiede la consegna e
     * una assertion vagante del PIC non deve raggiungere la CPU. */
    for (uint8_t line = 0; line < 16; line++)
    {
        irq_mask(line);
    }

    /* I vettori device passano al dispatcher unificato (EOI al LAPIC).
     * Le linee legacy migrate mantengono il loro handler ma il gate
     * resta quello di irq.c: intr_dispatch NON e' registrato sui
     * vettori legacy — la migrazione dell'EOI legacy avviene qui, nel
     * ramo g_ioapic_active del dispatcher irq.c. */
    irq_set_delivery_via_ioapic(true);
    g_ioapic_active = true;

    /* Riapri le linee che erano vive sotto il PIC (handler invariati). */
    for (uint8_t line = 0; line < 16; line++)
    {
        if (g_vector_handler[IRQ_BASE_VECTOR + line] != NULL)
        {
            intr_line_unmask(line);
        }
    }

    kprintf("[INTR] Migrato a consegna IOAPIC (PIC mascherato, "
            "%u GSI PCI armati).\n", pci_gsi_armed);
    return true;
}

/* Compat: le linee legacy sotto il dispatcher irq.c chiamano ancora i
 * suoi handler via s_handlers[16]. Quando una linea legacy viene
 * registrata, il suo handler va anche in g_vector_handler perche' la
 * migrazione lo trovi. Ponte usato da driver.c (irq_register). */
void intr_bridge_legacy_handler(uint8_t line, isr_handler_t handler)
{
    if (line < 16)
    {
        /* Legacy: solo il binding. Gate e dispatch restano quelli di
         * irq.c (vedi commento in intr_switch_to_ioapic). */
        g_vector_handler[IRQ_BASE_VECTOR + line] = handler;
    }
    else if (line < 32)
    {
        /* GSI PCI: percorso pieno — binding + isr + GATE nella
         * finestra 0x30..0x3F (identita' 32+gsi). E' il punto che
         * mancava: senza gate, il primo INTx era un #GP. */
        intr_set_vector_handler((uint8_t)(IRQ_BASE_VECTOR + line),
                                handler);
    }
}
