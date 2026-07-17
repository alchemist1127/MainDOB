/* boomtest — int 0x85 video boomerang smoke test.
 *
 * Fires a handful of dv_* opcodes via the kernel boomerang and shows
 * what each one returned.  The point is end-to-end validation of the
 * round-trip:
 *
 *   userspace `int 0x85`
 *     → kernel boomerang (saves caller CR3, switches to driver CR3)
 *       → driver asm shim (regs → cdecl)
 *         → driver C dispatcher (switch on opcode)
 *           → dv_* implementation
 *         ← return value in EAX
 *       ← back through the shim
 *     ← kernel restores CR3, iret to caller
 *
 * If the table comes up populated with the expected return codes, the
 * whole pipeline is wired correctly: the IDT entry is set, CR3 is
 * being switched both ways, the driver's slot is registered, and the
 * dispatcher is reaching the right case.
 *
 * Expected outcomes for the chosen opcodes are documented in the
 * table below.  A "PASS" / "FAIL" verdict per row makes it readable
 * at a glance.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <DobTable.h>
#include <DobPopup.h>
#include <dob/video.h>

#define MAX_ROWS 16

static char        key_buf[MAX_ROWS][48];
static char        val_buf[MAX_ROWS][96];
static const char *keys[MAX_ROWS];
static const char *vals[MAX_ROWS];
static int         row_count = 0;

static void add_row(const char *k, const char *v)
{
    if (row_count >= MAX_ROWS) return;
    snprintf(key_buf[row_count], sizeof(key_buf[0]), "%s", k);
    snprintf(val_buf[row_count], sizeof(val_buf[0]), "%s", v);
    keys[row_count] = key_buf[row_count];
    vals[row_count] = val_buf[row_count];
    row_count++;
}

/* One probe = one int 0x85 with a known opcode + arg shape, one
 * expected return code.  We render label, hex opcode, got, expected,
 * verdict. */
static void probe(const char *label, uint32_t opcode, int got, int expected)
{
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "op=0x%04x got=%d exp=%d %s",
             opcode, got, expected,
             (got == expected) ? "[PASS]" : "[FAIL]");
    add_row(label, tmp);
}

static int fail(const char *step)
{
    char body[160];
    snprintf(body, sizeof(body),
             "boomtest: il passo \"%s\" è fallito.\n"
             "Controlla che DobTable.mdl sia installato.", step);
    dobpopup_Error("boomtest", body);
    return 1;
}

int main(void)
{
    /* Probe 1 — DV_VTHREAD_YIELD(0).  bga's dv_vthread_yield is a
     * no-op that always returns DV_OK, so a successful round-trip
     * gives us 0.  This is the simplest "the boomerang works" check.
     * If this row says FAIL with got=-4, the driver isn't registered
     * (slot empty, kernel returned DV_ERR_NOTREADY). */
    probe("VTHREAD_YIELD",
          DV_VTHREAD_YIELD,
          dv_call1(DV_VTHREAD_YIELD, 0),
          DV_OK);

    /* Probe 2 — DV_DRIVER_INFO with no payload.  This opcode is not
     * wired in bga's fast dispatcher (it needs to return a struct,
     * which the register-only ABI can't carry yet), so the dispatcher
     * default case fires and we get DV_ERR_NOSUPPORT.  Proves that
     * the dispatcher is actually executing — not just a phantom
     * "everything returns DV_OK" stub. */
    probe("DRIVER_INFO (unwired)",
          DV_DRIVER_INFO,
          dv_call0(DV_DRIVER_INFO),
          DV_ERR_NOSUPPORT);

    /* Probe 3 — DV_VPROC_DETACH with a bogus handle.  The opcode IS
     * wired; bga's dv_vproc_detach validates the handle and rejects
     * it with DV_ERR_HANDLE.  Proves the dispatcher routes to the
     * correct dv_* function and forwards the args. */
    probe("VPROC_DETACH(0xDEADBEEF)",
          DV_VPROC_DETACH,
          dv_call1(DV_VPROC_DETACH, 0xDEADBEEF),
          DV_ERR_HANDLE);

    /* Probe 4 — DV_SURFACE_DESTROY with a bogus handle.  Same shape
     * as probe 3 but exercises a different switch case in the
     * dispatcher. */
    probe("SURFACE_DESTROY(0xDEADBEEF)",
          DV_SURFACE_DESTROY,
          dv_call1(DV_SURFACE_DESTROY, 0xDEADBEEF),
          DV_ERR_HANDLE);

    /* Probe 5 — DV_VSYNC_WAIT(0, 0).  bga's dv_vsync_wait returns
     * DV_ERR_NOSUPPORT (no hardware vsync on BGA).  Two-arg call,
     * exercises EBX + ECX delivery through the boomerang. */
    probe("VSYNC_WAIT(0,0)",
          DV_VSYNC_WAIT,
          dv_call2(DV_VSYNC_WAIT, 0, 0),
          DV_ERR_NOSUPPORT);

    /* Probe 6 — completely unknown opcode (0x0FFF reserved area).
     * Falls through bga's dispatcher default → DV_ERR_NOSUPPORT.
     * Sanity check that unknown opcodes don't crash anything. */
    probe("unknown(0x0FFF)",
          0x0FFF,
          dv_call0(0x0FFF),
          DV_ERR_NOSUPPORT);

    /* Render. */
    char svc[32];
    if (dobtable_Spawn(svc, sizeof(svc)) != 0)            return fail("Spawn");
    if (dobtable_SetTitle(svc, "int 0x85 boomerang test") != 0)
                                                          return fail("SetTitle");
    if (dobtable_SetHeaders(svc, "Probe", "Result") != 0) return fail("SetHeaders");
    if (dobtable_AddRows(svc, keys, vals, row_count) != 0) return fail("AddRows");
    if (dobtable_Show(svc) != 0)                          return fail("Show");
    return 0;
}
