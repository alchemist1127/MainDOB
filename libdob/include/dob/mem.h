#ifndef MAINDOB_LIBDOB_MEM_H
#define MAINDOB_LIBDOB_MEM_H

/*
 * .mem — userspace shared objects for MainDOB
 *
 * Load a PIC ELF shared object into the caller's address space. The
 * .mem fuses with the process: code and data become indistinguishable
 * from anything else the process allocated. No unload — it lives and
 * dies with the process. A bug in the .mem crashes the process the
 * same way a static-linked bug would.
 *
 * A .mem must export:
 *
 *   extern <T> __mem_exports;          // the API struct (mandatory)
 *   void        __mem_init(void);      // optional, run once after load
 *
 * Calls through a .mem export are a single `call` — no IPC, no
 * address-space switch.
 *
 * Usage:
 *
 *   #include <dob/mem.h>
 *   #include "compression_api.h"   // declares compression_api_t
 *
 *   void *blob; size_t sz;
 *   read_whole_file("compression.mem", &blob, &sz);
 *
 *   compression_api_t *api = dob_mem_load(blob, sz);
 *   free(blob);                    // kernel already copied it out
 *   if (!api) handle_error();
 *
 *   api->compress(input, input_len, output, &output_len);
 */

#include <sys/types.h>

/* Load a .mem blob.  `data` is a buffer of `size` bytes.
 * Returns a pointer to the .mem's __mem_exports struct on success, or
 * NULL on failure (bad ELF, out of VA space, unsupported relocation,
 * missing __mem_exports).  If the .mem exposed an __mem_init hook, it
 * has already been called by the time this function returns.
 *
 * Loading the same file twice yields two independent copies at two
 * different addresses.  There is no deduplication. */
void *dob_mem_load(const void *data, size_t size);

#endif /* MAINDOB_LIBDOB_MEM_H */
