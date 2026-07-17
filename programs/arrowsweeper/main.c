/* MainDOB Arrow Sweeper — campo fiorito con indicatori direzionali
 * Click sx = scopri, click dx = bandiera, R = restart
 * Triangolini colorati indicano la direzione delle mine vicine.
 * Cardinali: raggio 3 — Diagonali: raggio 1.
 * Griglia centrata e scalabile: la cella si adatta alla finestra. */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <app.h>
#include <DobInterface.h>
#include <dob/types.h>

#define COLS        16
#define ROWS        16
#define NUM_MINES   40
#define SCAN_RANGE  3
#define MAX_ARROWS  3
#define HEADER_H    24
#define INIT_CELL   32
#define MIN_CELL    12
#define SCR_W  (COLS * INIT_CELL)
#define SCR_H  (ROWS * INIT_CELL + HEADER_H)

#define COL_BG_DARK     0x000A0A0C
#define COL_CELL_HIDE   0x00080810
#define COL_CELL_REV    0x00231E1E
#define COL_CELL_MINE   0x001E1E8C
#define COL_CELL_HOVER  0x002D2819
#define COL_GRID_HI     0x0000E6E6     /* Ciano chiaro */
#define COL_GRID_LO     0x00008A8A     /* Ciano scuro */
#define COL_FLAG_POLE   0x00B4B4B4
#define COL_FLAG_TRI    0x00F0DC00
#define COL_FLAG_BASE   0x00969696
#define COL_MINE_BODY   0x00DCDCDC
#define COL_MINE_GLOW   0x005050FF
#define COL_ARROW_0     0x00FF3030     /* Rosso vivo */
#define COL_ARROW_1     0x0000FF40     /* Verde acceso */
#define COL_ARROW_2     0x00FFFF00     /* Giallo pieno */
#define COL_HDR         0x00333333
#define COL_HDR_T       0x00FFFFFF

static const int DR[] = {-1, 1, 0, 0,-1,-1, 1, 1};
static const int DC[] = { 0, 0,-1, 1,-1, 1,-1, 1};
static const uint32_t ARROW_COLS[MAX_ARROWS] = {COL_ARROW_0, COL_ARROW_1, COL_ARROW_2};

typedef struct
{
    bool mine, revealed, flagged;
    int  narrows;
    int  adir[MAX_ARROWS];
} Cell;

typedef enum { ST_FIRST, ST_PLAY, ST_LOST, ST_WON } State;

static Cell     grid[ROWS][COLS];
static State    state;
static int      revealed_cnt;
static int      hover_r = -1, hover_c = -1;
static uint32_t win_id;
static int      win_w = SCR_W, win_h = SCR_H;

/* Computed layout — updated on every resize */
static int      cell_sz = INIT_CELL;
static int      grid_ox = 0;
static int      grid_oy = HEADER_H;

static int iabs(int x) { return x < 0 ? -x : x; }
static int imin(int a, int b) { return a < b ? a : b; }

static void recalc_layout(void)
{
    int avail_w = win_w;
    int avail_h = win_h - HEADER_H;
    cell_sz = imin(avail_w / COLS, avail_h / ROWS);
    if (cell_sz < MIN_CELL) cell_sz = MIN_CELL;
    int gw = cell_sz * COLS;
    int gh = cell_sz * ROWS;
    grid_ox = (win_w - gw) / 2;
    grid_oy = HEADER_H + (avail_h - gh) / 2;
    if (grid_ox < 0) grid_ox = 0;
    if (grid_oy < HEADER_H) grid_oy = HEADER_H;
}

static void new_game(void)
{
    memset(grid, 0, sizeof grid);
    state = ST_FIRST;
    revealed_cnt = 0;
}

static void place_mines(int sr, int sc)
{
    int placed = 0;
    while (placed < NUM_MINES)
    {
        int r = (int)(random_u32() % ROWS);
        int c = (int)(random_u32() % COLS);
        if (grid[r][c].mine) continue;
        if (iabs(r - sr) <= 1 && iabs(c - sc) <= 1) continue;
        grid[r][c].mine = true;
        placed++;
    }
    for (int r = 0; r < ROWS; r++)
    {
        for (int c = 0; c < COLS; c++)
        {
            if (grid[r][c].mine) continue;
            int hd[8], hs[8], nh = 0;
            for (int d = 0; d < 8; d++)
            {
                /* Cardinali (d=0..3): scan fino a SCAN_RANGE.
                 * Diagonali (d=4..7): solo cella adiacente. */
                int range = (d < 4) ? SCAN_RANGE : 1;
                for (int s = 1; s <= range; s++)
                {
                    int nr = r + DR[d] * s, nc = c + DC[d] * s;
                    if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS) break;
                    if (grid[nr][nc].mine)
                    {
                        hd[nh] = d;
                        hs[nh] = s;
                        nh++;
                        break;
                    }
                }
            }
            for (int a = 0; a < nh - 1; a++)
            {
                for (int b = a + 1; b < nh; b++)
                {
                    if (hs[b] < hs[a])
                    {
                        int t;
                        t = hd[a]; hd[a] = hd[b]; hd[b] = t;
                        t = hs[a]; hs[a] = hs[b]; hs[b] = t;
                    }
                }
            }
            int n = nh < MAX_ARROWS ? nh : MAX_ARROWS;
            grid[r][c].narrows = n;
            for (int i = 0; i < n; i++)
                grid[r][c].adir[i] = hd[i];
        }
    }
}

static void flood(int r, int c)
{
    if (r < 0 || r >= ROWS || c < 0 || c >= COLS) return;
    Cell *cl = &grid[r][c];
    if (cl->revealed || cl->flagged || cl->mine) return;
    cl->revealed = true;
    revealed_cnt++;
    if (cl->narrows == 0)
    {
        for (int d = 0; d < 8; d++)
            flood(r + DR[d], c + DC[d]);
    }
}

static void click_cell(int r, int c)
{
    Cell *cl = &grid[r][c];
    if (cl->flagged || cl->revealed) return;
    if (cl->mine)
    {
        state = ST_LOST;
        for (int rr = 0; rr < ROWS; rr++)
            for (int cc = 0; cc < COLS; cc++)
                if (grid[rr][cc].mine)
                    grid[rr][cc].revealed = true;
    }
    else
    {
        flood(r, c);
        if (revealed_cnt == ROWS * COLS - NUM_MINES)
            state = ST_WON;
    }
}

/* ── Drawing ── */

static void hline(int x, int y, int w, uint32_t col)
{
    if (w > 0)
        dobui_FillRect(win_id, x, y, w, 1, col);
}

static void fill_poly(int *px, int *py, int n, uint32_t col)
{
    int minY = py[0], maxY = py[0];
    for (int i = 1; i < n; i++)
    {
        if (py[i] < minY) minY = py[i];
        if (py[i] > maxY) maxY = py[i];
    }
    for (int y = minY; y <= maxY; y++)
    {
        int xs[16], nx = 0;
        for (int i = 0; i < n; i++)
        {
            int j = (i + 1) % n, y0 = py[i], y1 = py[j];
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y))
            {
                if (nx < 16)
                    xs[nx++] = px[i] + (y - y0) * (px[j] - px[i]) / (y1 - y0);
            }
        }
        for (int a = 0; a < nx - 1; a++)
            for (int b = a + 1; b < nx; b++)
                if (xs[a] > xs[b])
                {
                    int t = xs[a];
                    xs[a] = xs[b];
                    xs[b] = t;
                }
        for (int k = 0; k + 1 < nx; k += 2)
            hline(xs[k], y, xs[k + 1] - xs[k] + 1, col);
    }
}

/* Draw a small filled triangle at the edge/corner of the cell
 * in the direction indicated by dir (index into DR/DC).
 *
 * Cardinal dirs → isoceles triangle centered on the edge
 * Diagonal dirs → right triangle in the corner
 *
 * dir:  0=up  1=down  2=left  3=right
 *       4=up-left  5=up-right  6=down-left  7=down-right */
static void draw_indicator(int x, int y, int dir, uint32_t col)
{
    int cs = cell_sz;
    int ts = cs / 3;        /* Triangle size */
    if (ts < 3) ts = 3;
    int m = 2;              /* Margin from cell edge */

    int px[3], py[3];

    switch (dir)
    {
        case 0: /* Up — triangle pointing UP (mine is above) */
            px[0] = x + cs / 2 - ts;  py[0] = y + m + ts;
            px[1] = x + cs / 2 + ts;  py[1] = y + m + ts;
            px[2] = x + cs / 2;       py[2] = y + m;
            break;
        case 1: /* Down — triangle pointing DOWN (mine is below) */
            px[0] = x + cs / 2 - ts;  py[0] = y + cs - m - ts;
            px[1] = x + cs / 2 + ts;  py[1] = y + cs - m - ts;
            px[2] = x + cs / 2;       py[2] = y + cs - m;
            break;
        case 2: /* Left — triangle pointing LEFT (mine is left) */
            px[0] = x + m + ts;  py[0] = y + cs / 2 - ts;
            px[1] = x + m + ts;  py[1] = y + cs / 2 + ts;
            px[2] = x + m;       py[2] = y + cs / 2;
            break;
        case 3: /* Right — triangle pointing RIGHT (mine is right) */
            px[0] = x + cs - m - ts;  py[0] = y + cs / 2 - ts;
            px[1] = x + cs - m - ts;  py[1] = y + cs / 2 + ts;
            px[2] = x + cs - m;       py[2] = y + cs / 2;
            break;
        case 4: /* Up-Left — top-left corner */
            px[0] = x + m;       py[0] = y + m;
            px[1] = x + m + ts;  py[1] = y + m;
            px[2] = x + m;       py[2] = y + m + ts;
            break;
        case 5: /* Up-Right — top-right corner */
            px[0] = x + cs - m;       py[0] = y + m;
            px[1] = x + cs - m - ts;  py[1] = y + m;
            px[2] = x + cs - m;       py[2] = y + m + ts;
            break;
        case 6: /* Down-Left — bottom-left corner */
            px[0] = x + m;       py[0] = y + cs - m;
            px[1] = x + m + ts;  py[1] = y + cs - m;
            px[2] = x + m;       py[2] = y + cs - m - ts;
            break;
        case 7: /* Down-Right — bottom-right corner */
            px[0] = x + cs - m;       py[0] = y + cs - m;
            px[1] = x + cs - m - ts;  py[1] = y + cs - m;
            px[2] = x + cs - m;       py[2] = y + cs - m - ts;
            break;
        default:
            return;
    }

    fill_poly(px, py, 3, col);
}

static void draw_mine(int cx, int cy)
{
    int rad = cell_sz / 5;
    if (rad < 2) rad = 2;
    for (int dy = -rad; dy <= rad; dy++)
        for (int dx = -rad; dx <= rad; dx++)
            if (dx * dx + dy * dy <= rad * rad)
                dobui_DrawPixel(win_id, cx + dx, cy + dy, COL_MINE_BODY);
    int sp = rad + 2;
    for (int i = -sp; i <= sp; i++)
    {
        dobui_DrawPixel(win_id, cx + i, cy, COL_MINE_BODY);
        dobui_DrawPixel(win_id, cx, cy + i, COL_MINE_BODY);
    }
    int rr = rad / 3;
    if (rr < 1) rr = 1;
    for (int dy = -rr; dy <= rr; dy++)
        for (int dx = -rr; dx <= rr; dx++)
            if (dx * dx + dy * dy <= rr * rr)
                dobui_DrawPixel(win_id, cx - rad / 3 + dx, cy - rad / 3 + dy, COL_MINE_GLOW);
}

static void draw_flag(int cx, int cy)
{
    int h = cell_sz / 3;
    if (h < 3) h = 3;
    int pw = cell_sz > 20 ? 2 : 1;
    dobui_FillRect(win_id, cx, cy - h, pw, h * 2, COL_FLAG_POLE);
    int tw = cell_sz / 4;
    if (tw < 4) tw = 4;
    int th = cell_sz / 3;
    if (th < 4) th = 4;
    int tpx[3] = {cx + pw, cx + pw + tw, cx + pw};
    int tpy[3] = {cy - h, cy - h + th / 2, cy - h + th};
    fill_poly(tpx, tpy, 3, COL_FLAG_TRI);
    int bw = cell_sz / 3;
    if (bw < 4) bw = 4;
    dobui_FillRect(win_id, cx - bw / 2, cy + h - 2, bw, 2, COL_FLAG_BASE);
}

static void draw(void)
{
    if (dobui_IsResizePending()) return;

    dobui_FillRect(win_id, 0, 0, win_w, win_h, COL_BG_DARK);

    /* Header */
    dobui_FillRect(win_id, 0, 0, win_w, HEADER_H, COL_HDR);
    char hdr[64];
    int fl = 0;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (grid[r][c].flagged) fl++;
    const char *s = state == ST_LOST ? "PERSO!"
                  : state == ST_WON  ? "VINTO!"
                  : "Arrow Sweeper";
    snprintf(hdr, sizeof hdr, " %s   Mine: %d/%d", s, fl, NUM_MINES);
    dobui_DrawText(win_id, 4, 4, hdr, COL_HDR_T, COL_HDR);

    /* Grid (centered) */
    for (int r = 0; r < ROWS; r++)
    {
        for (int c = 0; c < COLS; c++)
        {
            int x = grid_ox + c * cell_sz;
            int y = grid_oy + r * cell_sz;
            Cell *cl = &grid[r][c];

            uint32_t bg = cl->revealed
                ? (cl->mine ? COL_CELL_MINE : COL_CELL_REV)
                : (r == hover_r && c == hover_c && state <= ST_PLAY)
                    ? COL_CELL_HOVER : COL_CELL_HIDE;

            dobui_FillRect(win_id, x + 1, y + 1, cell_sz - 2, cell_sz - 2, bg);

            if (!cl->revealed)
            {
                hline(x, y, cell_sz, COL_GRID_HI);
                dobui_FillRect(win_id, x, y, 1, cell_sz, COL_GRID_HI);
                hline(x, y + cell_sz - 1, cell_sz, COL_GRID_LO);
                dobui_FillRect(win_id, x + cell_sz - 1, y, 1, cell_sz, COL_GRID_LO);
            }

            int cxc = x + cell_sz / 2;
            int cyc = y + cell_sz / 2;

            if (cl->revealed)
            {
                if (cl->mine)
                    draw_mine(cxc, cyc);
                else
                    for (int i = 0; i < cl->narrows; i++)
                        draw_indicator(x, y, cl->adir[i], ARROW_COLS[i]);
            }
            else if (cl->flagged)
            {
                draw_flag(cxc, cyc);
            }
        }
    }

    if (state == ST_LOST || state == ST_WON)
    {
        uint32_t oc = state == ST_LOST ? 0x00000060 : 0x00006000;
        for (int y = grid_oy; y < grid_oy + ROWS * cell_sz; y += 2)
            hline(grid_ox, y, COLS * cell_sz, oc);
    }

    dobui_Invalidate(win_id);
}

/* Convert pixel to grid cell, accounting for centering */
static bool pixel_to_cell(int px, int py, int *out_r, int *out_c)
{
    int lx = px - grid_ox;
    int ly = py - grid_oy;
    if (lx < 0 || ly < 0) return false;
    int gc = lx / cell_sz;
    int gr = ly / cell_sz;
    if (gc >= COLS || gr >= ROWS) return false;
    *out_r = gr;
    *out_c = gc;
    return true;
}

/* Event handlers */

void event_mouseclick(int mx, int my, uint8_t buttons)
{
    (void)buttons;
    int gr, gc;
    if (pixel_to_cell(mx, my, &gr, &gc))
    {
        hover_r = gr;
        hover_c = gc;

        if (state == ST_FIRST)
        {
            place_mines(gr, gc);
            state = ST_PLAY;
        }
        if (state == ST_PLAY)
            click_cell(gr, gc);
    }
    draw();
}

void event_rightclick(int mx, int my, uint8_t buttons)
{
    (void)buttons;
    int gr, gc;
    if (pixel_to_cell(mx, my, &gr, &gc))
    {
        hover_r = gr;
        hover_c = gc;

        if (state == ST_PLAY && !grid[gr][gc].revealed)
            grid[gr][gc].flagged = !grid[gr][gc].flagged;
    }
    draw();
}

void event_key(uint8_t k)
{
    if (k == 'r' || k == 'R')
    {
        new_game();
        draw();
    }
    else if (k == 'f' || k == 'F')
    {
        if (state == ST_PLAY
            && hover_r >= 0 && hover_r < ROWS
            && hover_c >= 0 && hover_c < COLS
            && !grid[hover_r][hover_c].revealed)
        {
            grid[hover_r][hover_c].flagged = !grid[hover_r][hover_c].flagged;
            draw();
        }
    }
}

void event_panel(int cmd_idx)
{
    (void)cmd_idx;
    new_game();
    draw();
}

void event_resize(int w, int h)
{
    win_w = w;
    win_h = h;
    recalc_layout();
    draw();
}

void event_start(void)
{
    win_id = dobui_window();
    draw();
}

int main(void)
{
    recalc_layout();
    new_game();
    dobui_set_panel("Nuova partita");
    dobui_run("Arrow Sweeper", SCR_W, SCR_H);
    return 0;
}
