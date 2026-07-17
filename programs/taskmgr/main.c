/* taskmgr.mdl — Gestione Attivita' di MainDOB (v2, a tab)
 *
 * Quattro tab:
 *   PROCESSI  — grid multicampo (PID, nome, uso, priorita', core,
 *               thread, memoria, pin), ordinata per uso decrescente,
 *               selezione agganciata al PID. Comandi pannello:
 *               Termina / Cambia priorita' / Aggiorna. Il cambio
 *               priorita' apre un overlay modale in-finestra.
 *   CPU       — sismografo dell'uso totale + valore corrente.
 *   RAM       — candele (usata/libera) + sismografo dell'uso %.
 *   CORE      — candele per-core + un sismografo per core (fino a 4).
 *
 * MISURA (ABI di SYS_TASK_SNAPSHOT): il kernel espone contatori
 * MONOTONI; le percentuali sono delta tra due poll (300 ms, il tick
 * del framework). Gli STORICI vivono QUI in ring buffer propri e i
 * grafici si alimentano con UpdateAll a ogni ridisegno: cosi'
 * sopravvivono al cambio tab e al resize (la re-Init di un widget ne
 * azzererebbe i campioni interni).
 *
 * RESIZE: event_resize aggiorna le dimensioni correnti, layout()
 * ri-inizializza la geometria dei widget e si ridisegna tutto — mai
 * piu' contenuto a misura fissa dentro una finestra cresciuta.
 *
 * Struttura (convenzione MainDOB): blocchi ESECUTIVI in alto (misura,
 * ring, formattazione, layout, disegno dei singoli pezzi), LOGICA in
 * basso (gli event handler orchestrano). */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <DobInterface.h>
#include <DobPopup.h>
#include <dob/types.h>
#include <dob/task.h>
#include <app.h>
#include <graph.h>
#include <grid.h>

/* ===================================================================
 * Tavolozza (stile DobFiles) e costanti
 * =================================================================== */

#define COL_BG         0x00102040
#define COL_FG         0x00FFD860
#define COL_DIM        0x008090B0
#define COL_TAB_ACT    0x00104070
#define COL_TAB_IDLE   0x00081830
#define COL_OVL_BG     0x00081830
#define COL_OVL_HI     0x00204070

#define TAB_H          26
#define PAD            10

#define HIST_LEN       128          /* ring: 128 * 300 ms ~ 38 s        */
#define CORE_STRIPS    4            /* sismografi per-core mostrati     */

enum { TAB_PROC = 0, TAB_CPU, TAB_RAM, TAB_CORE, TAB_COUNT };
static const char *TAB_NAME[TAB_COUNT] = { "Processi", "CPU", "RAM", "Core" };

/* ===================================================================
 * Stato
 * =================================================================== */

static int s_w = 640, s_h = 460;    /* dimensioni correnti finestra     */
static int s_tab = TAB_PROC;

static dob_tasksnap_t s_snap;
static int            s_nrows;

static dob_tasksnap_hdr_t s_prev_hdr;
static dob_tasksnap_row_t s_prev_rows[DOB_TASKSNAP_MAX_ROWS];
static int                s_prev_nrows;
static bool               s_have_prev;

static int s_cpu_pct[DOB_TASKSNAP_MAX_ROWS];
static int s_core_pct[DOB_TASKSNAP_MAX_CPUS];
static int s_total_pct;
static int s_order[DOB_TASKSNAP_MAX_ROWS];

/* Ring degli storici (alimentano i grafici via UpdateAll). */
static int s_hist_total[HIST_LEN];
static int s_hist_core[CORE_STRIPS][HIST_LEN];
static int s_hist_ram[HIST_LEN];
static int s_hist_n;                /* campioni validi (satura a LEN)   */

/* Widget della tab corrente (ri-Init a ogni layout/cambio tab). */
static dob_grid_t      s_grid;
static dob_linegraph_t s_lg_a;      /* sismografo principale            */
static dob_linegraph_t s_lg_core[CORE_STRIPS];
static dob_bargraph_t  s_bars;

/* Celle della grid: puntatori a buffer statici (la grid REFERENZIA). */
#define GCOLS 10
static dobgrid_col_t s_cols[GCOLS] = {
    { "PID",      48, DOBGRID_ALIGN_RIGHT },
    { "Processo",  0, DOBGRID_ALIGN_LEFT  },
    { "Uso",      52, DOBGRID_ALIGN_RIGHT },
    { "Pri",      40, DOBGRID_ALIGN_RIGHT },
    { "Core",     46, DOBGRID_ALIGN_RIGHT },
    { "Thr",      40, DOBGRID_ALIGN_RIGHT },
    { "Mem KB",   72, DOBGRID_ALIGN_RIGHT },  /* RAM vera               */
    { "MMIO KB",  72, DOBGRID_ALIGN_RIGHT },  /* aperture device (VRAM) */
    { "DMA KB",   64, DOBGRID_ALIGN_RIGHT },  /* buffer DMA tracciati   */
    { "Pin",      36, DOBGRID_ALIGN_LEFT  },
};
static char s_cell[DOB_TASKSNAP_MAX_ROWS][GCOLS][28];
static const char *s_cells[DOB_TASKSNAP_MAX_ROWS * GCOLS];

static int32_t s_sel_pid = -1;

/* Overlay "Cambia priorita'": modale in-finestra. */
static bool s_prio_open;
static int  s_prio_x, s_prio_y;     /* origine del riquadro             */
#define PRIO_W   240
#define PRIO_ROW 24
static const char *PRIO_LABEL[4] = {
    "0 - Massima (quanto 2 ms)",
    "1 - Alta    (quanto 5 ms)",
    "2 - Normale (quanto 10 ms)",
    "3 - Bassa   (quanto 20 ms)",
};

/* ===================================================================
 * ESECUTIVI — misura
 * =================================================================== */

static void compute_percentages(void)
{
    s_total_pct = 0;
    memset(s_core_pct, 0, sizeof(s_core_pct));
    memset(s_cpu_pct, 0, sizeof(int) * (unsigned)s_nrows);
    if (!s_have_prev)
    {
        return;
    }

    uint64_t dt = s_snap.hdr.now_ns - s_prev_hdr.now_ns;
    if (dt == 0)
    {
        return;
    }
    uint32_t ncpu = s_snap.hdr.ncpu;
    if (ncpu == 0 || ncpu > DOB_TASKSNAP_MAX_CPUS)
    {
        ncpu = 1;
    }

    for (uint32_t c = 0; c < ncpu; c++)
    {
        uint64_t didle = s_snap.hdr.cpu_idle_ns[c]
                       - s_prev_hdr.cpu_idle_ns[c];
        if (didle > dt)
        {
            didle = dt;
        }
        s_core_pct[c] = (int)(100u - (uint32_t)((didle * 100u) / dt));
        s_total_pct  += s_core_pct[c];
    }
    s_total_pct /= (int)ncpu;

    uint64_t machine = dt * ncpu;   /* 100%% di processo = la macchina  */
    for (int i = 0; i < s_nrows; i++)
    {
        for (int j = 0; j < s_prev_nrows; j++)
        {
            if (s_prev_rows[j].pid == s_snap.rows[i].pid)
            {
                uint64_t dc = s_snap.rows[i].cpu_ns
                            - s_prev_rows[j].cpu_ns;
                if (dc > machine)
                {
                    dc = machine;
                }
                s_cpu_pct[i] = (int)((dc * 100u) / machine);
                break;
            }
        }
    }
}

/* Spinge il campione corrente nei ring (shift sinistro: economico a
 * 128 int ogni 300 ms, e UpdateAll vuole l'array in ordine). */
static void push_history(void)
{
    memmove(s_hist_total, s_hist_total + 1, sizeof(int) * (HIST_LEN - 1));
    s_hist_total[HIST_LEN - 1] = s_total_pct;
    for (int c = 0; c < CORE_STRIPS; c++)
    {
        memmove(s_hist_core[c], s_hist_core[c] + 1,
                sizeof(int) * (HIST_LEN - 1));
        s_hist_core[c][HIST_LEN - 1] =
            (c < (int)s_snap.hdr.ncpu) ? s_core_pct[c] : 0;
    }
    uint32_t used_pct = s_snap.hdr.ram_total_mb
        ? ((s_snap.hdr.ram_total_mb - s_snap.hdr.ram_free_mb) * 100u)
          / s_snap.hdr.ram_total_mb : 0;
    memmove(s_hist_ram, s_hist_ram + 1, sizeof(int) * (HIST_LEN - 1));
    s_hist_ram[HIST_LEN - 1] = (int)used_pct;
    if (s_hist_n < HIST_LEN)
    {
        s_hist_n++;
    }
}

/* Ordinamento stabile per uso decrescente (pari uso: PID crescente). */
static void sort_rows(void)
{
    for (int i = 0; i < s_nrows; i++)
    {
        s_order[i] = i;
    }
    for (int i = 1; i < s_nrows; i++)
    {
        int k = s_order[i];
        int j = i - 1;
        while (j >= 0 &&
               (s_cpu_pct[s_order[j]] < s_cpu_pct[k] ||
                (s_cpu_pct[s_order[j]] == s_cpu_pct[k] &&
                 s_snap.rows[s_order[j]].pid > s_snap.rows[k].pid)))
        {
            s_order[j + 1] = s_order[j];
            j--;
        }
        s_order[j + 1] = k;
    }
}

/* ===================================================================
 * ESECUTIVI — formattazione grid e selezione per PID
 * =================================================================== */

static void format_grid_cells(void)
{
    for (int i = 0; i < s_nrows; i++)
    {
        dob_tasksnap_row_t *r = &s_snap.rows[s_order[i]];
        snprintf(s_cell[i][0], 28, "%d", (int)r->pid);
        snprintf(s_cell[i][1], 28, "%s", r->name);
        snprintf(s_cell[i][2], 28, "%d%%", s_cpu_pct[s_order[i]]);
        snprintf(s_cell[i][3], 28, "%u", (unsigned)r->priority);
        snprintf(s_cell[i][4], 28, "%u", (unsigned)r->home_cpu);
        snprintf(s_cell[i][5], 28, "%u", (unsigned)r->nthreads);
        snprintf(s_cell[i][6], 28, "%u", (unsigned)(r->mem_pages * 4u));
        if (r->dev_pages != 0)
        {
            snprintf(s_cell[i][7], 28, "%u",
                     (unsigned)(r->dev_pages * 4u));
        }
        else
        {
            s_cell[i][7][0] = '\0';    /* vuoto: quasi nessuno ne ha  */
        }
        if (r->dma_pages != 0)
        {
            snprintf(s_cell[i][8], 28, "%u",
                     (unsigned)(r->dma_pages * 4u));
        }
        else
        {
            s_cell[i][8][0] = '\0';
        }
        snprintf(s_cell[i][9], 28, "%s", r->pinned ? "si'" : "");
        for (int c = 0; c < GCOLS; c++)
        {
            s_cells[i * GCOLS + c] = s_cell[i][c];
        }
    }
}

static void selection_to_pid(void)
{
    int row = dobgrid_CurRow(&s_grid);
    s_sel_pid = (row >= 0 && row < s_nrows)
              ? s_snap.rows[s_order[row]].pid : -1;
}

static void selection_from_pid(void)
{
    if (s_sel_pid < 0)
    {
        return;
    }
    for (int i = 0; i < s_nrows; i++)
    {
        if (s_snap.rows[s_order[i]].pid == s_sel_pid)
        {
            dobgrid_SetCurrent(&s_grid, i, dobgrid_CurCol(&s_grid));
            return;
        }
    }
    s_sel_pid = -1;                 /* il processo e' morto              */
}

/* ===================================================================
 * ESECUTIVI — layout e disegno
 * =================================================================== */

/* Geometria dei widget della tab corrente, sulle dimensioni CORRENTI:
 * e' il cuore del fix del resize (prima tutto era cablato a 640x460). */
static void layout(void)
{
    uint32_t win = dobui_window();
    int top  = TAB_H + PAD;         /* sotto la striscia dei tab         */
    int w    = s_w - 2 * PAD;
    int h    = s_h - top - PAD;

    switch (s_tab)
    {
    case TAB_PROC:
        dobgrid_Init(&s_grid, win, PAD, top, w, h);
        dobgrid_SetColumns(&s_grid, s_cols, GCOLS);
        dobgrid_SetFrozenColumns(&s_grid, 1);
        dobgrid_SetSelectable(&s_grid, true);
        break;

    case TAB_CPU:
        doblg_Init(&s_lg_a, win, PAD, top + 24, w, h - 24,
                   HIST_LEN, 0, 100, COL_BG, COL_FG);
        break;

    case TAB_RAM:
        dobbg_Init(&s_bars, win, PAD, top + 24, w / 3, h - 24,
                   2, DOB_BAR_VERTICAL, NULL, COL_BG, 0, 100);
        doblg_Init(&s_lg_a, win, PAD + w / 3 + PAD, top + 24,
                   w - w / 3 - PAD, h - 24,
                   HIST_LEN, 0, 100, COL_BG, COL_FG);
        break;

    case TAB_CORE:
    {
        int ncpu = (int)s_snap.hdr.ncpu ? (int)s_snap.hdr.ncpu : 1;
        int strips = ncpu < CORE_STRIPS ? ncpu : CORE_STRIPS;
        dobbg_Init(&s_bars, win, PAD, top + 24, w / 3, h - 24,
                   ncpu, DOB_BAR_VERTICAL, NULL, COL_BG, 0, 100);
        int sx = PAD + w / 3 + PAD;
        int sw = w - w / 3 - PAD;
        int sh = (h - 24 - (strips - 1) * 6) / strips;
        for (int c = 0; c < strips; c++)
        {
            doblg_Init(&s_lg_core[c], win, sx,
                       top + 24 + c * (sh + 6), sw, sh,
                       HIST_LEN, 0, 100, COL_BG, COL_FG);
        }
        break;
    }
    }
}

static void draw_tabs(void)
{
    uint32_t win = dobui_window();
    int tw = s_w / TAB_COUNT;
    for (int t = 0; t < TAB_COUNT; t++)
    {
        uint32_t bg = (t == s_tab) ? COL_TAB_ACT : COL_TAB_IDLE;
        dobui_FillRect(win, t * tw, 0, tw, TAB_H, bg);
        dobui_DrawRect(win, t * tw, 0, tw, TAB_H, COL_DIM);
        dobui_DrawText(win, t * tw + 12, (TAB_H - 12) / 2,
                       TAB_NAME[t], COL_FG, bg);
    }
}

static void draw_tab_proc(void)
{
    format_grid_cells();
    dobgrid_SetRows(&s_grid, s_cells, s_nrows);
    selection_from_pid();
    dobgrid_Draw(&s_grid);
}

static void draw_tab_cpu(void)
{
    uint32_t win = dobui_window();
    char line[48];
    snprintf(line, sizeof(line), "Uso CPU totale: %3d%%", s_total_pct);
    dobui_DrawTextFixed(win, PAD, TAB_H + PAD, line, COL_FG, COL_BG);
    doblg_UpdateAll(&s_lg_a, s_hist_total, HIST_LEN);
    doblg_Draw(&s_lg_a);
}

static void draw_tab_ram(void)
{
    uint32_t win = dobui_window();
    uint32_t used = s_snap.hdr.ram_total_mb - s_snap.hdr.ram_free_mb;
    uint32_t upct = s_snap.hdr.ram_total_mb
        ? used * 100u / s_snap.hdr.ram_total_mb : 0;
    char line[64];
    snprintf(line, sizeof(line), "RAM: %u/%u MB (%u%%)",
             (unsigned)used, (unsigned)s_snap.hdr.ram_total_mb,
             (unsigned)upct);
    dobui_DrawTextFixed(win, PAD, TAB_H + PAD, line, COL_FG, COL_BG);
    snprintf(line, sizeof(line),
             "DMA (<16MB): %u KB liberi - slot %u/%u",
             (unsigned)(s_snap.hdr.dma_free_frames * 4u),
             (unsigned)(s_snap.hdr.dma_slots >> 16),
             (unsigned)(s_snap.hdr.dma_slots & 0xFFFFu));
    dobui_DrawTextFixed(win, PAD, TAB_H + PAD + 14, line, COL_DIM, COL_BG);

    int vals[2] = { (int)upct, (int)(100u - upct) };  /* usata|libera  */
    dobbg_UpdateAll(&s_bars, vals, 2);
    dobbg_Draw(&s_bars);
    doblg_UpdateAll(&s_lg_a, s_hist_ram, HIST_LEN);
    doblg_Draw(&s_lg_a);
}

static void draw_tab_core(void)
{
    uint32_t win = dobui_window();
    int ncpu = (int)s_snap.hdr.ncpu ? (int)s_snap.hdr.ncpu : 1;
    int strips = ncpu < CORE_STRIPS ? ncpu : CORE_STRIPS;
    char line[64];
    int  n = 0;
    n += snprintf(line + n, sizeof(line) - (unsigned)n, "Core:");
    for (int c = 0; c < ncpu && n < (int)sizeof(line) - 8; c++)
    {
        n += snprintf(line + n, sizeof(line) - (unsigned)n,
                      "  %d:%3d%%", c, s_core_pct[c]);
    }
    dobui_DrawTextFixed(win, PAD, TAB_H + PAD, line, COL_FG, COL_BG);

    dobbg_UpdateAll(&s_bars, s_core_pct, ncpu);
    dobbg_Draw(&s_bars);
    for (int c = 0; c < strips; c++)
    {
        doblg_UpdateAll(&s_lg_core[c], s_hist_core[c], HIST_LEN);
        doblg_Draw(&s_lg_core[c]);
    }
}

static void draw_prio_overlay(void)
{
    uint32_t win = dobui_window();
    int h = PRIO_ROW * 4 + 30;
    s_prio_x = (s_w - PRIO_W) / 2;
    s_prio_y = (s_h - h) / 2;
    dobui_FillRect(win, s_prio_x, s_prio_y, PRIO_W, h, COL_OVL_BG);
    dobui_DrawRect(win, s_prio_x, s_prio_y, PRIO_W, h, COL_FG);
    char t[48];
    snprintf(t, sizeof(t), "Priorita' per PID %d:", (int)s_sel_pid);
    dobui_DrawText(win, s_prio_x + 10, s_prio_y + 8, t, COL_FG, COL_OVL_BG);
    for (int i = 0; i < 4; i++)
    {
        int ry = s_prio_y + 28 + i * PRIO_ROW;
        dobui_FillRect(win, s_prio_x + 4, ry, PRIO_W - 8, PRIO_ROW - 2,
                       COL_OVL_HI);
        dobui_DrawTextFixed(win, s_prio_x + 12, ry + 5,
                            PRIO_LABEL[i], COL_FG, COL_OVL_HI);
    }
}

/* Ridisegno completo della finestra (tab strip + tab corrente). */
static void redraw_all(void)
{
    uint32_t win = dobui_window();
    dobui_FillRect(win, 0, 0, s_w, s_h, COL_BG);
    draw_tabs();
    switch (s_tab)
    {
    case TAB_PROC: draw_tab_proc(); break;
    case TAB_CPU:  draw_tab_cpu();  break;
    case TAB_RAM:  draw_tab_ram();  break;
    case TAB_CORE: draw_tab_core(); break;
    }
    if (s_prio_open)
    {
        draw_prio_overlay();
    }
    dobui_Invalidate(win);
}

/* ===================================================================
 * ESECUTIVI — azioni
 * =================================================================== */

static void poll_and_redraw(void)
{
    if (s_prio_open)
    {
        return;                     /* fermo sotto l'overlay modale      */
    }
    s_prev_hdr   = s_snap.hdr;
    memcpy(s_prev_rows, s_snap.rows, sizeof(s_prev_rows));
    s_prev_nrows = s_nrows;

    int n = dob_task_snapshot(&s_snap);
    if (n < 0)
    {
        return;
    }
    s_nrows = n;

    compute_percentages();
    s_have_prev = true;
    push_history();
    sort_rows();
    redraw_all();
}

static void kill_selected(void)
{
    if (s_sel_pid <= 0)
    {
        return;                     /* PID 0 = kernel: intoccabile       */
    }
    char q[96];
    snprintf(q, sizeof(q), "Terminare il processo PID %d?", (int)s_sel_pid);
    if (dobpopup_YesNo("Gestione Attivita'", q) == 0)
    {
        syscall1(SYS_KILL, (int)s_sel_pid);
        poll_and_redraw();
    }
}

static void set_panel_for_tab(void)
{
    if (s_tab == TAB_PROC)
    {
        dobui_set_panel("Termina processo\nCambia priorita'\nAggiorna");
    }
    else
    {
        dobui_set_panel("Aggiorna");
    }
}

static void switch_tab(int t)
{
    if (t == s_tab || t < 0 || t >= TAB_COUNT)
    {
        return;
    }
    s_tab = t;
    s_prio_open = false;
    layout();
    set_panel_for_tab();
    redraw_all();
}

/* Click nell'overlay: righe -> renice; fuori -> chiudi. */
static void prio_overlay_click(int x, int y)
{
    for (int i = 0; i < 4; i++)
    {
        int ry = s_prio_y + 28 + i * PRIO_ROW;
        if (x >= s_prio_x + 4 && x < s_prio_x + PRIO_W - 4 &&
            y >= ry && y < ry + PRIO_ROW - 2)
        {
            syscall2(SYS_PROC_SET_PRIORITY, (int)s_sel_pid, i);
            break;
        }
    }
    s_prio_open = false;
    poll_and_redraw();              /* riscontro immediato (colonna Pri) */
}

/* ===================================================================
 * LOGICA — gli event handler orchestrano
 * =================================================================== */

void event_start(void)
{
    s_w = dobui_width();
    s_h = dobui_height();
    layout();
    set_panel_for_tab();
    poll_and_redraw();
}

void event_tick(void)
{
    poll_and_redraw();
}

void event_resize(int w, int h)
{
    s_w = w;
    s_h = h;
    layout();                       /* geometria sui NUOVI limiti        */
    redraw_all();
}

void event_panel(int cmd_idx)
{
    if (s_tab != TAB_PROC)
    {
        poll_and_redraw();          /* unico comando: Aggiorna           */
        return;
    }
    switch (cmd_idx)
    {
    case 0: kill_selected(); break;
    case 1:
        if (s_sel_pid > 0)
        {
            s_prio_open = true;
            redraw_all();
        }
        break;
    case 2: poll_and_redraw(); break;
    }
}

void event_mouseclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (s_prio_open)
    {
        prio_overlay_click(x, y);
        return;
    }
    if (y < TAB_H)
    {
        switch_tab(x / (s_w / TAB_COUNT));
        return;
    }
    if (s_tab == TAB_PROC && dobgrid_OnClick(&s_grid, x, y))
    {
        selection_to_pid();
        dobgrid_Draw(&s_grid);
        dobui_Invalidate(dobui_window());
    }
}

void event_mousemove(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (s_tab == TAB_PROC && !s_prio_open &&
        dobgrid_OnDrag(&s_grid, x, y))
    {
        dobgrid_Draw(&s_grid);
        dobui_Invalidate(dobui_window());
    }
}

void event_mouserelease(int x, int y, uint8_t buttons)
{
    (void)x; (void)y; (void)buttons;
    if (s_tab == TAB_PROC)
    {
        dobgrid_OnRelease(&s_grid);
    }
}

void event_key(uint8_t key)
{
    if (s_prio_open)
    {
        if (key >= '0' && key <= '3' && s_sel_pid > 0)
        {
            syscall2(SYS_PROC_SET_PRIORITY, (int)s_sel_pid, key - '0');
        }
        s_prio_open = false;
        poll_and_redraw();
        return;
    }
    if (key == KEY_DELETE && s_tab == TAB_PROC)
    {
        kill_selected();
        return;
    }
    if (s_tab == TAB_PROC && dobgrid_OnKey(&s_grid, key))
    {
        selection_to_pid();
        dobgrid_Draw(&s_grid);
        dobui_Invalidate(dobui_window());
    }
}

void event_scroll(int delta)
{
    if (s_tab == TAB_PROC && !s_prio_open &&
        dobgrid_OnScroll(&s_grid, delta))
    {
        dobgrid_Draw(&s_grid);
        dobui_Invalidate(dobui_window());
    }
}

int main(void)
{
    dobui_set_tick(300);            /* il polling E' il tick             */
    dobui_run("Gestione Attivita'", 640, 460);
    return 0;
}
