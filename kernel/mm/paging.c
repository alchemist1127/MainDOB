#include "mm/paging.h"
#include "mm/pmm.h"
#include "lib/string.h"
#include "console/console.h"
#include "arch/x86/cpu.h"
#include "kernel.h"

/* Paging 1.1 — passaggio dal PD di boot (PSE 4 MB) a un PD kernel a
 * pagine 4 KB con recursive mapping (PDE 1023 -> il PD stesso).
 *
 * Invariante di zeroing (lezione 1.0): una page table nuova viene
 * azzerata PRIMA che la sua PDE diventi visibile. In fase 1 e'
 * garantito allocando le PT kernel nella zona DMA (<16 MB), coperta
 * dal direct-map di boot a KERNEL_VMA: si azzera tramite l'alias
 * fisico, POI si pubblica la PDE. */

#define RECURSIVE_PD_VADDR      0xFFFFF000u
#define RECURSIVE_PT_VADDR(i)   (0xFFC00000u + ((i) << 12))

/* Tetto del direct-map kernel: da KERNEL_VMA a KERNEL_VMA+256MB. Oltre
 * inizia l'heap (0xD0000000). RAM oltre questo tetto (macchine >256 MB)
 * non e' direct-mappata e va raggiunta con una finestra temporanea. */
#define DIRECT_MAP_LIMIT        0x10000000u

/* Verbo esecutivo: pubblica il tetto del direct-map (vedi paging.h). Un
 * solo compito, nessuna decisione. Costante, quindi chiamabile a
 * qualunque fase di boot (il PMM lo consulta prima di paging_init). */
uint32_t paging_direct_map_limit(void)
{
    return DIRECT_MAP_LIMIT;
}

static uint32_t s_kernel_pd_phys;

static inline uint32_t *live_pd(void)
{
    return (uint32_t *)RECURSIVE_PD_VADDR;
}

static inline uint32_t *live_pt(uint32_t pd_index)
{
    return (uint32_t *)RECURSIVE_PT_VADDR(pd_index);
}

static inline void *direct_map(uint32_t phys)
{
    return (void *)(phys + KERNEL_VMA);
}

/* === Verbi di costruzione =============================================== */

/* Frame per page table, azzerato tramite il direct-map di boot.
 *
 * Valido SOLO finche' il recursive mapping non e' vivo: durante la
 * costruzione della finestra direct-map (chicken-and-egg, la finestra
 * mappa se stessa). Il frame DEVE cadere nel direct-map, quindi < 16 MB:
 * qui la zona bassa non e' preferenza ma requisito fisico. */
static uint32_t alloc_table_via_directmap(void)
{
    uint32_t phys = pmm_alloc_frame(PMM_ZONE_DMA);
    if (phys == 0)
    {
        kpanic("paging: zona bassa esaurita per il direct-map di boot");
    }
    memset(direct_map(phys), 0, PAGE_SIZE);     /* zero PRIMA della PDE  */
    return phys;
}

/* Frame per page table a runtime, azzerato tramite il recursive
 * mapping.
 *
 * Il recursive mapping raggiunge QUALSIASI frame fisico una volta
 * installata la PDE, quindi la tabella puo' stare ovunque in RAM
 * (nessun vincolo di zona: preferiamo i frame bassi solo per ridurre la
 * finestra di gara sul PD kernel condiviso). Il chiamante installa la
 * PDE, POI azzera all'indirizzo recursive: l'ordine e' obbligato, prima
 * dell'install il recursive non risolve il frame. */
static uint32_t alloc_table_frame(void)
{
    uint32_t phys = pmm_alloc_frame(PMM_ZONE_PREFER_LOW);
    if (phys == 0)
    {
        kpanic("paging: RAM esaurita per page table");
    }
    return phys;
}

static void map_boot_direct_window(uint32_t *pd)
{
    /* Primi 16 MB fisici -> 0xC0000000+ a pagine 4 KB con bit GLOBAL:
     * kernel, moduli, VGA e le PT stesse restano raggiungibili. */
    for (uint32_t block = 0; block < BOOT_DIRECT_MAP_MB / 4u; block++)
    {
        uint32_t pt_phys = alloc_table_via_directmap();
        uint32_t *pt = (uint32_t *)direct_map(pt_phys);

        for (uint32_t i = 0; i < 1024u; i++)
        {
            uint32_t phys = block * 0x400000u + i * PAGE_SIZE;
            pt[i] = phys | PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL;
        }

        pd[KERNEL_PD_INDEX + block] = pt_phys | PTE_PRESENT | PTE_WRITABLE;
    }
}

static void install_recursive_entry(uint32_t *pd, uint32_t pd_phys)
{
    pd[RECURSIVE_PD_INDEX] = pd_phys | PTE_PRESENT | PTE_WRITABLE;
}

static void switch_to_4kb_paging(uint32_t pd_phys)
{
    /* Il nuovo PD usa solo pagine 4 KB: valido con PSE acceso o spento.
     * Prima si carica CR3, POI si spegne PSE (l'ordine inverso
     * invaliderebbe il PD di boot ancora attivo). */
    cpu_write_cr3(pd_phys);

    uint32_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 &= ~0x10u;                              /* clear PSE              */
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));
}

/* Garantisce l'esistenza della page table per pd_index. */
static void ensure_page_table(uint32_t pd_index, uint32_t flags)
{
    uint32_t *pd = live_pd();

    if (pd[pd_index] & PTE_PRESENT)
    {
        if ((flags & PTE_USER) && !(pd[pd_index] & PTE_USER))
        {
            pd[pd_index] |= PTE_USER;
            cpu_invlpg(RECURSIVE_PT_VADDR(pd_index));
        }
        return;
    }

    /* Frame ovunque in RAM: installa la PDE PRIMA, poi azzera tramite il
     * recursive mapping (che ora risolve il frame). L'ordine e' obbligato
     * — prima dell'install il recursive non lo raggiungerebbe. La finestra
     * tra install e zero e' coperta dal lock del PD kernel; su un PD di
     * processo il frame non e' ancora osservabile da altri. */
    uint32_t pt_phys = alloc_table_frame();
    pd[pd_index] = pt_phys | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    cpu_invlpg(RECURSIVE_PT_VADDR(pd_index));
    memset((void *)RECURSIVE_PT_VADDR(pd_index), 0, PAGE_SIZE);
}

/* === Orchestratore ====================================================== */

void paging_init(void)
{
    /* Il PD kernel viene azzerato via direct_map PRIMA che il recursive
     * mapping esista, quindi DEVE cadere nel direct-map di boot (< 16 MB):
     * requisito fisico, non preferenza. Un solo frame, allocato per primo
     * quando la zona bassa e' ancora capiente. */
    s_kernel_pd_phys = pmm_alloc_frame(PMM_ZONE_DMA);
    if (s_kernel_pd_phys == 0)
    {
        kpanic("paging: zona bassa esaurita per il kernel PD");
    }

    uint32_t *pd = (uint32_t *)direct_map(s_kernel_pd_phys);
    memset(pd, 0, PAGE_SIZE);

    map_boot_direct_window(pd);
    install_recursive_entry(pd, s_kernel_pd_phys);
    switch_to_4kb_paging(s_kernel_pd_phys);

    /* Pre-alloca le page table dell'intera finestra heap/VA kernel e
     * dell'area PD_TEMP: l'insieme delle PDE kernel diventa IMMUTABILE
     * dopo questo punto, quindi lo snapshot che paging_create_directory
     * copia in ogni PD di processo e' completo per sempre. Senza,
     * una PT kernel installata a runtime esisterebbe solo nel PD
     * allora attivo: fault kernel dagli altri address space. */
    for (uint32_t idx = PAGE_DIR_INDEX(0xD0000000u);
         idx < PAGE_DIR_INDEX(0xE0000000u); idx++)
    {
        ensure_page_table(idx, 0);
    }
    ensure_page_table(PAGE_DIR_INDEX(0xFF800000u), 0);

    /* Estende il direct-map dai 16 MB di boot a tutta la RAM usabile
     * (fino al tetto della finestra, 256 MB: sopra c'e' l'heap kernel).
     * L'invariante di tutto il kernel — "il frame fisico F e' leggibile
     * a KERNEL_VMA+F" — vale cosi' per OGNI frame, non solo i primi 16
     * MB: caricatore ELF, azzeramento pagine utente, SHM e ACPI vi si
     * appoggiano. I frame oltre il tetto restano non mappati qui e vanno
     * raggiunti con una finestra temporanea (macchine con >256 MB). */
    uint32_t ram_top = pmm_get_ram_top();
    uint32_t map_top = (ram_top > DIRECT_MAP_LIMIT) ? DIRECT_MAP_LIMIT
                                                    : ram_top;
    paging_ensure_direct_map(map_top);

    kprintf("[PAGE] Paging 4KB attivo, PD kernel a 0x%08x, "
            "direct-map %u MB\n",
            s_kernel_pd_phys, map_top >> 20);
}

/* Le mutazioni di page table passano da un unico lock: due CPU che
 * installano PDE/PTE in parallelo (stack kernel, heap, mmio) non
 * possono ne' perdere una mappatura ne' fare doppia allocazione della
 * stessa page table. Le letture (get_physical) restano senza lock:
 * accessi a parola, atomici su x86. */
#include "sync/spinlock.h"
static spinlock_t s_map_lock = SPINLOCK_INIT;

static void map_page_locked(uint32_t virt, uint32_t phys, uint32_t flags)
{
    ensure_page_table(PAGE_DIR_INDEX(virt), flags);

    uint32_t *pt = live_pt(PAGE_DIR_INDEX(virt));
    pt[PAGE_TABLE_INDEX(virt)] =
        (phys & 0xFFFFF000u) | (flags & 0xFFFu) | PTE_PRESENT;

    cpu_invlpg(virt);
}

static void unmap_page_locked(uint32_t virt)
{
    uint32_t *pd = live_pd();
    if ((pd[PAGE_DIR_INDEX(virt)] & PTE_PRESENT) == 0)
    {
        return;
    }

    uint32_t *pt = live_pt(PAGE_DIR_INDEX(virt));
    pt[PAGE_TABLE_INDEX(virt)] = 0;
    cpu_invlpg(virt);
}

void paging_map_page(uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_map_lock);
    map_page_locked(virt, phys, flags);
    spinlock_release_irqrestore(&s_map_lock, fl);
}

void paging_unmap_page(uint32_t virt)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_map_lock);
    unmap_page_locked(virt);
    spinlock_release_irqrestore(&s_map_lock, fl);
}

/* Verbo esecutivo di anti-residuo: restituisce i frame delle page table
 * UTENTE completamente svuotate nel range [start, end). Senza, un
 * processo longevo che mappa/smappa per mesi (buffer IPC che crescono e
 * si ritirano, DMA di driver, mmap di servizio) accumula PT orfane —
 * 4 KB permanenti per ogni finestra da 4 MB mai toccata: la cicatrice
 * resta fino alla morte del processo, che per un servizio eterno e' MAI.
 *
 * Contratto: SOLO tavole con PTE_USER nel PDE (le tavole kernel e il
 * direct-map non si toccano: sono per la vita), SOLO dal contesto che
 * possiede l'address space, e con il chiamante che esegue il proprio
 * tlb_shootdown_aspace DOPO (come gia' fa per gli unmap: qui il flush
 * locale copre TLB e paging-structure cache della finestra, lo
 * shootdown del chiamante copre gli altri core). Una PT e' vuota solo
 * se OGNI entry e' zero: qualunque bit residuo la salva — mai liberare
 * cio' che qualcun altro potrebbe considerare stato. Ritorna i byte
 * restituiti al PMM. L'ordine lock paging->pmm e' quello gia'
 * stabilito da ensure_page_table. */
uint32_t paging_release_empty_user_tables(uint32_t start, uint32_t end)
{
    if (end <= start)
    {
        return 0;
    }
    uint32_t freed = 0;
    uint32_t fl = spinlock_acquire_irqsave(&s_map_lock);
    uint32_t *pd = live_pd();
    uint32_t first = PAGE_DIR_INDEX(start);
    uint32_t last  = PAGE_DIR_INDEX(end - 1u);
    for (uint32_t idx = first; idx <= last; idx++)
    {
        if ((pd[idx] & PTE_PRESENT) == 0 || (pd[idx] & PTE_USER) == 0)
        {
            continue;
        }
        uint32_t *pt = live_pt(idx);
        bool empty = true;
        for (uint32_t k = 0; k < 1024u; k++)
        {
            if (pt[k] != 0)
            {
                empty = false;
                break;
            }
        }
        if (!empty)
        {
            continue;
        }
        uint32_t pt_phys = pd[idx] & 0xFFFFF000u;
        pd[idx] = 0;
        cpu_invlpg(idx << 22);              /* PDE-cache della finestra */
        cpu_invlpg(RECURSIVE_PT_VADDR(idx));/* finestra ricorsiva       */
        pmm_free_frame(pt_phys);
        freed += PAGE_SIZE;
    }
    spinlock_release_irqrestore(&s_map_lock, fl);
    return freed;
}

uint32_t paging_get_physical(uint32_t virt, uint32_t *out_flags)
{
    uint32_t *pd = live_pd();
    if ((pd[PAGE_DIR_INDEX(virt)] & PTE_PRESENT) == 0)
    {
        return 0;
    }

    uint32_t entry = live_pt(PAGE_DIR_INDEX(virt))[PAGE_TABLE_INDEX(virt)];
    if ((entry & PTE_PRESENT) == 0)
    {
        return 0;
    }

    if (out_flags != NULL)
    {
        *out_flags = entry & 0xFFFu;
    }
    return PTE_FRAME(entry) | (virt & 0xFFFu);
}

uint32_t paging_kernel_directory(void)
{
    return s_kernel_pd_phys;
}

bool paging_ensure_direct_map(uint32_t phys_end)
{
    if (phys_end > DIRECT_MAP_LIMIT)
    {
        kprintf("[PAGE] direct-map richiesto oltre il limite (0x%08x)\n",
                phys_end);
        return false;
    }

    uint32_t mapped_end = BOOT_DIRECT_MAP_MB * 1024u * 1024u;
    for (uint32_t phys = mapped_end; phys < phys_end; phys += PAGE_SIZE)
    {
        uint32_t virt = phys + KERNEL_VMA;
        if (paging_get_physical(virt, NULL) != 0)
        {
            continue;                           /* gia' coperto           */
        }
        paging_map_page(virt, phys,
                        PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL);
    }
    return true;
}

/* === Self-test di boot ================================================== */

bool paging_selftest(void)
{
    /* Mappa un frame nuovo in una finestra di test fuori dal direct-map,
     * scrive un pattern, verifica la traduzione, smonta e libera. */
    const uint32_t test_va = 0xE0000000u;

    uint32_t frame = pmm_alloc_frame(PMM_ZONE_ANY);
    if (frame == 0)
    {
        return false;
    }

    paging_map_page(test_va, frame, PTE_WRITABLE);

    volatile uint32_t *probe = (volatile uint32_t *)test_va;
    *probe = 0xD0B00011u;
    bool wrote = (*probe == 0xD0B00011u);

    bool translated = (paging_get_physical(test_va, NULL) == frame);

    paging_unmap_page(test_va);
    bool unmapped = (paging_get_physical(test_va, NULL) == 0);

    pmm_free_frame(frame);
    return wrote && translated && unmapped;
}

/* === Address space per-processo ========================================= */

uint32_t paging_current_directory(void)
{
    return cpu_read_cr3() & 0xFFFFF000u;
}

void paging_switch_directory(uint32_t pd_phys)
{
    cpu_write_cr3(pd_phys);
}

/* Slot temporanei per manipolare un PD non attivo, serializzati. */
#include "sync/spinlock.h"
static spinlock_t s_pd_temp_lock = SPINLOCK_INIT;
#define PD_TEMP_A 0xFF800000u
#define PD_TEMP_B 0xFF801000u

uint32_t paging_create_directory(void)
{
    uint32_t new_pd = pmm_alloc_frame(PMM_ZONE_ANY);
    if (new_pd == 0)
    {
        return 0;
    }

    uint32_t fl = spinlock_acquire_irqsave(&s_pd_temp_lock);

    paging_map_page(PD_TEMP_A, new_pd, PTE_WRITABLE);
    uint32_t *dst = (uint32_t *)PD_TEMP_A;
    uint32_t *cur = live_pd();

    /* Utente vuoto; PDE kernel condivise (768..1022); ricorsiva -> se stesso. */
    memset(dst, 0, KERNEL_PD_INDEX * sizeof(uint32_t));
    for (uint32_t i = KERNEL_PD_INDEX; i < RECURSIVE_PD_INDEX; i++)
    {
        dst[i] = cur[i];
    }
    dst[RECURSIVE_PD_INDEX] = new_pd | PTE_PRESENT | PTE_WRITABLE;

    paging_unmap_page(PD_TEMP_A);
    spinlock_release_irqrestore(&s_pd_temp_lock, fl);
    return new_pd;
}

void paging_destroy_directory(uint32_t pd_phys)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_pd_temp_lock);

    paging_map_page(PD_TEMP_A, pd_phys, PTE_WRITABLE);
    uint32_t *pd = (uint32_t *)PD_TEMP_A;

    /* Solo le PDE utente (0..767): le tabelle kernel sono condivise ed
     * eterne, non vanno liberate. Per ogni PT utente, libera i frame
     * fisici mappati, poi la PT stessa. */
    for (uint32_t i = 0; i < KERNEL_PD_INDEX; i++)
    {
        if ((pd[i] & PTE_PRESENT) == 0)
        {
            continue;
        }
        uint32_t pt_phys = PTE_FRAME(pd[i]);

        paging_map_page(PD_TEMP_B, pt_phys, PTE_WRITABLE);
        uint32_t *pt = (uint32_t *)PD_TEMP_B;
        for (uint32_t j = 0; j < 1024u; j++)
        {
            if (pt[j] & PTE_PRESENT)
            {
                pmm_free_frame(PTE_FRAME(pt[j]));
            }
        }
        pmm_free_frame(pt_phys);
    }

    paging_unmap_page(PD_TEMP_A);
    paging_unmap_page(PD_TEMP_B);
    spinlock_release_irqrestore(&s_pd_temp_lock, fl);

    pmm_free_frame(pd_phys);
}

/* Mappa una pagina in un PD ARBITRARIO (attivo o meno) commutando CR3
 * sotto IF=0: la finestra e' brevissima e a dimensione fissa (una PTE),
 * quindi rispetta D6 anche quando serve popolare l'AS di un figlio. */
void paging_unmap_in(uint32_t pd_phys, uint32_t virt)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_map_lock);
    uint32_t old = paging_current_directory();
    bool switched = (pd_phys != old);

    if (switched)
    {
        cpu_write_cr3(pd_phys);
    }
    unmap_page_locked(virt);
    if (switched)
    {
        cpu_write_cr3(old);
    }
    spinlock_release_irqrestore(&s_map_lock, fl);
}

uint32_t paging_get_physical_in(uint32_t pd_phys, uint32_t virt)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_map_lock);
    uint32_t old = paging_current_directory();
    bool switched = (pd_phys != old);

    if (switched)
    {
        cpu_write_cr3(pd_phys);
    }
    uint32_t phys = paging_get_physical(virt, NULL);
    if (switched)
    {
        cpu_write_cr3(old);
    }
    spinlock_release_irqrestore(&s_map_lock, fl);
    return phys;
}

void paging_map_in(uint32_t pd_phys, uint32_t virt, uint32_t phys,
                   uint32_t flags)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_map_lock);
    uint32_t old = paging_current_directory();
    bool switched = (pd_phys != old);

    if (switched)
    {
        cpu_write_cr3(pd_phys);
    }
    map_page_locked(virt, phys, flags);
    if (switched)
    {
        cpu_write_cr3(old);
    }
    spinlock_release_irqrestore(&s_map_lock, fl);
}
