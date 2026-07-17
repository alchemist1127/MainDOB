#ifndef MAINDOB_ARCH_CPU_FEATURES_H
#define MAINDOB_ARCH_CPU_FEATURES_H

#include "lib/types.h"

/* Rilevamento feature CPU — sondato UNA volta al boot da
 * cpu_features_init(). Ogni lettura successiva passa dagli accessor
 * inline sotto: un solo MOV da un globale statico, nessun branch sul
 * percorso caldo (D9: componente standard, un solo posto testato).
 *
 * Floor mainDOB: Pentium (P5, 1993) o successivo — sotto quella soglia
 * mancano CPUID/TSC/MSR/LAPIC e cpu_features_init() fa kpanic. Coerente
 * col target D1 (Compaq Armada E500 = Pentium III, Acer Extensa 5220 =
 * Core-class): nessun hardware realistico e' sotto floor.
 *
 * Le feature opzionali (TSC-deadline, TSC invariante, ARAT, x2APIC,
 * hypervisor) sono rilevate e registrate ma la loro assenza non e'
 * fatale: chi le usa (lapic.c, tsc.c) sceglie la modalita' migliore
 * disponibile. */

#define CPUF_CPUID          (1u << 0)   /* istruzione CPUID disponibile   */
#define CPUF_TSC             (1u << 1)   /* CPUID.01h:EDX.bit 4            */
#define CPUF_MSR             (1u << 2)   /* CPUID.01h:EDX.bit 5            */
#define CPUF_APIC            (1u << 3)   /* CPUID.01h:EDX.bit 9            */

#define CPUF_TSC_DEADLINE    (1u << 4)   /* CPUID.01h:ECX.bit 24           */
#define CPUF_INVARIANT_TSC   (1u << 5)   /* CPUID.80000007h:EDX.bit 8      */
#define CPUF_ARAT            (1u << 6)   /* CPUID.06h:EAX.bit 2            */
#define CPUF_HYPERVISOR      (1u << 7)   /* CPUID.01h:ECX.bit 31           */
#define CPUF_X2APIC          (1u << 8)   /* CPUID.01h:ECX.bit 21           */

typedef struct
{
    uint32_t features;
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    uint32_t max_basic_leaf;
    uint32_t max_ext_leaf;
    char     vendor[13];     /* "GenuineIntel" ecc., NUL-terminata        */
    char     brand[49];      /* stringa marketing, "" se non disponibile  */
} cpu_info_t;

/* Sonda e valida il floor. kpanic su assenza non recuperabile (niente
 * CPUID/TSC/MSR). Idempotente. */
void cpu_features_init(void);

/* Accessor sola lettura. NULL se cpu_features_init non e' ancora girata. */
const cpu_info_t *cpu_info(void);

extern const cpu_info_t *_cpu_info_ptr;

static inline bool cpu_has(uint32_t feat)
{
    return _cpu_info_ptr && (_cpu_info_ptr->features & feat) != 0;
}

#endif
