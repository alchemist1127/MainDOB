/* MainDOB Minesweeper — Scalable
 * 16x16 grid, 40 mines. Click to reveal, arrows+Enter or mouse. */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <app.h>
#include <DobInterface.h>
#include <dob/types.h>

#define GRID_W      16
#define GRID_H      16
#define MINE_COUNT  40
#define HEADER_H    24

#include <dobui_theme.h>
#define COL_BG      DOBUI_SURFACE
#define COL_HIDDEN  DOBUI_RELIEF
#define COL_REVEAL  DOBUI_INSET
#define COL_MINE    DOBUI_DANGER
#define COL_FLAG    DOBUI_INPUT
#define COL_TEXT    DOBUI_TEXT_ALT
#define COL_HDR     DOBUI_RELIEF
#define COL_HDR_T   DOBUI_TEXT_ALT
#define COL_GRID    DOBUI_INSET
#define COL_CURSOR  DOBUI_INPUT
#define COL_DEAD    DOBUI_DANGER
#define COL_WIN_BG  DOBUI_SUCCESS

static const uint32_t NUM_COLORS[9] = {
    0x00000000, 0x0000FFFF, 0x0000CC33, 0x00FFE000,
    0x0000AAFF, 0x00FF3333, 0x00FF44AA, 0x00FFFFFF, 0x00556699  /* numeri brillanti su nero (1..8) */
};

static uint32_t win_id;
static int win_w = 340, win_h = 364;

typedef struct
{
    bool mine;
    bool revealed;
    bool flagged;
    int  adj;
} cell_t;

static cell_t grid[GRID_H][GRID_W];
static int cursor_x, cursor_y;
static bool game_over, game_won;
static bool first_click;
static int revealed_count;
static uint32_t rng_state;

static int cell_size(void)
{
    int cw = win_w / GRID_W;
    int ch = (win_h - HEADER_H) / GRID_H;
    int c = (cw < ch) ? cw : ch;
    return (c < 8) ? 8 : c;
}

static uint32_t rng(void)
{
    rng_state = rng_state * 1103515245 + 12345;
    return (rng_state >> 16) & 0x7FFF;
}

static void count_adjacent(void)
{
    for (int y = 0; y < GRID_H; y++)
    {
        for (int x = 0; x < GRID_W; x++)
        {
            int count = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    int nx = x+dx, ny = y+dy;
                    if (nx >= 0 && nx < GRID_W && ny >= 0 && ny < GRID_H)
                        if (grid[ny][nx].mine) count++;
                }
            grid[y][x].adj = count;
        }
    }
}

static void place_mines(int safe_x, int safe_y)
{
    int placed = 0;
    while (placed < MINE_COUNT)
    {
        int x = (int)(rng() % GRID_W);
        int y = (int)(rng() % GRID_H);
        if (grid[y][x].mine) continue;
        int dx = x - safe_x, dy = y - safe_y;
        if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1) continue;
        grid[y][x].mine = true;
        placed++;
    }
    count_adjacent();
}

static void init_game(void)
{
    memset(grid, 0, sizeof(grid));
    cursor_x = GRID_W / 2;
    cursor_y = GRID_H / 2;
    game_over = false;
    game_won = false;
    first_click = true;
    revealed_count = 0;
    get_random(&rng_state, sizeof(rng_state));
}

static void flood_reveal(int x, int y)
{
    if (x < 0 || x >= GRID_W || y < 0 || y >= GRID_H) return;
    if (grid[y][x].revealed || grid[y][x].flagged || grid[y][x].mine) return;

    grid[y][x].revealed = true;
    revealed_count++;

    if (grid[y][x].adj == 0)
    {
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                flood_reveal(x+dx, y+dy);
    }
}

static void reveal(int x, int y)
{
    if (game_over || game_won) return;
    if (x < 0 || x >= GRID_W || y < 0 || y >= GRID_H) return;
    if (grid[y][x].revealed || grid[y][x].flagged) return;

    if (first_click)
    {
        place_mines(x, y);
        first_click = false;
    }

    if (grid[y][x].mine)
    {
        game_over = true;
        /* Reveal all mines */
        for (int yy = 0; yy < GRID_H; yy++)
            for (int xx = 0; xx < GRID_W; xx++)
                if (grid[yy][xx].mine)
                    grid[yy][xx].revealed = true;
        return;
    }

    flood_reveal(x, y);

    /* Check win */
    if (revealed_count >= GRID_W * GRID_H - MINE_COUNT)
        game_won = true;
}

static void toggle_flag(int x, int y)
{
    if (game_over || game_won) return;
    if (x < 0 || x >= GRID_W || y < 0 || y >= GRID_H) return;
    if (grid[y][x].revealed) return;
    grid[y][x].flagged = !grid[y][x].flagged;
}

static void draw(void)
{
    int cs = cell_size();
    int gw = GRID_W * cs, gh = GRID_H * cs;
    int ox = (win_w - gw) / 2;
    int oy = HEADER_H;

    /* Header */
    dobui_FillRect(win_id, 0, 0, win_w, HEADER_H, COL_HDR);
    char info[64];
    int flags = 0;
    for (int y = 0; y < GRID_H; y++)
        for (int x = 0; x < GRID_W; x++)
            if (grid[y][x].flagged) flags++;
    snprintf(info, sizeof(info), " Mine: %d/%d  Rivelate: %d",
             flags, MINE_COUNT, revealed_count);
    dobui_DrawText(win_id, 4, 4, info, COL_HDR_T, COL_HDR);

    /* Background */
    dobui_FillRect(win_id, 0, oy, win_w, win_h - oy, COL_BG);

    /* Grid */
    for (int y = 0; y < GRID_H; y++)
    {
        for (int x = 0; x < GRID_W; x++)
        {
            int px = ox + x * cs, py = oy + y * cs;
            cell_t *c = &grid[y][x];

            if (c->revealed)
            {
                if (c->mine)
                {
                    dobui_FillRect(win_id, px+1, py+1, cs-2, cs-2, COL_MINE);
                }
                else
                {
                    dobui_FillRect(win_id, px+1, py+1, cs-2, cs-2, COL_REVEAL);
                    if (c->adj > 0)
                    {
                        char num[2] = { '0' + (char)c->adj, '\0' };
                        int tx = px + (cs - 8) / 2;
                        int ty = py + (cs - 16) / 2;
                        dobui_DrawText(win_id, tx, ty, num,
                                       NUM_COLORS[c->adj], COL_REVEAL);
                    }
                }
            }
            else
            {
                dobui_FillRect(win_id, px+1, py+1, cs-2, cs-2, COL_HIDDEN);
                if (c->flagged)
                {
                    /* Flag marker */
                    dobui_FillRect(win_id, px+cs/3, py+cs/4, cs/3, cs/2, COL_FLAG);
                }
            }
            dobui_DrawRect(win_id, px, py, cs, cs, COL_GRID);
        }
    }

    /* Cursor */
    int cx = ox + cursor_x * cs, cy = oy + cursor_y * cs;
    dobui_DrawRect(win_id, cx, cy, cs, cs, COL_CURSOR);
    dobui_DrawRect(win_id, cx+1, cy+1, cs-2, cs-2, COL_CURSOR);

    /* Game over / win overlay */
    if (game_over)
    {
        int gx = win_w/2 - 80, gy = oy + gh/2 - 16;
        dobui_FillRect(win_id, gx, gy, 160, 32, COL_DEAD);
        dobui_DrawText(win_id, gx+8, gy+8, "GAME OVER!", COL_HDR_T, COL_DEAD);
    }
    else if (game_won)
    {
        int gx = win_w/2 - 80, gy = oy + gh/2 - 16;
        dobui_FillRect(win_id, gx, gy, 160, 32, COL_WIN_BG);
        dobui_DrawText(win_id, gx+8, gy+8, "HAI VINTO!", COL_HDR_T, COL_WIN_BG);
    }

    dobui_Invalidate(win_id);
}

/* Event handlers */

void event_key(uint8_t k)
{
    if (k == KEY_UP || k == 'w' || k == 'W')
    {
        if (cursor_y > 0) cursor_y--;
    }
    else if (k == KEY_DOWN || k == 's' || k == 'S')
    {
        if (cursor_y < GRID_H - 1) cursor_y++;
    }
    else if (k == KEY_LEFT || k == 'a' || k == 'A')
    {
        if (cursor_x > 0) cursor_x--;
    }
    else if (k == KEY_RIGHT || k == 'd' || k == 'D')
    {
        if (cursor_x < GRID_W - 1) cursor_x++;
    }
    else if (k == '\n' || k == ' ')
    {
        reveal(cursor_x, cursor_y);
    }
    else if (k == 'f' || k == 'F')
    {
        toggle_flag(cursor_x, cursor_y);
    }
    draw();
}

static void grid_coords(int lx, int ly, int *gx, int *gy)
{
    int cs = cell_size();
    int ox = (win_w - GRID_W * cs) / 2;
    int oy = HEADER_H;
    *gx = (lx - ox) / cs;
    *gy = (ly - oy) / cs;
}

void event_mouseclick(int lx, int ly, uint8_t buttons)
{
    (void)buttons;
    int gx, gy;
    grid_coords(lx, ly, &gx, &gy);
    if (gx >= 0 && gx < GRID_W && gy >= 0 && gy < GRID_H)
    {
        cursor_x = gx;
        cursor_y = gy;
        reveal(gx, gy);
    }
    draw();
}

void event_rightclick(int lx, int ly, uint8_t buttons)
{
    (void)buttons;
    int gx, gy;
    grid_coords(lx, ly, &gx, &gy);
    if (gx >= 0 && gx < GRID_W && gy >= 0 && gy < GRID_H)
    {
        cursor_x = gx;
        cursor_y = gy;
        toggle_flag(gx, gy);
    }
    draw();
}

void event_panel(int cmd_idx)
{
    (void)cmd_idx;
    init_game();
    draw();
}

void event_resize(int w, int h)
{
    win_w = w;
    win_h = h;
    draw();
}

void event_start(void)
{
    win_id = dobui_window();
    draw();
}

int main(void)
{
    init_game();
    dobui_set_panel("Nuova partita");
    dobui_run("Minesweeper", 340, 364);
    return 0;
}
