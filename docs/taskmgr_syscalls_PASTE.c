/* =====================================================================
 * Task-manager syscalls: paste-in handlers + wiring notes
 * =====================================================================
 *
 * These two syscalls expose the introspection snapshots (introspect.h)
 * to userspace, following the exact pattern of sys_meminfo: validate
 * the user buffer with is_user_ptr + user_check, fill a kernel buffer,
 * copy_to_user. The heavy lifting (the locked, value-only walk) is in
 * introspect_snapshot_processes / introspect_snapshot_threads, which
 * are NOT called while holding any syscall-side lock.
 *
 * ---------------------------------------------------------------------
 * STEP 1 — add syscall numbers to kernel/syscall/syscall.h
 * ---------------------------------------------------------------------
 *   #define SYS_PROC_LIST    83   // arg0=buf, arg1=max_records -> count
 *   #define SYS_THREAD_LIST  84   // arg0=buf, arg1=max_records -> count
 *
 * (83/84 are the next free slots after SYS_THREAD_CREATE=82.)
 *
 * ---------------------------------------------------------------------
 * STEP 2 — add the include near the top of kernel/syscall/syscall.c
 * ---------------------------------------------------------------------
 *   #include "proc/introspect.h"
 *
 * ---------------------------------------------------------------------
 * STEP 3 — paste the two handlers below into syscall.c (near sys_meminfo)
 * ---------------------------------------------------------------------
 *
 * ---------------------------------------------------------------------
 * STEP 4 — add two cases to the dispatch switch, in your existing style:
 * ---------------------------------------------------------------------
 *   case SYS_PROC_LIST:    return sys_proc_list(regs);
 *   case SYS_THREAD_LIST:  return sys_thread_list(regs);
 *
 * ---------------------------------------------------------------------
 * ABI contract for both calls:
 *   ebx (arg0) = user pointer to an array of records
 *   ecx (arg1) = capacity of that array, in records
 *   return     = number of records written (>=0), or -1 on bad args
 *
 * The record structs (proc_info_t / thread_info_t) are defined in
 * introspect.h and are ABI-stable (fixed scalar layout). Userspace
 * includes a matching definition. If written == max, the list was
 * truncated — size the buffer to MAX_PROCESSES / MAX_THREADS to be
 * sure of getting everything, or call once, read the truncation, and
 * grow.
 *
 * A note on cost while the lock is held: the process walk calls
 * vregion_total_committed per process, which is O(regions). For a
 * desktop with tens of processes this is microseconds under
 * process_lock — fine. If you ever push to thousands of processes and
 * notice latency spikes, drop committed_pages from the locked walk and
 * compute it lazily per-PID via a second call.
 * ===================================================================== */

/* SYS_PROC_LIST: enumerate live processes.
 * arg0 (ebx) = user pointer to proc_info_t[max]
 * arg1 (ecx) = max records
 * Returns: number of records written, or -1 on error. */
static int32_t sys_proc_list(isr_regs_t *regs)
{
    proc_info_t *ubuf = (proc_info_t *)regs->ebx;
    uint32_t     max  = regs->ecx;

    if (!ubuf || max == 0)
        return -1;
    /* Guard the multiply against overflow before sizing the check. */
    if (max > 0xFFFFFFFFu / sizeof(proc_info_t))
        return -1;
    uint32_t bytes = max * (uint32_t)sizeof(proc_info_t);
    if (!is_user_ptr(ubuf, bytes))
        return -1;
    if (!user_check(ubuf, bytes, true))
        return -1;

    /* Snapshot into a temporary kernel buffer, THEN copy to user — so
     * we never hold process_lock across paging / user-copy. For large
     * `max` this temp could be sizeable; cap what we materialize per
     * call to keep kernel stack/heap bounded. Here we use the heap. */
    proc_info_t *kbuf = (proc_info_t *)kmalloc(bytes);
    if (!kbuf)
        return -1;

    uint32_t n = introspect_snapshot_processes(kbuf, max);

    bool ok = copy_to_user(ubuf, kbuf, n * (uint32_t)sizeof(proc_info_t));
    kfree(kbuf);
    return ok ? (int32_t)n : -1;
}

/* SYS_THREAD_LIST: enumerate live threads.
 * arg0 (ebx) = user pointer to thread_info_t[max]
 * arg1 (ecx) = max records
 * Returns: number of records written, or -1 on error. */
static int32_t sys_thread_list(isr_regs_t *regs)
{
    thread_info_t *ubuf = (thread_info_t *)regs->ebx;
    uint32_t       max  = regs->ecx;

    if (!ubuf || max == 0)
        return -1;
    if (max > 0xFFFFFFFFu / sizeof(thread_info_t))
        return -1;
    uint32_t bytes = max * (uint32_t)sizeof(thread_info_t);
    if (!is_user_ptr(ubuf, bytes))
        return -1;
    if (!user_check(ubuf, bytes, true))
        return -1;

    thread_info_t *kbuf = (thread_info_t *)kmalloc(bytes);
    if (!kbuf)
        return -1;

    uint32_t n = introspect_snapshot_threads(kbuf, max);

    bool ok = copy_to_user(ubuf, kbuf, n * (uint32_t)sizeof(thread_info_t));
    kfree(kbuf);
    return ok ? (int32_t)n : -1;
}
