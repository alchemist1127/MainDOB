#ifndef MAINDOB_BOOT_STARTUP_H
#define MAINDOB_BOOT_STARTUP_H

#include "lib/types.h"

void        boot_startup_init(void);
const char *boot_get_startup_text(void);
uint32_t    boot_get_startup_len(void);

#endif
