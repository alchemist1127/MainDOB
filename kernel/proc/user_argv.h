#ifndef MAINDOB_PROC_USER_ARGV_H
#define MAINDOB_PROC_USER_ARGV_H

#include "lib/types.h"

#define USER_ARGV_MAX_BLOB_BYTES 4096u

/* Costruisce sul TOP dello stack utente il frame che il crt0 si
 * aspetta (layout 1:1 col 1.0, verificato sul sorgente):
 *
 *   [string pool]                <- ancorato a user_stack_top
 *   [padding a 4]
 *   argv[argc] = NULL ... argv[0]
 *   argc                         <- nuovo ESP (ritornato)
 *
 * DEVE essere chiamata con il PD del processo TARGET attivo.
 * Ritorna il nuovo ESP, o 0 su layout impossibile. */
uint32_t user_argv_setup(uint32_t user_stack_top, uint32_t argc,
                         const char *argv_strings, uint32_t argv_bytes);

#endif
