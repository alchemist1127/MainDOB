#include "arch/x86/fpu.h"
#include "arch/x86/cpu.h"
#include "lib/string.h"
#include "console/console.h"
#include "kernel.h"

#define CR0_MP  (1u << 1)
#define CR0_EM  (1u << 2)
#define CR0_TS  (1u << 3)
#define CR0_NE  (1u << 5)
#define CR4_OSFXSR      (1u << 9)
#define CR4_OSXMMEXCPT  (1u << 10)
#define CPUID_EDX_FXSR  (1u << 24)
#define CPUID_EDX_SSE   (1u << 25)
#define MXCSR_DEFAULT   0x1F80u

/* Immagine FXSAVE canonica, catturata UNA volta con un fxsave vero
 * (mai byte a mano: cosi' include l'MXCSR_MASK reale della CPU ed e'
 * garantita valida per fxrstor). Storage statico: aligned(16) onorato. */
static uint8_t s_template[FPU_AREA_SIZE]
    __attribute__((aligned(FPU_AREA_ALIGN)));
static bool s_template_ready;

/* === Verbi ============================================================== */

static bool cpu_supports_fxsr_sse(void)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(1));
    return (edx & CPUID_EDX_FXSR) && (edx & CPUID_EDX_SSE);
}

static void enable_in_control_registers(void)
{
    /* Ordine: PRIMA CR0/CR4, POI fninit/ldmxcsr — fninit con EM=1
     * farebbe fault, e MXCSR non esiste finche' OSFXSR e' spento. */
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(CR0_EM | CR0_TS);          /* FPU vera, eager                */
    cr0 |=  (CR0_MP | CR0_NE);          /* #MF nativo, non IRQ13 legacy   */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");

    uint32_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

static void reset_to_clean_baseline(void)
{
    __asm__ volatile ("fninit");
    uint32_t mxcsr = MXCSR_DEFAULT;
    __asm__ volatile ("ldmxcsr %0" : : "m"(mxcsr));
}

static void capture_canonical_template(void)
{
    if (!s_template_ready)
    {
        memset(s_template, 0, sizeof(s_template));
        fpu_save(s_template);
        s_template_ready = true;
    }
}

/* === Orchestratore ====================================================== */

void fpu_init_this_cpu(void)
{
    if (!cpu_supports_fxsr_sse())
    {
        kpanic("FPU: la CPU non ha FXSAVE/SSE (minimo: Pentium III)");
    }

    enable_in_control_registers();
    reset_to_clean_baseline();
    capture_canonical_template();

    kprintf("[FPU ] FPU/SSE eager attivi (fxsave/fxrstor).\n");
}

void fpu_seed_thread_area(void *area)
{
    /* kcalloc lascia l'area tutta-zero: e' un'immagine FXSAVE "valida"
     * ma PERICOLOSA (tutte le eccezioni FP smascherate). Il seed con il
     * template canonico DEVE avvenire prima del primo dispatch. */
    memcpy(area, s_template, FPU_AREA_SIZE);
}
