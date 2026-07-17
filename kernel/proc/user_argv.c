#include "proc/user_argv.h"
#include "proc/process.h"
#include "lib/string.h"

/* === Verbi ============================================================== */

static uint32_t align_down_4(uint32_t v)
{
    return v & ~3u;
}

static bool footprint_fits(uint32_t argc, uint32_t argv_bytes)
{
    if (argc > 1024)
    {
        return false;                   /* assurdo per costruzione        */
    }
    uint32_t ptrs = (argc + 1) * 4;
    return 4 + ptrs + argv_bytes + 3 <= USER_ARGV_MAX_BLOB_BYTES;
}

static bool emit_pointers(uint32_t ptrs_start, uint32_t strings_start,
                          uint32_t argc, uint32_t argv_bytes)
{
    uint32_t *argv_array = (uint32_t *)ptrs_start;
    uint32_t  offset = 0;

    for (uint32_t i = 0; i < argc; i++)
    {
        argv_array[i] = strings_start + offset;
        while (offset < argv_bytes &&
               ((const char *)strings_start)[offset] != '\0')
        {
            offset++;
        }
        if (offset >= argv_bytes && i + 1 < argc)
        {
            return false;               /* pool malformato                */
        }
        offset++;                       /* salta il NUL                   */
    }
    argv_array[argc] = 0;               /* sentinella NULL                */
    return true;
}

/* === Orchestratore ====================================================== */

uint32_t user_argv_setup(uint32_t user_stack_top, uint32_t argc,
                         const char *argv_strings, uint32_t argv_bytes)
{
    if (argc == 0)
    {
        argv_bytes = 0;                 /* anche zero-arg lascia un frame */
    }                                   /* valido (argc=0, argv[0]=NULL)  */
    if (!footprint_fits(argc, argv_bytes))
    {
        return 0;
    }

    uint32_t strings_start = user_stack_top - argv_bytes;
    uint32_t ptrs_end      = align_down_4(strings_start);
    uint32_t ptrs_start    = ptrs_end - (argc + 1) * 4;
    uint32_t new_esp       = ptrs_start - 4;

    uint32_t stack_base = USER_STACK_TOP - USER_STACK_PAGES * 4096u;
    if (new_esp < stack_base || new_esp >= user_stack_top)
    {
        return 0;                       /* sotto la base o wraparound     */
    }

    *(uint32_t *)new_esp = argc;
    if (argv_bytes > 0 && argv_strings != NULL)
    {
        memcpy((void *)strings_start, argv_strings, argv_bytes);
    }
    if (!emit_pointers(ptrs_start, strings_start, argc, argv_bytes))
    {
        return 0;
    }
    return new_esp;
}
