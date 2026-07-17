/* videotest — dobVideo introspection tool.
 *
 * Connects to the running video driver and shows a DobTable of
 * read-only diagnostics: driver identity, displays, current mode,
 * VRAM usage, vcores, capabilities.  Driver-agnostic: the driver
 * name is read at runtime from DRIVER_INFO and used in titles and
 * error messages, so the tool works against any DobVideoControl
 * provider (bga, mach64, ...).
 *
 * Transport.  The dobVideo protocol has two planes:
 *
 *   - Control plane (<DobVideoControl.h>, traditional IPC to the
 *     "DobVideoControl" service): driver_info, display, mode, and the
 *     capability / vcore introspection ops.  Callable by any process,
 *     including one without a vprocess — which is exactly what this
 *     tool is.  Everything here goes through the dobvc_* stub.
 *
 *   - Data plane (<dob/video.h>, int 0x85 boomerang): the per-frame
 *     hot path.  VRAM_INFO is the one query that lives only here, so
 *     it is issued directly via the boomerang (dv_call_pl).
 *
 * Historical note: this tool used to reach the driver by sending
 * data-plane DV_* opcodes over plain IPC.  That path was provisional
 * scaffolding inside the bga driver and has since been removed — the
 * driver's IPC front-end now serves the control plane only.  Issuing
 * a DV_* opcode over IPC today returns DV_ERR_NOSUPPORT (-3).  This
 * file was migrated to the two correct transports above.
 *
 * Pattern is the same as valueParser: spawn a DobTable, fill it,
 * Show, exit.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>

#include <DobTable.h>
#include <DobPopup.h>
#include <DobVideoControl.h>
#include <dob/registry.h>
#include <dob/types.h>
#include <dob/video.h>

#define MAX_ROWS 32

static char  key_buf[MAX_ROWS][48];
static char  val_buf[MAX_ROWS][96];
static const char *keys[MAX_ROWS];
static const char *vals[MAX_ROWS];
static int   row_count = 0;

/* Populated by query_driver_info() from dv_driver_info_t.name.
 * Used by fail() and the DobTable title so the tool reports the
 * actual active driver (bga / mach64 / ...) instead of hardcoding
 * one.  The fallback string is what gets shown if DRIVER_INFO
 * itself fails. */
static char  g_driver_name[40] = "(driver video)";

static void add_row(const char *k, const char *v)
{
    if (row_count >= MAX_ROWS) return;
    snprintf(key_buf[row_count], sizeof(key_buf[0]), "%s", k);
    snprintf(val_buf[row_count], sizeof(val_buf[0]), "%s", v);
    keys[row_count] = key_buf[row_count];
    vals[row_count] = val_buf[row_count];
    row_count++;
}

static int fail(const char *step)
{
    char body[200];
    snprintf(body, sizeof(body),
             "videotest: il passo \"%s\" è fallito.\n"
             "Controlla che il driver %s sia caricato\n"
             "(cerca [%s] Ready nei log di boot).",
             step, g_driver_name, g_driver_name);
    dobpopup_Error("videotest", body);
    return 1;
}

/* ==========================================================================
 *  Per-query helpers
 * ========================================================================== */

static void query_driver_info(void)
{
    dv_driver_info_t info;
    int rc = dobvc_DriverInfo(&info);
    if (rc != DV_OK)
    {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "(DriverInfo failed: rc=%d)", rc);
        add_row("Driver", tmp);
        return;
    }
    char tmp[96];

    /* Cache the driver short name for fail() messages and the DobTable
     * title.  Done before any add_row so the name is already correct if
     * a later step happens to fail. */
    snprintf(g_driver_name, sizeof(g_driver_name), "%s", info.name);

    add_row("Driver name",   info.name);
    add_row("Driver vendor", info.vendor);

    snprintf(tmp, sizeof(tmp), "%u.%u.%u",
             (unsigned)info.version_major,
             (unsigned)info.version_minor,
             (unsigned)info.version_patch);
    add_row("Driver version", tmp);

    snprintf(tmp, sizeof(tmp), "%04x:%04x",
             (unsigned)info.pci_vendor_id,
             (unsigned)info.pci_device_id);
    add_row("PCI vendor:device", tmp);
}

static void query_display(void)
{
    char tmp[96];

    uint32_t dcount = 0;
    if (dobvc_DisplayCount(&dcount) == DV_OK)
    {
        snprintf(tmp, sizeof(tmp), "%u", (unsigned)dcount);
        add_row("Display count", tmp);
    }

    dv_display_info_t d;
    if (dobvc_DisplayInfo(0, &d) == DV_OK)
    {
        add_row("Display 0",         d.name);
        add_row("Display connected", d.connected ? "yes" : "no");
        add_row("EDID present",      d.edid_present ? "yes" : "no");
    }

    dv_mode_t m;
    if (dobvc_ModeGet(0, &m) == DV_OK)
    {
        snprintf(tmp, sizeof(tmp), "%ux%u @ %u Hz",
                 (unsigned)m.width, (unsigned)m.height,
                 (unsigned)m.refresh_hz);
        add_row("Current mode", tmp);
    }

    static dv_mode_t modes[64];
    uint32_t mcount = 64;
    if (dobvc_ModeList(0, modes, &mcount) == DV_OK)
    {
        snprintf(tmp, sizeof(tmp), "%u", (unsigned)mcount);
        add_row("Available modes", tmp);
    }
}

static void query_vram(void)
{
    /* VRAM_INFO has no control-plane mirror — it is a data-plane-only
     * query.  Issue it straight through the int 0x85 boomerang; the
     * payload is the bidirectional dv_vram_info_t the driver fills.
     * vproc handle 0 = global VRAM stats (no vprocess required). */
    dv_vram_info_t vi;
    memset(&vi, 0, sizeof(vi));
    int rc = dv_call_pl(DV_VRAM_INFO, 0, 0, 0, &vi, sizeof(vi));
    if (rc != DV_OK)
    {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "(failed: rc=%d)", rc);
        add_row("VRAM", tmp);
        return;
    }
    char tmp[96];

    snprintf(tmp, sizeof(tmp), "%u KB", (unsigned)(vi.total_bytes / 1024));
    add_row("VRAM total", tmp);
    snprintf(tmp, sizeof(tmp), "%u KB", (unsigned)(vi.free_bytes / 1024));
    add_row("VRAM free", tmp);
    snprintf(tmp, sizeof(tmp), "%u KB", (unsigned)(vi.largest_contig_bytes / 1024));
    add_row("VRAM largest contig", tmp);
    snprintf(tmp, sizeof(tmp), "%u%%", (unsigned)vi.fragmentation_pct);
    add_row("VRAM fragmentation", tmp);

    /* Cmdlist pool stats (windows consume this, not VRAM). */
    if (vi.cmdlist_pool_total_bytes > 0)
    {
        uint32_t total_kb = vi.cmdlist_pool_total_bytes / 1024;
        uint32_t used_kb  = vi.cmdlist_pool_used_bytes  / 1024;
        uint32_t free_kb  = total_kb - used_kb;
        uint32_t pct      = (vi.cmdlist_pool_total_bytes
                              ? 100u * vi.cmdlist_pool_used_bytes
                                / vi.cmdlist_pool_total_bytes
                              : 0);
        snprintf(tmp, sizeof(tmp), "%u KB", total_kb);
        add_row("Cmdlist pool total", tmp);
        snprintf(tmp, sizeof(tmp), "%u KB (%u%%)", used_kb, pct);
        add_row("Cmdlist pool used", tmp);
        snprintf(tmp, sizeof(tmp), "%u KB", free_kb);
        add_row("Cmdlist pool free", tmp);
        snprintf(tmp, sizeof(tmp), "%u", (unsigned)vi.cmdlist_count);
        add_row("Cmdlist active", tmp);
    }
}

static void query_vcore(void)
{
    char tmp[96];

    uint32_t n = 0;
    if (dobvc_VcoreCount(&n) == DV_OK)
    {
        snprintf(tmp, sizeof(tmp), "%u", (unsigned)n);
        add_row("vcore count", tmp);
    }

    dv_vcore_info_t vc;
    if (dobvc_VcoreInfo(0, &vc) == DV_OK)
    {
        const char *kind_str;
        switch (vc.kind)
        {
            case DV_VCORE_FIXED_2D: kind_str = "fixed 2D";        break;
            case DV_VCORE_GENERAL:  kind_str = "general purpose"; break;
            case DV_VCORE_FRAGMENT: kind_str = "fragment";        break;
            case DV_VCORE_VERTEX:   kind_str = "vertex";          break;
            case DV_VCORE_COMPUTE:  kind_str = "compute";         break;
            default:                kind_str = "(other)";         break;
        }
        add_row("vcore[0] kind", kind_str);
        snprintf(tmp, sizeof(tmp), "max %u thread/block",
                 (unsigned)vc.max_threads_per_block);
        add_row("vcore[0] block", tmp);
    }
}

static void query_caps(void)
{
    char tmp[96];

    uint64_t caps;
    int rc = dobvc_CapQuery(&caps);
    if (rc != DV_OK)
    {
        snprintf(tmp, sizeof(tmp), "(failed: rc=%d)", rc);
        add_row("Capabilities", tmp);
        return;
    }

    char buf[96];
    buf[0] = '\0';
    if (caps & DV_CAP_PAGE_FLIP)        strcat(buf, "page-flip ");
    if (caps & DV_CAP_VSYNC)            strcat(buf, "vsync ");
    if (caps & DV_CAP_VRAM_MAP)         strcat(buf, "vram-map ");
    if (caps & DV_CAP_ACCELERATED_BLIT) strcat(buf, "accel-blit ");
    if (caps & DV_CAP_ALPHA_BLEND)      strcat(buf, "alpha ");
    if (caps & DV_CAP_GAMMA)            strcat(buf, "gamma ");
    if (buf[0] == '\0') strcpy(buf, "(none)");
    add_row("Capabilities", buf);

    uint64_t v;
    if (dobvc_CapQueryLimit(DV_LIMIT_MAX_RT_W, &v) == DV_OK)
    { snprintf(tmp, sizeof(tmp), "%u", (unsigned)v);
      add_row("Limit MAX_RT_W", tmp); }
    if (dobvc_CapQueryLimit(DV_LIMIT_MAX_TEX_W, &v) == DV_OK)
    { snprintf(tmp, sizeof(tmp), "%u", (unsigned)v);
      add_row("Limit MAX_TEX_W", tmp); }
    if (dobvc_CapQueryLimit(DV_LIMIT_MAX_LAYERS, &v) == DV_OK)
    { snprintf(tmp, sizeof(tmp), "%u", (unsigned)v);
      add_row("Limit MAX_LAYERS", tmp); }
    if (dobvc_CapQueryLimit(DV_LIMIT_MAX_VTHREAD, &v) == DV_OK)
    { snprintf(tmp, sizeof(tmp), "%u", (unsigned)v);
      add_row("Limit MAX_VTHREAD", tmp); }
}

/* ==========================================================================
 *  Entry point
 * ========================================================================== */

int main(void)
{
    /* Up-front existence check so a missing driver gets a clear popup
     * instead of every query failing with DV_ERR_NOTREADY.  The
     * dobvc_* stub re-discovers the port on its own. */
    if (dob_registry_wait("DobVideoControl", 5000) == 0)
    {
        dobpopup_Error("videotest",
            "Driver video non trovato.\n"
            "Il servizio DobVideoControl non è registrato.\n"
            "Verifica che un driver video (bga, mach64, ...) sia\n"
            "stato caricato da hotplug.");
        return 1;
    }

    query_driver_info();
    query_display();
    query_vram();
    query_vcore();
    query_caps();

    if (row_count == 0)
    {
        dobpopup_Error("videotest",
            "Nessuna query del driver è andata a buon fine.\n"
            "Controlla i log: il driver risponde sul port,\n"
            "ma non ritorna i dati attesi.");
        return 1;
    }

    char svc[32];
    if (dobtable_Spawn(svc, sizeof(svc)) != 0) return fail("Spawn");
    char title[56];
    snprintf(title, sizeof(title), "dobVideo - driver %s", g_driver_name);
    if (dobtable_SetTitle(svc, title) != 0)     return fail("SetTitle");
    if (dobtable_SetHeaders(svc, "Voce", "Valore") != 0)
                                                return fail("SetHeaders");
    if (dobtable_AddRows(svc, keys, vals, row_count) != 0)
                                                return fail("AddRows");
    if (dobtable_Show(svc) != 0)                return fail("Show");
    return 0;
}
