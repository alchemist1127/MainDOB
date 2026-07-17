#ifndef MAINDOB_DOB_THREAD_H
#define MAINDOB_DOB_THREAD_H

/* User-thread support.
 *
 * dob_thread_spawn() runs a function in a background thread that
 * shares the current process's address space. The main thread keeps
 * handling events normally. Use it for chains of blocking IPC calls
 * that would otherwise freeze the GUI (the worst offender being
 * "read a .mdl from disk and spawn from it").
 *
 * Worker blocks on real kernel wait queues via sync IPC — no polling.
 *
 * The caller keeps ownership of anything pointed to by `arg` until
 * the worker starts; after that the worker owns it. Idiom: strdup()
 * a path, let the worker free() it on exit.
 */

#include <sys/syscall.h>
#include <stdlib.h>
#include <sys/types.h>

typedef void (*dob_thread_fn)(void *arg);

/* Default worker stack: 16 KB.  Comfortable for deep IPC call chains
 * (dobfs_Open → dobfs_Read loop → spawn_from_data) plus payload
 * snapshots.  No guard page — keep worker bodies shallow. */
#define DOB_THREAD_STACK_DEFAULT (16 * 1024)

/* Heap ctx handed to the trampoline via ECX.  Opaque to callers. */
typedef struct
{
    dob_thread_fn fn;
    void         *arg;
    void         *stack_base;
} _dob_thread_ctx_t;

/* Defined in libdob/src/thread.c — out-of-line so its ABI is stable
 * (ECX = ctx on entry, no prologue register clobber). */
void _dob_thread_trampoline(void);

/* Spawn a background thread.  Returns new tid (>0) on success, -1 on
 * failure.  Worker runs fn(arg) in parallel with the caller. */
static inline int dob_thread_spawn(dob_thread_fn fn, void *arg)
{
    if (!fn) return -1;

    _dob_thread_ctx_t *ctx = (_dob_thread_ctx_t *)malloc(sizeof(*ctx));
    if (!ctx) return -1;

    void *stack = malloc(DOB_THREAD_STACK_DEFAULT);
    if (!stack) { free(ctx); return -1; }

    ctx->fn         = fn;
    ctx->arg        = arg;
    ctx->stack_base = stack;

    /* User stack grows down; ESP starts at the top.  16-byte align per
     * SysV i386 ABI. */
    uint32_t stack_top = (uint32_t)stack + DOB_THREAD_STACK_DEFAULT;
    stack_top &= ~0xFu;

    int tid = syscall3(SYS_THREAD_CREATE,
                       (int)(uintptr_t)_dob_thread_trampoline,
                       (int)(uintptr_t)ctx,
                       (int)stack_top);
    if (tid < 0)
    {
        free(stack);
        free(ctx);
        return -1;
    }
    return tid;
}

#endif /* MAINDOB_DOB_THREAD_H */
