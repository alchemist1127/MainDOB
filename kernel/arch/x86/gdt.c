#include "arch/x86/gdt.h"
#include "lib/string.h"
#include "console/console.h"
#include "proc/percpu.h"   /* il TSS per-CPU vive in this_cpu()->tss    */

/* Un GDT per CPU. Ogni CPU ha il PROPRIO GDT affinche' (a) il suo
 * descrittore TSS sia privato — caricare TR non lotta con un'altra CPU
 * sul bit "busy" — e (b) il suo descrittore GDT_SEL_PERCPU punti al
 * proprio blocco per-CPU, dando un fast path %gs corretto. Le voci
 * codice/dati condivise sono identiche su tutte. */

typedef struct __attribute__((packed))
{
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} gdt_entry_t;

typedef struct __attribute__((packed))
{
    uint16_t limit;
    uint32_t base;
} gdt_ptr_t;

#define GDT_ENTRIES 7   /* null, kcode, kdata, ucode, udata, tss, percpu */

static gdt_entry_t s_gdt[MAX_CPUS][GDT_ENTRIES];
static gdt_ptr_t   s_gdt_ptr[MAX_CPUS];

/* === Verbi ============================================================== */

static void describe_segment(uint32_t slot, int index, uint32_t base,
                             uint32_t limit, uint8_t access,
                             uint8_t granularity)
{
    s_gdt[slot][index].base_low    = (uint16_t)(base & 0xFFFF);
    s_gdt[slot][index].base_mid    = (uint8_t)((base >> 16) & 0xFF);
    s_gdt[slot][index].base_high   = (uint8_t)((base >> 24) & 0xFF);
    s_gdt[slot][index].limit_low   = (uint16_t)(limit & 0xFFFF);
    s_gdt[slot][index].granularity = (uint8_t)(((limit >> 16) & 0x0F) | granularity);
    s_gdt[slot][index].access      = access;
}

static void describe_flat_segments(uint32_t slot)
{
    describe_segment(slot, 0, 0, 0x00000000, 0x00, 0x00);   /* null       */
    describe_segment(slot, 1, 0, 0x000FFFFF, 0x9A, 0xC0);   /* kernel code*/
    describe_segment(slot, 2, 0, 0x000FFFFF, 0x92, 0xC0);   /* kernel data*/
    describe_segment(slot, 3, 0, 0x000FFFFF, 0xFA, 0xC0);   /* user code  */
    describe_segment(slot, 4, 0, 0x000FFFFF, 0xF2, 0xC0);   /* user data  */
}

/* Voce 5: TSS DI QUESTA CPU. Voce 6 (SMP): base %gs per-CPU di questa
 * CPU — con GS = GDT_SEL_PERCPU, [gs:0] legge g_cpus[slot].self ==
 * &g_cpus[slot]. Granularita' a byte, 32 bit, limite = un blocco. */
static void describe_tss_and_percpu(uint32_t slot)
{
    tss_entry_t *tss = &g_cpus[slot].tss;
    memset(tss, 0, sizeof(tss_entry_t));
    tss->ss0 = GDT_SEL_KDATA;

    uint32_t tss_base  = (uint32_t)tss;
    uint32_t tss_limit = sizeof(tss_entry_t) - 1;
    describe_segment(slot, 5, tss_base, tss_limit, 0x89, 0x00);

#ifdef MAINDOB_SMP
    g_cpus[slot].self = &g_cpus[slot];
    describe_segment(slot, 6, (uint32_t)&g_cpus[slot],
                     sizeof(percpu_t) - 1, 0x92, 0x40);
#endif
}

static void build_gdt(uint32_t slot)
{
    describe_flat_segments(slot);
    describe_tss_and_percpu(slot);

    s_gdt_ptr[slot].limit = sizeof(s_gdt[slot]) - 1;
    s_gdt_ptr[slot].base  = (uint32_t)&s_gdt[slot];
}

static void load_gdt_and_flush_segments(uint32_t slot)
{
    gdt_ptr_t *ptr = &s_gdt_ptr[slot];
    __asm__ volatile (
        "lgdt %0            \n"
        "mov  $0x10, %%ax   \n"
        "mov  %%ax, %%ds    \n"
        "mov  %%ax, %%es    \n"
        "mov  %%ax, %%fs    \n"
        "mov  %%ax, %%gs    \n"
        "mov  %%ax, %%ss    \n"
        "ljmp $0x08, $1f    \n"
        "1:                 \n"
        : : "m"(*ptr) : "eax", "memory");
}

static void load_task_register(void)
{
    uint16_t sel = GDT_SEL_TSS;
    __asm__ volatile ("ltr %0" : : "r"(sel));
}

/* === Orchestratori ======================================================= */

void gdt_init(void)
{
    /* La boot CPU e' sempre slot 0. build_gdt/load usano lo slot
     * esplicito, non this_cpu(): girano sulla BSP ma this_cpu() (build
     * SMP) consulta gia' %gs, che pero' resta il selettore di boot fino
     * al caricamento di GDT_SEL_PERCPU qui sotto. */
    build_gdt(0);
    load_gdt_and_flush_segments(0);
    load_task_register();

#ifdef MAINDOB_SMP
    /* Carica il selettore %gs per-CPU sulla boot CPU e lo MANTIENE: da
     * qui e' il GS di regime del kernel, quindi this_cpu() prende il
     * fast path %gs in ogni contesto kernel. Gli stub di ingresso
     * interrupt ricaricano GDT_SEL_PERCPU all'entrata e il context
     * switch non tocca mai i registri di segmento, quindi GS resta
     * per-CPU per tutta l'esecuzione kernel; il ring3 gira col GS
     * utente e gli stub lo ripristinano all'iret. */
    {
        uint16_t pc_sel = GDT_SEL_PERCPU;
        __asm__ volatile ("movw %0, %%gs" :: "r"(pc_sel));

        percpu_t *via_gs;
        __asm__ volatile ("movl %%gs:0, %0" : "=r"(via_gs));
        if (via_gs == &g_cpus[0])
        {
            kprintf("[GDT ] Self-test base %%gs: OK (gs:0 -> blocco cpu0).\n");
        }
        else
        {
            kprintf("[GDT ] Self-test base %%gs: FALLBACK (gs:0=%p, atteso %p).\n",
                    (void *)via_gs, (void *)&g_cpus[0]);
        }
    }
#endif

    kprintf("[GDT ] Segmenti flat + TSS caricati (CPU 0).\n");
}

#ifdef MAINDOB_SMP
void gdt_ap_init(uint32_t slot)
{
    /* Costruisce e carica il GDT/TSS/base-%gs di QUESTA AP. L'AP arriva
     * qui sul GDT flat del trampolino (codice/dati piatti identici a
     * 0x08/0x10), quindi ricaricare sul proprio GDT e' trasparente; il
     * guadagno e' il descrittore TSS privato e la base %gs per-CPU. */
    build_gdt(slot);
    load_gdt_and_flush_segments(slot);
    load_task_register();

    uint16_t pc_sel = GDT_SEL_PERCPU;
    __asm__ volatile ("movw %0, %%gs" :: "r"(pc_sel));
}
#endif

void gdt_set_kernel_stack(uint32_t esp0)
{
    this_cpu()->tss.esp0 = esp0;
}
