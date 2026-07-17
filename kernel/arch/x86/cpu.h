#ifndef MAINDOB_ARCH_CPU_H
#define MAINDOB_ARCH_CPU_H

#include "lib/types.h"

static inline void cpu_cli(void)
{
    __asm__ volatile ("cli");
}

static inline void cpu_sti(void)
{
    __asm__ volatile ("sti");
}

static inline void cpu_halt(void)
{
    __asm__ volatile ("hlt");
}

static inline __attribute__((noreturn)) void cpu_halt_forever(void)
{
    for (;;)
    {
        __asm__ volatile ("cli; hlt");
    }
}

static inline uint32_t irq_save(void)
{
    uint32_t flags;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags)
{
    __asm__ volatile ("push %0; popf" : : "r"(flags) : "memory", "cc");
}

static inline uint32_t cpu_read_cr2(void)
{
    uint32_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline uint32_t cpu_read_cr3(void)
{
    uint32_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void cpu_write_cr3(uint32_t v)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory");
}

static inline void cpu_invlpg(uint32_t vaddr)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
}

static inline uint64_t cpu_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* CPUID a 4 uscite (usato da cpu_features.c per l'enumerazione completa;
 * la 1.0 riscopriva EDX bit 4 in ogni chiamante che serviva il TSC —
 * qui c'e' un solo punto testato, vedi cpu_features.h). */
static inline void cpu_cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                             uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile ("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf));
}

#endif
