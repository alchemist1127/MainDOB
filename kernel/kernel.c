#include "kernel.h"
#include "console/console.h"
#include "boot/boot_info.h"
#include "arch/x86/gdt.h"
#include "arch/x86/idt.h"
#include "arch/x86/isr.h"
#include "arch/x86/irq.h"
#include "arch/x86/pit.h"
#include "arch/x86/tsc.h"
#include "arch/x86/cpu.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/kheap.h"
#include "mm/dma_pool.h"
#include "time/clock.h"
#include "time/event.h"
#include "time/timer.h"
#include "proc/thread.h"
#include "proc/scheduler.h"
#include "proc/process.h"
#include "proc/workqueue.h"
#include "proc/futex.h"
#include "ipc/channel.h"
#include "ipc/port.h"
#include "registry.h"
#include "syscall/syscall.h"
#include "mm/shm.h"
#include "sync/event_group.h"
#include "time/rtc.h"
#include "krt/entropy.h"
#include "arch/x86/fpu.h"
#include "boot/startup.h"
#include "boot/bootfs.h"
#include "acpi/acpi.h"
#include "arch/x86/intr.h"
#include "irq/pirq.h"
#include "lib/string.h"
#include "sync/spinlock.h"
#include "arch/x86/cpu_features.h"
#include "arch/x86/lapic.h"
#include "arch/x86/ioapic.h"
#include "arch/x86/smp.h"

/* === Stage 1: console + boot info ======================================= */

/* Logo ASCII osservato dal kernel 1.0 (kernel/kernel.c, print_banner):
 * stesso wordmark "MainDOB", invariato — e' identita' visiva, non ABI,
 * ma non c'e' motivo di ridisegnarlo. Sotto, codename/versione/copyright
 * (il 1.0 non aveva un codename separato; qui lo aggiungiamo perche' il
 * 1.1 ce l'ha, vedi kernel.h). */
static void print_banner(void)
{
    console_set_color(CON_LIGHT_CYAN, CON_BLACK);
    kprintf("  __  __       _       ____   ___  ____  \n");
    kprintf(" |  \\/  | __ _(_)_ __ |  _ \\ / _ \\| __ ) \n");
    kprintf(" | |\\/| |/ _` | | '_ \\| | | | | | |  _ \\ \n");
    kprintf(" | |  | | (_| | | | | | |_| | |_| | |_) |\n");
    kprintf(" |_|  |_|\\__,_|_|_| |_|____/ \\___/|____/ \n");
    console_set_color(CON_LIGHT_GREY, CON_BLACK);
    kprintf(" %s - Kernel %s (%s)\n", MAINDOB_NAME, MAINDOB_VERSION_STRING,
            MAINDOB_CODENAME);
    kprintf(" %s\n\n", MAINDOB_COPYRIGHT);
}

static void stage_console_online(uint32_t magic, uint32_t mbi_phys)
{
    console_init();
    print_banner();
    boot_info_init(magic, mbi_phys);
}

/* === Stage 2: CPU + interrupt =========================================== */

static void stage_cpu_online(void)
{
    cpu_features_init();                /* prima di tutto: serve a tsc/lapic */
    gdt_init();
    idt_init();
    isr_init();
    irq_init();
    fpu_init_this_cpu();                /* prima di qualunque thread      */
}

/* === Stage 3: memoria =================================================== */

static void stage_memory_online(void)
{
    pmm_init();
    paging_init();
    kheap_init();
    dma_pool_init();                    /* riserva contigua a RAM vergine */
    acpi_init();                        /* RSDP in RAM bassa; le tabelle  */
}                                       /* oltre i 16MB usano scratch-map */

/* === Stage 4: tempo ===================================================== */

/* Trattiene la splash sullo schermo per `ms` millisecondi. Puro busy-
 * wait (niente scheduler vivo): su TSC calibrato spinna sul monotono;
 * nel fallback no-TSC (periodico gia' avviato dallo stage) conta i
 * tick del PIT. */
static void splash_hold_ms(uint32_t ms)
{
    if (tsc_hz() != 0)
    {
        tsc_busy_wait_ns(MS_TO_NS(ms));
        return;
    }
    uint64_t target = pit_ticks() + ms;
    while (pit_ticks() < target)
    {
        __asm__ volatile ("pause");
    }
}

/* Il PIT non genera piu' alcun tick sulle macchine con TSC: la
 * calibrazione e' una finestra mode-0 SINCRONA (polling, IF=0 ok) e il
 * monotono e' il TSC da subito. Il periodico parte SOLO qui, una
 * volta, se la calibrazione fallisce — l'unico punto di decisione;
 * time_event_try_enable trovera' tsc_hz()==0 e restera' sul periodico. */
static void stage_time_online(void)
{
    timer_subsystem_init();
    tsc_init();                         /* finestra PIT sincrona, no IRQ */
    if (tsc_hz() == 0)
    {
        pit_init();                     /* estremo fallback: periodico   */
    }
    clock_init();
    cpu_sti();
    splash_hold_ms(3000);               /* la splash resta visibile 3s   */
    rtc_init();
    entropy_init();
}

/* === Stage 5: esecuzione + servizi kernel =============================== */

static void stage_kernel_services_online(void)
{
    workqueue_init();
    process_init();
    thread_init();
    scheduler_init();
    thread_bootstrap_idle();
    irq_set_post_eoi_hook(scheduler_preempt_if_needed);

    ipc_init();
    deathwatch_init();
    registry_init();
    futex_init();
    shm_init();
    event_group_init_subsystem();
    syscall_init();

    scheduler_start();
}

/* === Stage 5bis: SMP (LAPIC/IOAPIC/AP) =================================== */

static void stage_smp_online(void)
{
    lapic_init();                        /* indipendente da ACPI (usa l'MSR)*/
    time_event_try_enable();             /* tickless se LAPIC+TSC usabili   */
    ioapic_init();                       /* no-op se la MADT non ne riporta */
    smp_boot_aps();                      /* no-op su uniprocessore/no-LAPIC */

    /* Detection del bridge SEMPRE, non solo a switch riuscito: il
     * backend PIIX serve proprio alle macchine SENZA IOAPIC (Armada:
     * PIC mode, SYS_IRQ_WIRE_DEVICE) — gated dietro lo switch era una
     * regressione sulla 1.0, che chiamava pirq_init incondizionata. Il
     * backend ICH fotografa il routing INTx->PIRQ->GSI che il layer
     * driver usera' per la risoluzione deterministica in modo IOAPIC. */
    pirq_init();

    /* Migra la consegna IRQ dispositivo dal PIC all'IOAPIC: abilita
     * INTx PCI risolti dal chipset (ICH) o empiricamente, MSI e lo
     * steering per-CPU. No-op (resta il PIC) se nessun IOAPIC e'
     * presente — es. Armada senza APIC. Le linee legacy vive (IRQ0 del
     * PIT incluso, via override GSI2) restano consegnate: la migrazione
     * riapre solo cio' che aveva un handler. */
    intr_switch_to_ioapic();
}

/* === Stage 6: verifica ================================================== */

static void run_selftest(const char *name, bool (*test)(void))
{
    kprintf("[TEST] %s... ", name);
    if (!test())
    {
        kpanic("self-test '%s' FALLITO", name);
    }
    kprintf("OK\n");
}

/* Heartbeat della sorgente tempo ATTIVA: arma un timer reale e attende
 * il fire dormendo — verifica l'intera catena heap -> nucleo eventi ->
 * LAPIC one-shot (o PIT -> drain in fallback). Col PIT mascherato dal
 * modo eventi, contare i tick non avrebbe senso. */
static volatile bool s_hb_fired;

static void hb_fire(void *arg UNUSED)
{
    s_hb_fired = true;
}

static bool heartbeat_selftest(void)
{
    timer_t hb;
    s_hb_fired = false;
    timer_init(&hb, hb_fire, NULL);
    if (!timer_arm_in(&hb, MS_TO_NS(5)))
    {
        return false;
    }

    uint64_t deadline = clock_now_ns() + MS_TO_NS(200);
    while (!s_hb_fired && clock_now_ns() < deadline)
    {
        __asm__ volatile ("sti; hlt");  /* sveglia = il fire stesso */
    }
    timer_cancel(&hb);
    return s_hb_fired;
}

static void stage_verify_hardware(void)
{
    run_selftest("Heartbeat sorgente tempo", heartbeat_selftest);
    run_selftest("PMM alloc/free/double-free/riuso", pmm_selftest);
    run_selftest("Paging map/traduzione/unmap", paging_selftest);
    run_selftest("Heap slab/double-free/riuso/large", kheap_selftest);
    run_selftest("Scheduler interleave + sleep", scheduler_selftest);
}

/* === Stage 7: rapporto del kernel ======================================= */

/* Il rapporto chiude cio' che il kernel puo' GARANTIRE: gli stage 1-6.
 * Va stampato PRIMA dello spawn dei moduli, non dopo: lo spawn e'
 * asincrono (i servizi girano sulle altre CPU, molti parcheggiati su
 * needs:) e kmain e' il thread idle — stampato in coda, il riquadro
 * compariva in MEZZO al bring-up dei driver dichiarando "operativo" un
 * sistema a meta', con i log dei moduli prima e dopo. Ora l'ordine
 * sullo schermo e' causale: verita' del kernel nel riquadro, poi
 * l'avvio dei moduli con i loro log a seguire. */
static void stage_report_kernel_ready(void)
{
    pmm_stats_t mem;
    pmm_get_stats(&mem);

    console_set_color(CON_LIGHT_GREEN, CON_BLACK);
    kprintf("\n================================================\n");
    kprintf("  %s %s - kernel operativo\n", MAINDOB_NAME,
            MAINDOB_VERSION_STRING);
    kprintf("================================================\n");
    console_set_color(CON_LIGHT_GREY, CON_BLACK);
    kprintf("  RAM: %u MB liberi / %u MB totali - uptime %u ms\n",
            (mem.free_frames  * PAGE_SIZE) >> 20,
            (mem.total_frames * PAGE_SIZE) >> 20,
            (uint32_t)clock_now_ms());
    kprintf("  Processi, IPC (payload staged + direct-switch), SHM,\n");
    kprintf("  eventi, RTC, RNG, wait/kill/spawn, brk, futex -\n");
    kprintf("  syscall ABI 1:1 col kernel precedente.\n");
    kprintf("  Disco ATA, .mem, shutdown ACPI S5, OOM recovery pronti.\n");
    kprintf("  Scheduler per-CPU, tickless (LAPIC one-shot), AP nello\n");
    kprintf("  scheduler; IOAPIC/MSI/INTx empirico, layer driver attivo.\n");
    kprintf("  Avvio dei servizi da Startup_modules...\n\n");
}

/* === Stage 8: moduli userspace da Startup_modules ======================= */

/* Estrae il valore di `key` (es. "needs:") da flags in `out`.
 * Ritorna true se presente. */
static bool flag_extract_value(const char *flags, const char *key,
                               char *out, uint32_t cap)
{
    if (flags == NULL)
    {
        return false;
    }
    const char *hit = strstr(flags, key);
    if (hit == NULL)
    {
        return false;
    }
    const char *v = hit + strlen(key);
    uint32_t n = 0;
    while (v[n] != '\0' && v[n] != ' ' && v[n] != '\t' && n + 1 < cap)
    {
        out[n] = v[n];
        n++;
    }
    out[n] = '\0';
    return n > 0;
}

/* === Primary: cache dei blob e respawn ==================================
 *
 * bootfs muore a fine boot (il disco e' dei moduli): il respawn di un
 * primary non puo' rileggere dal filesystem. Il blob ELF viene quindi
 * COPIATO in cache al primo spawn — pochi MB per una manciata di
 * servizi, il prezzo per non restare mai senza UI/FS/input. La lista
 * di auto-promozione e' 1:1 con la 1.0: solo i servizi la cui assenza
 * rompe "apri un programma" o "vedi qualcosa a schermo"; i programmi
 * utente NON ci finiscono mai (o hanno il flag nel manifest, o
 * niente). */

#define PRIMARY_MAX 8

typedef struct
{
    char     name[64];
    char     flags[128];
    void    *blob;
    uint32_t size;
    bool     respawn_pending;   /* morto: da risorgere al prossimo idle   */
} primary_entry_t;

static primary_entry_t s_primaries[PRIMARY_MAX];

static const char *const k_auto_primary[] = {
    "DobFileSystem",
    "modules",
    "dobinterface",
    "hotplug",
    "config",
    "inputd",
};

static bool primary_name_matches(const char *name)
{
    for (uint32_t i = 0;
         i < sizeof(k_auto_primary) / sizeof(k_auto_primary[0]); i++)
    {
        if (strcmp(name, k_auto_primary[i]) == 0) return true;
    }
    return false;
}

/* Copia il blob in cache (idempotente per nome: il respawn riusa la
 * voce). Fallimento = niente cache: il servizio girera' senza rete di
 * respawn, e la riga di log lo dice. */
static void primary_cache_store(const char *name, const char *flags,
                                const void *elf, uint32_t size)
{
    primary_entry_t *slot = NULL;
    for (uint32_t i = 0; i < PRIMARY_MAX; i++)
    {
        if (s_primaries[i].blob != NULL &&
            strcmp(s_primaries[i].name, name) == 0) return;   /* gia' li' */
        if (slot == NULL && s_primaries[i].blob == NULL)
            slot = &s_primaries[i];
    }
    if (slot == NULL)
    {
        kprintf("[BOOT] primary '%s': cache piena, respawn NON attivo\n",
                name);
        return;
    }
    void *copy = kmalloc(size);
    if (copy == NULL)
    {
        kprintf("[BOOT] primary '%s': niente memoria per la cache, "
                "respawn NON attivo\n", name);
        return;
    }
    memcpy(copy, elf, size);
    strncpy(slot->name, name, sizeof(slot->name) - 1);
    strncpy(slot->flags, flags ? flags : "", sizeof(slot->flags) - 1);
    slot->blob = copy;
    slot->size = size;
}

/* Respawn di un primary dalla cache (chiamato dal workqueue del
 * teardown, vedi process.c). Il needs: originale viene ignorato: al
 * respawn i servizi richiesti sono in piedi da un pezzo, e parcheggiare
 * un dobinterface risorto su needs:video lo lascerebbe fermo per
 * sempre (il registry non ri-annuncia). I flags restano (privilegi). */
void kernel_respawn_primary(const char *name)
{
    for (uint32_t i = 0; i < PRIMARY_MAX; i++)
    {
        if (s_primaries[i].blob == NULL ||
            strcmp(s_primaries[i].name, name) != 0) continue;

        kprintf("[BOOT] respawn del primary '%s'\n", name);
        int32_t pid = process_spawn_elf_ex(s_primaries[i].name,
                                           s_primaries[i].blob,
                                           s_primaries[i].size,
                                           s_primaries[i].flags,
                                           0, NULL, 0);
        if (pid < 0)
        {
            kprintf("[BOOT] respawn di '%s' FALLITO (%d)\n", name, pid);
            return;
        }
        process_t *np = process_get_ref((pid_t)pid);
        if (np != NULL) { np->is_primary = true; process_put(np); }
        return;
    }
    kprintf("[BOOT] respawn: '%s' non in cache\n", name);
}

/* Canale di respawn NON-PERDIBILE: il teardown di un primary (in idle)
 * marca il suo slot invece di allocare+accodare sulla workqueue; idle
 * esegue i pending dopo i teardown. Lo slot vive nella tabella fissa,
 * quindi zero allocazioni e nessun limite di coda da traboccare.
 * s_primary_lock protegge SOLO il flag (l'ELF si carica fuori dal lock). */
static spinlock_t s_primary_lock = SPINLOCK_INIT;

void kernel_request_respawn(const char *name)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_primary_lock);
    for (uint32_t i = 0; i < PRIMARY_MAX; i++)
    {
        if (s_primaries[i].blob != NULL &&
            strcmp(s_primaries[i].name, name) == 0)
        {
            s_primaries[i].respawn_pending = true;
            break;
        }
    }
    spinlock_release_irqrestore(&s_primary_lock, fl);
}

void kernel_run_pending_respawns(void)
{
    for (uint32_t i = 0; i < PRIMARY_MAX; i++)
    {
        /* Rivendica il flag sotto lock (un solo core risorge lo slot),
         * poi carica l'ELF FUORI dal lock. Il nome e' stabile (scritto
         * una volta a cache-store), quindi la lettura fuori lock e' sicura. */
        uint32_t fl = spinlock_acquire_irqsave(&s_primary_lock);
        bool pending = s_primaries[i].respawn_pending;
        if (pending)
        {
            s_primaries[i].respawn_pending = false;
        }
        spinlock_release_irqrestore(&s_primary_lock, fl);

        if (pending)
        {
            kernel_respawn_primary(s_primaries[i].name);
        }
    }
}

/* Spawna un modulo dal suo blob ELF gia' in RAM (letto prima dal
 * prefetch, quando bootfs possedeva ancora il disco). Non tocca il disco:
 * il naming, il parsing di needs:/driver/primary e la cache primary
 * lavorano tutti sul blob passato. */
static bool spawn_module_blob(const char *path, const char *flags,
                              void *elf, uint32_t size)
{
    /* Nome = ultimo componente del path. */
    const char *name = path;
    for (const char *p = path; *p != '\0'; p++)
    {
        if (*p == '/')
        {
            name = p + 1;
        }
    }

    /* needs:NOME — il modulo parte solo quando NOME e' nel registry:
     * il main thread nasce parcheggiato, zero istruzioni prima. */
    char need[64];
    int32_t pid;
    if (flag_extract_value(flags, "needs:", need, sizeof(need)))
    {
        pid = process_spawn_elf_needs(name, elf, size, flags, need);
        if (pid >= 0)
        {
            kprintf("[BOOT] %s (PID %d) parcheggiato su needs:%s\n",
                    name, pid, need);
        }
    }
    else
    {
        pid = process_spawn_elf_ex(name, elf, size, flags, 0, NULL, 0);
    }
    if (pid < 0)
    {
        kprintf("[BOOT] spawn di '%s' fallito\n", path);
        return false;
    }
    /* Primary: flag esplicito nel manifest o auto-promozione (nome
     * senza estensione, come la 1.0). */
    char clean[64];
    uint32_t cn = 0;
    while (name[cn] != '\0' && name[cn] != '.' && cn + 1 < sizeof(clean))
    {
        clean[cn] = name[cn];
        cn++;
    }
    clean[cn] = '\0';
    bool primary = (flags != NULL && strstr(flags, "primary") != NULL);
    if (!primary && primary_name_matches(clean))
    {
        primary = true;
        kprintf("[BOOT] '%s' auto-promosso a primary (servizio "
                "vitale)\n", clean);
    }
    if (primary)
    {
        primary_cache_store(clean, flags, elf, size);
        process_t *pp = process_get_ref((pid_t)pid);
        if (pp != NULL) { pp->is_primary = true; process_put(pp); }
    }

    kprintf("[BOOT] modulo '%s' -> PID %d%s%s\n", name, pid,
            (flags != NULL && strstr(flags, "driver")) ? " (driver)" : "",
            primary ? " [primary]" : "");
    return true;
}

/* Formato (1:1 col 1.0): una riga per modulo, path + flags separati da
 * TAB; righe vuote e commenti '#' ignorati. Il kernel guarda solo
 * "driver"; il resto e' metadato opaco servito da GET_MODULE_FLAGS. */
/* Parsa una riga in (path, flags). Ritorna false per righe vuote o di
 * commento; true se ha estratto un modulo. Nessuno spawn, nessuna
 * lettura: e' solo il decoder del formato, usato dal prefetch. */
static bool parse_startup_line(const char *line, const char *line_end,
                               char *path_out, uint32_t path_sz,
                               char *flags_out, uint32_t flags_sz)
{
    while (line < line_end &&
           (*line == ' ' || *line == '\t' || *line == '\r'))
    {
        line++;
    }
    if (line >= line_end || *line == '#')
    {
        return false;
    }

    const char *path_start = line;
    const char *p = line;
    while (p < line_end && *p != '\t')
    {
        p++;
    }
    uint32_t path_len = (uint32_t)(p - path_start);
    while (path_len > 0 && path_start[path_len - 1] == ' ')
    {
        path_len--;
    }
    if (path_len == 0 || path_len >= path_sz)
    {
        return false;
    }

    memcpy(path_out, path_start, path_len);
    path_out[path_len] = '\0';

    const char *flags_start = (p < line_end && *p == '\t') ? p + 1 : p;
    uint32_t flags_len = (uint32_t)(line_end - flags_start);
    if (flags_len >= flags_sz)
    {
        flags_len = flags_sz - 1;
    }
    memcpy(flags_out, flags_start, flags_len);
    flags_out[flags_len] = '\0';
    return true;
}

/* === Stage 8: prefetch dei blob, poi spawn =============================
 *
 * Il reader di boot (bootfs) e il driver del disco userspace condividono
 * lo STESSO controller. Su AHCI la porta ha stato persistente (command
 * list, PxCI): appena il primo driver del disco (ata/ahci, in cima a
 * Startup_modules) viene schedulato, ferma e riprogramma la porta sotto i
 * piedi del kernel, e ogni bootfs_read_file successiva fallisce (su IDE il
 * PIO e' stateless e la cosa non si notava). La regola gia' scritta negli
 * header — "bootfs non si usa dopo che i moduli partono" — va quindi
 * rispettata alla lettera: PRIMA si leggono in RAM tutti i blob (bootfs ha
 * il disco in esclusiva), si chiude bootfs, POI si spawnano in ordine. Sul
 * live (ramdisk) il comportamento e' identico. */

/* Voci di Startup_modules (il file di avvio), NON i moduli GRUB del
 * multiboot: quello e' BOOT_MODULES_MAX di boot_info.h (16). Il nome
 * uguale con valore diverso ridefiniva il simbolo dell'header — unico
 * warning residuo di ogni sweep dalla b74: due concetti, due nomi. */
#define STARTUP_MODULES_MAX 32

typedef struct
{
    char     path[256];
    char     flags[128];
    void    *blob;
    uint32_t size;
} boot_module_ent_t;

static boot_module_ent_t s_boot_modules[STARTUP_MODULES_MAX];
static int               s_boot_module_count;

/* Legge in RAM il blob ELF di ogni modulo elencato, mentre bootfs possiede
 * ancora il disco. Alla fine chiude bootfs: da qui il disco e' dei driver. */
static void module_prefetch_all(void)
{
    const char *text = boot_get_startup_text();
    const char *p    = text;
    const char *end  = text + boot_get_startup_len();
    s_boot_module_count = 0;

    while (p < end && s_boot_module_count < STARTUP_MODULES_MAX)
    {
        const char *line_end = p;
        while (line_end < end && *line_end != '\n')
        {
            line_end++;
        }

        char path[256], flags[128];
        if (parse_startup_line(p, line_end, path, sizeof(path),
                               flags, sizeof(flags)))
        {
            uint32_t size = 0;
            void *elf = bootfs_read_file(path, &size);
            if (elf == NULL)
            {
                kprintf("[BOOT] modulo '%s' illeggibile\n", path);
            }
            else
            {
                void *copy = kmalloc(size);
                if (copy == NULL)
                {
                    kprintf("[BOOT] modulo '%s': memoria insufficiente "
                            "(%u byte)\n", path, size);
                }
                else
                {
                    memcpy(copy, elf, size);
                    boot_module_ent_t *e =
                        &s_boot_modules[s_boot_module_count++];
                    strncpy(e->path,  path,  sizeof(e->path) - 1);
                    strncpy(e->flags, flags, sizeof(e->flags) - 1);
                    e->blob = copy;
                    e->size = size;
                }
            }
        }
        p = (line_end < end) ? line_end + 1 : end;
    }

    bootfs_shutdown();                  /* letture finite: il disco e' dei moduli */
}

static void stage_userspace_online(void)
{
    boot_startup_init();
    module_prefetch_all();              /* passata 1: leggi tutto, chiudi bootfs */

    /* Passata 2: spawn in ordine dai blob in cache. spawn_elf copia l'ELF
     * nell'AS del nuovo processo e i primary tengono gia' la propria copia
     * di respawn, quindi il blob del prefetch si libera subito dopo. */
    int count = 0;
    for (int i = 0; i < s_boot_module_count; i++)
    {
        boot_module_ent_t *e = &s_boot_modules[i];
        if (spawn_module_blob(e->path, e->flags, e->blob, e->size))
        {
            count++;
        }
        kfree(e->blob);
        e->blob = NULL;
    }

    if (s_boot_module_count > 0)
    {
        kprintf("[BOOT] %d moduli avviati da Startup_modules.\n", count);
    }
}

/* === Orchestratore ====================================================== */

void kmain(uint32_t magic, uint32_t mbi_phys);

void kmain(uint32_t magic, uint32_t mbi_phys)
{
    stage_console_online(magic, mbi_phys);
    stage_cpu_online();
    stage_memory_online();
    stage_time_online();
    stage_kernel_services_online();
    stage_smp_online();
    stage_verify_hardware();
    stage_report_kernel_ready();
    stage_userspace_online();
    idle_entry();
}
