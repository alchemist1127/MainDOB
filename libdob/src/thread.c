/* User-thread trampoline.
 *
 * See <dob/thread.h> for the public API.  This file hosts the entry
 * trampoline — it lives in a .c (not a header) because it must be a
 * real out-of-line function with known ABI: the kernel iret lands
 * exactly at its first instruction with ECX = ctx pointer, and we
 * need that register untouched.  A static inline would let the
 * compiler dissolve it into callers and destroy the assumption.
 */

#include <dob/thread.h>

void _dob_thread_trampoline(void)
{
    _dob_thread_ctx_t *ctx;

    /* Recover ctx from ECX BEFORE any other statement.  The compiler's
     * function prologue (push ebp; mov ebp, esp) does not touch ECX,
     * and no C code runs ahead of this inline asm, so ECX still holds
     * what context_setup placed there. */
    __asm__ volatile ("movl %%ecx, %0" : "=r"(ctx));

    dob_thread_fn fn = ctx->fn;
    void         *arg = ctx->arg;

    /* Free the ctx struct now — the three fields we care about live
     * in local registers / stack slots from here on. */
    free(ctx);

    fn(arg);

    /* Worker body returned.  Clean exit: SYS_THREAD_EXIT never returns.
     * The user stack buffer leaks until process death — a small and
     * bounded cost paid for not having to free a stack we're still on. */
    syscall0(SYS_THREAD_EXIT);

    /* Unreachable.  If we ever get here the kernel is broken; hlt will
     * fault in user mode and the process will crash loudly rather than
     * silently corrupting. */
    for (;;) __asm__ volatile ("hlt");
}
