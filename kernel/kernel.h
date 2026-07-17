#ifndef MAINDOB_KERNEL_H
#define MAINDOB_KERNEL_H

#include "lib/types.h"

/* === Identità e versione ============================================== */
#define MAINDOB_NAME            "MainDOB"
#define MAINDOB_CODENAME        "EDOLIN"   /* linea 1.1 (la 1.0 e' EDIOB) */

/* Schema a 5 componenti, ABI 1.0 (SYS_GETVERSION riporta ESATTAMENTE
 * questi cinque uint32, in quest'ordine). REGOLA: incrementare
 * MAINDOB_VERSION_BUILD di 1 prima di OGNI compilazione. */
#define MAINDOB_VERSION_MAJOR    1  /* generazione: riscrittura 1.1       */
#define MAINDOB_VERSION_MINOR    1
#define MAINDOB_VERSION_PATCH    0  /* milestone di roadmap               */
#define MAINDOB_VERSION_REVISION 1
#define MAINDOB_VERSION_BUILD    140

#define _MAINDOB_TOSTR2(x) #x
#define _MAINDOB_TOSTR(x)  _MAINDOB_TOSTR2(x)
#define MAINDOB_VERSION_STRING          \
    _MAINDOB_TOSTR(MAINDOB_VERSION_MAJOR)    "." \
    _MAINDOB_TOSTR(MAINDOB_VERSION_MINOR)    "." \
    _MAINDOB_TOSTR(MAINDOB_VERSION_PATCH)    "." \
    _MAINDOB_TOSTR(MAINDOB_VERSION_REVISION) "." \
    _MAINDOB_TOSTR(MAINDOB_VERSION_BUILD)
#define MAINDOB_COPYRIGHT       "Copyright (C) Dob Systems & Technologies"

/* === Configurazione compile-time ====================================== */
#define KERNEL_VMA              0xC0000000u
#define KERNEL_PHYS_BASE        0x00100000u
#define BOOT_DIRECT_MAP_MB      16u          /* fisico mappato dal boot    */

/* === Diagnostica ======================================================= */
/* kpanic: SOLO per violazioni di invarianti interne (vedi DESIGN.md D3). */
void kpanic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

#define KASSERT(cond)                                                     \
    do                                                                    \
    {                                                                     \
        if (!(cond))                                                      \
        {                                                                 \
            kpanic("ASSERT fallita: %s (%s:%d)", #cond, __FILE__, __LINE__); \
        }                                                                 \
    } while (0)

#endif
