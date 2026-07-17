/* Rilevamento feature CPU — probe CPUID di boot. Rileva CPUID (flip di
 * EFLAGS.21; kpanic se assente), poi le foglie standard 0/1/6 ed estese
 * 0x80000000..07 per vendor, family/model, feature bit, TSC invariante,
 * ARAT e brand string; kpanic se manca una feature obbligatoria.
 * Risultato in un cpu_info_t statico, sola lettura via cpu_info()/cpu_has().
 *
 * Osservato dal kernel 1.0 (kernel/arch/x86/cpu_features.c): impianto
 * gia' corretto e specifico per l'hardware D1 (la resurrezione del LAPIC
 * BIOS-disabled e' scritta apposta per il Compaq Armada E500), quindi
 * portato senza modifiche di sostanza — solo gli include verso le
 * componenti standard 1.1 (kernel.h/console.h al posto di printf.h). */

#include "arch/x86/cpu_features.h"
#include "arch/x86/msr.h"
#include "arch/x86/cpu.h"
#include "kernel.h"
#include "console/console.h"
#include "lib/string.h"

static cpu_info_t s_info;
static bool       s_initialized = false;

/* Puntatore a linkage esterno usato dall'inline cpu_has(). NULL finche'
 * cpu_features_init() non completa — protegge chi chiama troppo presto. */
const cpu_info_t *_cpu_info_ptr = NULL;

/* === Verbi ============================================================== */

/* Prova a invertire il bit 21 di EFLAGS (ID). Se il bit mantiene il nuovo
 * valore, CPUID e' supportata — funziona fino al 486 che la supporta e
 * riporta correttamente l'assenza su 386/486 iniziali. */
static bool detect_cpuid_support(void)
{
    uint32_t orig, modified;

    __asm__ volatile (
        "pushfl\n\t"
        "pushfl\n\t"
        "xorl $0x00200000,(%%esp)\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %0\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %1\n\t"
        : "=r"(modified), "=r"(orig)
        :
        : "memory");

    return ((orig ^ modified) & 0x00200000u) != 0;
}

static void decode_leaf_0(cpu_info_t *info)
{
    uint32_t eax, ebx, ecx, edx;
    cpu_cpuid(0, &eax, &ebx, &ecx, &edx);

    info->max_basic_leaf = eax;

    *(uint32_t *)&info->vendor[0] = ebx;
    *(uint32_t *)&info->vendor[4] = edx;
    *(uint32_t *)&info->vendor[8] = ecx;
    info->vendor[12] = '\0';
}

static void decode_leaf_1(cpu_info_t *info)
{
    if (info->max_basic_leaf < 1)
    {
        return;
    }

    uint32_t eax, ebx, ecx, edx;
    cpu_cpuid(1, &eax, &ebx, &ecx, &edx);

    /* Family/model/stepping (SDM): stepping[3:0] model[7:4] family[11:8]
     * ext_model[19:16] ext_family[27:20]. Display family somma ext_family
     * quando family==0xF; display model somma ext_model<<4 quando
     * family==0x6 o 0xF. */
    uint32_t base_family = (eax >> 8) & 0xF;
    uint32_t base_model  = (eax >> 4) & 0xF;
    uint32_t ext_family  = (eax >> 20) & 0xFF;
    uint32_t ext_model   = (eax >> 16) & 0xF;

    info->stepping = eax & 0xF;
    info->family = base_family + (base_family == 0x0F ? ext_family : 0);
    info->model  = base_model |
                   ((base_family == 0x06 || base_family == 0x0F) ? (ext_model << 4) : 0);

    if (edx & (1u <<  4)) info->features |= CPUF_TSC;
    if (edx & (1u <<  5)) info->features |= CPUF_MSR;
    if (edx & (1u <<  9)) info->features |= CPUF_APIC;

    if (ecx & (1u << 24)) info->features |= CPUF_TSC_DEADLINE;
    if (ecx & (1u << 21)) info->features |= CPUF_X2APIC;
    if (ecx & (1u << 31)) info->features |= CPUF_HYPERVISOR;
}

static void decode_leaf_6(cpu_info_t *info)
{
    if (info->max_basic_leaf < 6)
    {
        return;
    }

    uint32_t eax, ebx, ecx, edx;
    cpu_cpuid(6, &eax, &ebx, &ecx, &edx);

    /* EAX bit 2: Always Running APIC Timer. Se assente (pre-Nehalem) il
     * LAPIC timer si ferma nei C-state profondi; il floor D1 non entra
     * in C-state aggressivi, quindi l'assenza non e' un problema qui —
     * informa solo la scelta futura di sorgente tick. */
    if (eax & (1u << 2)) info->features |= CPUF_ARAT;
}

static void decode_extended(cpu_info_t *info)
{
    uint32_t eax, ebx, ecx, edx;
    cpu_cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    info->max_ext_leaf = eax;

    if (info->max_ext_leaf >= 0x80000004)
    {
        uint32_t *brand_words = (uint32_t *)info->brand;
        for (uint32_t leaf = 0x80000002, off = 0; leaf <= 0x80000004; leaf++, off += 4)
        {
            cpu_cpuid(leaf, &eax, &ebx, &ecx, &edx);
            brand_words[off + 0] = eax;
            brand_words[off + 1] = ebx;
            brand_words[off + 2] = ecx;
            brand_words[off + 3] = edx;
        }
        info->brand[48] = '\0';
    }
    else
    {
        info->brand[0] = '\0';
    }

    /* TSC invariante: foglia 0x80000007, EDX bit 8 (Nehalem/K8-tardo+). */
    if (info->max_ext_leaf >= 0x80000007)
    {
        cpu_cpuid(0x80000007, &eax, &ebx, &ecx, &edx);
        if (edx & (1u << 8)) info->features |= CPUF_INVARIANT_TSC;
    }
}

static bool brand_contains(const char *brand, const char *needle)
{
    if (!brand[0]) return false;
    for (const char *p = brand; *p; p++)
    {
        const char *a = p, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

/* CPUID.01h ECX[31] e' il segnale primario; alcuni setup (QEMU TCG) lo
 * saltano, quindi due fallback: la foglia vendor hypervisor 0x40000000+
 * e una scansione del brand string (QEMU/VMware/Bochs/"Virtual..."). */
static void detect_hypervisor_extra(cpu_info_t *info)
{
    if (info->features & CPUF_HYPERVISOR) return;

    uint32_t eax, ebx, ecx, edx;
    cpu_cpuid(0x40000000, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x40000000 && (ebx | ecx | edx) != 0)
    {
        info->features |= CPUF_HYPERVISOR;
        return;
    }

    if (brand_contains(info->brand, "QEMU") ||
        brand_contains(info->brand, "Virtual") ||
        brand_contains(info->brand, "VMware") ||
        brand_contains(info->brand, "Bochs"))
    {
        info->features |= CPUF_HYPERVISOR;
    }
}

/* Resurrezione del LAPIC su silicio BIOS-disabled. Alcuni BIOS di laptop
 * pre-ACPI (Compaq Armada E500, coetanei ThinkPad IBM) spediscono il
 * LAPIC hardware-disabilitato via IA32_APIC_BASE.GLOBAL_ENABLE=0 (il
 * loro SO nativo era solo-PIC). Il silicio P6 funziona: e' solo il bit
 * enable dell'MSR ad essere spento, quindi CPUID.01h:EDX.9 legge 0 e il
 * controllo di floor farebbe kpanic a torto. Riprova con una scrittura
 * MSR (serve CPUF_MSR) e ri-sonda CPUID — il bit si aggiorna nello stesso
 * flusso di istruzioni sul P6+. Se davvero assente, il bit resta 0 e il
 * floor fa kpanic legittimamente. */
static void revive_bios_disabled_lapic(cpu_info_t *info)
{
    if ((info->features & CPUF_APIC) || !(info->features & CPUF_MSR))
    {
        return;
    }

    /* Valida rdmsr stessa prima di fidarsi di qualunque lettura: due MSR
     * garantite non-zero su un P6 funzionante. Se leggono 0 rdmsr/wrmsr
     * e' rotta a livello toolchain/runtime e nessun fix su APIC_BASE puo'
     * funzionare. */
    uint64_t tsc     = rdmsr(0x10);   /* IA32_TSC     */
    uint64_t mtrrcap = rdmsr(0xFE);   /* IA32_MTRRCAP */
    kprintf("[CPU ] rdmsr probe: TSC=0x%x_%x MTRRCAP=0x%x_%x\n",
            (uint32_t)(tsc >> 32),     (uint32_t)tsc,
            (uint32_t)(mtrrcap >> 32), (uint32_t)mtrrcap);

    uint64_t apic_base = rdmsr(MSR_IA32_APIC_BASE);
    kprintf("[CPU ] APIC assente in CPUID. IA32_APIC_BASE = 0x%x_%x\n",
            (uint32_t)(apic_base >> 32), (uint32_t)apic_base);

    if (apic_base & APIC_BASE_GLOBAL_ENABLE)
    {
        kprintf("[CPU ] GLOBAL_ENABLE gia' 1 ma CPUID non riporta APIC. "
                "Possibile silicio fuse-disabled.\n");
        return;
    }

    kprintf("[CPU ] GLOBAL_ENABLE era 0, tento scrittura MSR...\n");
    uint64_t new_val = apic_base | APIC_BASE_GLOBAL_ENABLE;
    if ((new_val & APIC_BASE_ADDR_MASK) == 0)
    {
        new_val |= 0xFEE00000u;   /* base di default su ogni P6+         */
    }
    wrmsr(MSR_IA32_APIC_BASE, new_val);

    uint64_t after = rdmsr(MSR_IA32_APIC_BASE);
    kprintf("[CPU ] IA32_APIC_BASE dopo la scrittura = 0x%x_%x\n",
            (uint32_t)(after >> 32), (uint32_t)after);

    uint32_t eax, ebx, ecx, edx;
    cpu_cpuid(1, &eax, &ebx, &ecx, &edx);
    kprintf("[CPU ] CPUID.01h:EDX dopo resurrezione = 0x%x (bit APIC %d)\n",
            edx, (edx >> 9) & 1);

    if (edx & (1u << 9))
    {
        info->features |= CPUF_APIC;
        kprintf("[CPU ] Local APIC era BIOS-disabled, riattivato via "
                "MSR IA32_APIC_BASE.\n");
    }
}

/* === API ================================================================ */

void cpu_features_init(void)
{
    if (s_initialized) return;

    memset(&s_info, 0, sizeof(s_info));

    if (!detect_cpuid_support())
    {
        kpanic("cpu_features: CPUID non supportata (CPU pre-Pentium). "
               "mainDOB richiede hardware Pentium-class o successivo.");
    }
    s_info.features |= CPUF_CPUID;

    decode_leaf_0(&s_info);
    decode_leaf_1(&s_info);
    decode_extended(&s_info);
    decode_leaf_6(&s_info);

    /* Dopo decode_extended: usa il brand string come segnale di fallback. */
    detect_hypervisor_extra(&s_info);

    revive_bios_disabled_lapic(&s_info);

    /* Floor: CPUID+TSC+MSR obbligatori (senza non si legge il tempo, non
     * si scrivono MSR, non si identifica la CPU). APIC opzionale: se
     * assente il tick source resta il PIT — piu' grezzo, mai fatale. */
    const uint32_t mandatory = CPUF_CPUID | CPUF_TSC | CPUF_MSR;
    if ((s_info.features & mandatory) != mandatory)
    {
        kpanic("cpu_features: floor non soddisfatto (serve CPUID+TSC+MSR). "
               "Bitset rilevato = 0x%x", s_info.features);
    }

    if (!(s_info.features & CPUF_APIC))
    {
        kprintf("[CPU ] ATTENZIONE: nessun Local APIC utilizzabile. "
                "Il kernel restera' su PIT/uniprocessore.\n");
    }

    /* NB: qui dentro _cpu_info_ptr non e' ancora pubblicato (vedi sotto),
     * quindi si legge s_info.features direttamente — cpu_has() a questo
     * punto risolverebbe sempre falso (il puntatore e' ancora NULL) a
     * prescindere dai bit gia' scritti, dando un falso allarme anche
     * quando l'hypervisor o il TSC invariante SONO stati rilevati. */
    if (!(s_info.features & CPUF_INVARIANT_TSC) &&
        !(s_info.features & CPUF_HYPERVISOR))
    {
        kprintf("[CPU ] ATTENZIONE: TSC stabile non rilevato (ne' "
                "INVARIANT_TSC ne' HYPERVISOR). Brand=\"%s\". Il clock "
                "puo' derivare; l'aritmetica del riancoraggio resta "
                "comunque sicura.\n",
                s_info.brand[0] ? s_info.brand : "(nessuno)");
    }

    /* Pubblica PRIMA di alzare il flag: cpu_has() non vede mai una
     * lettura spezzata (accademico su x86, ma manteniamo la disciplina
     * in vista dell'SMP). */
    _cpu_info_ptr = &s_info;
    __asm__ volatile ("" ::: "memory");
    s_initialized = true;

    kprintf("[CPU ] Vendor: %s, Family %u Model %u Stepping %u\n",
            s_info.vendor, s_info.family, s_info.model, s_info.stepping);
    if (s_info.brand[0])
    {
        kprintf("[CPU ] Brand:  %s\n", s_info.brand);
    }
    kprintf("[CPU ] Feature opzionali:%s%s%s%s\n",
            cpu_has(CPUF_TSC_DEADLINE)  ? " TSC-deadline" : "",
            cpu_has(CPUF_INVARIANT_TSC) ? " TSC-invariante" : "",
            cpu_has(CPUF_ARAT)          ? " ARAT" : "",
            cpu_has(CPUF_HYPERVISOR)    ? " hypervisor" : "");
}

const cpu_info_t *cpu_info(void)
{
    return s_initialized ? &s_info : NULL;
}
