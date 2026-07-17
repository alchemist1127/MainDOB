#ifndef MAINDOB_ARCH_MSR_H
#define MAINDOB_ARCH_MSR_H

#include "lib/types.h"

/* Model-Specific Register: RDMSR/WRMSR piu' gli indici usati dal
 * sottosistema SMP (Local APIC base, TSC, TSC-deadline, x2APIC).
 *
 * MSR e' segnalato da CPUID.01h:EDX.bit 5 (CPUF_MSR in cpu_features.h);
 * ogni chiamante deve girare DOPO cpu_features_init(). Un indice ECX
 * sconosciuto genera #GP: solo gli indici nominati sotto (o uno
 * validato altrove contro vendor/family) vanno usati. */

#define MSR_IA32_APIC_BASE      0x0000001Bu  /* base LAPIC + bit enable  */
#define MSR_IA32_TSC            0x00000010u
#define MSR_IA32_TSC_DEADLINE   0x000006E0u   /* target TSC-deadline     */

/* MSR x2APIC (attivi quando IA32_APIC_BASE.EXTD e' settato). Un registro
 * MMIO xAPIC all'offset `off` mappa su MSR 0x800 + (off>>4), ECCETTO
 * l'ICR che in x2APIC collassa la coppia ICR_HI/ICR_LO in un solo MSR
 * a 64 bit. */
#define MSR_X2APIC_APICID       0x00000802u   /* id LAPIC (sola lettura) */
#define MSR_X2APIC_ICR          0x00000830u   /* interrupt command 64bit */

/* Layout IA32_APIC_BASE */
#define APIC_BASE_BSP           (1u << 8)     /* questa CPU e' la BSP    */
#define APIC_BASE_X2APIC        (1u << 10)
#define APIC_BASE_GLOBAL_ENABLE (1u << 11)
#define APIC_BASE_ADDR_MASK     0xFFFFF000u   /* bit 12..31 = base fisica*/

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

#endif
