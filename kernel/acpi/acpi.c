#include "acpi/acpi.h"
#include "kernel.h"
#include "mm/mmio_map.h"
#include "console/console.h"
#include "lib/string.h"

/* L'RSDP sta sotto 1 MB, raggiungibile dal direct-map del boot
 * (KERNEL_VMA + phys). Le tabelle puntate possono stare in ACPI-reclaim
 * oltre il direct-map: ognuna e' mappata a richiesta via mmio_map
 * (cache_disable=false: sono RAM, non registri) e smontata dopo l'uso. */

#define PHYS_TO_DIRECT(p)   ((void *)(KERNEL_VMA + (uint32_t)(p)))
#define DIRECT_LIMIT        (BOOT_DIRECT_MAP_MB * 1024u * 1024u)

#define EBDA_POINTER_ADDR   0x40Eu      /* segmento EBDA (16 bit)       */
#define BIOS_AREA_START     0xE0000u
#define BIOS_AREA_END       0x100000u

#define ACPI_MAX_IOAPICS    8u
#define ACPI_MAX_OVERRIDES  16u
#define ACPI_MAX_CPUS       8u

typedef struct
{
    uint8_t  source;
    uint32_t gsi;
    uint16_t flags;
} iso_info_t;

static bool               s_initialized;
static uint32_t           s_lapic_phys;

static acpi_ioapic_info_t s_ioapics[ACPI_MAX_IOAPICS];
static uint32_t           s_ioapic_n;
static iso_info_t         s_overrides[ACPI_MAX_OVERRIDES];
static uint32_t           s_override_n;

static acpi_cpu_info_t    s_cpus[ACPI_MAX_CPUS];
static uint32_t           s_cpu_n;
static uint32_t           s_cpu_enabled_n;

static acpi_pm_info_t     s_pm;         /* zero-init: present = false   */

/* === Verbi: checksum e RSDP ============================================ */

static bool checksum_ok(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
    {
        sum = (uint8_t)(sum + p[i]);
    }
    return sum == 0;
}

static bool rsdp_validate(const acpi_rsdp_t *cand)
{
    if (memcmp(cand->signature, "RSD PTR ", 8) != 0)
    {
        return false;
    }
    if (!checksum_ok(cand, 20))         /* checksum ACPI 1.0 (20 byte) */
    {
        return false;
    }
    if (cand->revision >= 2)            /* 2.0+: checksum esteso        */
    {
        if (cand->length < sizeof(acpi_rsdp_t)
            || !checksum_ok(cand, cand->length))
        {
            return false;
        }
    }
    return true;
}

/* Scansione di [start, end) fisici (dentro il direct-map) su confini di
 * 16 byte, come da spec. */
static const acpi_rsdp_t *rsdp_scan_span(uint32_t start, uint32_t end)
{
    for (uint32_t addr = start; addr + 8 <= end; addr += 16)
    {
        const acpi_rsdp_t *cand = (const acpi_rsdp_t *)PHYS_TO_DIRECT(addr);
        if (rsdp_validate(cand))
        {
            return cand;
        }
    }
    return NULL;
}

static const acpi_rsdp_t *rsdp_find(void)
{
    /* 1. Il primo KB dell'EBDA (segmento a 16 bit in 0x40E). */
    uint16_t ebda_seg = *(const uint16_t *)PHYS_TO_DIRECT(EBDA_POINTER_ADDR);
    uint32_t ebda = (uint32_t)ebda_seg << 4;
    if (ebda >= 0x400 && ebda + 1024 <= DIRECT_LIMIT)
    {
        const acpi_rsdp_t *r = rsdp_scan_span(ebda, ebda + 1024);
        if (r != NULL)
        {
            return r;
        }
    }

    /* 2. L'area BIOS read-only 0xE0000..0xFFFFF. */
    return rsdp_scan_span(BIOS_AREA_START, BIOS_AREA_END);
}

/* === Verbi: mappatura tabelle ========================================== */

/* Mappa una tabella per indirizzo fisico, valida il checksum, restituisce
 * puntatore + cookie per lo smontaggio. Prima mappa il solo header per
 * leggere la lunghezza, poi rimappa l'intera tabella. */
static acpi_sdt_header_t *map_table(uint64_t phys, uint32_t *virt,
                                    uint32_t *pages)
{
    if (phys == 0)
    {
        return NULL;
    }

    uint32_t hv, hp;
    acpi_sdt_header_t *hdr = (acpi_sdt_header_t *)
        mmio_map(phys, sizeof(acpi_sdt_header_t), false, &hv, &hp);
    if (hdr == NULL)
    {
        return NULL;
    }
    uint32_t len = hdr->length;
    mmio_unmap(hv, hp);

    if (len < sizeof(acpi_sdt_header_t) || len > (4u * 1024u * 1024u))
    {
        return NULL;                    /* lunghezza implausibile */
    }

    acpi_sdt_header_t *full = (acpi_sdt_header_t *)
        mmio_map(phys, len, false, virt, pages);
    if (full == NULL)
    {
        return NULL;
    }
    if (!checksum_ok(full, full->length))
    {
        mmio_unmap(*virt, *pages);
        return NULL;
    }
    return full;
}

/* === Verbi: parsing MADT =============================================== */

static void madt_take_lapic(const acpi_madt_lapic_t *lapic)
{
    if (s_cpu_n >= ACPI_MAX_CPUS)
    {
        return;
    }
    bool enabled = (lapic->flags & ACPI_MADT_LAPIC_ENABLED) != 0;
    s_cpus[s_cpu_n].apic_id = lapic->apic_id;
    s_cpus[s_cpu_n].acpi_id = lapic->acpi_id;
    s_cpus[s_cpu_n].enabled = enabled;
    s_cpu_n++;
    if (enabled)
    {
        s_cpu_enabled_n++;
    }
}

static void madt_take_ioapic(const acpi_madt_ioapic_t *io)
{
    if (s_ioapic_n >= ACPI_MAX_IOAPICS)
    {
        return;
    }
    s_ioapics[s_ioapic_n].address  = io->address;
    s_ioapics[s_ioapic_n].gsi_base = io->gsi_base;
    s_ioapics[s_ioapic_n].id       = io->ioapic_id;
    s_ioapic_n++;
}

static void madt_take_override(const acpi_madt_iso_t *iso)
{
    if (s_override_n >= ACPI_MAX_OVERRIDES)
    {
        return;
    }
    s_overrides[s_override_n].source = iso->source;
    s_overrides[s_override_n].gsi    = iso->gsi;
    s_overrides[s_override_n].flags  = iso->flags;
    s_override_n++;
    kprintf("[ACPI] Override MADT: IRQ %u -> GSI %u (flags 0x%x)\n",
            (uint32_t)iso->source, iso->gsi, (uint32_t)iso->flags);
}

/* MADT: header, uint32 base LAPIC, uint32 flags, poi record variabili
 * (tipo, lunghezza, corpo). Gli override sono essenziali: su quasi ogni
 * macchina ACPI il PIT (IRQ0) arriva su GSI2 — senza il remap il timer
 * non arriva mai. */
static void parse_madt(const acpi_sdt_header_t *madt)
{
    const uint8_t *base = (const uint8_t *)madt;
    if (madt->length < sizeof(acpi_sdt_header_t) + 8)
    {
        return;
    }

    s_lapic_phys = *(const uint32_t *)(base + sizeof(acpi_sdt_header_t));

    uint32_t off = sizeof(acpi_sdt_header_t) + 8;
    while (off + 2 <= madt->length)
    {
        uint8_t type = base[off];
        uint8_t rlen = base[off + 1];
        if (rlen < 2)
        {
            break;                      /* malformata: niente loop infinito */
        }

        if (type == 0 && off + sizeof(acpi_madt_lapic_t) <= madt->length)
        {
            madt_take_lapic((const acpi_madt_lapic_t *)(base + off));
        }
        else if (type == 1 && off + sizeof(acpi_madt_ioapic_t) <= madt->length)
        {
            madt_take_ioapic((const acpi_madt_ioapic_t *)(base + off));
        }
        else if (type == 2 && off + sizeof(acpi_madt_iso_t) <= madt->length)
        {
            madt_take_override((const acpi_madt_iso_t *)(base + off));
        }

        off += rlen;
    }

    if (s_lapic_phys != 0 || s_ioapic_n != 0 || s_cpu_n != 0)
    {
        kprintf("[ACPI] MADT: LAPIC 0x%08x  %u CPU (%u abilitate)  "
                "%u IOAPIC  %u override\n",
                s_lapic_phys, s_cpu_n, s_cpu_enabled_n,
                s_ioapic_n, s_override_n);
    }
}

/* === Verbi: FADT + \_S5 ================================================ */

/* Lettore minimale di \_S5: cerca "_S5_" nel byte stream AML del DSDT e
 * estrae gli SLP_TYP dal Package dichiarato. Riconosce SOLO la forma
 * Name(\_S5, Package(){a, b, ...}); ogni avanzamento e' bounds-checked
 * — una tabella troncata non blocca ne' legge oltre la mappatura. Sugli
 * emulatori il fallback 0 e' corretto; sul ferro vero (Armada, Extensa)
 * i valori del firmware sono quelli che rendono lo spegnimento reale. */
static void scan_dsdt_s5(uint64_t dsdt_phys)
{
    uint32_t virt, pages;
    acpi_sdt_header_t *dsdt = map_table(dsdt_phys, &virt, &pages);
    if (dsdt == NULL)
    {
        return;
    }

    const uint8_t *p   = (const uint8_t *)dsdt;
    uint32_t       len = dsdt->length;

    for (uint32_t i = sizeof(acpi_sdt_header_t); i + 4 < len; i++)
    {
        if (p[i] != '_' || p[i+1] != 'S' || p[i+2] != '5' || p[i+3] != '_')
        {
            continue;
        }

        /* Serve una Name() (NameOp 0x08, con opzionale prefisso '\')
         * il cui valore e' un Package (PackageOp 0x12). */
        const uint8_t *s = &p[i];
        bool name_ok = (i >= 1 && s[-1] == 0x08)
                    || (i >= 2 && s[-2] == 0x08 && s[-1] == '\\');
        if (!name_ok || s[4] != 0x12)
        {
            continue;
        }

        s += 5;                             /* "_S5_" + PackageOp        */
        if ((uint32_t)(s - p) >= len) break;
        s += ((*s & 0xC0) >> 6) + 1;        /* byte di PkgLength         */
        if ((uint32_t)(s - p) >= len) break;
        s += 1;                             /* NumElements               */
        if ((uint32_t)(s - p) >= len) break;

        /* Elemento 0 (SLP_TYPa): BytePrefix(0x0A)+valore, oppure un
         * opcode Zero/One il cui byte E' gia' il valore. */
        if (*s == 0x0A)
        {
            s++;
            if ((uint32_t)(s - p) >= len) break;
        }
        s_pm.slp_typ_a = (uint8_t)(*s & 0x07);
        s++;
        if ((uint32_t)(s - p) < len)        /* elemento 1 (SLP_TYPb)     */
        {
            if (*s == 0x0A)
            {
                s++;
            }
            if ((uint32_t)(s - p) < len)
            {
                s_pm.slp_typ_b = (uint8_t)(*s & 0x07);
            }
        }
        s_pm.s5_found = true;
        kprintf("[ACPI] \\_S5: SLP_TYPa=%u SLP_TYPb=%u\n",
                (uint32_t)s_pm.slp_typ_a, (uint32_t)s_pm.slp_typ_b);
        break;
    }

    mmio_unmap(virt, pages);
}

/* FADT: registri PM fissi per lo spegnimento S5 + puntatore al DSDT.
 * Lettura per offset di spec (stabili nella porzione letta), con
 * guardia sulla tabella troncata. */
static void parse_fadt(const acpi_sdt_header_t *fadt)
{
    const uint8_t *b = (const uint8_t *)fadt;
    if (fadt->length < 90)              /* fino a PM1_CNT_LEN (byte 89) */
    {
        return;
    }

    s_pm.smi_cmd     = *(const uint32_t *)(b + 48);
    s_pm.acpi_enable = b[52];
    s_pm.pm1a_cnt    = *(const uint32_t *)(b + 64);
    s_pm.pm1b_cnt    = *(const uint32_t *)(b + 68);
    s_pm.pm1_cnt_len = b[89];
    s_pm.present     = (s_pm.pm1a_cnt != 0);

    kprintf("[ACPI] FADT: PM1a_CNT 0x%04x PM1b_CNT 0x%04x "
            "SMI_CMD 0x%04x ACPI_EN 0x%02x\n",
            s_pm.pm1a_cnt, s_pm.pm1b_cnt, s_pm.smi_cmd,
            (uint32_t)s_pm.acpi_enable);

    /* DSDT: X_DSDT a 64 bit (2.0+, byte 140) se presente, altrimenti il
     * puntatore a 32 bit (byte 40). */
    uint64_t dsdt_phys = 0;
    if (fadt->length >= 148)
    {
        dsdt_phys = *(const uint64_t *)(b + 140);
    }
    if (dsdt_phys == 0 && fadt->length >= 44)
    {
        dsdt_phys = *(const uint32_t *)(b + 40);
    }
    if (dsdt_phys != 0)
    {
        scan_dsdt_s5(dsdt_phys);
    }
}

/* === Verbi: walk della radice ========================================== */

static void handle_table(uint64_t phys)
{
    uint32_t virt, pages;
    acpi_sdt_header_t *hdr = map_table(phys, &virt, &pages);
    if (hdr == NULL)
    {
        return;
    }

    if (memcmp(hdr->signature, "APIC", 4) == 0)         /* MADT */
    {
        parse_madt(hdr);
    }
    else if (memcmp(hdr->signature, "FACP", 4) == 0)    /* FADT */
    {
        parse_fadt(hdr);
    }
    /* MCFG: rimandato al milestone PCI (vedi acpi.h). */

    mmio_unmap(virt, pages);
}

static void walk_root(uint64_t root_phys, bool is_xsdt)
{
    uint32_t virt, pages;
    acpi_sdt_header_t *root = map_table(root_phys, &virt, &pages);
    if (root == NULL)
    {
        return;
    }

    uint32_t entry_size = is_xsdt ? 8 : 4;
    uint32_t count = (root->length - sizeof(acpi_sdt_header_t)) / entry_size;
    const uint8_t *ptrs = (const uint8_t *)root + sizeof(acpi_sdt_header_t);

    for (uint32_t i = 0; i < count; i++)
    {
        uint64_t tbl = is_xsdt
                     ? *(const uint64_t *)(ptrs + i * 8)
                     : *(const uint32_t *)(ptrs + i * 4);
        handle_table(tbl);
    }

    mmio_unmap(virt, pages);
}

/* === API =============================================================== */

void acpi_init(void)
{
    if (s_initialized)
    {
        return;
    }
    s_initialized = true;

    const acpi_rsdp_t *rsdp = rsdp_find();
    if (rsdp == NULL)
    {
        kprintf("[ACPI] Nessun RSDP: piattaforma senza ACPI "
                "(PIC + uniprocessore).\n");
        return;
    }

    kprintf("[ACPI] RSDP trovato, revisione %u\n", (uint32_t)rsdp->revision);

    /* XSDT (puntatori a 64 bit) su 2.0+, altrimenti RSDT. */
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0)
    {
        walk_root(rsdp->xsdt_address, true);
    }
    else
    {
        walk_root((uint64_t)rsdp->rsdt_address, false);
    }
}

uint32_t acpi_lapic_phys(void)
{
    return s_lapic_phys;
}

uint32_t acpi_ioapic_count(void)
{
    return s_ioapic_n;
}

bool acpi_ioapic_get(uint32_t index, acpi_ioapic_info_t *out)
{
    if (index >= s_ioapic_n || out == NULL)
    {
        return false;
    }
    *out = s_ioapics[index];
    return true;
}

uint32_t acpi_resolve_gsi(uint8_t irq, uint16_t *out_flags)
{
    for (uint32_t i = 0; i < s_override_n; i++)
    {
        if (s_overrides[i].source == irq)
        {
            if (out_flags != NULL)
            {
                *out_flags = s_overrides[i].flags;
            }
            return s_overrides[i].gsi;
        }
    }
    if (out_flags != NULL)
    {
        *out_flags = 0;
    }
    return irq;                         /* nessun override: identita' */
}

uint32_t acpi_cpu_count(void)
{
    return s_cpu_n;
}

uint32_t acpi_cpu_enabled_count(void)
{
    return s_cpu_enabled_n;
}

bool acpi_cpu_get(uint32_t index, acpi_cpu_info_t *out)
{
    if (index >= s_cpu_n || out == NULL)
    {
        return false;
    }
    *out = s_cpus[index];
    return true;
}

const acpi_pm_info_t *acpi_pm(void)
{
    return &s_pm;
}
