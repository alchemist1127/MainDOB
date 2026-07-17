/* MainDOB Filesystem Operations — registry lookup.
 *
 * The registered table is a small NULL-terminated array of fs_ops_t
 * pointers. Adding a new filesystem means dropping a new entry here
 * and shipping its <name>_ops.c. */

#include <string.h>
#include <dob/fs_ops.h>
#include <dob/partition.h>   /* MBR_TYPE_FAT32_LBA, etc. */

static const fs_ops_t *registry[] =
{
    &fat32_ops,
    &exfat_ops,
    NULL,
};

const fs_ops_t *
fs_ops_for_mbr_type(uint8_t mbr_type)
{
    for (int i = 0; registry[i]; i++)
        if (registry[i]->mbr_type == mbr_type)
            return registry[i];
    /* FAT32 has two MBR type bytes (0x0B CHS, 0x0C LBA). The fat32_ops
     * declares 0x0C as its canonical "format me as this"; treat 0x0B
     * as the same filesystem for detection / probe purposes. */
    if (mbr_type == MBR_TYPE_FAT32_CHS)
        return &fat32_ops;
    return NULL;
}

const fs_ops_t *
fs_ops_for_name(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; registry[i]; i++)
        if (strcmp(registry[i]->name, name) == 0)
            return registry[i];
    return NULL;
}
