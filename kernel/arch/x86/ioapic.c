/* MainDOB — backend I/O APIC. Vedi ioapic.h per motivazione e scope.
 *
 * Accesso ai registri: ogni IOAPIC espone due registri MMIO — IOREGSEL a
 * base+0x00 (seleziona un indice di registro indiretto) e IOWIN a
 * base+0x10 (legge/scrive il registro selezionato). La voce di
 * redirezione N e' un valore a 64 bit su due registri indiretti,
 * 0x10+2N (basso) e 0x11+2N (alto). La MMIO va mappata non cacheabile,
 * come il LAPIC. */

#include "arch/x86/ioapic.h"
#include "sync/spinlock.h"
#include "arch/x86/lapic.h"
#include "arch/x86/cpu_features.h"
#include "mm/mmio_map.h"
#include "mm/pmm.h"        /* PAGE_SIZE */
#include "acpi/acpi.h"
#include "console/console.h"

#define IOAPIC_REG_VER      0x01
#define IOAPIC_REG_REDTBL0  0x10   /* voce N a 0x10+2N (basso) / +1 (alto) */

#define IOAPIC_IOREGSEL     0x00
#define IOAPIC_IOWIN        0x10

#define REDIR_LO_VECTOR_MASK 0x000000FFu
#define REDIR_LO_DELMODE_FIX 0x00000000u
#define REDIR_LO_DESTMODE_PH 0x00000000u
#define REDIR_LO_POLARITY_LO 0x00002000u   /* bit 13: 1 = active low     */
#define REDIR_LO_TRIGGER_LVL 0x00008000u   /* bit 15: 1 = level          */
#define REDIR_LO_MASKED      0x00010000u   /* bit 16: 1 = mascherato     */

#define IOAPIC_MAX 8

typedef struct
{
    volatile uint32_t *mmio;
    uint32_t           gsi_base;
    uint32_t           gsi_count;
    uint8_t            id;
    bool               present;
} ioapic_t;

static ioapic_t s_ioapics[IOAPIC_MAX];
static uint32_t s_ioapic_n;
static bool     s_available;

static uint32_t ioapic_read(ioapic_t *io, uint8_t reg)
{
    io->mmio[IOAPIC_IOREGSEL / 4] = reg;
    return io->mmio[IOAPIC_IOWIN / 4];
}

static void ioapic_write(ioapic_t *io, uint8_t reg, uint32_t val)
{
    io->mmio[IOAPIC_IOREGSEL / 4] = reg;
    io->mmio[IOAPIC_IOWIN / 4] = val;
}

static ioapic_t *ioapic_for_gsi(uint32_t gsi)
{
    for (uint32_t i = 0; i < s_ioapic_n; i++)
    {
        ioapic_t *io = &s_ioapics[i];
        if (io->present && gsi >= io->gsi_base && gsi < io->gsi_base + io->gsi_count)
        {
            return io;
        }
    }
    return NULL;
}

/* Serializzazione SMP della finestra di registro indicizzata:
 * IOREGSEL+IOWIN e' una coppia non atomica — un core A seleziona X, un
 * core B seleziona Y, l'accesso IOWIN di A colpisce Y. Dato che
 * mask/unmask sono read-modify-write di voci di redirezione, una
 * coppia concorrente puo' corrompere vettori/destinazioni/bit-mask di
 * una linea SCORRELATA. Ogni operazione pubblica prende questo lock
 * attorno all'intera sequenza RMW (non per singolo accesso: lettura e
 * riscrittura devono essere UNA sezione critica). */
static spinlock_t s_ioapic_lock = SPINLOCK_INIT;

static void write_redir(ioapic_t *io, uint32_t pin, uint32_t lo, uint32_t hi)
{
    uint8_t idx = (uint8_t)(IOAPIC_REG_REDTBL0 + pin * 2);
    /* Prima la dword alta, poi la bassa: scrivere la bassa (col bit
     * mask) per ultima rende la voce coerente prima che possa essere
     * smascherata. */
    ioapic_write(io, (uint8_t)(idx + 1), hi);
    ioapic_write(io, idx, lo);
}

static uint32_t read_redir_lo(ioapic_t *io, uint32_t pin)
{
    return ioapic_read(io, (uint8_t)(IOAPIC_REG_REDTBL0 + pin * 2));
}

/* === API pubblica ========================================================= */

bool ioapic_init(void)
{
    if (s_available) return true;

    if (!cpu_has(CPUF_APIC)) return false;
    uint32_t n = acpi_ioapic_count();
    if (n == 0) return false;

    for (uint32_t i = 0; i < n && s_ioapic_n < IOAPIC_MAX; i++)
    {
        acpi_ioapic_info_t info;
        if (!acpi_ioapic_get(i, &info)) continue;

        ioapic_t *io = &s_ioapics[s_ioapic_n];
        uint32_t virt, pages;
        io->mmio = (volatile uint32_t *)mmio_map(info.address, PAGE_SIZE,
                                                 true, &virt, &pages);
        if (!io->mmio)
        {
            kprintf("[IOAPIC] mapping fallito per base 0x%08x - saltato\n",
                    info.address);
            continue;
        }
        io->id       = info.id;
        io->gsi_base = info.gsi_base;

        uint32_t ver = ioapic_read(io, IOAPIC_REG_VER);
        io->gsi_count = ((ver >> 16) & 0xFF) + 1;
        io->present = true;

        for (uint32_t pin = 0; pin < io->gsi_count; pin++)
        {
            write_redir(io, pin, REDIR_LO_MASKED, 0);
        }

        kprintf("[IOAPIC] id %u base 0x%08x GSI %u..%u (%u ingressi)\n",
                io->id, info.address, io->gsi_base,
                io->gsi_base + io->gsi_count - 1, io->gsi_count);

        s_ioapic_n++;
    }

    s_available = (s_ioapic_n > 0);
    return s_available;
}

bool ioapic_available(void) { return s_available; }

bool ioapic_covers_gsi(uint32_t gsi)
{
    return ioapic_for_gsi(gsi) != NULL;
}

int ioapic_route_gsi(uint32_t gsi, uint8_t vector, uint8_t lapic_id,
                     uint16_t mps_flags)
{
    ioapic_t *io = ioapic_for_gsi(gsi);
    if (!io) return -1;
    uint32_t fl = spinlock_acquire_irqsave(&s_ioapic_lock);

    uint32_t pin = gsi - io->gsi_base;

    uint32_t lo = (uint32_t)vector & REDIR_LO_VECTOR_MASK;
    lo |= REDIR_LO_DELMODE_FIX | REDIR_LO_DESTMODE_PH;

    if ((mps_flags & 0x3u) == 0x3u)   /* MPS INTI: active low            */
    {
        lo |= REDIR_LO_POLARITY_LO;
    }
    if ((mps_flags & 0xCu) == 0xCu)   /* MPS INTI: level triggered       */
    {
        lo |= REDIR_LO_TRIGGER_LVL;
    }

    lo |= REDIR_LO_MASKED;   /* programmata mascherata: l'unmask e' esplicito */

    uint32_t hi = (uint32_t)lapic_id << 24;

    write_redir(io, pin, lo, hi);
    spinlock_release_irqrestore(&s_ioapic_lock, fl);
    return 0;
}

void ioapic_mask_gsi(uint32_t gsi)
{
    ioapic_t *io = ioapic_for_gsi(gsi);
    if (!io) return;
    uint32_t pin = gsi - io->gsi_base;
    uint32_t fl = spinlock_acquire_irqsave(&s_ioapic_lock);
    uint32_t lo = read_redir_lo(io, pin);
    write_redir(io, pin, lo | REDIR_LO_MASKED,
               ioapic_read(io, (uint8_t)(IOAPIC_REG_REDTBL0 + pin * 2 + 1)));
    spinlock_release_irqrestore(&s_ioapic_lock, fl);
}

void ioapic_unmask_gsi(uint32_t gsi)
{
    ioapic_t *io = ioapic_for_gsi(gsi);
    if (!io) return;
    uint32_t pin = gsi - io->gsi_base;
    uint32_t fl = spinlock_acquire_irqsave(&s_ioapic_lock);
    uint32_t lo = read_redir_lo(io, pin);
    write_redir(io, pin, lo & ~REDIR_LO_MASKED,
               ioapic_read(io, (uint8_t)(IOAPIC_REG_REDTBL0 + pin * 2 + 1)));
    spinlock_release_irqrestore(&s_ioapic_lock, fl);
}
