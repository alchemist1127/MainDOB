/* MainDOB Tetris — Event-driven
 * WASD or arrows to move, W/Up to rotate, Space for hard drop. */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <app.h>
#include <DobInterface.h>
#include <dob/types.h>

#define FIELD_W     10
#define FIELD_H     20
#define HEADER_H    24
#define SIDE_CHARS  12

#define COL_BG      0x00111111
#define COL_GRID    0x00222222
#define COL_TEXT    0x00FFFFFF
#define COL_HDR     0x00333333
#define COL_SIDE    0x00181818
#define COL_DEAD    0x000000AA

static const int8_t PIECES[7][4][4][2] =
{
    {{{0,-1},{0,0},{0,1},{0,2}},{{-1,0},{0,0},{1,0},{2,0}},
     {{0,-1},{0,0},{0,1},{0,2}},{{-1,0},{0,0},{1,0},{2,0}}},
    {{{0,0},{1,0},{0,1},{1,1}},{{0,0},{1,0},{0,1},{1,1}},
     {{0,0},{1,0},{0,1},{1,1}},{{0,0},{1,0},{0,1},{1,1}}},
    {{{-1,0},{0,0},{1,0},{0,-1}},{{0,-1},{0,0},{0,1},{1,0}},
     {{-1,0},{0,0},{1,0},{0,1}},{{0,-1},{0,0},{0,1},{-1,0}}},
    {{{0,0},{1,0},{-1,1},{0,1}},{{0,-1},{0,0},{1,0},{1,1}},
     {{0,0},{1,0},{-1,1},{0,1}},{{0,-1},{0,0},{1,0},{1,1}}},
    {{{-1,0},{0,0},{0,1},{1,1}},{{1,-1},{1,0},{0,0},{0,1}},
     {{-1,0},{0,0},{0,1},{1,1}},{{1,-1},{1,0},{0,0},{0,1}}},
    {{{-1,-1},{-1,0},{0,0},{1,0}},{{0,-1},{1,-1},{0,0},{0,1}},
     {{-1,0},{0,0},{1,0},{1,1}},{{0,-1},{0,0},{-1,1},{0,1}}},
    {{{-1,0},{0,0},{1,0},{1,-1}},{{0,-1},{0,0},{0,1},{1,1}},
     {{-1,1},{-1,0},{0,0},{1,0}},{{-1,-1},{0,-1},{0,0},{0,1}}}
};

static const uint32_t PIECE_COLORS[7] =
{
    0x00FFFF00, 0x0000FFFF, 0x00FF00FF, 0x0000FF00,
    0x000000FF, 0x00FF0000, 0x000088FF
};

static uint8_t field[FIELD_H][FIELD_W];
static int cur_piece, cur_rot, cur_x, cur_y;
static int next_piece;
static int score, lines, level;
static bool game_over;
static uint32_t rng_state;

static int cell_size(void)
{
    int side_w = SIDE_CHARS * 8;
    int cw = (dobui_width() - side_w) / FIELD_W;
    int ch = (dobui_height() - HEADER_H) / FIELD_H;
    int c = (cw < ch) ? cw : ch;
    return (c < 6) ? 6 : c;
}

static uint32_t rng(void)
{
    rng_state = rng_state * 1103515245 + 12345;
    return (rng_state >> 16) & 0x7FFF;
}

static bool fits(int piece, int rot, int px, int py)
{
    for (int i = 0; i < 4; i++)
    {
        int x = px + PIECES[piece][rot][i][0];
        int y = py + PIECES[piece][rot][i][1];
        if (x < 0 || x >= FIELD_W || y >= FIELD_H) return false;
        if (y >= 0 && field[y][x]) return false;
    }
    return true;
}

static void lock_piece(void)
{
    for (int i = 0; i < 4; i++)
    {
        int x = cur_x + PIECES[cur_piece][cur_rot][i][0];
        int y = cur_y + PIECES[cur_piece][cur_rot][i][1];
        if (y >= 0 && y < FIELD_H && x >= 0 && x < FIELD_W)
            field[y][x] = (uint8_t)(cur_piece + 1);
    }
}

static int clear_lines(void)
{
    int cleared = 0;
    for (int y = FIELD_H - 1; y >= 0; y--)
    {
        bool full = true;
        for (int x = 0; x < FIELD_W; x++)
        {
            if (!field[y][x]) { full = false; break; }
        }
        if (full)
        {
            cleared++;
            for (int yy = y; yy > 0; yy--)
                memcpy(field[yy], field[yy - 1], FIELD_W);
            memset(field[0], 0, FIELD_W);
            y++;
        }
    }
    return cleared;
}

static void spawn_piece(void)
{
    cur_piece = next_piece;
    next_piece = (int)(rng() % 7);
    cur_rot = 0;
    cur_x = FIELD_W / 2;
    cur_y = 0;
    if (!fits(cur_piece, cur_rot, cur_x, cur_y))
        game_over = true;
}

static void init_game(void)
{
    memset(field, 0, sizeof(field));
    score = 0; lines = 0; level = 1;
    game_over = false;
    get_random(&rng_state, sizeof(rng_state));
    next_piece = (int)(rng() % 7);
    spawn_piece();
}

static void do_clear_score(void)
{
    int cl = clear_lines();
    if (cl > 0)
    {
        int pts[] = {0, 100, 300, 500, 800};
        score += pts[cl] * level;
        lines += cl;
        level = lines / 10 + 1;
    }
}

static void draw(void)
{
    uint32_t wid = dobui_window();
    int w = dobui_width();
    int h = dobui_height();
    int cs = cell_size();
    int fw = FIELD_W * cs;
    int fh = FIELD_H * cs;
    int side_w = w - fw;
    int fy = HEADER_H;

    dobui_FillRect(wid, 0, 0, w, HEADER_H, COL_HDR);
    char info[64];
    snprintf(info, sizeof(info), " Tetris  P:%d L:%d Lv:%d", score, lines, level);
    dobui_DrawText(wid, 4, 4, info, COL_TEXT, COL_HDR);

    dobui_FillRect(wid, 0, fy, fw, fh, COL_BG);

    for (int y = 0; y < FIELD_H; y++)
    {
        for (int x = 0; x < FIELD_W; x++)
        {
            if (field[y][x])
                dobui_FillRect(wid, x * cs + 1, fy + y * cs + 1, cs - 2, cs - 2,
                               PIECE_COLORS[field[y][x] - 1]);
            else
                dobui_DrawRect(wid, x * cs, fy + y * cs, cs, cs, COL_GRID);
        }
    }

    if (!game_over)
    {
        uint32_t c = PIECE_COLORS[cur_piece];
        for (int i = 0; i < 4; i++)
        {
            int x = cur_x + PIECES[cur_piece][cur_rot][i][0];
            int y = cur_y + PIECES[cur_piece][cur_rot][i][1];
            if (y >= 0 && y < FIELD_H && x >= 0 && x < FIELD_W)
                dobui_FillRect(wid, x * cs + 1, fy + y * cs + 1, cs - 2, cs - 2, c);
        }
    }

    dobui_FillRect(wid, fw, fy, side_w, fh, COL_SIDE);
    int sx = fw + 8;
    dobui_DrawText(wid, sx, fy + 8, "Prossimo:", COL_TEXT, COL_SIDE);

    int pcs = cs < 12 ? cs : 12;
    uint32_t nc = PIECE_COLORS[next_piece];
    for (int i = 0; i < 4; i++)
    {
        int px = sx + 20 + PIECES[next_piece][0][i][0] * pcs;
        int py = fy + 36 + PIECES[next_piece][0][i][1] * pcs;
        dobui_FillRect(wid, px, py, pcs - 2, pcs - 2, nc);
    }

    if (game_over)
    {
        int gx = fw / 2 - 60, gy = fy + fh / 2 - 12;
        dobui_FillRect(wid, gx - 4, gy - 4, 128, 28, COL_DEAD);
        dobui_DrawText(wid, gx, gy, "GAME OVER", COL_TEXT, COL_DEAD);
    }

    dobui_Invalidate(wid);
}

/* Event handlers */

void event_key(uint8_t k)
{
    if (game_over) return;

    if ((k == 'a' || k == 'A' || k == KEY_LEFT) && fits(cur_piece, cur_rot, cur_x - 1, cur_y))
        cur_x--;
    else if ((k == 'd' || k == 'D' || k == KEY_RIGHT) && fits(cur_piece, cur_rot, cur_x + 1, cur_y))
        cur_x++;
    else if (k == 'w' || k == 'W' || k == KEY_UP)
    {
        int nr = (cur_rot + 1) % 4;
        if (fits(cur_piece, nr, cur_x, cur_y)) cur_rot = nr;
    }
    else if ((k == 's' || k == 'S' || k == KEY_DOWN) && fits(cur_piece, cur_rot, cur_x, cur_y + 1))
    {
        cur_y++;
        score += 1;
    }
    else if (k == ' ')
    {
        while (fits(cur_piece, cur_rot, cur_x, cur_y + 1))
        {
            cur_y++;
            score += 2;
        }
        lock_piece();
        do_clear_score();
        spawn_piece();
    }
    draw();
}

void event_panel(int cmd_idx)
{
    (void)cmd_idx;
    init_game();
    dobui_set_tick(500);
    draw();
}

void event_resize(int w, int h)
{
    (void)w; (void)h;
    draw();
}

void event_tick(void)
{
    if (!game_over)
    {
        if (fits(cur_piece, cur_rot, cur_x, cur_y + 1))
        {
            cur_y++;
        }
        else
        {
            lock_piece();
            do_clear_score();
            spawn_piece();
        }
        draw();
    }
    /* Next tick at current drop speed */
    int drop_ms = 500 - (level - 1) * 40;
    if (drop_ms < 80) drop_ms = 80;
    dobui_set_tick((uint32_t)drop_ms);
}

void event_start(void)
{
    draw();
}

int main(void)
{
    init_game();
    dobui_set_tick(500);
    dobui_set_panel("Nuova partita");
    dobui_run("Tetris", 260, 344);
    return 0;
}
