#ifndef MAINDOB_ACPI_ACPI_H
#define MAINDOB_ACPI_ACPI_H

#include "lib/types.h"

/* ACPI MainDOB — lettore minimale di tabelle statiche. NON e'
 * un'implementazione ACPI generale: niente interprete AML (salvo la
 * scansione mirata di \_S5), niente power management runtime. Recupera:
 *
 *   MADT ("APIC") — LAPIC base, CPU logiche (enumerazione SMP), IOAPIC,
 *                   Interrupt Source Override (IRQ legacy -> GSI).
 *   FADT ("FACP") — registri PM fissi per lo spegnimento S5, piu' i
 *                   SLP_TYP dal \_S5 del DSDT.
 *
 * Ogni entry point degrada in silenzio: senza RSDP (QEMU 'pc', Compaq
 * Armada E500) i contatori rispondono 0 e il chiamante resta su
 * PIC/uniprocessore/porte legacy senza branch di piattaforma.
 * MCFG/ECAM: rimandato al milestone PCI (nessun consumer ancora). */

/* --- Layout firmware (definiti dalla spec: packed, non toccare) ------- */

typedef struct
{
    char     signature[8];      /* "RSD PTR " (spazio finale incluso)  */
    uint8_t  checksum;          /* copre i byte 0..19                  */
    char     oem_id[6];
    uint8_t  revision;          /* 0 = ACPI 1.0, 2 = ACPI 2.0+         */
    uint32_t rsdt_address;
    /* Solo ACPI 2.0+ (revision >= 2): */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum; /* copre `length` byte                 */
    uint8_t  reserved[3];
} PACKED acpi_rsdp_t;

/* Header comune (36 byte) di ogni tabella di sistema. */
typedef struct
{
    char     signature[4];
    uint32_t length;            /* lunghezza totale, header incluso    */
    uint8_t  revision;
    uint8_t  checksum;          /* l'intera tabella somma a 0          */
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} PACKED acpi_sdt_header_t;

/* Record MADT tipo 0: Processor Local APIC (una per CPU logica). */
typedef struct
{
    uint8_t  type;              /* 0 */
    uint8_t  length;            /* 8 */
    uint8_t  acpi_id;
    uint8_t  apic_id;           /* bersaglio INIT/SIPI                 */
    uint32_t flags;             /* bit0 = Enabled                      */
} PACKED acpi_madt_lapic_t;

#define ACPI_MADT_LAPIC_ENABLED  0x1

/* Record MADT tipo 1: I/O APIC. */
typedef struct
{
    uint8_t  type;              /* 1 */
    uint8_t  length;            /* 12 */
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t address;           /* base fisica MMIO                    */
    uint32_t gsi_base;          /* primo GSI gestito                   */
} PACKED acpi_madt_ioapic_t;

/* Record MADT tipo 2: Interrupt Source Override. */
typedef struct
{
    uint8_t  type;              /* 2 */
    uint8_t  length;            /* 10 */
    uint8_t  bus;               /* sempre 0 (ISA)                      */
    uint8_t  source;            /* IRQ legacy rimappato                */
    uint32_t gsi;               /* GSI effettivo di consegna           */
    uint16_t flags;             /* MPS INTI: polarita' + trigger       */
} PACKED acpi_madt_iso_t;

#define ACPI_MADT_POLARITY_MASK  0x3
#define ACPI_MADT_POLARITY_HIGH  0x1
#define ACPI_MADT_POLARITY_LOW   0x3
#define ACPI_MADT_TRIGGER_MASK   0xC
#define ACPI_MADT_TRIGGER_EDGE   0x4
#define ACPI_MADT_TRIGGER_LEVEL  0xC

/* --- Risultati d'enumerazione (per i consumer) ------------------------ */

typedef struct
{
    uint32_t address;           /* base fisica MMIO                    */
    uint32_t gsi_base;
    uint8_t  id;
} acpi_ioapic_info_t;

typedef struct
{
    uint8_t  apic_id;           /* bersaglio SIPI                      */
    uint8_t  acpi_id;
    bool     enabled;           /* flag Enabled della MADT             */
} acpi_cpu_info_t;

/* Registri PM fissi dal FADT + SLP_TYP dal \_S5 (per SYS_SHUTDOWN).
 * `present` false = niente FADT usabile (emulatori senza ACPI):
 * SYS_SHUTDOWN usa le porte legacy note. */
typedef struct
{
    bool     present;
    uint32_t pm1a_cnt;          /* porta I/O PM1a_CNT_BLK              */
    uint32_t pm1b_cnt;          /* PM1b_CNT_BLK (0 se assente)         */
    uint32_t smi_cmd;           /* porta SMI command (0 se assente)    */
    uint8_t  acpi_enable;       /* valore per entrare in modo ACPI     */
    uint8_t  pm1_cnt_len;

    bool     s5_found;          /* \_S5 letto dal DSDT                 */
    uint8_t  slp_typ_a;         /* SLP_TYP per PM1a (3 bit)            */
    uint8_t  slp_typ_b;
} acpi_pm_info_t;

/* --- API --------------------------------------------------------------- */

/* Trova l'RSDP, cammina RSDT/XSDT, parsea MADT + FADT (+ \_S5). Dopo
 * paging + heap (usa mmio_map per le tabelle oltre il direct-map).
 * Idempotente; mai fault su macchina senza ACPI. */
void acpi_init(void);

/* Base fisica del Local APIC dalla MADT (0 se assente). */
uint32_t acpi_lapic_phys(void);

/* IOAPIC scoperte e dettaglio per indice. 0/false senza MADT. */
uint32_t acpi_ioapic_count(void);
bool     acpi_ioapic_get(uint32_t index, acpi_ioapic_info_t *out);

/* Risolve un IRQ ISA legacy nel GSI di consegna effettivo, applicando
 * gli override della MADT (e' cio' che fa funzionare IRQ0 -> GSI2 del
 * timer). Ritorna il GSI (== irq senza override); *out_flags riceve i
 * flag MPS INTI (0 senza override). */
uint32_t acpi_resolve_gsi(uint8_t irq, uint16_t *out_flags);

/* Enumerazione CPU dalla MADT. 0 = niente ACPI: la macchina e'
 * uniprocessore, la BSP in esecuzione e' l'unica CPU. */
uint32_t acpi_cpu_count(void);          /* voci totali (anche disabled) */
uint32_t acpi_cpu_enabled_count(void);  /* avviabili, BSP inclusa       */
bool     acpi_cpu_get(uint32_t index, acpi_cpu_info_t *out);

/* Info PM cachate dal FADT. Valido dopo acpi_init(). */
const acpi_pm_info_t *acpi_pm(void);

#endif /* MAINDOB_ACPI_ACPI_H */
