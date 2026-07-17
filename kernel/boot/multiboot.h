#ifndef MAINDOB_BOOT_MULTIBOOT_H
#define MAINDOB_BOOT_MULTIBOOT_H

#include "lib/types.h"

#define MULTIBOOT_BOOTLOADER_MAGIC  0x2BADB002u

#define MB_FLAG_MEM     (1u << 0)
#define MB_FLAG_MMAP    (1u << 6)
#define MB_FLAG_MODS    (1u << 3)

#define MULTIBOOT_MMAP_AVAILABLE    1u

typedef struct __attribute__((packed))
{
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
} multiboot_info_t;

typedef struct __attribute__((packed))
{
    uint32_t size;              /* NON include se stesso                  */
    uint64_t base;
    uint64_t length;
    uint32_t type;
} multiboot_mmap_entry_t;

typedef struct __attribute__((packed))
{
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t string;
    uint32_t reserved;
} multiboot_module_t;

#endif
