/* griddemo — DataGrid widget demonstration.
 *
 * Fills a dob_grid_t with dummy data to exercise every part of the
 * N-column grid:
 *   - a FROZEN ID column (always visible on the left);
 *   - resizable columns (drag the dividers between headers);
 *   - vertical scrolling (mouse wheel, Up/Down, PgUp/PgDn);
 *   - column-step horizontal scrolling (Left/Right, or click a column
 *     that is clipped at the right edge);
 *   - the ACTIVATE seam (double-click or Enter on a cell), which here
 *     just reports the cell in the status line.
 *
 * In the real DDC database app, on_activate() is where the host opens
 * the type-specific editor (textbox / dropdown / checkbox / date) over
 * dobgrid_CellRect() and writes the result back into its model. This
 * demo stops at the seam on purpose.
 */

#include <dob/types.h>
#include <stdio.h>          /* snprintf */
#include <DobInterface.h>
#include "app.h"
#include "grid.h"

#define MARGIN  10
#define TOPBAR  36          /* status strip above the grid */

/* Columns — caller-owned; the grid writes back `width` when a divider
 * is dragged. Widths sum to more than the default window content
 * width, so the frozen ID column and horizontal scrolling are both
 * exercised out of the box. */
static dobgrid_col_t cols[] = {
    { "ID",    48,  DOBGRID_ALIGN_RIGHT },
    { "Nome",  130, DOBGRID_ALIGN_LEFT  },
    { "Citta", 120, DOBGRID_ALIGN_LEFT  },
    { "Email", 210, DOBGRID_ALIGN_LEFT  },
    { "Eta",   56,  DOBGRID_ALIGN_RIGHT },
    { "Ruolo", 140, DOBGRID_ALIGN_LEFT  },
};
#define NCOLS ((int)(sizeof(cols) / sizeof(cols[0])))

/* Cells — flat, row-major, stride = NCOLS. Declared as an array of
 * `const char *const`, so it decays cleanly to the `const char *const *`
 * the grid expects (no qualifier-conversion warning). Caller-owned and
 * alive for the whole program lifetime, as the grid references it. */
static const char *const cells[] = {
/*   ID     Nome       Citta       Email             Eta   Ruolo             */
    "1",  "Anna",    "Torino",   "anna@dob.io",    "31", "Sviluppatrice",
    "2",  "Bruno",   "Milano",   "bruno@dob.io",   "27", "Designer",
    "3",  "Carla",   "Roma",     "carla@dob.io",   "44", "Project manager",
    "4",  "Dario",   "Napoli",   "dario@dob.io",   "38", "Sysadmin",
    "5",  "Elena",   "Firenze",  "elena@dob.io",   "29", "QA",
    "6",  "Fabio",   "Bologna",  "fabio@dob.io",   "51", "Architetto",
    "7",  "Giada",   "Venezia",  "giada@dob.io",   "33", "Data analyst",
    "8",  "Hugo",    "Genova",   "hugo@dob.io",    "26", "Stagista",
    "9",  "Ilaria",  "Palermo",  "ilaria@dob.io",  "40", "Team lead",
    "10", "Luca",    "Bari",     "luca@dob.io",    "35", "Backend",
    "11", "Marta",   "Verona",   "marta@dob.io",   "28", "Frontend",
    "12", "Nicola",  "Trieste",  "nicola@dob.io",  "47", "DBA",
    "13", "Olga",    "Padova",   "olga@dob.io",    "30", "UX writer",
    "14", "Paolo",   "Cagliari", "paolo@dob.io",   "42", "DevOps",
    "15", "Rita",    "Perugia",  "rita@dob.io",    "36", "Security",
    "16", "Sergio",  "Ancona",   "sergio@dob.io",  "39", "Support",
};
#define NROWS ((int)(sizeof(cells) / sizeof(cells[0]) / NCOLS))

static dob_grid_t g;
static char status[160] =
    "Frecce/rotella: naviga | trascina i bordi colonna | doppio-click o Invio";

static void redraw(void)
{
    uint32_t win = dobui_window();
    int W = dobui_width();
    int H = dobui_height();

    dobui_FillRect(win, 0, 0, W, H, DOBUI_SURFACE);         /* window bg    */
    dobui_DrawText(win, MARGIN, 10, status, DOBUI_TEXT, DOBUI_SURFACE);
    dobgrid_Draw(&g);
    dobui_Invalidate(win);
}

/* The editing seam. In the DB app this opens the right editor for the
 * column's type over dobgrid_CellRect(); here it just reports the
 * activated cell in the status line. */
static void on_activate(dob_grid_t *gg, int row, int col)
{
    const char *v = dobgrid_CellAt(gg, row, col);
    snprintf(status, sizeof(status),
             "Attivata: riga %d, colonna \"%s\" = \"%s\"",
             row, cols[col].title, v ? v : "");
}

void event_start(void)
{
    dobgrid_Init(&g, dobui_window(),
                 MARGIN, TOPBAR,
                 dobui_width()  - 2 * MARGIN,
                 dobui_height() - TOPBAR - MARGIN);
    dobgrid_SetColumns(&g, cols, NCOLS);
    dobgrid_SetRows(&g, cells, NROWS);
    dobgrid_SetFrozenColumns(&g, 1);        /* pin the ID column */
    dobgrid_SetSelectable(&g, true);
    dobgrid_SetActivate(&g, on_activate, NULL);
    dobgrid_SetFocus(&g, true);
    dobui_set_panel("Esci");
    redraw();
}

void event_key(uint8_t key)
{
    if (!dobgrid_OnKey(&g, key))
    {
        if (key == 'q' || key == 27) { dobui_quit(); return; }   /* q / ESC */
    }
    redraw();
}

void event_mouseclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    dobgrid_OnClick(&g, x, y);
    redraw();
}

void event_dblclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    dobgrid_OnDblClick(&g, x, y);
    redraw();
}

void event_mouserelease(int x, int y, uint8_t buttons)
{
    (void)x; (void)y; (void)buttons;
    dobgrid_OnRelease(&g);
    redraw();
}

void event_mousemove(int x, int y, uint8_t buttons)     /* drag only */
{
    (void)buttons;
    dobgrid_OnDrag(&g, x, y);
    redraw();
}

void event_scroll(int delta)
{
    dobgrid_OnScroll(&g, delta);
    redraw();
}

void event_resize(int w, int h)
{
    g.w = w - 2 * MARGIN;
    g.h = h - TOPBAR - MARGIN;
    if (g.w < 0) g.w = 0;
    if (g.h < 0) g.h = 0;
    redraw();
}

void event_panel(int cmd_idx)
{
    if (cmd_idx == 0) dobui_quit();          /* "Esci" */
}

int main(void)
{
    dobui_run("Grid Demo", 560, 380);
    return 0;
}
