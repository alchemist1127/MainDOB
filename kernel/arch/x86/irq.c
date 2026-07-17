#include "arch/x86/irq.h"
#include "arch/x86/ports.h"
#include "arch/x86/lapic.h"
#include "console/console.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20
#define PIC_READ_ISR 0x0B

static irq_handler_t s_handlers[16];
static uint32_t      s_spurious;
static void        (*s_post_eoi_hook)(void);
static bool          s_delivery_via_ioapic;   /* default: PIC 8259      */

bool irq_delivery_via_ioapic(void)
{
    return s_delivery_via_ioapic;
}

void irq_set_delivery_via_ioapic(bool active)
{
    s_delivery_via_ioapic = active;
}

/* === Verbi PIC ========================================================== */

static void pic_remap_to_32(void)
{
    outb(PIC1_CMD, 0x11); io_wait();    /* ICW1: init + ICW4              */
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, IRQ_BASE_VECTOR);      io_wait();   /* ICW2 master    */
    outb(PIC2_DATA, IRQ_BASE_VECTOR + 8);  io_wait();   /* ICW2 slave     */
    outb(PIC1_DATA, 0x04); io_wait();   /* ICW3: slave su IRQ2            */
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();   /* ICW4: 8086 mode                */
    outb(PIC2_DATA, 0x01); io_wait();
}

static void pic_mask_everything(void)
{
    outb(PIC1_DATA, 0xFB);              /* tutto mascherato tranne IRQ2   */
    outb(PIC2_DATA, 0xFF);              /* (cascata verso lo slave)       */
}

static void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8)
    {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}

/* IRQ 7/15 fantasma: sul ferro (rumore di linea, race del PIC) arrivano
 * interrupt senza bit ISR corrispondente. Vanno riconosciuti e scartati
 * SENZA EOI sul master per il 7 (e con EOI solo al master per il 15),
 * altrimenti si perde un EOI legittimo. */
static bool absorb_spurious(uint8_t irq)
{
    /* Questa logica e' un idioma dell'8259 e vale SOLO finche' la
     * consegna e' PIC. Sotto IOAPIC i PIC sono fuori dal giro e il loro
     * ISR legge ZERO per definizione: un IRQ 7/15 REALE consegnato via
     * IOAPIC verrebbe sempre classificato fantasma e scartato PRIMA di
     * lapic_eoi() — bit ISR del LAPIC incastrato a priorita' 0x2F, e
     * OGNI vettore device 0x2x su quella CPU morto per sempre (tastiera
     * 0x21, mouse 0x2C, ata 0x2E...), con timer e IPI (vettori alti)
     * vivi. Le spurie vere del LAPIC arrivano sul vettore SVR e hanno
     * gia' il loro handler (niente EOI, corretto) in lapic.c. */
    if (s_delivery_via_ioapic)
    {
        return false;
    }

    if (irq == 7)
    {
        outb(PIC1_CMD, PIC_READ_ISR);
        if ((inb(PIC1_CMD) & 0x80) == 0)
        {
            s_spurious++;
            return true;
        }
    }
    else if (irq == 15)
    {
        outb(PIC2_CMD, PIC_READ_ISR);
        if ((inb(PIC2_CMD) & 0x80) == 0)
        {
            s_spurious++;
            outb(PIC1_CMD, PIC_EOI);    /* il master ha visto la cascata  */
            return true;
        }
    }
    return false;
}

/* === Dispatcher ========================================================= */

static void irq_dispatch(isr_regs_t *regs)
{
    uint8_t irq = (uint8_t)(regs->vector - IRQ_BASE_VECTOR);

    if (absorb_spurious(irq))
    {
        return;
    }

    irq_handler_t handler = s_handlers[irq];
    if (handler != NULL)
    {
        handler(regs);
    }

    /* EOI al controller giusto: 8259 finche' la consegna e' PIC, LAPIC
     * una volta migrati all'IOAPIC (le stesse linee legacy arrivano da
     * li'). */
    if (s_delivery_via_ioapic)
    {
        lapic_eoi();
    }
    else
    {
        pic_send_eoi(irq);
    }

    /* Punto UNICO di preemption da interrupt: dopo l'EOI, a frame IRQ
     * completo, il kernel puo' switchare in sicurezza. */
    irq_run_post_eoi_hook();
}

/* Invoca il hook di preemption post-EOI (se registrato). Esposto perche'
 * OGNI dispatcher di interrupt hardware deve rispettare la stessa
 * invariante: dopo EOI + handler, rivaluta la preemption pendente. Sia
 * irq_dispatch (linee PIC legacy) sia intr_dispatch (consegna IOAPIC) lo
 * chiamano; altrimenti un IRQ di dispositivo che sveglia un thread non
 * innescherebbe lo switch e il thread resterebbe READY non schedulato fino
 * al prossimo interrupt (freeze su idle, latenze altrove). */
void irq_run_post_eoi_hook(void)
{
    if (s_post_eoi_hook != NULL)
    {
        s_post_eoi_hook();
    }
}

/* === API ================================================================ */

void irq_init(void)
{
    pic_remap_to_32();
    pic_mask_everything();

    for (uint8_t irq = 0; irq < 16; irq++)
    {
        isr_register_handler((uint8_t)(IRQ_BASE_VECTOR + irq), irq_dispatch);
    }

    kprintf("[IRQ ] PIC rimappato a 32-47, linee mascherate.\n");
}

void irq_register_handler(uint8_t irq, irq_handler_t handler)
{
    if (irq < 16)
    {
        s_handlers[irq] = handler;
        irq_unmask(irq);
    }
}

void irq_unmask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (uint8_t)(irq & 7);
    outb(port, inb(port) & (uint8_t)~(1u << bit));
}

void irq_mask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (uint8_t)(irq & 7);
    outb(port, inb(port) | (uint8_t)(1u << bit));
}

void irq_set_post_eoi_hook(void (*hook)(void))
{
    s_post_eoi_hook = hook;
}

uint32_t irq_spurious_count(void)
{
    return s_spurious;
}
