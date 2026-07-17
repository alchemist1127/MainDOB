#include "syscall/syscall.h"
#include "arch/x86/idt.h"
#include "arch/x86/gdt.h"
#include "arch/x86/isr.h"
#include "arch/x86/ports.h"
#include "arch/x86/irq.h"
#include "krt/uaccess.h"
#include "proc/process.h"
#include "proc/tasksnap.h"
#include "proc/thread.h"
#include "proc/scheduler.h"
#include "proc/futex.h"
#include "proc/elf.h"
#include "syscall/video_boomerang.h"
#include "ipc/channel.h"
#include "ipc/port.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "arch/x86/tlb.h"
#include "mm/vregion.h"
#include "mm/kheap.h"
#include "time/clock.h"
#include "registry.h"
#include "mm/shm.h"
#include "sync/event_group.h"
#include "time/rtc.h"
#include "krt/entropy.h"
#include "arch/x86/tsc.h"
#include "boot/startup.h"
#include "proc/user_argv.h"
#include "acpi/acpi.h"
#include "boot/bootfs.h"
#include "proc/workqueue.h"
#include "lib/string.h"
#include "console/console.h"
#include "kernel.h"

static syscall_handler_t s_table[SYSCALL_MAX];
bool oom_try_recover(void);

/* Ogni handler legge gli argomenti dalla convenzione registri del 1.0:
 * arg0=ebx, arg1=ecx, arg2=edx, arg3=esi. Ritorno in eax (via return). */

/* === Verbi condivisi ==================================================== */

bool caller_is_driver(void)
{
    process_t *p = process_current();
    return p != NULL && (p->privileges & PRIV_DRIVER);
}

/* === Process =========================================================== */

static int32_t sys_exit(isr_regs_t *regs)
{
    process_t *p = process_current();
    if (p != NULL)
    {
        p->exit_code = (int32_t)regs->ebx;
        wait_queue_wake_all(&p->exit_waiters);
        process_destroy(p);
    }
    thread_exit();                      /* non ritorna                    */
    return 0;
}

static int32_t sys_yield(isr_regs_t *regs UNUSED)
{
    scheduler_yield();
    return 0;
}

static int32_t sys_getpid(isr_regs_t *regs UNUSED)
{
    process_t *p = process_current();
    return p != NULL ? p->pid : -1;
}

/* ABI 1.0: ritorna 0=dead/sconosciuto, 1=vivo, 2=zombie; per zombie e
 * dead l'exit code viene consegnato in EBX al ritorno (secondo valore
 * di ritorno via registro, come dma_alloc). */
static int32_t sys_proc_status(isr_regs_t *regs)
{
    process_t *p = process_get_ref((pid_t)regs->ebx);
    if (p == NULL)
    {
        return 0;                       /* dead/sconosciuto               */
    }
    int32_t rc;
    if (p->state == PROC_ZOMBIE)
    {
        regs->ebx = (uint32_t)p->exit_code;
        rc = 2;
    }
    else if (p->state == PROC_DEAD)
    {
        regs->ebx = (uint32_t)p->exit_code;
        rc = 0;
    }
    else
    {
        rc = 1;                         /* vivo                           */
    }
    process_put(p);
    return rc;
}

static int32_t sys_get_privileges(isr_regs_t *regs)
{
    process_t *p = process_get_ref((pid_t)regs->ebx);
    if (p == NULL)
    {
        return 0;
    }
    int32_t priv = (int32_t)p->privileges;
    process_put(p);
    return priv;
}

static int32_t sys_set_privileges(isr_regs_t *regs)
{
    /* make_driver: solo un driver puo' promuovere un figlio. */
    if (!caller_is_driver())
    {
        return -1;
    }
    process_t *child = process_get_ref((pid_t)regs->ebx);
    if (child == NULL)
    {
        return -1;
    }
    child->privileges |= PRIV_DRIVER;
    process_put(child);
    return 0;
}

static int32_t sys_thread_exit(isr_regs_t *regs UNUSED)
{
    thread_exit();
    return 0;
}

static int32_t sys_set_priority(isr_regs_t *regs)
{
    uint32_t prio = regs->ebx;
    scheduler_set_priority(current_thread, prio);
    return 0;
}

/* === IPC (ABI 1:1) ===================================================== */

static int32_t sys_port_create(isr_regs_t *regs UNUSED)
{
    process_t *p = process_current();
    return ipc_port_create(p ? p->pid : 0);
}

static int32_t sys_port_destroy(isr_regs_t *regs)
{
    ipc_port_destroy(regs->ebx);
    return 0;
}

/* === sys_send: verbi ==================================================== */

/* Fast-path 1.0: i payload fino a questa soglia vengono copiati nel
 * buffer di stack del syscall frame — zero kmalloc/kfree sul percorso
 * IPC piu' battuto del sistema. Lo stack kernel e' 8 KB con guard
 * page: 1 KB qui e' il compromesso gia' validato dall'1.0. */
#define SEND_PAYLOAD_STACK_MAX 1024u

/* Pavimento sotto il quale il buffer overflow IPC non si ritira mai
 * (32 KB): sotto, il costo del release+realloc supera i frame tenuti. */
#define IPC_OVERFLOW_SHRINK_FLOOR_PAGES 8u

/* Snapshot del payload del messaggio in memoria kernel (elimina il
 * TOCTOU): <= soglia va in stack_buf, oltre su heap. *heap_out riceve
 * l'eventuale buffer heap, che il chiamante libera in ogni percorso.
 * Ritorna IPC_OK o il codice d'errore da restituire cosi' com'e'. */
static int32_t send_snapshot_payload(ipc_message_t *msg,
                                     uint8_t *stack_buf, void **heap_out)
{
    *heap_out = NULL;
    if (msg->payload == NULL || msg->payload_size == 0)
    {
        return IPC_OK;
    }
    if (!uaccess_check(msg->payload, msg->payload_size, false))
    {
        return IPC_ERR_INVALID;
    }

    void *dst = stack_buf;
    if (msg->payload_size > SEND_PAYLOAD_STACK_MAX)
    {
        dst = kmalloc(msg->payload_size);
        if (dst == NULL)
        {
            return IPC_ERR_NO_MEMORY;
        }
        *heap_out = dst;
    }
    /* copy_from_user, MAI memcpy nudo su puntatore utente: il check
     * sopra e' il confine, ma un thread fratello puo' smappare nella
     * race (TOCTOU) — il memcpy faultava a ring 0 senza extable. La
     * copia fault-safe fallisce pulita, e da b134 costa uguale. */
    if (!copy_from_user(dst, msg->payload, msg->payload_size))
    {
        if (*heap_out != NULL)
        {
            kfree(*heap_out);
            *heap_out = NULL;
        }
        return IPC_ERR_INVALID;
    }
    msg->payload = dst;
    return IPC_OK;
}

/* Consegna del payload della reply nel buffer IPC per-processo del
 * sender (IPC_BUF_VADDR, cap IPC_BUF_SIZE — ABI 1.0) e rilascio del
 * buffer kernel staged, in OGNI percorso. */

/* Copia un payload kernel nel buffer IPC del processo RICEVENTE e
 * ritorna l'indirizzo utente. SOLO copia: non libera nulla — la
 * proprieta' del buffer kernel varia per percorso (prestato dal sender
 * bloccato vs posseduto dopo snapshot) e la decide il chiamante.
 *
 * Offset bump-with-wrap per-processo: un server che gestisce una receive
 * ed emette una propria IPC sincrona prima di rispondere non deve
 * sovrascrivere il payload appena consegnato. Allineato a 16, torna a 0
 * quando la coda non basta. Ritorna NULL se non copiabile (il chiamante
 * azzera payload). */
/* Libera l'intera regione overflow (pagine + vregion). Chiamata solo
 * quando un payload nuovo eccede la capacita' corrente (grow = release
 * + realloc); al teardown del processo pensa la distruzione generica
 * dell'AS, che smappa e libera ogni vregion utente committata. */
static void ipc_free_overflow(process_t *proc)
{
    if (proc->ipc_overflow_vaddr == 0 || proc->ipc_overflow_pages == 0)
        return;

    for (uint32_t i = 0; i < proc->ipc_overflow_pages; i++)
    {
        uint32_t v = proc->ipc_overflow_vaddr + i * PAGE_SIZE;
        uint32_t phys = paging_get_physical(v, NULL);
        if (phys)
        {
            paging_unmap_page(v);
            pmm_free_frame(phys);
        }
    }
    paging_release_empty_user_tables(proc->ipc_overflow_vaddr,
                                     proc->ipc_overflow_vaddr +
                                     proc->ipc_overflow_pages * PAGE_SIZE);
    vregion_free(&proc->vm_regions, proc->ipc_overflow_vaddr);
    proc->ipc_overflow_vaddr    = 0;
    proc->ipc_overflow_pages    = 0;
    proc->ipc_overflow_capacity = 0;
}

static void *ipc_copy_to_user_buf(const void *kdata, uint32_t size)
{
    process_t *proc = process_current();
    if (kdata == NULL || size == 0 || proc == NULL ||
        !proc->ipc_buf_ready)
    {
        return NULL;
    }

    /* Percorso medio/grande (semantica 1.0): payload oltre il buffer
     * fisso -> regione overflow per-processo persistente. A regime
     * (capacita' sufficiente) e' una sola memcpy. Il grow rilascia e
     * rialloca; vm_lock perche' su SMP tocca vregion e paging del
     * processo (contesto receive: puo' dormire). */
    if (size > IPC_BUF_SIZE)
    {
        uint32_t pages_needed = (size + PAGE_SIZE - 1) / PAGE_SIZE;

        mutex_lock(&proc->vm_lock);

        /* LOGICA di dimensionamento con isteresi (auto-pulizia): il
         * buffer si RIUSA se basta e non e' sovradimensionato; si
         * RITIRA (release + realloc a misura, sotto lo stesso vm_lock,
         * nel contesto del proprietario) quando il fabbisogno e' sceso
         * a un quarto o meno della capacita' — cosi' un servizio di
         * lunga durata che ha attraversato un picco torna alla taglia
         * del lavoro corrente invece di portarsi dietro l'high-water
         * mark per sempre. La soglia 4x e il pavimento evitano il
         * thrash col carico che respira; sotto il pavimento non si
         * ritira mai (il release+realloc costerebbe piu' dei frame). */
        bool oversized = (proc->ipc_overflow_capacity >=
                              pages_needed * 4u &&
                          proc->ipc_overflow_capacity >
                              IPC_OVERFLOW_SHRINK_FLOOR_PAGES);

        if (proc->ipc_overflow_vaddr != 0 &&
            pages_needed <= proc->ipc_overflow_capacity && !oversized)
        {
            void *dst = (void *)proc->ipc_overflow_vaddr;
            memcpy(dst, kdata, size);
            mutex_unlock(&proc->vm_lock);
            return dst;
        }

        if (proc->ipc_overflow_vaddr != 0)
            ipc_free_overflow(proc);

        uint32_t vaddr = vregion_alloc(&proc->vm_regions, pages_needed,
                                       0x60000000u, 0x6FFFFFFFu,
                                       VREG_IPC | VREG_USER_RW, -1);
        if (vaddr == 0)
            vaddr = vregion_alloc(&proc->vm_regions, pages_needed,
                                  0x00400000u, 0x7FEFFFFFu,
                                  VREG_IPC | VREG_USER_RW, -1);
        if (vaddr == 0)
        {
            mutex_unlock(&proc->vm_lock);
            goto clamp;
        }

        /* Frame fisici: prima contigui (un solo giro di lock pmm),
         * poi per-frame su frammentazione. */
        uint32_t mapped = 0;
        uint32_t base_phys = pmm_alloc_contiguous(pages_needed,
                                                  PMM_ZONE_ANY);
        if (base_phys != 0)
        {
            for (uint32_t i = 0; i < pages_needed; i++)
            {
                paging_map_page(vaddr + i * PAGE_SIZE,
                                base_phys + i * PAGE_SIZE,
                                PTE_PRESENT | PTE_WRITABLE | PTE_USER);
                mapped++;
            }
        }
        else
        {
            for (uint32_t i = 0; i < pages_needed; i++)
            {
                uint32_t phys = pmm_alloc_frame(PMM_ZONE_ANY);
                if (phys == 0) break;
                paging_map_page(vaddr + i * PAGE_SIZE, phys,
                                PTE_PRESENT | PTE_WRITABLE | PTE_USER);
                mapped++;
            }
        }

        if (mapped < pages_needed)
        {
            for (uint32_t i = 0; i < mapped; i++)
            {
                uint32_t v = vaddr + i * PAGE_SIZE;
                uint32_t phys = paging_get_physical(v, NULL);
                if (phys) { paging_unmap_page(v); pmm_free_frame(phys); }
            }
            vregion_free(&proc->vm_regions, vaddr);
            mutex_unlock(&proc->vm_lock);
            goto clamp;
        }

        proc->ipc_overflow_vaddr    = vaddr;
        proc->ipc_overflow_pages    = pages_needed;
        proc->ipc_overflow_capacity = pages_needed;
        vregion_t *vr = vregion_find_exact(&proc->vm_regions, vaddr);
        if (vr) vr->committed = pages_needed;

        memcpy((void *)vaddr, kdata, size);
        mutex_unlock(&proc->vm_lock);
        return (void *)vaddr;

clamp:
        /* Ne' vspace ne' frame: payload strippato. UNA riga per boot
         * lo nomina — perderlo in silenzio e' costato giorni. */
        {
            static bool reported;
            if (!reported)
            {
                reported = true;
                kprintf("[IPC ] payload %u B: overflow non allocabile, "
                        "strippato (pid ricevente %d).\n",
                        size, proc->pid);
            }
        }
        return NULL;
    }

    uint32_t off = (proc->ipc_buf_offset + 15u) & ~15u;
    uint32_t asz = (size + 15u) & ~15u;
    if (off + asz > IPC_BUF_SIZE)
    {
        off = 0;
    }

    void *dst = (void *)(IPC_BUF_VADDR + off);
    if (!copy_to_user(dst, kdata, size))
    {
        return NULL;
    }
    proc->ipc_buf_offset = (off + asz >= IPC_BUF_SIZE) ? 0 : (off + asz);
    return dst;
}

/* Consegna del payload di un messaggio al ricevente: copia nel buffer
 * utente e riscrive il puntatore. Libera il buffer kernel SOLO se
 * `owned` (posseduto dopo snapshot); se prestato (sender bloccato), il
 * sender lo libera al risveglio. Il ricevente non vede MAI un puntatore
 * kernel in msg->payload. */
static void deliver_payload_to_user(ipc_message_t *msg, bool owned)
{
    void *kbuf = msg->payload;
    if (kbuf == NULL || msg->payload_size == 0)
    {
        msg->payload      = NULL;
        msg->payload_size = 0;
        return;
    }

    void *ubuf = ipc_copy_to_user_buf(kbuf, msg->payload_size);
    if (owned)
    {
        kfree(kbuf);
    }
    if (ubuf != NULL)
    {
        msg->payload = ubuf;
    }
    else
    {
        msg->payload      = NULL;
        msg->payload_size = 0;
    }
}

/* Reply: il buffer kernel e' sempre posseduto (snapshot in ipc_reply_staged). */
static void send_deliver_reply_payload(ipc_message_t *reply)
{
    deliver_payload_to_user(reply, true);
}

/* === sys_send: orchestratore ============================================ */

static int32_t sys_send(isr_regs_t *regs)
{
    ipc_message_t msg, reply;
    if (!copy_from_user(&msg, (void *)regs->ecx, sizeof(msg)))
    {
        return IPC_ERR_INVALID;
    }
    memset(&reply, 0, sizeof(reply));

    uint8_t stack_buf[SEND_PAYLOAD_STACK_MAX];
    void   *heap_buf;
    int32_t ret = send_snapshot_payload(&msg, stack_buf, &heap_buf);
    if (ret != IPC_OK)
    {
        return ret;
    }

    ret = ipc_send_sync(regs->ebx, &msg, &reply);

    if (heap_buf != NULL)
    {
        kfree(heap_buf);
    }
    if (ret == IPC_OK)
    {
        send_deliver_reply_payload(&reply);
        if (regs->edx != 0)
        {
            copy_to_user((void *)regs->edx, &reply, sizeof(reply));
        }
    }
    return ret;
}

static int32_t receive_to_user(uint32_t port_id, void *user_msg, bool blocking)
{
    ipc_message_t msg;
    bool owned = false;
    int32_t ret = blocking
                ? ipc_receive(port_id, &msg, &owned)
                : ipc_receive_nowait(port_id, &msg, &owned);

    if (ret != IPC_OK)
    {
        return ret;
    }

    /* Il payload arriva come buffer KERNEL. Copialo nel buffer IPC del
     * ricevente e riscrivi il puntatore PRIMA di consegnare in ring 3.
     * deliver_ libera il buffer solo se `owned` (snapshot posseduto); se
     * prestato dal sender bloccato (sync, zero-copia), il sender lo
     * libera al risveglio. */
    if (msg.payload != NULL && msg.payload_size > 0)
    {
        deliver_payload_to_user(&msg, owned);
    }

    if (user_msg != NULL)
    {
        copy_to_user(user_msg, &msg, sizeof(msg));
    }
    return ret;
}

static int32_t sys_receive(isr_regs_t *regs)
{
    return receive_to_user(regs->ebx, (void *)regs->ecx, true);
}

static int32_t sys_receive_nowait(isr_regs_t *regs)
{
    return receive_to_user(regs->ebx, (void *)regs->ecx, false);
}

static int32_t sys_reply(isr_regs_t *regs)
{
    ipc_message_t reply;
    if (!copy_from_user(&reply, (void *)regs->ecx, sizeof(reply)))
    {
        return IPC_ERR_INVALID;
    }

    /* Payload: snapshot in kernel (elimina il TOCTOU) e staging con
     * lock per-entry — vedi ipc_reply_staged. Cap: IPC_BUF_SIZE, e'
     * dove il sender lo ricevera'. */
    void *snap = NULL;
    uint32_t size = 0;
    if (reply.payload != NULL && reply.payload_size > 0)
    {
        if (reply.payload_size > IPC_BUF_SIZE)
        {
            return IPC_ERR_INVALID;
        }
        if (!uaccess_check(reply.payload, reply.payload_size, false))
        {
            return IPC_ERR_INVALID;
        }
        snap = kmalloc(reply.payload_size);
        if (snap == NULL)
        {
            return IPC_ERR_NO_MEMORY;
        }
        if (!copy_from_user(snap, reply.payload, reply.payload_size))
        {
            kfree(snap);                /* race di unmap: fallisci pulito */
            return IPC_ERR_INVALID;
        }
        size = reply.payload_size;
    }
    reply.payload = NULL;
    return ipc_reply_staged((tid_t)regs->ebx, &reply, snap, size);
}

static int32_t sys_post(isr_regs_t *regs)
{
    ipc_message_t msg;
    if (!copy_from_user(&msg, (void *)regs->ecx, sizeof(msg)))
    {
        return IPC_ERR_INVALID;
    }
    void *snap = NULL;
    if (msg.payload != NULL && msg.payload_size > 0)
    {
        if (!uaccess_check(msg.payload, msg.payload_size, false))
        {
            return IPC_ERR_INVALID;
        }
        snap = kmalloc(msg.payload_size);
        if (snap == NULL)
        {
            return IPC_ERR_NO_MEMORY;
        }
        if (!copy_from_user(snap, msg.payload, msg.payload_size))
        {
            kfree(snap);                /* race di unmap: fallisci pulito */
            return IPC_ERR_INVALID;
        }
        msg.payload = snap;
    }
    int32_t ret = ipc_post(regs->ebx, &msg);
    if (msg.payload != NULL)            /* non rubato: liberalo           */
    {
        kfree(msg.payload);
    }
    return ret;
}

/* Generazione corrente della porta (0 = morta): il chiamante la
 * cattura al binding e la ripassa a sys_post_ck per una consegna
 * anti-ABA. */
static int32_t sys_port_gen(isr_regs_t *regs)
{
    return (int32_t)ipc_port_generation(regs->ebx);
}

/* Post verificato per generazione. ebx=port_id, ecx=gen attesa,
 * edx=msg utente. Snapshot del payload come sys_post (elimina il
 * TOCTOU); ipc_post_checked ritorna IPC_ERR_DEAD se l'id e' stato
 * riciclato. */
static int32_t sys_post_ck(isr_regs_t *regs)
{
    ipc_message_t msg;
    if (!copy_from_user(&msg, (void *)regs->edx, sizeof(msg)))
    {
        return IPC_ERR_INVALID;
    }
    void *snap = NULL;
    if (msg.payload != NULL && msg.payload_size > 0)
    {
        if (!uaccess_check(msg.payload, msg.payload_size, false))
        {
            return IPC_ERR_INVALID;
        }
        snap = kmalloc(msg.payload_size);
        if (snap == NULL)
        {
            return IPC_ERR_NO_MEMORY;
        }
        if (!copy_from_user(snap, msg.payload, msg.payload_size))
        {
            kfree(snap);                /* race di unmap: fallisci pulito */
            return IPC_ERR_INVALID;
        }
        msg.payload = snap;
    }
    int32_t ret = ipc_post_checked(regs->ebx, regs->ecx, &msg);
    if (msg.payload != NULL)            /* non rubato (fallito): liberalo */
    {
        kfree(msg.payload);
    }
    return ret;
}

/* Registra un death-watch. ebx=target_port, ecx=target_gen,
 * edx=notify_port, esi=code. La notify_port deve appartenere al
 * chiamante (nessuno registra recapiti altrui). Ritorna 0/1/-1 come
 * deathwatch_add. */
static int32_t sys_watch_port(isr_regs_t *regs)
{
    process_t *proc = process_current();
    if (proc == NULL)
    {
        return -1;
    }

    /* La porta di recapito deve essere del chiamante. */
    ipc_port_t *np = ipc_port_get(regs->edx);
    if (np == NULL)
    {
        return -1;
    }
    bool mine = (np->owner_pid == proc->pid);
    ipc_port_put(np);
    if (!mine)
    {
        return -1;
    }

    return deathwatch_add((int32_t)proc->pid, regs->edx, regs->esi,
                          regs->ebx, regs->ecx);
}

static int32_t sys_notify(isr_regs_t *regs)
{
    return ipc_notify(regs->ebx, regs->ecx);
}

static int32_t sys_wait_notify(isr_regs_t *regs)
{
    return (int32_t)ipc_wait_notify(regs->ebx);
}

/* === Memory (ABI 1:1) ================================================== */

/* Smonta `pages` pagine utente da `virt` nell'AS corrente, a blocchi:
 * smonta il blocco, shootdown mirato all'address-space, e solo dopo
 * (se free_phys) restituisce i frame al pool — un frame non e' mai nel
 * pool mentre una traduzione, anche remota, lo raggiunge ancora.
 * Chiamante tiene proc->vm_lock. */
static void unmap_user_range(process_t *proc, uint32_t virt, uint32_t pages,
                             bool free_phys)
{
    uint32_t phys[64];

    for (uint32_t done = 0; done < pages; )
    {
        uint32_t n = pages - done;
        if (n > 64u)
        {
            n = 64u;
        }
        uint32_t base = virt + done * PAGE_SIZE;

        for (uint32_t i = 0; i < n; i++)
        {
            uint32_t a = base + i * PAGE_SIZE;
            phys[i] = free_phys ? paging_get_physical(a, NULL) : 0;
            paging_unmap_page(a);
        }

        paging_release_empty_user_tables(base, base + n * PAGE_SIZE);
        tlb_shootdown_aspace(proc->page_directory, base, n);

        for (uint32_t i = 0; i < n; i++)
        {
            if (phys[i] != 0)
            {
                pmm_free_frame(phys[i] & 0xFFFFF000u);
            }
        }
        done += n;
    }
}

static int32_t sys_mmap(isr_regs_t *regs)
{
    uint32_t virt  = regs->ebx;
    uint32_t pages = regs->ecx;
    if (pages == 0 || pages > 4096)
    {
        return 0;
    }
    process_t *proc = process_current();
    if (proc == NULL)
    {
        return 0;
    }

    mutex_lock(&proc->vm_lock);
    if (virt == 0)
    {
        virt = vregion_alloc(&proc->vm_regions, pages,
                             0x40000000u, 0x5FFFFFFFu,
                             VREG_MMAP | VREG_USER_RW, -1);
        if (virt == 0)
        {
            mutex_unlock(&proc->vm_lock);
            return 0;
        }
    }
    else
    {
        if ((virt & 0xFFFu) != 0)
        {
            mutex_unlock(&proc->vm_lock);
            return 0;
        }
        if (vregion_insert(&proc->vm_regions, virt, pages,
                           VREG_MMAP | VREG_USER_RW) == NULL)
        {
            mutex_unlock(&proc->vm_lock);
            return 0;
        }
    }

    for (uint32_t i = 0; i < pages; i++)
    {
        uint32_t addr = virt + i * PAGE_SIZE;
        uint32_t frame = pmm_alloc_frame(PMM_ZONE_ANY);
        if (frame == 0)
        {
            unmap_user_range(proc, virt, i, true);
            vregion_free(&proc->vm_regions, virt);
            mutex_unlock(&proc->vm_lock);
            return 0;
        }
        memset((void *)(frame + KERNEL_VMA), 0, PAGE_SIZE);
        paging_map_page(addr, frame, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    vregion_t *r = vregion_find_exact(&proc->vm_regions, virt);
    if (r != NULL)
    {
        r->committed = pages;
    }
    mutex_unlock(&proc->vm_lock);
    return (int32_t)virt;
}

static int32_t sys_munmap(isr_regs_t *regs)
{
    uint32_t virt  = regs->ebx;
    uint32_t pages = regs->ecx;
    if (pages == 0 || (virt & 0xFFFu) != 0)
    {
        return -1;
    }
    process_t *proc = process_current();
    if (proc == NULL)
    {
        return -1;
    }

    mutex_lock(&proc->vm_lock);
    vregion_t *r = vregion_find_exact(&proc->vm_regions, virt);
    if (r == NULL || (r->flags & VREG_FIXED))
    {
        mutex_unlock(&proc->vm_lock);
        return -1;
    }
    bool free_phys = !(r->flags & VREG_DEVICE);
    unmap_user_range(proc, virt, pages, free_phys);
    vregion_free(&proc->vm_regions, virt);
    mutex_unlock(&proc->vm_lock);
    return 0;
}

/* SYS_TASK_SNAPSHOT: header + righe nel buffer utente (ebx, capienza
 * ecx). Le righe si raccolgono in un buffer KERNEL (kmalloc) sotto il
 * lifecycle-lock e si copiano DOPO il rilascio: copy_to_user puo'
 * fault-are, mai con uno spinlock in mano. Ritorna il numero di righe
 * scritte o -1. */
static int32_t sys_task_snapshot(isr_regs_t *regs)
{
    uint8_t *ubuf = (uint8_t *)regs->ebx;
    uint32_t cap  = regs->ecx;
    if (ubuf == NULL || cap < sizeof(dob_tasksnap_hdr_t))
    {
        return -1;
    }

    uint32_t max_rows = (cap - sizeof(dob_tasksnap_hdr_t))
                      / sizeof(dob_tasksnap_row_t);
    if (max_rows > DOB_TASKSNAP_MAX_ROWS)
    {
        max_rows = DOB_TASKSNAP_MAX_ROWS;
    }

    dob_tasksnap_row_t *rows = NULL;
    if (max_rows > 0)
    {
        rows = kmalloc(max_rows * sizeof(*rows));
        if (rows == NULL)
        {
            return -1;
        }
        memset(rows, 0, max_rows * sizeof(*rows));
    }

    int nrows = (rows != NULL) ? task_snapshot_collect(rows, max_rows) : 0;
    if (nrows < 0)
    {
        nrows = 0;
    }
    /* DMA per riga: DOPO il collettore, col solo lock del pool —
     * mai annidato sotto il lifecycle-lock. */
    for (int i = 0; i < nrows; i++)
    {
        rows[i].dma_pages = dma_track_pages_of(rows[i].pid);
    }

    dob_tasksnap_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.version = DOB_TASKSNAP_VERSION;
    hdr.now_ns  = clock_now_ns();
    for (uint32_t c = 0; c < DOB_TASKSNAP_MAX_CPUS; c++)
    {
        uint64_t idle;
        if (scheduler_cpu_idle_ns(c, &idle))
        {
            hdr.cpu_idle_ns[c] = idle;
            hdr.ncpu = c + 1;           /* i core online sono un prefisso */
        }
    }
    pmm_stats_t ms;
    pmm_get_stats(&ms);
    hdr.ram_total_mb = (ms.total_frames * PAGE_SIZE) >> 20;
    hdr.ram_free_mb  = (ms.free_frames  * PAGE_SIZE) >> 20;
    hdr.dma_free_frames = ms.dma_free;
    hdr.dma_slots       = dma_track_slot_pressure();
    hdr.nproc        = (uint32_t)nrows;

    bool ok = copy_to_user(ubuf, &hdr, sizeof(hdr));
    if (ok && nrows > 0)
    {
        ok = copy_to_user(ubuf + sizeof(hdr), rows,
                          (uint32_t)nrows * sizeof(*rows));
    }
    if (rows != NULL)
    {
        kfree(rows);
    }
    return ok ? nrows : -1;
}

/* SYS_PROC_SET_PRIORITY: ebx=pid, ecx=prio (0..3). Modello di fiducia
 * di MainDOB (mono-utente): qualunque processo puo' renice-are gli
 * altri; il kernel (PID 0) resta intoccabile. */
static int32_t sys_proc_set_priority(isr_regs_t *regs)
{
    return thread_renice_by_pid((pid_t)regs->ebx, regs->ecx);
}

static int32_t sys_meminfo(isr_regs_t *regs)
{
    process_t *proc = process_current();
    if (proc == NULL)
    {
        return -1;
    }
    pmm_stats_t s;
    pmm_get_stats(&s);

    uint32_t buf[5];
    buf[0] = 0;                         /* pagine committed processo      */
    buf[1] = proc->vm_regions.count;
    buf[2] = (s.total_frames * PAGE_SIZE) >> 20;
    buf[3] = (s.free_frames  * PAGE_SIZE) >> 20;
    buf[4] = s.total_frames ? ((s.total_frames - s.free_frames) * 100u)
                              / s.total_frames : 100u;
    if (!copy_to_user((void *)regs->ebx, buf, sizeof(buf)))
    {
        return -1;
    }
    return 0;
}

/* === Time ============================================================== */

/* ABI 1.0: nonostante il nome, arg0 (ebx) sono MILLISECONDI — la libc
 * lo espone come sleep_ms(). La granularita' fine sta nel backend
 * (scheduler in ns), non nel contratto. */
static int32_t sys_nanosleep(isr_regs_t *regs)
{
    scheduler_sleep_ns((uint64_t)regs->ebx * 1000000ull);
    return 0;
}

/* ABI 1.0: int32 di millisecondi da boot, in eax, nessun argomento
 * (libc: clock_ms()). Tronca dopo ~24 giorni di uptime: il contratto
 * legacy lo accetta; chi serve piu' portata usa SYS_GETTIME (orologio
 * reale) o delta di sys_clock_us. */
static int32_t sys_clock_gettime(isr_regs_t *regs UNUSED)
{
    return (int32_t)(clock_now_ns() / 1000000ull);
}

/* === Version =========================================================== */

/* ABI 1.0: scrive ESATTAMENTE 5 uint32 — major, minor, patch,
 * revision, build (benchmark: "v%u.%u.%u.%u.%u"). */
static int32_t sys_getversion(isr_regs_t *regs)
{
    uint32_t v[5] = {
        MAINDOB_VERSION_MAJOR, MAINDOB_VERSION_MINOR,
        MAINDOB_VERSION_PATCH, MAINDOB_VERSION_REVISION,
        MAINDOB_VERSION_BUILD
    };
    return copy_to_user((void *)regs->ebx, v, sizeof(v)) ? 0 : -1;
}

/* === Futex ============================================================= */

static int32_t sys_futex(isr_regs_t *regs)
{
    uint32_t op = regs->ebx;
    if (op == FUTEX_WAIT)
    {
        return futex_wait(regs->ecx, regs->edx, regs->esi);
    }
    if (op == FUTEX_WAKE)
    {
        return futex_wake(regs->ecx, regs->edx);
    }
    return -1;
}

/* === Registry ========================================================== */

static int32_t sys_reg_register(isr_regs_t *regs)
{
    char name[REGISTRY_NAME_MAX];
    if (copy_string_from_user(name, (const char *)regs->ebx,
                              sizeof(name)) < 0)
    {
        return -1;
    }
    return registry_register(name, regs->ecx,
                             process_current() ? process_current()->pid : 0);
}

static int32_t sys_reg_find(isr_regs_t *regs)
{
    char name[REGISTRY_NAME_MAX];
    if (copy_string_from_user(name, (const char *)regs->ebx,
                              sizeof(name)) < 0)
    {
        return -1;
    }
    return (int32_t)registry_find(name);
}

static int32_t sys_reg_unregister(isr_regs_t *regs)
{
    char name[REGISTRY_NAME_MAX];
    if (copy_string_from_user(name, (const char *)regs->ebx,
                              sizeof(name)) < 0)
    {
        return -1;
    }
    registry_unregister(name, process_current() ? process_current()->pid : 0);
    return 0;
}

static int32_t sys_reg_wait(isr_regs_t *regs)
{
    char name[REGISTRY_NAME_MAX];
    if (copy_string_from_user(name, (const char *)regs->ebx,
                              sizeof(name)) < 0)
    {
        return -1;
    }
    return (int32_t)registry_wait(name, regs->ecx);
}

/* === Debug (driver only) =============================================== */

static int32_t sys_debug_print(isr_regs_t *regs)
{
    if (!caller_is_driver())
    {
        return -1;
    }
    char line[256];
    if (copy_string_from_user(line, (const char *)regs->ebx,
                              sizeof(line)) < 0)
    {
        return -1;
    }
    kprintf("[USER] %s\n", line);
    return 0;
}

/* === Process: wait / kill / thread_create ============================== */

static bool caller_may_reap(process_t *caller, process_t *target)
{
    return target->parent_pid == caller->pid
        || caller->pid == 0
        || (caller->privileges & PRIV_DRIVER);
}

static int32_t sys_wait(isr_regs_t *regs)
{
    pid_t child_pid = (pid_t)regs->ebx;

    process_t *caller = process_current();
    process_t *child  = process_get_ref(child_pid);
    if (caller == NULL || child == NULL)
    {
        if (child != NULL) process_put(child);
        return -1;
    }
    if (!caller_may_reap(caller, child))
    {
        process_put(child);
        return -1;                      /* due waiter = double-destroy    */
    }

    /* prepare-first su exit_waiters (semantica 1.0). Il ref tiene VIVO il
     * PCB del figlio attraverso lo yield: non serve piu' il re-get grezzo
     * per rivalidare un puntatore che poteva essere liberato — lo stato
     * riflette ZOMBIE/DEAD quando muore, e la struct resta finche' non
     * facciamo put. */
    while (child->state != PROC_ZOMBIE && child->state != PROC_DEAD)
    {
        wait_prepare(&child->exit_waiters, 0);
        if (child->state == PROC_ZOMBIE || child->state == PROC_DEAD)
        {
            wait_cancel();
            break;
        }
        scheduler_yield();
        wait_finish();
    }

    int32_t code = child->exit_code;
    process_destroy(child);
    process_put(child);
    return code;
}

static int32_t sys_kill(isr_regs_t *regs)
{
    pid_t target_pid = (pid_t)regs->ebx;
    process_t *target = process_get_ref(target_pid);
    process_t *caller = process_current();
    if (target == NULL || target->pid == 0 || caller == NULL)
    {
        if (target != NULL) process_put(target);
        return -1;
    }
    if (caller->pid != 0 && !(caller->privileges & PRIV_DRIVER) &&
        caller->pid != target->parent_pid)
    {
        process_put(target);
        return -1;
    }

    target->exit_code = -1;
    wait_queue_wake_all(&target->exit_waiters);
    process_destroy(target);
    process_put(target);
    return 0;
}

static int32_t sys_thread_create(isr_regs_t *regs)
{
    uint32_t entry = regs->ebx;
    uint32_t arg   = regs->ecx;
    uint32_t ustk  = regs->edx;

    if (entry == 0 || entry >= USER_SPACE_LIMIT)
    {
        return -1;
    }
    if (ustk == 0 || ustk > USER_SPACE_LIMIT)
    {
        return -1;
    }

    process_t *proc = process_current();
    if (proc == NULL || proc->pid == 0)
    {
        return -1;
    }

    thread_t *t = thread_create_user(proc, entry, arg, ustk);
    return (t != NULL) ? t->tid : -1;
}

/* === Memory: brk (semantica 1.0: lazy init, cap 0x40000000) ============ */

static int32_t sys_brk(isr_regs_t *regs)
{
    uint32_t new_brk = regs->ebx;
    /* AS-affine: durante la fase in-driver del boomerang video il CR3
     * installato e' quello del DRIVER ma process_current() resta il
     * chiamante (migrating-thread). La malloc del driver (surface
     * SYSRAM, shadow) chiama sbrk da li': contabilita' del brk e
     * paging_map_page devono agire sullo STESSO processo — quello del
     * CR3 attivo — o i heap di driver e chiamante si corrompono a
     * vicenda (blocchi sovrapposti: schermo che si auto-fagocita). */
    process_t *proc = video_boomerang_as_process();
    if (proc == NULL || proc->pid == 0)
    {
        return -1;
    }

    mutex_lock(&proc->vm_lock);
    if (proc->brk_start == 0)
    {
        proc->brk_start   = 0x10000000u;
        proc->brk_current = proc->brk_start;
    }
    if (proc->heap_region == NULL)
    {
        proc->heap_region =
            vregion_find_exact(&proc->vm_regions, proc->brk_start);
        if (proc->heap_region == NULL)
        {
            proc->heap_region =
                vregion_insert(&proc->vm_regions, proc->brk_start, 0,
                               VREG_HEAP | VREG_USER_RW);
        }
    }

    if (new_brk == 0)
    {
        int32_t cur = (int32_t)proc->brk_current;
        mutex_unlock(&proc->vm_lock);
        return cur;                             /* query                  */
    }
    if (proc->heap_region == NULL || new_brk > 0x40000000u)
    {
        mutex_unlock(&proc->vm_lock);
        return -1;
    }
    if (new_brk < proc->brk_start)
    {
        int32_t cur = (int32_t)proc->brk_current;
        mutex_unlock(&proc->vm_lock);
        return cur;                             /* shrink sotto start: no */
    }

    uint32_t old_page = ALIGN_UP(proc->brk_current, PAGE_SIZE);
    uint32_t new_page = ALIGN_UP(new_brk, PAGE_SIZE);
    vregion_t *hr = proc->heap_region;

    if (new_page > old_page)
    {
        uint32_t new_pages = (new_page - proc->brk_start) / PAGE_SIZE;
        if (hr->next != NULL &&
            proc->brk_start + new_pages * PAGE_SIZE > hr->next->base)
        {
            mutex_unlock(&proc->vm_lock);
            return -1;                  /* invaderebbe la regione dopo    */
        }

        /* hr->pages aggiornato solo DOPO il loop: un OOM a meta' fa
         * rollback completo, niente frame orfani (semantica 1.0). */
        for (uint32_t addr = old_page; addr < new_page; addr += PAGE_SIZE)
        {
            uint32_t frame = pmm_alloc_frame(PMM_ZONE_ANY);
            if (frame == 0 && oom_try_recover())
            {
                frame = pmm_alloc_frame(PMM_ZONE_ANY);
            }
            if (frame == 0)
            {
                unmap_user_range(proc, old_page,
                                 (addr - old_page) / PAGE_SIZE, true);
                mutex_unlock(&proc->vm_lock);
                return -1;
            }
            memset((void *)(frame + KERNEL_VMA), 0, PAGE_SIZE);
            paging_map_page(addr, frame,
                            PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        }
        hr->pages     = new_pages;
        hr->committed = new_pages;
    }
    else if (new_page < old_page)
    {
        unmap_user_range(proc, new_page, (old_page - new_page) / PAGE_SIZE,
                         true);
        hr->pages     = (new_page - proc->brk_start) / PAGE_SIZE;
        hr->committed = hr->pages;
    }

    proc->brk_current = new_brk;
    int32_t ret = (int32_t)proc->brk_current;
    mutex_unlock(&proc->vm_lock);
    return ret;
}

/* === SHM (ABI 1.0: create scrive il vaddr del creatore) ================ */

static int32_t sys_shm_create(isr_regs_t *regs)
{
    process_t *proc = process_current();
    if (proc == NULL || proc->pid == 0)
    {
        return -1;
    }

    uint32_t vaddr = 0;
    int32_t id = shm_create(regs->ebx, proc, &vaddr);
    if (id < 0)
    {
        return -1;
    }
    if (regs->ecx != 0 && !uaccess_put_u32(regs->ecx, vaddr))
    {
        shm_unmap(id, proc);
        return -1;
    }
    return id;
}

static int32_t sys_shm_map(isr_regs_t *regs)
{
    process_t *proc = process_current();
    if (proc == NULL || proc->pid == 0)
    {
        return -1;
    }

    uint32_t vaddr = 0;
    if (shm_map((int32_t)regs->ebx, proc, &vaddr) != 0)
    {
        return -1;
    }
    if (regs->ecx != 0 && !uaccess_put_u32(regs->ecx, vaddr))
    {
        shm_unmap((int32_t)regs->ebx, proc);
        return -1;
    }
    return 0;
}

static int32_t sys_shm_unmap(isr_regs_t *regs)
{
    process_t *proc = process_current();
    if (proc == NULL)
    {
        return -1;
    }
    return shm_unmap((int32_t)regs->ebx, proc);
}

/* === Event group (ABI 1.0) ============================================= */

static int32_t sys_event_create(isr_regs_t *regs UNUSED)
{
    process_t *p = process_current();
    return event_group_create(p ? p->pid : 0);
}

static int32_t sys_event_setclear(isr_regs_t *regs)
{
    int32_t  gid  = (int32_t)regs->ebx;
    uint32_t bits = regs->ecx;
    switch (regs->edx)
    {
        case EVENT_OP_SET:    return event_group_set(gid, bits);
        case EVENT_OP_CLEAR:  return event_group_clear(gid, bits);
        case EVENT_OP_PULSE:  return event_group_pulse(gid, bits);
        case EVENT_OP_POISON: return event_group_poison(gid);
        case EVENT_OP_RESET:  return event_group_reset(gid);
        default:              return -1;
    }
}

static int32_t sys_event_wait(isr_regs_t *regs)
{
    uint32_t flags_snapshot = 0;
    int r = event_group_wait((int32_t)regs->ebx, regs->ecx,
                             (uint8_t)regs->edx, regs->esi,
                             &flags_snapshot);
    if (regs->edi != 0)
    {
        uaccess_put_u32(regs->edi, flags_snapshot);
    }
    return r;
}

static int32_t sys_event_getflags(isr_regs_t *regs)
{
    return (int32_t)event_group_get_flags((int32_t)regs->ebx);
}

/* === Time: RTC, clock_us, sleep_us ===================================== */

static int32_t sys_gettime(isr_regs_t *regs)
{
    dob_realtime_t t;
    clock_get_realtime(&t);

    uint32_t kbuf[7] = {
        t.year, t.month, t.day, t.hour, t.minute, t.second, t.ms
    };
    return copy_to_user((void *)regs->ebx, kbuf, sizeof(kbuf)) ? 0 : -1;
}

static int32_t sys_clock_us(isr_regs_t *regs UNUSED)
{
    /* ABI 1.0: int32 di microsecondi, usato come delta (il wrap si
     * cancella nella sottrazione lato utente). */
    return (int32_t)(clock_now_ns() / 1000ull);
}

/* ABI 1.0: riporta chi consegna gli IRQ dei device — 1=IOAPIC,
 * 0=PIC 8259. Aperta a ogni processo (e' solo informativa). */
static int32_t sys_intr_delivery_mode(isr_regs_t *regs UNUSED)
{
    return irq_delivery_via_ioapic() ? 1 : 0;
}

/* ABI 1.0: busy-wait NON schedulante, cap rigido a 100 µs. Oltre il
 * cap RITORNA -1 (rifiuto, non clamp): e' la valvola anti-event-driven
 * per i percorsi real-time dei driver; per attese piu' lunghe si passa
 * dallo scheduler (nanosleep). La libc (busy_wait_us) spezza in chunk
 * da <=100 µs contando su questo contratto. */
static int32_t sys_sleep_us(isr_regs_t *regs)
{
    uint32_t us = regs->ebx;
    if (us > 100u)
    {
        return -1;
    }

    uint64_t target = clock_now_ns() + (uint64_t)us * 1000ull;
    while (clock_now_ns() < target)
    {
        __asm__ volatile ("pause");
    }
    return 0;
}

/* === Entropy =========================================================== */

static int32_t sys_random(isr_regs_t *regs)
{
    uint32_t len = regs->ecx;
    if (len == 0 || len > 4096)
    {
        return -1;
    }
    if (!uaccess_check((void *)regs->ebx, len, true))
    {
        return -1;
    }

    uint8_t stack_buf[256];
    void *kbuf = stack_buf;
    bool heap = false;
    if (len > sizeof(stack_buf))
    {
        kbuf = kmalloc(len);
        if (kbuf == NULL)
        {
            return -1;
        }
        heap = true;
    }

    entropy_get_bytes(kbuf, len);
    bool ok = copy_to_user((void *)regs->ebx, kbuf, len);
    if (heap)
    {
        kfree(kbuf);
    }
    return ok ? 0 : -1;
}

/* === Spawn da dati utente (ABI 1.0: data,size,name_hint,argv_blob) ===== */

static int32_t sys_spawn_data(isr_regs_t *regs)
{
    uint32_t size = regs->ecx;
    if (regs->ebx == 0 || size == 0 || size > 1024u * 1024u)
    {
        return -1;
    }
    if (!uaccess_check((void *)regs->ebx, size, false))
    {
        return -1;
    }

    void *kbuf = kmalloc(size);
    if (kbuf == NULL)
    {
        return -1;
    }
    if (!copy_from_user(kbuf, (void *)regs->ebx, size))
    {
        kfree(kbuf);                    /* race di unmap: fallisci pulito */
        return -1;
    }

    /* Name hint: un puntatore invalido degrada a "spawned", mai fallisce
     * lo spawn (semantica 1.0). */
    char name[PROCESS_NAME_MAX] = "spawned";
    if (regs->edx != 0)
    {
        char tmp[PROCESS_NAME_MAX];
        if (copy_string_from_user(tmp, (const char *)regs->edx,
                                  sizeof(tmp)) > 0)
        {
            memcpy(name, tmp, sizeof(name));
        }
    }

    /* argv blob [argc u32][size u32][string pool] su heap (mai 4 KB
     * sullo stack kernel — semantica e motivazione 1.0), validato per
     * intero PRIMA dello spawn: conteggio NUL == argc, cosi' uno spawn
     * non parte mai per un blob rotto. */
    uint8_t *argv_kbuf = NULL;
    uint32_t argc = 0, argv_bytes = 0;
    const char *argv_strings = NULL;

    if (regs->esi != 0)
    {
        uint32_t hdr[2];
        if (!copy_from_user(hdr, (void *)regs->esi, sizeof(hdr)) ||
            hdr[1] < 8 || hdr[1] > USER_ARGV_MAX_BLOB_BYTES ||
            !uaccess_check((void *)regs->esi, hdr[1], false))
        {
            kfree(kbuf);
            return -1;
        }

        argv_kbuf = (uint8_t *)kmalloc(hdr[1]);
        if (argv_kbuf == NULL)
        {
            kfree(kbuf);
            return -1;
        }
        if (!copy_from_user(argv_kbuf, (void *)regs->esi, hdr[1]))
        {
            kfree(argv_kbuf);           /* race di unmap: fallisci pulito */
            kfree(kbuf);
            return -1;
        }
        argc         = hdr[0];
        argv_strings = (const char *)(argv_kbuf + 8);
        argv_bytes   = hdr[1] - 8;

        uint32_t nul_count = 0;
        for (uint32_t i = 0; i < argv_bytes; i++)
        {
            if (argv_strings[i] == '\0')
            {
                nul_count++;
            }
        }
        if (nul_count != argc)
        {
            kfree(argv_kbuf);
            kfree(kbuf);
            return -1;
        }
    }

    int32_t pid = process_spawn_elf_ex(name, kbuf, size, NULL,
                                       argc, argv_strings, argv_bytes);
    kfree(argv_kbuf);
    kfree(kbuf);
    return pid;
}

/* === Boot: Startup_modules ============================================== */

static int32_t sys_get_startup(isr_regs_t *regs)
{
    uint32_t buf_size = regs->ecx;
    if (regs->ebx == 0 || buf_size == 0)
    {
        return -1;
    }

    const char *text = boot_get_startup_text();
    uint32_t len = boot_get_startup_len();
    if (len >= buf_size)
    {
        len = buf_size - 1;
    }
    if (!copy_to_user((void *)regs->ebx, text, len))
    {
        return -1;
    }
    uint8_t nul = 0;
    copy_to_user((void *)(regs->ebx + len), &nul, 1);
    return (int32_t)len;
}

static int32_t sys_get_module_flags(isr_regs_t *regs)
{
    uint32_t buf_size = regs->ecx;
    process_t *caller = process_current();
    if (regs->ebx == 0 || buf_size == 0 || caller == NULL)
    {
        return -1;
    }

    uint32_t len = strlen(caller->flags_str);
    if (len >= buf_size)
    {
        len = buf_size - 1;
    }
    if (!copy_to_user((void *)regs->ebx, caller->flags_str, len))
    {
        return -1;
    }
    uint8_t nul = 0;
    copy_to_user((void *)(regs->ebx + len), &nul, 1);
    return (int32_t)len;
}

/* === Home dir sandbox =================================================== */

static int32_t sys_get_home_dir(isr_regs_t *regs)
{
    process_t *target = process_get_ref((pid_t)regs->ebx);
    uint32_t buf_size = regs->edx;
    if (target == NULL || regs->ecx == 0 || buf_size == 0)
    {
        if (target != NULL) process_put(target);
        return -1;
    }

    uint32_t len = strlen(target->home_dir) + 1;
    if (len > buf_size)
    {
        len = buf_size;
    }
    int32_t rc = copy_to_user((void *)regs->ecx, target->home_dir, len)
               ? 0        /* success: 0 per il contratto di unistd.h */
               : -1;
    process_put(target);
    return rc;
}

/* === Reboot (driver-only, 8042) ========================================= */

static int32_t sys_reboot(isr_regs_t *regs UNUSED)
{
    if (!caller_is_driver())
    {
        return -1;
    }
    kprintf("[SYS ] Reboot richiesto: reset via 8042.\n");
    outb(0x64, 0xFE);
    for (;;)
    {
        __asm__ volatile ("hlt");
    }
}

/* TEST: panic deliberato, per verificare che la schermata di panic sia
 * VISIBILE (video_force_text_mode: reset video a modo testo) anche con la
 * GUI/BGA attiva. Chiamata dal companion 'panictest'. NON gated: e' uno
 * strumento di test/diagnostica. kpanic non torna. */
static int32_t sys_panic_test(isr_regs_t *regs UNUSED)
{
    process_t *p = process_current();
    kpanic("panic_test: panic deliberato per verifica schermata (PID %d)",
           p ? (int)p->pid : -1);
    return 0;   /* mai raggiunto */
}

/* Il driver video registra il framebuffer attivo (indirizzo fisico +
 * geometria) perche' il panic possa disegnarci il testo. Solo driver. */
static int32_t sys_set_panic_fb(isr_regs_t *regs)
{
    if (!caller_is_driver())
    {
        return -1;
    }
    console_register_panic_fb(regs->ebx, regs->ecx, regs->edx,
                              regs->esi, regs->edi);
    return 0;
}

/* === OOM recovery (impianto 1.0: uccide il piu' grande non-driver) ===== */

/* Ritorna true se ha differito un kill (il chiamante puo' ritentare
 * l'allocazione dopo che idle avra' drenato la teardown_q). */
bool oom_try_recover(void);
bool oom_try_recover(void)
{
    pmm_stats_t st;
    pmm_get_stats(&st);
    if (st.pressure < 95)
    {
        return false;                   /* non ancora critico            */
    }

    pid_t    victim = -1;
    uint32_t victim_pages = 0;
    for (pid_t i = 2; i < MAX_PROCESSES; i++)    /* salta kernel(0)/init(1)*/
    {
        process_t *p = process_get_ref(i);
        if (p == NULL)
        {
            continue;
        }
        /* PCB pinnato: la reclamation dei sotto-oggetti (vm_regions) e'
         * differita a refcount 0, quindi la traversata qui e' sicura da
         * un free concorrente. */
        if (!(p->privileges & PRIV_DRIVER) &&
            p->state != PROC_DEAD && p->state != PROC_ZOMBIE)
        {
            uint32_t pages = vregion_total_committed(&p->vm_regions);
            if (pages > victim_pages)
            {
                victim_pages = pages;
                victim = i;
            }
        }
        process_put(p);
    }
    if (victim < 0)
    {
        return false;
    }

    /* Il kill vero e' differito sul canale intrusivo non-perdibile
     * (teardown_q), non qui: uccidere un processo mentre si tiene
     * memoria/lock del chiamante e' fragile, e uno scarto a coda piena
     * vanificherebbe proprio il recupero dalla memoria esaurita. */
    process_t *p = process_get_ref(victim);
    if (p == NULL)
    {
        return false;
    }
    kprintf("[OOM ] uccisione PID %d per liberare memoria\n", victim);
    process_destroy_deferred(p, -1);
    process_put(p);
    return true;
}

/* === Shutdown (ACPI S5, impianto 1.0) ================================== */

#define ACPI_PM1_SCI_EN   0x0001u
#define ACPI_SLP_EN       (1u << 13)
#define ACPI_SCI_SPINS    1000000

static int32_t sys_shutdown(isr_regs_t *regs UNUSED)
{
    kprintf("[SYS ] Shutdown richiesto. Spegnimento...\n");
    __asm__ volatile ("cli");

    const acpi_pm_info_t *pm = acpi_pm();
    if (pm != NULL && pm->present && pm->pm1a_cnt != 0)
    {
        /* Entra in modalita' ACPI se il firmware espone lo SMI command
         * (serve su PIIX4 come l'Armada E500 in modalita' legacy);
         * inerte se ACPI e' gia' attivo. Bounded: non puo' bloccarsi. */
        if (pm->smi_cmd != 0 && pm->acpi_enable != 0)
        {
            outb((uint16_t)pm->smi_cmd, pm->acpi_enable);
            for (int i = 0; i < ACPI_SCI_SPINS; i++)
            {
                if (inw((uint16_t)pm->pm1a_cnt) & ACPI_PM1_SCI_EN)
                {
                    break;
                }
                io_wait();
            }
        }

        uint8_t ta = pm->s5_found ? pm->slp_typ_a : 0;
        uint8_t tb = pm->s5_found ? pm->slp_typ_b : 0;
        outw((uint16_t)pm->pm1a_cnt,
             (uint16_t)(((ta & 0x07u) << 10) | ACPI_SLP_EN));
        if (pm->pm1b_cnt != 0)
        {
            outw((uint16_t)pm->pm1b_cnt,
                 (uint16_t)(((tb & 0x07u) << 10) | ACPI_SLP_EN));
        }

        /* NON cadere nelle porte emulatore: su PIIX4 il PM block sta a
         * 0x600 e sovrascriverle mid-transition fa crashare l'Armada
         * (lezione 1.0). Con un PM1_CNT reale, il peggio qui e' un halt
         * pulito, mai un crash. */
        for (;;)
        {
            __asm__ volatile ("cli; hlt");
        }
    }

    /* Nessun FADT usabile: emulatore senza ACPI. Le porte note sono
     * l'unica leva e non possono collidere con un PM block che non c'e'. */
    outw(0x604,  0x2000);   /* QEMU >= 2.0, Bochs */
    outw(0xB004, 0x2000);   /* QEMU 1.x           */
    outw(0x4004, 0x3400);   /* VirtualBox         */
    outw(0x600,  0x34);     /* SeaBIOS variants   */
    for (;;)
    {
        __asm__ volatile ("cli; hlt");
    }
    __builtin_unreachable();
}

/* === .mem shared objects =============================================== */

#define MEM_MAX_BLOB (16u * 1024u * 1024u)

static int32_t sys_mem_load(isr_regs_t *regs)
{
    uint32_t size = regs->ecx;
    uint32_t out_init = regs->edx;
    if (regs->ebx == 0 || size == 0 || size > MEM_MAX_BLOB)
    {
        return 0;
    }
    if (!uaccess_check((void *)regs->ebx, size, false))
    {
        return 0;
    }
    if (out_init != 0 && !uaccess_check((void *)out_init, 4, true))
    {
        return 0;
    }

    process_t *proc = process_current();
    if (proc == NULL || proc->pid == 0)
    {
        return 0;
    }

    /* Snapshot in kernel: il loader legge l'ELF mentre e' attivo il PD
     * del figlio, dove il buffer utente non e' mappato. */
    void *kbuf = kmalloc(size);
    if (kbuf == NULL)
    {
        return 0;
    }
    if (!copy_from_user(kbuf, (void *)regs->ebx, size))
    {
        kfree(kbuf);                    /* race di unmap: fallisci pulito */
        return 0;
    }

    elf_shared_result_t res = elf_load_shared(proc, kbuf, size);
    kfree(kbuf);
    if (!res.success)
    {
        return 0;
    }

    if (out_init != 0)
    {
        uint32_t init_abs = res.init_offset
                          ? res.base + res.init_offset : 0;
        uaccess_put_u32(out_init, init_abs);
    }
    return (int32_t)(res.base + res.exports_offset);
}

/* === Live CD ramdisk =================================================== */

static int32_t sys_live_query(isr_regs_t *regs UNUSED)
{
    return (int32_t)bootfs_live_blob_size_bytes();
}

static int32_t sys_live_read(isr_regs_t *regs)
{
    uint32_t lba   = regs->ebx;
    uint32_t count = regs->ecx;
    uint32_t ubuf  = regs->edx;

    const void *blob = bootfs_live_blob_ptr();
    uint32_t    size = bootfs_live_blob_size_bytes();
    if (blob == NULL || size == 0 || count == 0 || count > 1024)
    {
        return -1;
    }

    uint32_t needed = count * 512u;
    uint64_t offset = (uint64_t)lba * 512u;
    if (offset + needed > size)
    {
        return -1;
    }
    if (!copy_to_user((void *)ubuf, (const uint8_t *)blob + offset, needed))
    {
        return -1;
    }
    return 0;
}

/* === Non ancora implementate (arrivano in 1.1.3/1.1.4) ================= */
/* Ritornano un errore pulito, MAI panic (D3): l'userspace le vede come
 * "non disponibili", esattamente come un servizio non ancora avviato. */
static int32_t sys_unimplemented(isr_regs_t *regs UNUSED)
{
    return -1;
}

/* === Dispatcher ======================================================== */

static void syscall_dispatch(isr_regs_t *regs)
{
    uint32_t num = regs->eax;
    if (num >= SYSCALL_MAX || s_table[num] == NULL)
    {
        regs->eax = (uint32_t)(-1);
        return;
    }
    regs->eax = (uint32_t)s_table[num](regs);
}

void syscall_register(uint32_t num, syscall_handler_t handler)
{
    if (num < SYSCALL_MAX)
    {
        s_table[num] = handler;
    }
}

void syscall_init(void)
{
    for (uint32_t i = 0; i < SYSCALL_MAX; i++)
    {
        s_table[i] = sys_unimplemented;
    }

    syscall_register(SYS_EXIT,           sys_exit);
    syscall_register(SYS_YIELD,          sys_yield);
    syscall_register(SYS_GETPID,         sys_getpid);
    syscall_register(SYS_PROC_STATUS,    sys_proc_status);
    syscall_register(SYS_GET_PRIVILEGES, sys_get_privileges);
    syscall_register(SYS_SET_PRIVILEGES, sys_set_privileges);
    syscall_register(SYS_THREAD_EXIT,    sys_thread_exit);
    syscall_register(SYS_SET_PRIORITY,   sys_set_priority);

    syscall_register(SYS_PORT_CREATE,    sys_port_create);
    syscall_register(SYS_PORT_DESTROY,   sys_port_destroy);
    syscall_register(SYS_SEND,           sys_send);
    syscall_register(SYS_RECEIVE,        sys_receive);
    syscall_register(SYS_RECEIVE_NOWAIT, sys_receive_nowait);
    syscall_register(SYS_REPLY,          sys_reply);
    syscall_register(SYS_POST,           sys_post);
    syscall_register(SYS_PORT_GEN,       sys_port_gen);
    syscall_register(SYS_POST_CK,        sys_post_ck);
    syscall_register(SYS_WATCH_PORT,     sys_watch_port);
    syscall_register(SYS_PANIC_TEST,     sys_panic_test);
    syscall_register(SYS_SET_PANIC_FB,   sys_set_panic_fb);
    syscall_register(SYS_NOTIFY,         sys_notify);
    syscall_register(SYS_WAIT_NOTIFY,    sys_wait_notify);

    syscall_register(SYS_MMAP,           sys_mmap);
    syscall_register(SYS_MUNMAP,         sys_munmap);
    syscall_register(SYS_MEMINFO,        sys_meminfo);
    syscall_register(SYS_TASK_SNAPSHOT,  sys_task_snapshot);
    syscall_register(SYS_PROC_SET_PRIORITY, sys_proc_set_priority);

    syscall_register(SYS_NANOSLEEP,      sys_nanosleep);
    syscall_register(SYS_CLOCK_GETTIME,  sys_clock_gettime);
    syscall_register(SYS_GETVERSION,     sys_getversion);
    syscall_register(SYS_FUTEX,          sys_futex);

    syscall_register(SYS_REG_REGISTER,   sys_reg_register);
    syscall_register(SYS_REG_UNREGISTER, sys_reg_unregister);
    syscall_register(SYS_REG_FIND,       sys_reg_find);
    syscall_register(SYS_REG_WAIT,       sys_reg_wait);

    syscall_register(SYS_DEBUG_PRINT,    sys_debug_print);

    syscall_register(SYS_WAIT,           sys_wait);
    syscall_register(SYS_KILL,           sys_kill);
    syscall_register(SYS_THREAD_CREATE,  sys_thread_create);
    syscall_register(SYS_BRK,            sys_brk);
    syscall_register(SYS_SHM_CREATE,     sys_shm_create);
    syscall_register(SYS_SHM_MAP,        sys_shm_map);
    syscall_register(SYS_SHM_UNMAP,      sys_shm_unmap);
    syscall_register(SYS_EVENT_CREATE,   sys_event_create);
    syscall_register(SYS_EVENT_SETCLEAR, sys_event_setclear);
    syscall_register(SYS_EVENT_WAIT,     sys_event_wait);
    syscall_register(SYS_EVENT_GETFLAGS, sys_event_getflags);
    syscall_register(SYS_GETTIME,        sys_gettime);
    syscall_register(SYS_CLOCK_US,       sys_clock_us);
    syscall_register(SYS_SLEEP_US,       sys_sleep_us);
    syscall_register(SYS_INTR_DELIVERY_MODE, sys_intr_delivery_mode);
    syscall_register(SYS_RANDOM,         sys_random);
    syscall_register(SYS_SPAWN_DATA,     sys_spawn_data);
    syscall_register(SYS_GET_HOME_DIR,   sys_get_home_dir);
    syscall_register(SYS_REBOOT,         sys_reboot);
    syscall_register(SYS_GET_STARTUP,    sys_get_startup);
    syscall_register(SYS_GET_MODULE_FLAGS, sys_get_module_flags);
    syscall_register(SYS_SHUTDOWN,       sys_shutdown);
    syscall_register(SYS_MEM_LOAD,       sys_mem_load);
    syscall_register(SYS_LIVE_QUERY,     sys_live_query);
    syscall_register(SYS_LIVE_READ,      sys_live_read);

    driver_syscalls_init();

    /* Gate userspace: int 0x80 accessibile da ring 3 (DPL 3). */
    extern void syscall_stub(void);
    idt_set_gate(SYSCALL_INT, (uint32_t)syscall_stub,
                 GDT_SEL_KCODE, IDT_FLAG_INT_USER);
    isr_register_syscall(syscall_dispatch);

    kprintf("[SYS ] %u syscall registrate (ABI 1:1 con 1.0).\n",
            SYSCALL_MAX);
}
