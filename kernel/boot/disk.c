#include "boot/disk.h"
#include "arch/x86/ports.h"
#include "arch/x86/pci_cfg.h"
#include "console/console.h"
#include "mm/paging.h"
#include "mm/mmio_map.h"
#include "mm/pmm.h"          /* PAGE_SIZE                                  */
#include "kernel.h"          /* KERNEL_VMA (virt->phys dei buffer DMA)     */

/* Disco di boot — SOLO FASE DI BOOT (come bootfs). Legge settori mentre
 * il kernel ha accesso esclusivo al disco (no scheduler, no driver, IRQ
 * spenti: attese in polling bounded). Due backend, un'unica API:
 *   - ATA PIO LBA28 sui canali IDE legacy (default e caso comune);
 *   - AHCI/SATA in fallback (disco dietro un controller ich9/ICH), READ
 *     DMA EXT in polling. Additivo: se l'AHCI fallisce si resta in ATA e
 *     il comportamento e' identico a prima. Dopo bootfs_shutdown nessuno
 *     deve piu' chiamare qui: il driver userspace prende gli stessi
 *     registri. */

/* ======================================================================
 * Registri ATA (offset dalla base di canale)
 * ==================================================================== */
#define ATAR_DATA       0
#define ATAR_ERROR      1
#define ATAR_SECCOUNT   2
#define ATAR_LBA0       3
#define ATAR_LBA1       4
#define ATAR_LBA2       5
#define ATAR_DRIVE      6
#define ATAR_STATUS     7
#define ATAR_COMMAND    7

#define ATA_SR_BSY      0x80
#define ATA_SR_DRQ      0x08
#define ATA_SR_ERR      0x01

#define ATA_CMD_READ     0x20
#define ATA_CMD_IDENTIFY 0xEC

typedef struct { uint16_t io; uint16_t ctrl; } ata_channel_t;
static const ata_channel_t s_channels[2] =
{
    { 0x1F0, 0x3F6 },   /* primario   */
    { 0x170, 0x376 }    /* secondario */
};

/* Stato del backend ATA (posizione scelta da ata_boot_init). */
static bool     s_present;
static uint16_t s_io;
static uint16_t s_ctrl;
static uint8_t  s_drive;                /* 0 master, 1 slave              */
static uint32_t s_total_sectors;

/* Quale backend usa boot_disk_read: ATA di default, AHCI solo se il bus
 * IDE e' vuoto ma un disco AHCI risponde. */
#define BOOT_MODE_ATA   0
#define BOOT_MODE_AHCI  1
static int s_boot_mode = BOOT_MODE_ATA;

/* ======================================================================
 * Blocchi esecutivi: ATA PIO
 * ==================================================================== */

static void wait_not_busy(uint16_t ctrl)
{
    int timeout = 100000;
    while ((inb(ctrl) & ATA_SR_BSY) && --timeout > 0)
    {
    }
}

/* Ordine ATA corretto: BSY deve cadere PRIMA che ERR/DRQ abbiano senso
 * (mentre BSY e' alto sono indefiniti). 4 letti = ~400ns di settle. */
static bool wait_data_ready(uint16_t ctrl)
{
    for (int i = 0; i < 4; i++)
    {
        (void)inb(ctrl);
    }

    int timeout = 1000000;
    while (--timeout > 0)
    {
        uint8_t st = inb(ctrl);
        if (st & ATA_SR_BSY)
        {
            continue;
        }
        if (st & ATA_SR_ERR)
        {
            return false;
        }
        if (st & ATA_SR_DRQ)
        {
            return true;
        }
    }
    return false;                       /* disco assente/morto: mai spin  */
}

static void clear_error(uint16_t io, uint16_t ctrl)
{
    inb(io + ATAR_STATUS);
    inb(io + ATAR_ERROR);
    outb(io + ATAR_DRIVE, 0xE0);
    wait_not_busy(ctrl);
}

/* IDENTIFY su (canale, drive): salta ATAPI/SATA-ATAPI e posizioni vuote.
 * Su successo riempie *sectors (e logga il modello) e ritorna true. */
static bool identify_drive(uint16_t io, uint16_t ctrl, uint8_t drive,
                           uint32_t *sectors)
{
    outb(io + ATAR_DRIVE, drive ? 0xB0 : 0xA0);
    for (int i = 0; i < 4; i++)
    {
        (void)inb(ctrl);
    }

    outb(io + ATAR_SECCOUNT, 0);
    outb(io + ATAR_LBA0, 0);
    outb(io + ATAR_LBA1, 0);
    outb(io + ATAR_LBA2, 0);
    outb(io + ATAR_COMMAND, ATA_CMD_IDENTIFY);
    for (int i = 0; i < 4; i++)
    {
        (void)inb(ctrl);
    }

    uint8_t st = inb(io + ATAR_STATUS);
    if (st == 0 || st == 0xFF)
    {
        return false;                   /* bus flottante / niente device  */
    }
    wait_not_busy(ctrl);

    /* LBA1/LBA2 non-zero dopo IDENTIFY = firma ATAPI/SATA-ATAPI: non e'
     * un disco ATA (che li lascia a zero). Cosi' si salta il lettore. */
    if (inb(io + ATAR_LBA1) != 0 || inb(io + ATAR_LBA2) != 0)
    {
        return false;
    }
    if (!wait_data_ready(ctrl))
    {
        return false;
    }

    uint16_t ident[256];
    for (int i = 0; i < 256; i++)
    {
        ident[i] = inw(io + ATAR_DATA);
    }

    uint32_t lba28 = ident[60] | ((uint32_t)ident[61] << 16);
    uint64_t lba48 = (uint64_t)ident[100]        | ((uint64_t)ident[101] << 16)
                   | ((uint64_t)ident[102] << 32) | ((uint64_t)ident[103] << 48);
    uint32_t total = lba48
                   ? (lba48 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)lba48)
                   : lba28;
    if (total == 0)
    {
        return false;
    }

    char model[42];
    for (int i = 0; i < 20; i++)
    {
        model[i * 2]     = (char)(ident[27 + i] >> 8);
        model[i * 2 + 1] = (char)(ident[27 + i] & 0xFF);
    }
    model[40] = '\0';
    int e = 39;
    while (e >= 0 && model[e] == ' ')
    {
        model[e--] = '\0';
    }
    kprintf("[DISK] ATA %s/%s: %s (%u MB)\n",
            (ctrl == 0x3F6) ? "primario" : "secondario",
            drive ? "slave" : "master", model, total / 2048u);

    *sectors = total;
    return true;
}

/* Scandisce tutte le posizioni IDE legacy (primario/secondario x
 * master/slave) e seleziona il primo disco ATA trovato. */
static bool ata_boot_init(void)
{
    for (int c = 0; c < 2; c++)
    {
        for (uint8_t d = 0; d < 2; d++)
        {
            uint32_t sectors = 0;
            if (identify_drive(s_channels[c].io, s_channels[c].ctrl,
                               d, &sectors))
            {
                s_io            = s_channels[c].io;
                s_ctrl          = s_channels[c].ctrl;
                s_drive         = d;
                s_total_sectors = sectors;
                s_present       = true;
                return true;
            }
        }
    }
    return false;
}

/* Legge `count` settori a `lba` dal canale/drive selezionato (LBA28). */
static bool ata_boot_read(uint32_t lba, uint32_t count, void *buf)
{
    if (!s_present)
    {
        return false;
    }

    uint16_t *p    = (uint16_t *)buf;
    uint8_t   dsel = s_drive ? 0xF0 : 0xE0;     /* LBA28 slave/master     */

    for (uint32_t s = 0; s < count; s++)
    {
        uint32_t sector = lba + s;
        bool ok = false;

        for (int retry = 0; retry < 3; retry++)
        {
            wait_not_busy(s_ctrl);
            outb(s_io + ATAR_DRIVE, dsel | ((sector >> 24) & 0x0F));
            outb(s_io + ATAR_SECCOUNT, 1);
            outb(s_io + ATAR_LBA0, sector & 0xFF);
            outb(s_io + ATAR_LBA1, (sector >> 8) & 0xFF);
            outb(s_io + ATAR_LBA2, (sector >> 16) & 0xFF);
            outb(s_io + ATAR_COMMAND, ATA_CMD_READ);

            if (wait_data_ready(s_ctrl))
            {
                for (int i = 0; i < 256; i++)
                {
                    p[i] = inw(s_io + ATAR_DATA);
                }
                ok = true;
                break;
            }
            clear_error(s_io, s_ctrl);
        }

        if (!ok)
        {
            return false;
        }
        p += 256;
    }
    return true;
}

/* ======================================================================
 * Blocchi esecutivi: AHCI (fallback SATA)
 *
 * Gira dopo paging+pmm (ordine di startup.c): l'ABAR (MMIO alto) si
 * mappa con mmio_map (finestra kernel dedicata, non cacheable). IRQ
 * spenti: ogni completamento e' in polling (PxCI/PxTFD). I buffer DMA
 * statici vivono nel direct-map basso, quindi la loro fisica e' subito
 * virt - KERNEL_VMA, cio' che l'engine AHCI vuole in command list/PRDT.
 * ==================================================================== */

/* Registri globali HBA (offset da ABAR). */
#define HBA_GHC             0x04
#define HBA_GHC_AE          (1u << 31)  /* AHCI Enable                    */
#define HBA_PI              0x0C        /* Ports Implemented (bitmap)     */

/* Registri per-porta (base = ABAR + 0x100 + port*0x80). */
#define PORT_CLB            0x00        /* Command List Base (phys)       */
#define PORT_CLBU           0x04
#define PORT_FB             0x08        /* FIS Base (phys)                */
#define PORT_FBU            0x0C
#define PORT_IS             0x10        /* Interrupt Status               */
#define PORT_CMD            0x18        /* Command and Status             */
#define PORT_TFD            0x20        /* Task File Data                 */
#define PORT_SSTS           0x28        /* SATA Status                    */
#define PORT_SERR           0x30        /* SATA Error (write-1-to-clear)  */
#define PORT_CI             0x38        /* Command Issue                  */

#define PORT_CMD_ST         (1u << 0)   /* Start                          */
#define PORT_CMD_FRE        (1u << 4)   /* FIS Receive Enable             */
#define PORT_CMD_FR         (1u << 14)  /* FIS Receive Running            */
#define PORT_CMD_CR         (1u << 15)  /* Command List Running           */

#define PORT_TFD_BSY        (1u << 7)
#define PORT_TFD_DRQ        (1u << 3)
#define PORT_TFD_ERR        (1u << 0)

#define PORT_IS_TFES        (1u << 30)  /* Task File Error Status         */
#define PORT_SSTS_DET_MASK  0x0F
#define PORT_SSTS_DET_READY 0x03        /* device presente + PHY su       */

#define ATA_CMD_READ_DMA_EX 0x25        /* READ DMA EXT (LBA48)           */
#define FIS_TYPE_H2D        0x27

/* Layout on-wire, identico a quello del driver AHCI userspace. */
typedef struct {
    uint8_t  fis_type, flags, command, feature_lo;
    uint8_t  lba0, lba1, lba2, device;
    uint8_t  lba3, lba4, lba5, feature_hi;
    uint16_t count;
    uint8_t  icc, control;
    uint8_t  reserved[4];
} __attribute__((packed)) bf_fis_h2d_t;

typedef struct {
    uint16_t flags, prdtl;
    uint32_t prdbc, ctba, ctbau;
    uint32_t reserved[4];
} __attribute__((packed)) bf_cmd_header_t;

typedef struct {
    uint32_t dba, dbau, reserved, dbc;
} __attribute__((packed)) bf_prdt_entry_t;

typedef struct {
    uint8_t         cfis[64];
    uint8_t         acmd[16];
    uint8_t         reserved[48];
    bf_prdt_entry_t prdt[1];
} __attribute__((packed, aligned(128))) bf_cmd_table_t;

/* Strutture DMA per l'unica porta/slot usati in boot. Page-aligned: ogni
 * struttura soddisfa gli allineamenti AHCI (1 KB cmd list / 256 B FIS) e
 * cade nel direct-map basso (virt->phys banale). */
static bf_cmd_header_t bf_cmd_list[32] __attribute__((aligned(4096)));
static uint8_t         bf_fis_area[256] __attribute__((aligned(4096)));
static bf_cmd_table_t  bf_cmd_table     __attribute__((aligned(4096)));

static volatile uint8_t *s_ahci_abar;       /* ABAR mappato (virtuale)    */
static uintptr_t         s_ahci_port_base;   /* ABAR + 0x100 + port*0x80   */

/* --- Accessori: virt->phys e I/O sui registri HBA/porta ---------------- */

static inline uint32_t bf_virt_to_phys(const void *v)
{
    return (uint32_t)(uintptr_t)v - KERNEL_VMA;   /* statico nel direct-map */
}
static inline uint32_t mmio_r32(uint32_t off)
{
    return *(volatile uint32_t *)(s_ahci_abar + off);
}
static inline void mmio_w32(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(s_ahci_abar + off) = val;
}
static inline uint32_t port_r32(uint32_t off)
{
    return *(volatile uint32_t *)(s_ahci_port_base + off);
}
static inline void port_w32(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(s_ahci_port_base + off) = val;
}

/* --- Blocco PCI: individua i controller AHCI --------------------------- */

/* Ciclo config via arch/x86/pci_cfg (possessore unico, serializzato).
 * bootfs gira prima dello spawn dei driver, quindi qui non c'e' mai
 * concorrenza — ma la regola D9 vuole UN ciclo config nel kernel, non
 * tre copie di cui una "tanto sicura". */
static uint32_t pci_cfg_r32(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off)
{
    return pci_cfg_read32(bus, slot, fn, off);
}

/* ABAR (BAR5) dello (skip)-esimo controller AHCI (classe 01:06) in ordine
 * di enumerazione, o 0 se non ce ne sono altri. L'enumerazione copre tutte
 * le 8 funzioni di ogni slot perche' un device PCI puo' essere
 * multi-funzione e l'AHCI del chipset e' tipicamente una funzione non-zero
 * (es. 00:1f.2); walkare ogni indice permette al chiamante di provarli
 * tutti, non solo il primo. */
static uint32_t pci_find_ahci_abar(int skip)
{
    int seen = 0;
    for (uint16_t bus = 0; bus < 256; bus++)
    {
        for (uint8_t slot = 0; slot < 32; slot++)
        {
            for (uint8_t fn = 0; fn < 8; fn++)
            {
                uint32_t id = pci_cfg_r32((uint8_t)bus, slot, fn, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;   /* funzione assente */

                uint32_t cls        = pci_cfg_r32((uint8_t)bus, slot, fn, 0x08);
                uint8_t  base_class = (cls >> 24) & 0xFF;
                uint8_t  sub_class  = (cls >> 16) & 0xFF;
                if (base_class == 0x01 && sub_class == 0x06)
                {
                    if (seen++ < skip) continue;
                    uint32_t bar5 = pci_cfg_r32((uint8_t)bus, slot, fn, 0x24);
                    return bar5 & 0xFFFFFFF0u;            /* maschera i flag */
                }
            }
        }
    }
    return 0;
}

/* --- Blocchi di ciclo-vita della porta --------------------------------- */

/* PxSERR e' write-1-to-clear e latcha gli errori di link (incluso DIAG.X
 * dello stabilirsi del PHY): finche' restano latchati l'HBA non fa
 * avanzare BSY ne' completa il comando. Azzerarli (con PxIS) e' quindi una
 * precondizione, non una toppa: fa parte sia del bring-up sia dell'emissione
 * di ogni comando. */
static void ahci_port_clear_errors(void)
{
    port_w32(PORT_SERR, 0xFFFFFFFFu);
    port_w32(PORT_IS,   0xFFFFFFFFu);
}

/* Attende che la porta sia inattiva (ne' BSY ne' DRQ nel Task File). Poll
 * bounded: in boot gli IRQ sono spenti, un device assente non deve mai
 * mandare in spin. */
static void ahci_port_wait_idle(void)
{
    for (int i = 0; i < 1000000; i++)
    {
        if (!(port_r32(PORT_TFD) & (PORT_TFD_BSY | PORT_TFD_DRQ))) break;
    }
}

static void ahci_stop_port(void)
{
    uint32_t cmd = port_r32(PORT_CMD);
    cmd &= ~PORT_CMD_ST;
    cmd &= ~PORT_CMD_FRE;
    port_w32(PORT_CMD, cmd);
    for (int i = 0; i < 1000000; i++)
    {
        if (!(port_r32(PORT_CMD) & (PORT_CMD_CR | PORT_CMD_FR))) break;
    }
}

static void ahci_start_port(void)
{
    for (int i = 0; i < 1000000; i++)
    {
        if (!(port_r32(PORT_CMD) & PORT_CMD_CR)) break;
    }
    uint32_t cmd = port_r32(PORT_CMD);
    cmd |= PORT_CMD_FRE;
    cmd |= PORT_CMD_ST;
    port_w32(PORT_CMD, cmd);
}

/* Prima porta implementata che segnala un device (DET==3). Lo stabilirsi
 * del link SATA non e' istantaneo su silicio reale: la scoperta include
 * per contratto un settle bounded del PHY, cosi' un link ancora in salita
 * non viene scambiato per "nessun disco". Ritorna l'indice o -1. */
static int ahci_port_find_device(uint32_t ports_implemented)
{
    for (int attempt = 0; attempt < 400; attempt++)
    {
        for (int p = 0; p < 32; p++)
        {
            if (!(ports_implemented & (1u << p))) continue;
            uint32_t pbase = 0x100 + (uint32_t)p * 0x80;
            uint32_t ssts  = *(volatile uint32_t *)(s_ahci_abar + pbase + PORT_SSTS);
            if ((ssts & PORT_SSTS_DET_MASK) == PORT_SSTS_DET_READY) return p;
        }
        for (volatile uint32_t d = 0; d < 300000u; d++) { }   /* settle PHY  */
    }
    return -1;
}

/* --- Blocchi esecutivi: bring-up controller + read --------------------- */

/* Porta su la prima porta con disco su UN controller (dato il suo ABAR
 * fisico). Ritorna true se un disco di boot e' pronto su di essa. */
static bool ahci_try_controller(uint32_t abar_phys)
{
    uint32_t map_virt = 0, map_pages = 0;
    /* HBA regs + tutte le 32 porte: 0x100 + 32*0x80 = 0x1100 byte. */
    void *base = mmio_map((uint64_t)abar_phys, 0x1100u, true,
                          &map_virt, &map_pages);
    if (!base)
    {
        return false;
    }
    s_ahci_abar = (volatile uint8_t *)base;

    /* AHCI Enable prima di leggere i registri di porta: in modalita' compat
     * la finestra di porta risponde con valori privi di senso e nessuna
     * porta mostrerebbe DET==3. */
    mmio_w32(HBA_GHC, mmio_r32(HBA_GHC) | HBA_GHC_AE);

    int port = ahci_port_find_device(mmio_r32(HBA_PI));
    if (port < 0)
    {
        mmio_unmap(map_virt, map_pages);
        s_ahci_abar = 0;
        return false;
    }
    s_ahci_port_base = (uintptr_t)s_ahci_abar + 0x100 + (uintptr_t)port * 0x80;

    /* Ferma la porta, puntala alla nostra command list + FIS area, azzera
     * gli errori di link, riparti. */
    ahci_stop_port();

    for (int i = 0; i < 32; i++)
    {
        bf_cmd_list[i].flags = 0;
        bf_cmd_list[i].prdtl = 0;
        bf_cmd_list[i].prdbc = 0;
        bf_cmd_list[i].ctba  = 0;
        bf_cmd_list[i].ctbau = 0;
    }
    for (int i = 0; i < 256; i++) bf_fis_area[i] = 0;

    port_w32(PORT_CLB,  bf_virt_to_phys(bf_cmd_list));
    port_w32(PORT_CLBU, 0);
    port_w32(PORT_FB,   bf_virt_to_phys(bf_fis_area));
    port_w32(PORT_FBU,  0);
    ahci_port_clear_errors();

    ahci_start_port();

    kprintf("[DISK] AHCI: disco di boot su porta %d (ABAR 0x%08x)\n",
            port, abar_phys);
    return true;
}

/* Cammina OGNI controller AHCI finche' uno da' un disco di boot usabile. */
static bool ahci_boot_init(void)
{
    for (int idx = 0; ; idx++)
    {
        uint32_t abar = pci_find_ahci_abar(idx);
        if (!abar) return false;            /* niente piu' controller AHCI */
        if (ahci_try_controller(abar))
            return true;
    }
}

/* Legge `count` settori a `lba` via la porta AHCI in buf. Polling. */
static bool ahci_boot_read(uint32_t lba, uint32_t count, void *buf)
{
    if (!s_ahci_abar || count == 0) return false;
    if (count > 128) return false;          /* una PRDT: bootfs sta sotto  */

    /* Ogni comando parte da porta pulita e inattiva: gli errori latchati
     * bloccherebbero l'avanzamento, e il Task File deve aver rilasciato
     * BSY/DRQ dal comando precedente. */
    ahci_port_clear_errors();
    ahci_port_wait_idle();

    /* Command header (slot 0): read, FIS length = 5 dword. */
    bf_cmd_list[0].flags = (sizeof(bf_fis_h2d_t) / 4) & 0x1F;   /* CFL       */
    bf_cmd_list[0].prdtl = 1;
    bf_cmd_list[0].prdbc = 0;
    bf_cmd_list[0].ctba  = bf_virt_to_phys(&bf_cmd_table);
    bf_cmd_list[0].ctbau = 0;

    {
        uint8_t *t = (uint8_t *)&bf_cmd_table;
        for (unsigned i = 0; i < sizeof(bf_cmd_table); i++) t[i] = 0;
    }
    uint32_t bytes = count * 512u;
    bf_cmd_table.prdt[0].dba  = bf_virt_to_phys(buf);
    bf_cmd_table.prdt[0].dbau = 0;
    bf_cmd_table.prdt[0].dbc  = (bytes - 1) | (1u << 31);       /* byte cnt, IOC */

    bf_fis_h2d_t *fis = (bf_fis_h2d_t *)bf_cmd_table.cfis;
    fis->fis_type = FIS_TYPE_H2D;
    fis->flags    = 0x80;                   /* C=1: comando                */
    fis->command  = ATA_CMD_READ_DMA_EX;
    fis->lba0   = (uint8_t)(lba);
    fis->lba1   = (uint8_t)(lba >> 8);
    fis->lba2   = (uint8_t)(lba >> 16);
    fis->device = 0x40;                     /* LBA mode                    */
    fis->lba3   = (uint8_t)(lba >> 24);
    fis->lba4   = 0;
    fis->lba5   = 0;
    fis->count  = (uint16_t)count;

    port_w32(PORT_CI, 1u << 0);
    (void)port_r32(PORT_CI);                /* posta la write prima di pollare */

    bool ok = false;
    for (int i = 0; i < 5000000; i++)
    {
        if (!(port_r32(PORT_CI) & (1u << 0))) { ok = true; break; }
        if (port_r32(PORT_IS) & PORT_IS_TFES) break;   /* task file error   */
    }
    if (!ok) return false;
    if (port_r32(PORT_TFD) & PORT_TFD_ERR) return false;

    return true;
}

/* ======================================================================
 * Logica principale: prova ATA, poi AHCI. In fondo, orchestra i blocchi.
 * ==================================================================== */

bool boot_disk_init(void)
{
    if (ata_boot_init())
    {
        s_boot_mode = BOOT_MODE_ATA;
        return true;
    }

    kprintf("[DISK] Nessun disco ATA IDE; provo AHCI...\n");
    if (ahci_boot_init())
    {
        s_boot_mode = BOOT_MODE_AHCI;
        return true;
    }

    kprintf("[DISK] Nessun disco ATA/AHCI.\n");
    return false;
}

bool boot_disk_read(uint32_t lba, uint32_t count, void *buf)
{
    return (s_boot_mode == BOOT_MODE_AHCI)
         ? ahci_boot_read(lba, count, buf)
         : ata_boot_read(lba, count, buf);
}

uint32_t boot_disk_total_sectors(void)
{
    return s_total_sectors;
}
