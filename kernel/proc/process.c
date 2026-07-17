#include "proc/process.h"
#include "proc/thread.h"
#include "proc/workqueue.h"
#include "proc/scheduler.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/kheap.h"
#include "mm/vregion.h"
#include "krt/handle_table.h"
#include "ipc/port.h"
#include "ipc/channel.h"
#include "syscall/syscall.h"
#include "mm/shm.h"
#include "sync/event_group.h"
#include "sync/atomic.h"
#include "sync/spinlock.h"
#include "registry.h"
#include "lib/string.h"
#include "console/console.h"
#include "arch/x86/cpu.h"
#include "kernel.h"

/* Tabella processi: usa il componente standard krt/handle_table
 * (slot+generation+freelist) invece della logica ad-hoc del 1.0. */
static void build_home_dir(process_t *proc, const char *name);

static handle_table_t s_proc_table;
static list_t         s_all_processes;
static process_t     *s_kernel_proc;

/* Copre le liste intrusive condivise fra i core: s_all_processes e le
 * liste children/sibling. La handle table ha il suo lock interno e la
 * lista/tabella thread e' coperta dal lock di thread.c (s_table_lock):
 * questo lock e' disgiunto da entrambi e non vi si annida, quindi non
 * serve un ordine globale. Senza, due process_create/destroy concorrenti
 * (es. il boot che spawna moduli mentre hotplug fa SYS_SPAWN sull'altro
 * core) corrompono la lista -> scrittura a un puntatore spazzatura. */
static spinlock_t s_proclist_lock = SPINLOCK_INIT;

/* === Verbi di costruzione =============================================== */

/* Verbo esecutivo: materializza un range utente ANONIMO a indirizzo
 * fisso in un PD, da frame INDIPENDENTI (mai un blocco contiguo).
 * Compito unico e standardizzato, zero policy: il chiamante logico
 * decide base/pagine e registra il vregion; questo blocco alloca,
 * azzera (nessun residuo del frame riciclato -> niente info-leak fra
 * processi), mappa USER_RW. E' ATOMICO: se un frame manca, sgancia e
 * libera quelli gia' posati e torna false, senza lasciare mezzo range
 * appeso. Riusato da build_address_space (buffer IPC) e map_user_stack:
 * un solo idioma per ogni regione anonima per-processo. */
static bool map_anon_range(process_t *proc, uint32_t base, uint32_t pages)
{
    for (uint32_t i = 0; i < pages; i++)
    {
        uint32_t frame = pmm_alloc_frame(PMM_ZONE_ANY);
        if (frame == 0)
        {
            /* Rollback del solo lavoro di questo verbo: i frame gia'
             * posati (0..i-1) tornano al PMM e le loro PTE spariscono,
             * cosi' il verbo e' tutto-o-niente anche in isolamento. */
            for (uint32_t k = 0; k < i; k++)
            {
                uint32_t v = base + k * PAGE_SIZE;
                uint32_t f = paging_get_physical_in(proc->page_directory, v);
                paging_unmap_in(proc->page_directory, v);
                if (f != 0)
                {
                    pmm_free_frame(f);
                }
            }
            return false;
        }
        memset((void *)(frame + KERNEL_VMA), 0, PAGE_SIZE);
        paging_map_in(proc->page_directory, base + i * PAGE_SIZE, frame,
                      PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }
    return true;
}

static bool build_address_space(process_t *proc, const char *name)
{
    proc->page_directory = paging_create_directory();
    if (proc->page_directory == 0)
    {
        return false;
    }

    /* Buffer IPC per-processo (ABI 1.0: 64 KB a IPC_BUF_VADDR). Frame
     * INDIPENDENTI: il buffer si tocca solo per via virtuale, quindi la
     * contiguita' fisica non serve — pretenderla a ogni nascita
     * frammentava la RAM e faceva fallire lo spawn con memoria libera ma
     * sparpagliata. Cosi' l'unica domanda contigua resta quella dei
     * consumatori DMA veri. */
    if (!map_anon_range(proc, IPC_BUF_VADDR, IPC_BUF_PAGES))
    {
        paging_destroy_directory(proc->page_directory);
        return false;
    }
    proc->ipc_buf_ready = true;

    /* Placement con LOCALITA': la CPU del creatore, salvo squilibrio
     * oltre lo slack (allora la meno carica online). La distribuzione
     * avviene ALLA NASCITA, la migrazione non esiste (per ora): la
     * localita' tiene client e server sulla stessa CPU, dove il
     * direct-switch IPC scatta e i wake non pagano il cross-core. Il
     * vecchio round-robin cieco spargeva le catene IPC su core
     * diversi: ogni messaggio un wake remoto. */
    proc->home_cpu = (uint8_t)scheduler_pick_cpu_local();
    mutex_init(&proc->vm_lock);
    vregion_init(&proc->vm_regions);
    vregion_t *ipc = vregion_insert(&proc->vm_regions, IPC_BUF_VADDR,
                                    IPC_BUF_PAGES,
                                    VREG_IPC | VREG_FIXED | VREG_USER_RW);
    if (ipc != NULL)
    {
        ipc->committed = IPC_BUF_PAGES;
    }

    build_home_dir(proc, name);
    return true;
}

static void build_home_dir(process_t *proc, const char *name)
{
    /* /SYSTEM/PROGRAMS/<name>/ — sandbox path del 1.0. */
    const char *prefix = "/SYSTEM/PROGRAMS/";
    size_t i = 0;
    for (const char *p = prefix; *p && i < sizeof(proc->home_dir) - 2; p++)
    {
        proc->home_dir[i++] = *p;
    }

    /* L'home dir e' la CARTELLA del programma (/SYSTEM/PROGRAMS/<name>/),
     * non il file .mdl. A seconda dello spawner il nome del processo puo'
     * arrivare con l'estensione ".mdl" (es. MainDOB_Setup.mdl): senza
     * togliergliela l'home dir diventa ".../MainDOB_Setup.mdl/", che non
     * combacia ne' con la cartella reale (rompe il sandbox in scrittura del
     * programma sulla propria home) ne' con la whitelist di
     * config_area_allowed (che usa nomi puliti) — ed e' esattamente cio' che
     * negava all'installer la lettura di /SYSTEM/CONFIG/Associations.
     * Un nome gia' pulito non viene toccato. */
    size_t nlen = 0;
    while (name[nlen]) nlen++;
    if (nlen >= 4 && name[nlen - 4] == '.' &&
        (name[nlen - 3] == 'm' || name[nlen - 3] == 'M') &&
        (name[nlen - 2] == 'd' || name[nlen - 2] == 'D') &&
        (name[nlen - 1] == 'l' || name[nlen - 1] == 'L'))
    {
        nlen -= 4;
    }
    for (size_t k = 0; k < nlen && i < sizeof(proc->home_dir) - 2; k++)
    {
        proc->home_dir[i++] = name[k];
    }
    proc->home_dir[i++] = '/';
    proc->home_dir[i]   = '\0';
}

static void init_process_lists(process_t *proc, pid_t parent_pid)
{
    list_init(&proc->threads);
    list_init(&proc->children);
    list_init(&proc->owned_ports);
    list_node_init(&proc->sibling_node);
    list_node_init(&proc->global_node);
    list_node_init(&proc->teardown_link);
    wait_queue_init(&proc->exit_waiters);

    process_t *parent = process_get_by_pid(parent_pid);
    if (parent != NULL)
    {
        list_push_back(&parent->children, &proc->sibling_node);
    }
}

/* === API ================================================================ */

void process_init(void)
{
    if (!handle_table_init(&s_proc_table, MAX_PROCESSES, HT_REUSE_MONOTONIC))
    {
        kpanic("process: impossibile allocare la tabella processi");
    }
    list_init(&s_all_processes);

    /* PID 0 = processo kernel, ospita tutti i thread kernel. */
    s_kernel_proc = (process_t *)kcalloc(1, sizeof(process_t));
    KASSERT(s_kernel_proc != NULL);
    s_kernel_proc->pid            = 0;
    s_kernel_proc->parent_pid     = -1;
    s_kernel_proc->state          = PROC_RUNNING;
    refcount_init(&s_kernel_proc->refcount, 1);   /* mai rilasciata: PID 0
                                                   * non si distrugge      */
    s_kernel_proc->privileges     = PRIV_DRIVER;
    s_kernel_proc->page_directory = paging_kernel_directory();
    strncpy(s_kernel_proc->name, "kernel", PROCESS_NAME_MAX);
    list_init(&s_kernel_proc->threads);
    list_init(&s_kernel_proc->children);
    list_init(&s_kernel_proc->owned_ports);
    wait_queue_init(&s_kernel_proc->exit_waiters);
    vregion_init(&s_kernel_proc->vm_regions);

    /* Slot 0 della tabella non e' assegnabile; il kernel proc vive a
     * parte ma e' raggiungibile via process_get_by_pid(0). */
    list_push_back(&s_all_processes, &s_kernel_proc->global_node);

    kprintf("[PROC] Sottosistema processi pronto (max %u). Kernel = PID 0.\n",
            (uint32_t)MAX_PROCESSES);
}

process_t *process_create(const char *name, pid_t parent_pid)
{
    process_t *proc = (process_t *)kcalloc(1, sizeof(process_t));
    if (proc == NULL)
    {
        return NULL;
    }

    /* refcount 1 = referenza dello slot della handle table (rilasciata da
     * finalize). I kfree dei path d'errore qui sotto sono su un PCB non
     * ancora pubblicato: nessun process_get_ref puo' vederlo, quindi il
     * kfree diretto e' corretto e non tocca il refcount. */
    refcount_init(&proc->refcount, 1);

    proc->state      = PROC_CREATING;
    proc->parent_pid = parent_pid;
    strncpy(proc->name, name, PROCESS_NAME_MAX - 1);

    if (!build_address_space(proc, name))
    {
        kfree(proc);
        return NULL;
    }

    handle_ref_t ref = handle_table_insert(&s_proc_table, proc);
    if (ref.id == 0)
    {
        /* Il buffer IPC e' gia' mappato nel PD: paging_destroy_directory
         * libera quei 16 frame indipendenti dalle loro PTE. Non liberarli
         * a mano qui o sarebbe un doppio free. */
        paging_destroy_directory(proc->page_directory);
        kfree(proc);
        kprintf("[PROC] tabella piena\n");
        return NULL;
    }

    proc->pid = (pid_t)ref.id;

    /* Pubblicazione nelle liste condivise sotto lock: da qui il PCB e'
     * raggiungibile dagli altri core. init_process_lists aggancia anche il
     * sibling_node alla lista children del padre, quindi rientra nel lock. */
    uint32_t fl = spinlock_acquire_irqsave(&s_proclist_lock);
    init_process_lists(proc, parent_pid);
    list_push_back(&s_all_processes, &proc->global_node);
    spinlock_release_irqrestore(&s_proclist_lock, fl);

    /* Riga di spawn del debug: PID, nome e SMISTAMENTO — core assegnato
     * (home, dove vivranno tutti i suoi thread) e core del creatore,
     * cosi' dal log si legge a colpo d'occhio se la localita' ha tenuto
     * (home == creatore) o se il bilanciatore ha sparso. */
    kprintf("[PROC] PID %d '%s' creato (core %u, spawnato da core %u).\n",
            proc->pid, proc->name,
            (uint32_t)proc->home_cpu,
            this_cpu()->cpu_index);
    return proc;
}

process_t *process_get_by_pid(pid_t pid)
{
    if (pid == 0)
    {
        return s_kernel_proc;
    }
    return (process_t *)handle_table_get(&s_proc_table, (uint32_t)pid);
}

/* Reclamation finale del PCB: gira quando il refcount tocca 0 — dopo che
 * finalize ha svolto il lavoro home-core (unmap device, SHM) e rilasciato
 * la referenza dello slot, e dopo che ogni process_get_ref e' stato messo.
 * Solo lavoro core-agnostico (temp mapping / kfree): puo' girare su
 * QUALSIASI core dall'ultimo put. Tenendo i sotto-oggetti (vm_regions,
 * PD) fino a qui, un getter con un ref puo' attraversarli in sicurezza. */
static void process_reclaim(process_t *proc)
{
    vregion_destroy(&proc->vm_regions);
    if (proc->page_directory != 0)
    {
        paging_destroy_directory(proc->page_directory);
    }
    kfree(proc);
}

/* Lookup PINNATO. get+inc sotto s_proclist_lock, lo STESSO lock sotto cui
 * destroy_local rimuove lo slot: o vediamo lo slot (e incrementiamo, il
 * PCB non puo' essere reclamato finche' non facciamo put) o vediamo NULL
 * (gia' rimosso). L'inc/dec del refcount e' atomico, quindi il drop a 0
 * e' lock-free (nessun nuovo inc puo' apparire dopo la rimozione). */
process_t *process_get_ref(pid_t pid)
{
    if (pid == 0)
    {
        refcount_inc(&s_kernel_proc->refcount);
        return s_kernel_proc;
    }

    uint32_t fl = spinlock_acquire_irqsave(&s_proclist_lock);
    process_t *p = (process_t *)handle_table_get(&s_proc_table, (uint32_t)pid);
    if (p != NULL)
    {
        refcount_inc(&p->refcount);
    }
    spinlock_release_irqrestore(&s_proclist_lock, fl);
    return p;
}

void process_put(process_t *proc)
{
    if (proc == NULL)
    {
        return;
    }
    if (refcount_dec(&proc->refcount))
    {
        process_reclaim(proc);          /* mai per PID 0: refcount mai a 0 */
    }
}

process_t *process_current(void)
{
    /* In fase 3 un thread ricorda il suo processo via owner (aggiunto
     * al thread layer di 1.1.2). Fallback: kernel proc. */
    return current_thread && current_thread->owner
         ? current_thread->owner : s_kernel_proc;
}

/* Fase 1 LOCALE del teardown: gira SEMPRE sulla home-core del processo
 * (chiamata da process_destroy quando gia' a casa, o dal drain della
 * teardown_q sulla home-core). Rende il PCB irraggiungibile e libera le
 * risorse NON legate all'address space; l'AS e il PCB stesso li libera
 * process_finalize quando thread_count==0. Il chiamante ha GIA' rivendicato
 * destroy_claimed: qui non si rivendica ne' si re-instrada. */
/* Respawn dei primary: NON qui (caricare un ELF sotto i lock del
 * lifecycle sarebbe il modo piu' rapido per un deadlock) e NON sulla
 * workqueue perdibile. Il primary vive nella tabella FISSA s_primaries[]
 * di kernel.c (nome+blob sopravvivono al PCB): marchiamo il suo slot come
 * "da risorgere" e idle lo esegue dopo i teardown. Non-perdibile, senza
 * allocazioni. Definito in kernel.c, accanto alla cache dei blob. */
extern void kernel_request_respawn(const char *name);

void process_destroy_local(process_t *proc)
{
    proc->state = PROC_ZOMBIE;          /* leggibile da sys_wait finche'  */
                                        /* il PCB esiste                  */

    /* Uccidi i thread ancora vivi (mai il chiamante). Il verbo stacca ogni
     * vittima dalla lista del processo sotto il lifecycle-lock e la distrugge
     * fuori dal lock: niente race cross-core sulla lista, niente annidamento. */
    thread_kill_others_of(proc);

    driver_cleanup_process(proc->pid);
    ipc_ports_cleanup_owner(proc->pid);     /* le sue porte muoiono ->    */
                                            /* deathwatch_fire notifica i  */
                                            /* watcher (in ipc_port_destroy) */
    deathwatch_cleanup_watcher(proc->pid);  /* sgancia i watch che AVEVA   */
                                            /* registrato lui              */
    registry_cleanup_owner(proc->pid);
    event_group_cleanup_process(proc->pid);

    /* Uscita dalle liste condivise sotto lock: stacca il PCB dalla lista
     * globale e dalla lista children del padre, e reparenta i figli ancora
     * vivi al kernel (PID 0). Senza il reparent, i sibling_node dei figli
     * resterebbero a puntare in un PCB che verra' reclamato -> nodo
     * penzolante -> corruzione al primo uso della lista.
     * handle_table_remove sta QUI dentro (stesso lock di process_get_ref):
     * dopo, nessun get_ref puo' piu' incrementare -> il refcount puo' solo
     * scendere, e il drop a 0 e' sicuro senza lock. */
    {
        uint32_t fl = spinlock_acquire_irqsave(&s_proclist_lock);
        handle_table_remove(&s_proc_table, (uint32_t)proc->pid);
        list_remove(&proc->global_node);
        if (list_node_is_linked(&proc->sibling_node))
        {
            list_remove(&proc->sibling_node);
        }
        list_node_t *pos, *tmp;
        list_for_each_safe(pos, tmp, &proc->children)
        {
            process_t *child = list_entry(pos, process_t, sibling_node);
            list_remove(&child->sibling_node);
            child->parent_pid = 0;
            list_push_back(&s_kernel_proc->children, &child->sibling_node);
        }
        spinlock_release_irqrestore(&s_proclist_lock, fl);
    }

    /* Primary (semantica 1.0): respawn differito, accodato ORA che
     * porte, IRQ, claim e registry del morto sono stati rilasciati —
     * il risorto non trova nomi occupati dal proprio cadavere. Vale
     * per qualunque via di morte: fault, exit, kill. */
    if (proc->is_primary)
    {
        /* Marca lo slot primary come da risorgere: idle lo esegue dopo
         * i teardown, quando porte/IRQ/claim/registry del morto sono gia'
         * stati rilasciati (il risorto non trova nomi occupati dal
         * proprio cadavere). Non-perdibile, senza allocazioni. */
        kernel_request_respawn(proc->name);
    }

    /* Claim single-winner: se non resta alcun thread, finalizza ora;
     * altrimenti l'ultimo reap finalizzera' esattamente una volta. */
    if (process_arm_teardown(proc))
    {
        process_finalize(proc);
    }
}

/* Fase 1 del teardown: rende il PCB irraggiungibile e libera le risorse
 * NON legate all'address space. L'AS e il PCB stesso sono liberati da
 * process_finalize solo quando thread_count==0 (nessuna CPU sul CR3):
 * e' il fix del double-free del PCB, portato dal 1.0. */
void process_destroy(process_t *proc)
{
    if (proc == NULL || proc->pid == 0)
    {
        return;                         /* mai il kernel                  */
    }

    /* RIVENDICA PRIMA DI INSTRADARE. sys_exit, sys_kill, il teardown
     * differito da fault/watchdog/OOM (process_destroy_deferred) e sys_wait
     * possono puntare allo STESSO processo da percorsi/core diversi. Chi
     * porta destroy_claimed 0->1 diventa
     * l'unico proprietario del teardown; gli altri tornano subito. Cosi' il
     * PCB che sto per accodare in teardown_q e' GIA' mio: nessun'altra via
     * puo' finalizzarlo/liberarlo prima che la home-core dreni la coda (era
     * il bug: instradare un PCB non ancora rivendicato -> puntatore penzolante
     * -> use-after-free sotto churn di processi su SMP). Chi perde la
     * rivendicazione, in particolare, non instrada un PCB che potrebbe
     * sparire. */
    if (atomic_xchg(&proc->destroy_claimed, 1u) != 0u)
    {
        return;
    }

    /* Ora che il PCB e' rivendicato: instrada sulla home-core. Se remota,
     * il suo idle esegue process_destroy_local (senza ri-rivendicare — il
     * lavoro glielo abbiamo trasferito). Se gia' a casa / boot / UP, false:
     * eseguiamo qui. Sulla home-core kill+reap+finalize sono tutti locali
     * (un solo thread per core, nessun core sul CR3). */
    if (scheduler_route_process_teardown(proc))
    {
        return;
    }

    process_destroy_local(proc);
}

/* Termina un processo da un contesto NON sicuro per la distruzione
 * immediata (fault handler, callback timer del watchdog, percorso OOM
 * sotto i lock mm): fissa l'esito e differisce il teardown sul canale
 * intrusivo non-perdibile (teardown_q via scheduler_defer_*), MAI sulla
 * workqueue best-effort. Rivendicazione single-winner: piu' richieste
 * sullo stesso PCB (es. fault + watchdog) collassano nel primo claim,
 * quindi la chiamata e' idempotente e senza allocazioni. */
void process_destroy_deferred(process_t *proc, int32_t exit_code)
{
    if (proc == NULL || proc->pid == 0)
    {
        return;                         /* mai il kernel                  */
    }
    if (atomic_xchg(&proc->destroy_claimed, 1u) != 0u)
    {
        return;                         /* gia' in teardown               */
    }

    proc->exit_code = exit_code;
    proc->state     = PROC_ZOMBIE;      /* leggibile da sys_wait          */
    wait_queue_wake_all(&proc->exit_waiters);

    scheduler_defer_process_teardown(proc);
}

bool process_arm_teardown(process_t *proc)
{
    return thread_arm_process_teardown(proc);
}

/* Smonta le regioni DEVICE (MMIO mappato da mmap_phys) prima della
 * distruzione del PD: quegli indirizzi fisici sono registri hardware,
 * non frame RAM — paging_destroy_directory NON deve passarli a
 * pmm_free_frame. Carica brevemente il CR3 del morente, quindi gira con
 * gli interrupt spenti (thread_count==0 garantisce accesso esclusivo a
 * questo CR3: nessun altro core puo' averlo caricato). */
static void finalize_unmap_device_regions(process_t *proc)
{
    uint32_t fl = irq_save();
    uint32_t old_pd = paging_current_directory();
    bool switched = (proc->page_directory != 0 &&
                     proc->page_directory != old_pd);
    if (switched)
    {
        paging_switch_directory(proc->page_directory);
    }

    for (vregion_t *vr = proc->vm_regions.head; vr != NULL; vr = vr->next)
    {
        if (vr->flags & VREG_DEVICE)
        {
            for (uint32_t p = 0; p < vr->pages; p++)
            {
                paging_unmap_page(vr->base + p * PAGE_SIZE);
            }
        }
    }

    if (switched)
    {
        paging_switch_directory(old_pd);
    }
    irq_restore(fl);
}

void process_finalize(process_t *proc)
{
    if (proc == NULL || proc->pid == 0)
    {
        return;
    }

    /* Smonta le SHM PRIMA di distruggere il PD: i loro frame sono del
     * segmento (refcount), non del processo — paging_destroy_directory
     * li libererebbe una seconda volta. Con l'unmap qui, il PD non li
     * vede piu' e il refcount decide chi libera davvero. */
    shm_cleanup_process(proc);

    /* Stessa logica per l'MMIO dei driver: registri hardware, non RAM. */
    finalize_unmap_device_regions(proc);

    /* La reclamation dei sotto-oggetti (vm_regions, PD) e del PCB e'
     * differita a refcount 0: qui rilasciamo la referenza dello slot.
     * Se un process_get_ref e' ancora in volo, i sotto-oggetti restano
     * vivi (il getter puo' attraversarli) e la reclamation slitta al suo
     * put. Il lavoro home-core (unmap device/SHM) e' gia' stato fatto
     * sopra, quando thread_count==0 garantiva l'accesso esclusivo al CR3. */
    if (refcount_dec(&proc->refcount))
    {
        process_reclaim(proc);
    }
}

/* === Spawn da ELF (senza cli lungo: la copia dei segmenti sta in
 *     elf_load via paging_map_in — D6) ================================== */

#include "proc/elf.h"
#include "proc/user_argv.h"

static uint32_t map_user_stack(process_t *proc)
{
    uint32_t base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    if (!map_anon_range(proc, base, USER_STACK_PAGES))
    {
        return 0;
    }
    vregion_insert(&proc->vm_regions, base, USER_STACK_PAGES,
                   VREG_STACK | VREG_FIXED | VREG_USER_RW);
    return USER_STACK_TOP;
}

/* Scrive il frame argv sul TOP dello stack del figlio. La scrittura
 * richiede il PD del figlio attivo: finestra IF=0 + switch CR3,
 * bounded (<= 4 KB di memcpy) — dentro il budget D6. Senza il cli, una
 * preemption ripristinerebbe al resume il CR3 salvato nel NOSTRO
 * contesto (il PD kernel) e le scritture finirebbero nell'AS sbagliato. */
static uint32_t stage_argv_in_child(process_t *child, uint32_t argc,
                                    const char *argv_strings,
                                    uint32_t argv_bytes)
{
    uint32_t fl = irq_save();
    uint32_t old_pd = paging_current_directory();

    paging_switch_directory(child->page_directory);
    uint32_t esp = user_argv_setup(USER_STACK_TOP, argc,
                                   argv_strings, argv_bytes);
    paging_switch_directory(old_pd);

    irq_restore(fl);
    return esp;
}

int32_t process_spawn_elf_ex(const char *name, const void *elf,
                             uint32_t size, const char *flags,
                             uint32_t argc, const char *argv_strings,
                             uint32_t argv_bytes)
{
    process_t *proc = process_create(name, process_current()
                                     ? process_current()->pid : 0);
    if (proc == NULL)
    {
        return -1;
    }

    if (flags != NULL)
    {
        strncpy(proc->flags_str, flags, sizeof(proc->flags_str) - 1);
        if (strstr(flags, "driver") != NULL)
        {
            proc->privileges |= PRIV_DRIVER;
        }
    }

    elf_load_result_t r = elf_load(proc, elf, size);
    if (!r.success)
    {
        process_destroy(proc);
        return -1;
    }

    proc->brk_start   = r.brk;
    proc->brk_current = r.brk;

    if (map_user_stack(proc) == 0)
    {
        process_destroy(proc);
        return -1;
    }

    /* SEMPRE, anche a zero argomenti: il crt0 trova comunque un frame
     * (argc=0, argv[0]=NULL) valido — semantica 1.0. */
    uint32_t user_esp = stage_argv_in_child(proc, argc,
                                            argv_strings, argv_bytes);
    if (user_esp == 0)
    {
        process_destroy(proc);
        return -1;
    }

    thread_t *t = thread_create_user(proc, r.entry_point, 0, user_esp);
    if (t == NULL)
    {
        process_destroy(proc);
        return -1;
    }

    proc->state = PROC_READY;
    return proc->pid;
}

int32_t process_spawn_elf_needs(const char *name, const void *elf,
                                uint32_t size, const char *flags,
                                const char *need_name)
{
    process_t *proc = process_create(name, process_current()
                                     ? process_current()->pid : 0);
    if (proc == NULL)
    {
        return -1;
    }
    if (flags != NULL)
    {
        strncpy(proc->flags_str, flags, sizeof(proc->flags_str) - 1);
        if (strstr(flags, "driver") != NULL)
        {
            proc->privileges |= PRIV_DRIVER;
        }
    }

    elf_load_result_t r = elf_load(proc, elf, size);
    if (!r.success)
    {
        process_destroy(proc);
        return -1;
    }
    proc->brk_start   = r.brk;
    proc->brk_current = r.brk;

    if (map_user_stack(proc) == 0)
    {
        process_destroy(proc);
        return -1;
    }
    uint32_t user_esp = stage_argv_in_child(proc, 0, NULL, 0);
    if (user_esp == 0)
    {
        process_destroy(proc);
        return -1;
    }

    /* Sospeso: nessuna istruzione utente prima che il bisogno esista. */
    thread_t *t = thread_create_user_suspended(proc, r.entry_point, 0,
                                               user_esp);
    if (t == NULL)
    {
        process_destroy(proc);
        return -1;
    }
    proc->state = PROC_READY;

    /* Il registry decide: nome gia' presente -> sblocco immediato;
     * assente -> parcheggio, sblocco al register. */
    registry_park_or_start(need_name, t);
    return proc->pid;
}

int32_t process_spawn_elf(const char *name, const void *elf, uint32_t size)
{
    return process_spawn_elf_ex(name, elf, size, NULL, 0, NULL, 0);
}
