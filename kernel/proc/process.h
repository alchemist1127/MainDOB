#ifndef MAINDOB_PROC_PROCESS_H
#define MAINDOB_PROC_PROCESS_H

#include "lib/types.h"
#include "lib/list.h"
#include "mm/vregion.h"
#include "proc/wait.h"
#include "sync/mutex.h"
#include "krt/refcount.h"

#define MAX_PROCESSES       65535   /* monotono: finestra anti-ABA ampia (0xFFFF), robusta su sessioni lunghe */
#define PROCESS_NAME_MAX    64

/* ABI userspace 1:1 col 1.0 (DESIGN D10). */
#define IPC_BUF_VADDR       0x7FF00000u
#define IPC_BUF_PAGES       16u
#define IPC_BUF_SIZE        (IPC_BUF_PAGES * 4096u)

/* Overflow IPC (semantica 1.0): consegna dei payload > IPC_BUF_SIZE in
 * una regione per-processo PERSISTENTE che cresce a high-water mark e
 * viene liberata al teardown dell'AS (o su grow). A regime: una memcpy,
 * zero churn pmm/paging — e' il percorso di letture file grandi e
 * framebuffer. Campi definiti in process_t. */
#define USER_CODE_BASE      0x00400000u
#define USER_STACK_TOP      0xBFFE0000u
#define USER_STACK_PAGES    16u

/* Privilegi (bitmask, come SYS_GET_PRIVILEGES del 1.0). */
#define PRIV_DRIVER         (1u << 0)

typedef int32_t pid_t;

typedef enum
{
    PROC_CREATING = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_ZOMBIE,
    PROC_DEAD
} proc_state_t;

typedef struct process
{
    pid_t          pid;
    pid_t          parent_pid;
    uint8_t        home_cpu;            /* CPU dei thread del processo:
                                         * le strutture per-processo si
                                         * appoggiano al pinning        */
    char           name[PROCESS_NAME_MAX];
    proc_state_t   state;
    uint32_t       privileges;

    uint32_t       page_directory;      /* CR3 fisico                     */
    mutex_t        vm_lock;             /* serializza vm_regions + brk    */
    vregion_list_t vm_regions;
    bool           ipc_buf_ready;       /* regione IPC materializzata:     */
                                        /* frame indipendenti a            */
                                        /* IPC_BUF_VADDR (nessuna base      */
                                        /* fisica — accesso solo virtuale)  */
    uint32_t       ipc_buf_offset;      /* bump-with-wrap: IPC annidati   */

    /* Regione overflow IPC (payload > IPC_BUF_SIZE), vedi commento
     * sopra IPC_BUF_PAGES. */
    uint32_t       ipc_overflow_vaddr;
    uint32_t       ipc_overflow_pages;      /* pagine mappate ora        */
    uint32_t       ipc_overflow_capacity;   /* high-water mark in pagine */
    uint32_t       brk_start;
    uint32_t       brk_current;
    vregion_t     *heap_region;         /* cache: vregion del brk         */

    uint32_t       thread_count;
    refcount_t     refcount;            /* vita del PCB: 1 = referenza dello
                                         * slot (rilasciata da finalize);
                                         * process_get_ref aggiunge, put
                                         * toglie. A 0 -> reclamation.     */
    bool           awaiting_teardown;   /* gate del double-free PCB (1.0) */
    volatile uint32_t destroy_claimed;  /* xchg: un solo vincitore fa la fase 1 */
    bool           queued_for_teardown; /* gia' in coda su una home-core?   */
    list_node_t    teardown_link;       /* nodo nella teardown_q della home */

    /* === Migrazione cooperativa (work stealing a grana di processo) ===
     * pinned: il kernel INCHIODA il processo alla sua home — driver con
     * linee IRQ (la RTE dell'IOAPIC punta alla home: migrare il thread
     * e lasciare l'interrupt sull'altro core ricreerebbe i wake
     * cross-core) e il driver video (il boomerang int 0x85 presta il
     * suo CR3 a thread di ALTRI core: l'invariante "solo la home e' sul
     * CR3" per lui non vale gia', non va toccato). Scritto solo dal
     * kernel, mai azzerato.
     * last_migration_ns: cooldown anti-thrash — un processo appena
     * migrato non rimigra per MIGRATION_COOLDOWN_NS; protegge anche la
     * localita' IPC appena ricostruita. Scritto solo dal donatore sotto
     * il lifecycle-lock. */
    bool           pinned;
    uint64_t       last_migration_ns;

    /* Tempo CPU dei thread GIA' MORTI di questo processo (ns),
     * accumulato in reap_detach sotto il lifecycle-lock. Lo snapshot
     * somma: vivi (dai thread) + questo. */
    uint64_t       cpu_time_dead_ns;
    int32_t        exit_code;

    list_t         threads;
    list_t         children;
    list_t         owned_ports;     /* cleanup IPC O(porte possedute)     */
    list_node_t    sibling_node;
    list_node_t    global_node;
    wait_queue_t   exit_waiters;

    char           home_dir[128];
    char           flags_str[128];  /* stringa flags da Startup_modules   */

    /* Servizio PRIMARY (semantica 1.0): dichiarato dal flag "primary"
     * in Startup_modules o auto-promosso (lista in kernel.c). Alla
     * distruzione — per QUALUNQUE via: fault, exit, kill — il teardown
     * accoda il respawn dal blob in cache (kernel_respawn_primary):
     * i vitali non lasciano mai il sistema senza UI/FS/input. */
    bool           is_primary;
                                    /* — opaca al kernel (ABI 1.0)        */
} process_t;

void       process_init(void);
process_t *process_create(const char *name, pid_t parent_pid);
process_t *process_get_by_pid(pid_t pid);
/* Lookup PINNATO: incrementa il refcount del PCB, cosi' il chiamante puo'
 * dereferenziarlo (anche attraverso uno yield) senza che un teardown
 * cross-core lo liberi. Bilanciare SEMPRE con process_put. NULL se il pid
 * non esiste piu'. Usare questo (non process_get_by_pid) ogni volta che si
 * dereferenzia un PCB potenzialmente ESTRANEO. */
process_t *process_get_ref(pid_t pid);
void       process_put(process_t *proc);
process_t *process_current(void);
void       process_destroy(process_t *proc);
/* Termina un processo differendo il teardown sul canale intrusivo
 * non-perdibile (mai la workqueue). Per contesti non sicuri alla
 * distruzione immediata: fault handler, callback timer, percorso OOM. */
void       process_destroy_deferred(process_t *proc, int32_t exit_code);
/* Fase 1 locale del teardown, sulla home-core; destroy_claimed gia' preso.
 * Chiamata da process_destroy (a casa) e dal drain della teardown_q. */
void       process_destroy_local(process_t *proc);
void       process_finalize(process_t *proc);   /* AS teardown, count==0  */

/* Aggancio col thread layer per il claim single-winner del teardown. */
bool process_arm_teardown(process_t *proc);     /* true = finalize ora    */

/* Crea un processo utente da un blob ELF gia' in memoria kernel e ne
 * avvia il thread iniziale in ring 3. Ritorna il pid, o < 0. */
int32_t process_spawn_elf(const char *name, const void *elf, uint32_t size);

/* Variante completa: flags (opachi, il kernel cerca solo "driver"),
 * argv blob gia' validato dal chiamante (conteggio NUL == argc). */
int32_t process_spawn_elf_ex(const char *name, const void *elf,
                             uint32_t size, const char *flags,
                             uint32_t argc, const char *argv_strings,
                             uint32_t argv_bytes);

/* Come _ex, ma se `need_name` non e' ancora nel registry il main
 * thread nasce SOSPESO e viene parcheggiato: riparte al register di
 * quel nome, senza aver eseguito un'istruzione (needs: di
 * Startup_modules). */
int32_t process_spawn_elf_needs(const char *name, const void *elf,
                                uint32_t size, const char *flags,
                                const char *need_name);

#endif
