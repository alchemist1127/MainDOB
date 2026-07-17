#include "boot/boot_info.h"
#include "boot/multiboot.h"
#include "console/console.h"
#include "kernel.h"
#include "lib/string.h"

boot_info_t g_boot_info;

/* Le strutture GRUB stanno in memoria bassa: sono raggiungibili tramite
 * il direct-map di boot (primi 16 MB fisici a KERNEL_VMA). */
static inline const void *phys_to_boot_virt(uint32_t phys)
{
    return (const void *)(phys + KERNEL_VMA);
}

/* === Verbi ============================================================== */

static void reject_bad_bootloader(uint32_t magic)
{
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
    {
        kpanic("boot: magic multiboot errato (0x%08x)", magic);
    }
}

static void snapshot_memory_map(const multiboot_info_t *mbi)
{
    if ((mbi->flags & MB_FLAG_MMAP) == 0)
    {
        kpanic("boot: il bootloader non fornisce la memory map");
    }

    uint32_t offset = 0;
    while (offset < mbi->mmap_length &&
           g_boot_info.mmap_count < BOOT_MMAP_MAX)
    {
        const multiboot_mmap_entry_t *e =
            (const multiboot_mmap_entry_t *)
                phys_to_boot_virt(mbi->mmap_addr + offset);

        boot_mmap_entry_t *out = &g_boot_info.mmap[g_boot_info.mmap_count++];
        out->base   = e->base;
        out->length = e->length;
        out->type   = e->type;

        if (e->type == MULTIBOOT_MMAP_AVAILABLE)
        {
            g_boot_info.total_memory_kb += (uint32_t)(e->length / 1024u);
        }

        offset += e->size + sizeof(uint32_t);
    }
}

static void snapshot_modules(const multiboot_info_t *mbi)
{
    if ((mbi->flags & MB_FLAG_MODS) == 0 || mbi->mods_count == 0)
    {
        return;
    }

    const multiboot_module_t *mods =
        (const multiboot_module_t *)phys_to_boot_virt(mbi->mods_addr);

    uint32_t count = mbi->mods_count;
    if (count > BOOT_MODULES_MAX)
    {
        count = BOOT_MODULES_MAX;
    }

    for (uint32_t i = 0; i < count; i++)
    {
        boot_module_t *m = &g_boot_info.modules[i];
        m->phys_start = mods[i].mod_start;
        m->phys_end   = mods[i].mod_end;
        m->tag[0]     = '\0';

        /* La stringa del modulo vive in memoria bassa GRUB: va copiata
         * ORA (il PMM ricicla quelle pagine subito dopo). */
        if (mods[i].string != 0)
        {
            const char *tag =
                (const char *)phys_to_boot_virt(mods[i].string);
            strncpy(m->tag, tag, BOOT_MODULE_TAG_MAX - 1);
            m->tag[BOOT_MODULE_TAG_MAX - 1] = '\0';
        }
        g_boot_info.module_count++;
    }
}

static void report_summary(void)
{
    kprintf("[BOOT] Memory map: %u entry, RAM %u MB, %u moduli\n",
            g_boot_info.mmap_count,
            g_boot_info.total_memory_kb / 1024u,
            g_boot_info.module_count);
}

/* === Orchestratore ====================================================== */

void boot_info_init(uint32_t magic, uint32_t mbi_phys)
{
    reject_bad_bootloader(magic);

    const multiboot_info_t *mbi =
        (const multiboot_info_t *)phys_to_boot_virt(mbi_phys);

    snapshot_memory_map(mbi);
    snapshot_modules(mbi);
    report_summary();
}

const boot_module_t *boot_info_find_module(const char *tag)
{
    for (uint32_t i = 0; i < g_boot_info.module_count; i++)
    {
        if (strstr(g_boot_info.modules[i].tag, tag) != NULL)
        {
            return &g_boot_info.modules[i];
        }
    }
    return NULL;
}
