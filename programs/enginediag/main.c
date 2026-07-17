/* enginediag — mach64 2D-engine bring-up diagnostic.
 *
 * Issues the private MACH64_DV_ENGINE_DIAG boomerang opcode: the driver
 * performs a hardware solid-fill probe and returns the engine status
 * registers read immediately after.  We render them in a DobTable so the
 * 2D engine can be debugged on real hardware without serial logs.
 *
 * The decisive row is "HW fill wrote?": it compares the framebuffer pixel
 * the hardware blitter should have written against the requested colour.
 *
 * Same shape as videotest: query, spawn a DobTable, fill, Show, exit.
 */
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <DobTable.h>
#include <DobPopup.h>
#include <dob/registry.h>
#include <dob/types.h>
#include <dob/video.h>

#include "mach64_diag.h"   /* opcode + payload struct (shared with driver) */

#define MAX_ROWS 48
static char  key_buf[MAX_ROWS][48];
static char  val_buf[MAX_ROWS][96];
static const char *keys[MAX_ROWS];
static const char *vals[MAX_ROWS];
static int   row_count = 0;

static void add_row(const char *k, const char *v)
{
    if (row_count >= MAX_ROWS) return;
    snprintf(key_buf[row_count], sizeof(key_buf[0]), "%s", k);
    snprintf(val_buf[row_count], sizeof(val_buf[0]), "%s", v);
    keys[row_count] = key_buf[row_count];
    vals[row_count] = val_buf[row_count];
    row_count++;
}

static void addx(const char *k, uint32_t v)
{
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "0x%08X", (unsigned)v);
    add_row(k, tmp);
}

static int fail(const char *what)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "enginediag: %s ha fallito.", what);
    dobpopup_Error("enginediag", msg);
    return 1;
}

int main(void)
{
    if (dob_registry_wait("DobVideoControl", 5000) == 0)
    {
        dobpopup_Error("enginediag",
            "Driver video non trovato.\n"
            "Il servizio DobVideoControl non è registrato.");
        return 1;
    }

    mach64_engine_diag_t d;
    memset(&d, 0, sizeof(d));
    int rc = dv_call_pl(MACH64_DV_ENGINE_DIAG, 0, 0, 0, &d, sizeof(d));
    if (rc != DV_OK)
    {
        char tmp[80];
        snprintf(tmp, sizeof(tmp),
                 "Opcode diagnostico non supportato (rc=%d).\n"
                 "Il driver caricato è mach64 aggiornato?", rc);
        dobpopup_Error("enginediag", tmp);
        return 1;
    }

    /* --- DECISIVE: is the engine actually executing the draw? --- */
    {
        char tmp[96];
        bool busy = (d.gui_stat_during & 1u) != 0u;
        snprintf(tmp, sizeof(tmp), "%s (GUI_STAT durante=0x%08X)",
                 busy ? "SI - engine STA disegnando"
                      : "NO - idle subito (non esegue il fill)",
                 (unsigned)d.gui_stat_during);
        add_row(">> engine esegue?", tmp);
    }
    /* --- readback cross-check (unreliable per PRG 5.2.1.3) --- */
    {
        bool wrote = (d.fb_pixel_after == d.fill_color);
        char tmp[110];
        snprintf(tmp, sizeof(tmp), "%s (letto 0x%08X) [inaffidabile]",
                 wrote ? "scrive" : "vuoto", (unsigned)d.fb_pixel_after);
        add_row("HW fill (readback)", tmp);
    }
    /* --- decode GUI_STAT scissor-clip flags (RRG p.3-61):
     *     bit8 X<left, bit9 X>right, bit10 Y<top, bit11 Y>bottom --- */
    {
        uint32_t g = d.gui_stat_after;
        char tmp[96];
        snprintf(tmp, sizeof(tmp), "%s%s%s%s%s",
                 (g & (1u<<8))  ? "X<LEFT "   : "",
                 (g & (1u<<9))  ? "X>RIGHT "  : "",
                 (g & (1u<<10)) ? "Y<TOP "    : "",
                 (g & (1u<<11)) ? "Y>BOTTOM " : "",
                 (g & 0x0F00u)  ? "" : "(nessun clip)");
        add_row(">> clip scissor", tmp);
    }
    /* --- latched geometry, read from the RW DST_X/Y/W/H registers --- */
    {
        char tmp[100];
        uint32_t yx = d.dst_y_x_rb, hw = d.dst_height_width_rb;
        snprintf(tmp, sizeof(tmp),
                 "X=%u Y=%u  W=%u H=%u  (atteso X100 Y50 W300 H200)",
                 (unsigned)(yx & 0xFFFFu), (unsigned)(yx >> 16),
                 (unsigned)(hw & 0xFFFFu), (unsigned)(hw >> 16));
        add_row("geometria latched", tmp);
    }
    /* --- memory type (block-write is SGRAM-only) --- */
    {
        char tmp[80];
        uint32_t mt = d.cnfg_stat0 & 0x7u;
        const char *s;
        switch (mt) {
            case 4: s = "SDRAM";            break;
            case 5: s = "SGRAM";            break;
            case 6: s = "SDRAM 2:1 32-bit"; break;
            case 7: s = "SGRAM 2:1 32-bit"; break;
            default: s = "(altro)";         break;
        }
        snprintf(tmp, sizeof(tmp), "%s (CNFG_STAT0=0x%08X)", s, (unsigned)d.cnfg_stat0);
        add_row("Tipo memoria", tmp);
    }

    /* --- engine enable / identity --- */
    {
        char tmp[64];
        bool gui_en = (d.gen_test_cntl & 0x00000100u) != 0u;  /* GEN_GUI_EN bit8 */
        snprintf(tmp, sizeof(tmp), "%s (GEN_TEST_CNTL=0x%08X)",
                 gui_en ? "abilitato" : "DISABILITATO", (unsigned)d.gen_test_cntl);
        add_row("GUI engine", tmp);
    }
    addx("CONFIG_CHIP_ID", d.chip_id);
    addx("BUS_CNTL", d.bus_cntl);

    /* --- engine busy state around the fill --- */
    {
        char tmp[80];
        snprintf(tmp, sizeof(tmp), "before=%s  after=%s",
                 (d.gui_stat_before & 1u) ? "BUSY" : "idle",
                 (d.gui_stat_after  & 1u) ? "BUSY" : "idle");
        add_row("ENGINE_BUSY", tmp);
    }
    addx("GUI_STAT during", d.gui_stat_during);
    addx("GUI_STAT after", d.gui_stat_after);
    addx("FIFO_STAT before", d.fifo_stat_before);
    addx("FIFO_STAT after", d.fifo_stat_after);

    /* --- setup register read-back (should hold the programmed values) --- */
    addx("DST_OFF_PITCH rb", d.dst_off_pitch_rb);
    addx("SC_RIGHT rb", d.sc_right_rb);
    addx("SC_BOTTOM rb", d.sc_bottom_rb);
    addx("DP_MIX rb", d.dp_mix_rb);
    addx("DP_WRITE_MASK rb", d.dp_write_mask_rb);
    addx("DP_SRC rb", d.dp_src_rb);
    addx("DP_PIX_WIDTH rb", d.dp_pix_width_rb);
    addx("DP_FRGD_CLR rb", d.dp_frgd_clr_rb);
    addx("CONFIG_CNTL", d.config_cntl);
    addx("MEM_CNTL", d.mem_cntl);

    {
        char tmp[48];
        snprintf(tmp, sizeof(tmp), "%d", (int)d.engine_reset_rc);
        add_row("engine setup rc", tmp);
    }

    /* --- Display timing: the numbers that decide the flash strategy --- */
    if (d.timing_valid)
    {
        char tmp[64];
        add_row("--- TIMING (us) ---", "");
        snprintf(tmp, sizeof(tmp), "%u us", (unsigned)d.vblank_us);
        add_row("vblank window", tmp);
        snprintf(tmp, sizeof(tmp), "%u us", (unsigned)d.frame_interval_us);
        add_row("active span", tmp);
        snprintf(tmp, sizeof(tmp), "%u us", (unsigned)d.compose_full_us);
        add_row("full-screen fill", tmp);
        snprintf(tmp, sizeof(tmp), "%u us", (unsigned)d.blit_region_us);
        add_row("region blit 300x200", tmp);

        /* Verdict: does a full compose fit in the blank?  And a region blit? */
        if (d.vblank_us > 0u)
        {
            const char *v1 = (d.compose_full_us <= d.vblank_us)
                             ? "SI (compose intero ci sta)"
                             : "NO (compose sfora il blank)";
            add_row("full compose nel blank?", v1);
            const char *v2 = (d.blit_region_us <= d.vblank_us)
                             ? "SI (blit regione ci sta)"
                             : "NO (anche il blit sfora)";
            add_row("blit regione nel blank?", v2);
        }
    }
    else
    {
        add_row("--- TIMING ---", "non misurato");
    }

    /* --- VRAM budget: does a 3 MB second buffer (page-flip) fit? --- */
    {
        char tmp[64];
        add_row("--- VRAM ---", "");
        snprintf(tmp, sizeof(tmp), "%u KiB", (unsigned)d.vram_total_kib);
        add_row("VRAM totale", tmp);
        snprintf(tmp, sizeof(tmp), "%u KiB", (unsigned)d.vram_free_kib);
        add_row("VRAM libera", tmp);
        snprintf(tmp, sizeof(tmp), "%u KiB", (unsigned)d.second_buf_kib);
        add_row("2o buffer richiede", tmp);
        const char *fit = (d.vram_free_kib >= d.second_buf_kib)
                          ? "SI (page-flip possibile)"
                          : "NO (VRAM insufficiente)";
        add_row("2o buffer ci sta?", fit);

        /* Runtime state, not prediction: did the driver actually bring up
         * real double-buffering, or fall back to single-buffer? This is the
         * line to read on the physical E500 (no serial logs) to know whether
         * the anti-flicker page-flip is live. */
        const char *db = d.double_buffered
                         ? "SI (page-flip ATTIVO)"
                         : "NO (single-buffer, fallback)";
        add_row("double buffer attivo?", db);
    }

    /* --- Compose layer order (paint order, last dv_compose) ---
     * Each row is one layer as the driver painted it, low z first (painted
     * first = bottom) to high z last (painted last = top). The desktop
     * icons live in the backbuf cmdlist layer at z=0; windows are cmdlist
     * layers at z>=10 and must appear BELOW the z=0 row here (later in the
     * list). If a window's row shows z<10, or no window row appears, the
     * icons-on-top bug is in layer setup; if order looks right, it's paint. */
    {
        char tmp[64];
        add_row("--- COMPOSE Z-ORDER ---", "");
        if (d.compose_layer_count == 0)
            add_row("(nessun layer)", "compose mai eseguito?");
        for (uint32_t i = 0; i < d.compose_layer_count && i < 16u; i++)
        {
            char key[24];
            snprintf(key, sizeof(key), "L%u z=%d %s", (unsigned)i,
                     (int)d.compose_layer_z[i],
                     d.compose_layer_is_cmdlist[i] ? "cmd" : "surf");
            snprintf(tmp, sizeof(tmp), "x=%u w=%u",
                     (unsigned)d.compose_layer_x[i],
                     (unsigned)d.compose_layer_w[i]);
            add_row(key, tmp);
        }
    }

    char svc[32];
    if (dobtable_Spawn(svc, sizeof(svc)) != 0) return fail("Spawn");
    if (dobtable_SetTitle(svc, "enginediag - mach64 2D") != 0) return fail("SetTitle");
    if (dobtable_SetHeaders(svc, "Registro", "Valore") != 0)   return fail("SetHeaders");
    if (dobtable_AddRows(svc, keys, vals, row_count) != 0)     return fail("AddRows");
    if (dobtable_Show(svc) != 0)                               return fail("Show");
    return 0;
}
