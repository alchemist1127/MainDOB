/* MainDOB Graph Demo
 *
 * Showcase for the three DobUITools graph controls:
 *
 *   - linegraph : a scrolling CPU-style strip chart (range 0..100,
 *                 clamp mode), fed one sample per tick.
 *   - bargraph  : the same six values drawn both VERTICAL and
 *                 HORIZONTAL, to show the single control + orientation
 *                 enum.
 *   - piechart  : five proportional slices with a legend.
 *
 * Not a real application — purely a visual test.  Uses the app.h
 * event framework: a periodic event_tick drives the animation, and the
 * panel offers Pausa / Rigenera / Autoscale.
 *
 * Drawing model (same as every app.h program): the command list is
 * cleared on every Invalidate, so draw_all() repaints the whole window
 * each frame and calls dobui_Invalidate once at the end.
 */

#include <stdio.h>
#include <app.h>
#include <DobInterface.h>
#include <dob/types.h>

#include <label.h>
#include <graph.h>

#define WIN_W   700
#define WIN_H   540

#define BAR_N   6
#define PIE_N   5

/* Colours are 0x00RRGGBB. */
#include <dobui_theme.h>
#define COL_WIN_BG    DOBUI_SURFACE
#define COL_FG        DOBUI_TEXT
#define COL_TITLE_FG  DOBUI_TEXT_ALT
#define COL_TITLE_BG  DOBUI_RELIEF

#define COL_LG_BG     DOBUI_INSET
#define COL_LG_LINE   DOBUI_SUCCESS

#define COL_PANEL_BG  DOBUI_INSET

static uint32_t win_id;
static int win_w = WIN_W, win_h = WIN_H;

/* Controls */
static dob_label_t    lbl_title, lbl_line, lbl_vbars, lbl_hbars, lbl_pie, lbl_status;
static dob_linegraph_t lg;
static dob_bargraph_t  vbars, hbars;
static dob_piechart_t  pie;

/* Shared palette (Tableau-ish), RGB. */
static const uint32_t bar_colors[BAR_N] = {
    0x004E79A7, 0x00F28E2B, 0x00E15759,
    0x0076B7B2, 0x0059A14F, 0x00EDC948
};
static const uint32_t pie_colors[PIE_N] = {
    0x004E79A7, 0x00F28E2B, 0x00E15759, 0x0076B7B2, 0x0059A14F
};

/* State */
static int  cpu = 40;                 /* current line-graph sample */
static int  bar_vals[BAR_N];
static int  pie_vals[PIE_N];
static bool paused       = false;
static bool lg_autoscale = false;
static unsigned tick_count = 0;

/* Tiny LCG — synthetic data, no entropy needed. */
static uint32_t rng_state = 0x1234ABCDu;
static uint32_t rng_next(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state;
}
static int rng_range(int lo, int hi)
{
    if (hi <= lo) return lo;
    uint32_t span = (uint32_t)(hi - lo + 1);
    return lo + (int)((rng_next() >> 8) % span);
}

/* === Setup === */

static void init_controls(void)
{
    doblbl_InitWithBg(&lbl_title, win_id, 0, 0,
                      " Graph Demo  —  linegraph . bargraph . piechart ",
                      COL_TITLE_FG, COL_TITLE_BG);
    lbl_title.pad = 6;

    doblbl_Init(&lbl_line,  win_id, 0, 0, "Line graph (CPU, range 0..100, scorre):");
    doblbl_Init(&lbl_vbars, win_id, 0, 0, "Bar graph verticale:");
    doblbl_Init(&lbl_hbars, win_id, 0, 0, "Bar graph orizzontale (stessi dati):");
    doblbl_Init(&lbl_pie,   win_id, 0, 0, "Pie chart con legenda:");
    doblbl_Init(&lbl_status,win_id, 0, 0, "");
    lbl_line.col_text = lbl_vbars.col_text = lbl_hbars.col_text =
        lbl_pie.col_text = lbl_status.col_text = COL_FG;
    lbl_line.col_bg = lbl_vbars.col_bg = lbl_hbars.col_bg =
        lbl_pie.col_bg = lbl_status.col_bg = COL_WIN_BG;

    /* Line graph: 80 samples, fixed range, clamp (autoscale off). */
    doblg_Init(&lg, win_id, 0, 0, 10, 10, 80, 0, 100, COL_LG_BG, COL_LG_LINE);
    lg.show_border = true;
    doblg_SetAutoscale(&lg, lg_autoscale);

    /* Same control, two orientations, shared palette + 0..100 range. */
    dobbg_Init(&vbars, win_id, 0, 0, 10, 10, BAR_N, DOB_BAR_VERTICAL,
               bar_colors, COL_PANEL_BG, 0, 100);
    dobbg_Init(&hbars, win_id, 0, 0, 10, 10, BAR_N, DOB_BAR_HORIZONTAL,
               bar_colors, COL_PANEL_BG, 0, 100);

    dobpie_Init(&pie, win_id, 0, 0, 10, 10, PIE_N, pie_colors, COL_PANEL_BG, true);
    dobpie_SetLabel(&pie, 0, "Sistema");
    dobpie_SetLabel(&pie, 1, "Utente");
    dobpie_SetLabel(&pie, 2, "Cache");
    dobpie_SetLabel(&pie, 3, "Libera");
    dobpie_SetLabel(&pie, 4, "Altro");
}

/* Assign geometry from the current window size (re-runs on resize). */
static void layout(void)
{
    int m    = 12;
    int colW = (win_w - 3 * m) / 2; if (colW < 80) colW = 80;
    int colL = m;
    int colR = m + colW + m;
    int colRW = win_w - colR - m;   if (colRW < 80) colRW = 80;

    lbl_title.x = m; lbl_title.y = 8;

    lbl_line.x = m;  lbl_line.y = 40;
    lg.x = m; lg.y = 60; lg.w = win_w - 2 * m; lg.h = 110;

    int row2 = 186;
    lbl_vbars.x = colL; lbl_vbars.y = row2;
    vbars.x = colL; vbars.y = row2 + 20; vbars.w = colW; vbars.h = 150;

    int row3 = vbars.y + vbars.h + 14;
    lbl_hbars.x = colL; lbl_hbars.y = row3;
    hbars.x = colL; hbars.y = row3 + 20; hbars.w = colW; hbars.h = 120;

    lbl_pie.x = colR; lbl_pie.y = row2;
    pie.x = colR; pie.y = row2 + 20; pie.w = colRW; pie.h = 200;

    lbl_status.x = m; lbl_status.y = win_h - 22;
}

static void update_status(void)
{
    char buf[160];
    snprintf(buf, sizeof(buf),
             "CPU: %d%%    Autoscale linea: %s    %s",
             cpu, lg_autoscale ? "ON" : "OFF",
             paused ? "[PAUSA]" : "in esecuzione");
    doblbl_SetText(&lbl_status, buf);
}

static void regenerate(void)
{
    cpu = rng_range(20, 80);
    doblg_UpdateAll(&lg, NULL, 0);          /* clear the strip, refill from right */

    for (int i = 0; i < BAR_N; i++) bar_vals[i] = rng_range(10, 100);
    dobbg_UpdateAll(&vbars, bar_vals, BAR_N);
    dobbg_UpdateAll(&hbars, bar_vals, BAR_N);

    for (int i = 0; i < PIE_N; i++) pie_vals[i] = rng_range(5, 40);
    dobpie_UpdateAll(&pie, pie_vals, PIE_N);
}

/* === Drawing === */

static void draw_all(void)
{
    dobui_FillRect(win_id, 0, 0, win_w, win_h, COL_WIN_BG);

    doblbl_Draw(&lbl_title);

    doblbl_Draw(&lbl_line);
    doblg_Draw(&lg);

    doblbl_Draw(&lbl_vbars);
    dobbg_Draw(&vbars);

    doblbl_Draw(&lbl_hbars);
    dobbg_Draw(&hbars);

    doblbl_Draw(&lbl_pie);
    dobpie_Draw(&pie);

    doblbl_Draw(&lbl_status);

    dobui_Invalidate(win_id);
}

/* === Events === */

void event_start(void)
{
    win_id = dobui_window();
    init_controls();
    layout();
    regenerate();
    update_status();
    draw_all();
}

void event_tick(void)
{
    if (paused) return;
    tick_count++;

    /* Line: random walk, pinned to [0,100] (shows clamp). */
    cpu += rng_range(-9, 9);
    if (cpu < 0)   cpu = 0;
    if (cpu > 100) cpu = 100;
    doblg_UpdateLast(&lg, cpu);

    /* Bars: a gentler walk, every third tick. */
    if (tick_count % 3 == 0)
    {
        for (int i = 0; i < BAR_N; i++)
        {
            bar_vals[i] += rng_range(-12, 12);
            if (bar_vals[i] < 0)   bar_vals[i] = 0;
            if (bar_vals[i] > 100) bar_vals[i] = 100;
        }
        dobbg_UpdateAll(&vbars, bar_vals, BAR_N);
        dobbg_UpdateAll(&hbars, bar_vals, BAR_N);
    }

    update_status();
    draw_all();     /* authoritative full-frame repaint + Invalidate */
}

void event_panel(int cmd_idx)
{
    if (cmd_idx == 0)
        paused = !paused;
    else if (cmd_idx == 1)
        regenerate();
    else if (cmd_idx == 2)
    {
        lg_autoscale = !lg_autoscale;
        doblg_SetAutoscale(&lg, lg_autoscale);
    }
    update_status();
    draw_all();
}

void event_resize(int w, int h)
{
    win_w = w;
    win_h = h;
    layout();
    draw_all();
}

int main(void)
{
    dobui_set_panel("Pausa\nRigenera\nAutoscale linea");
    dobui_set_tick(150);
    dobui_run("Graph Demo", WIN_W, WIN_H);
    return 0;
}
