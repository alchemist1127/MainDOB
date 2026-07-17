#include "boot/startup.h"
#include "boot/bootfs.h"
#include "boot/boot_info.h"
#include "lib/string.h"
#include "console/console.h"

/* Carica /SYSTEM/CONFIG/Startup_modules (impianto 1.0): via ramdisk
 * "live_fs" se GRUB lo fornisce; il percorso da disco ATA arriva col
 * porting del backend disk (1.1.0.0.5). Il testo resta in un buffer
 * statico e viene servito a userspace da SYS_GET_STARTUP. */

#define STARTUP_PATH "/SYSTEM/CONFIG/Startup_modules"

static char     s_text[4096];
static uint32_t s_len;

void boot_startup_init(void)
{
    s_text[0] = '\0';
    s_len     = 0;

    const boot_module_t *live = boot_info_find_module("live_fs");
    bool fs_ready;

    if (live != NULL)
    {
        kprintf("[BOOT] avvio live: filesystem in RAM.\n");
        fs_ready = bootfs_init_ramdisk(live->phys_start,
                                       live->phys_end - live->phys_start);
    }
    else
    {
        kprintf("[BOOT] inizializzazione disco di boot...\n");
        fs_ready = bootfs_init();       /* backend ATA PIO               */
    }

    if (!fs_ready)
    {
        kprintf("[BOOT] nessun filesystem utilizzabile.\n");
        return;
    }

    uint32_t size = 0;
    const void *data = bootfs_read_file(STARTUP_PATH, &size);
    if (data == NULL)
    {
        kprintf("[BOOT] %s assente\n", STARTUP_PATH);
        return;
    }

    if (size >= sizeof(s_text))
    {
        size = sizeof(s_text) - 1;
    }
    memcpy(s_text, data, size);
    s_text[size] = '\0';
    s_len = size;

    kprintf("[BOOT] Startup_modules: %u byte.\n", s_len);
}

const char *boot_get_startup_text(void)
{
    return s_text;
}

uint32_t boot_get_startup_len(void)
{
    return s_len;
}
