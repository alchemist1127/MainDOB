#ifndef MAINDOB_BOOT_INFO_H
#define MAINDOB_BOOT_INFO_H

#include "lib/types.h"

/* Snapshot kernel-owned dei dati del bootloader. Copiato SUBITO in
 * strutture nostre: le strutture GRUB vivono in memoria fisica bassa che
 * il PMM potrebbe riusare — mai tenerne puntatori vivi oltre il boot. */

#define BOOT_MMAP_MAX       32u
#define BOOT_MODULES_MAX    16u

typedef struct
{
    uint64_t base;
    uint64_t length;
    uint32_t type;
} boot_mmap_entry_t;

#define BOOT_MODULE_TAG_MAX 32u

typedef struct
{
    uint32_t phys_start;
    uint32_t phys_end;
    char     tag[BOOT_MODULE_TAG_MAX];  /* stringa GRUB (es. "live_fs")   */
} boot_module_t;

typedef struct
{
    uint32_t          mmap_count;
    boot_mmap_entry_t mmap[BOOT_MMAP_MAX];
    uint32_t          module_count;
    boot_module_t     modules[BOOT_MODULES_MAX];
    uint32_t          total_memory_kb;
} boot_info_t;

extern boot_info_t g_boot_info;

void boot_info_init(uint32_t magic, uint32_t mbi_phys);
const boot_module_t *boot_info_find_module(const char *tag);

#endif
