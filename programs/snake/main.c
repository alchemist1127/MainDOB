/* MainDOB Snake — Event-driven
 * WASD or arrow keys to move. Resizable window. */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <app.h>
#include <DobInterface.h>
#include <dob/types.h>

#define GRID_W      30
#define GRID_H      20
#define HEADER_H    24
#define MAX_SNAKE   600

#define COL_BG      0x00111111
#define COL_GRID    0x00222222
#define COL_SNAKE   0x0000CC00
#define COL_HEAD    0x0000FF00
#define COL_FOOD    0x000000FF
#define COL_TEXT    0x00FFFFFF
#define COL_HDR     0x00333333
#define COL_DEAD    0x000000AA

#define DIR_UP      0
#define DIR_RIGHT   1
#define DIR_DOWN    2
#define DIR_LEFT    3

static int snake_x[MAX_SNAKE], snake_y[MAX_SNAKE];
static int snake_len = 3;
static int direction = DIR_RIGHT, next_dir = DIR_RIGHT;
static int food_x, food_y;
static int score = 0;
static bool game_over = false;
static uint32_t rng_state;

static int cell_size(void)
{
    int cw = dobui_width() / GRID_W;
    int ch = (dobui_height() - HEADER_H) / GRID_H;
    int c = (cw < ch) ? cw : ch;
    return (c < 4) ? 4 : c;
}

static uint32_t rng(void)
{
    rng_state = rng_state * 1103515245 + 12345;
    return (rng_state >> 16) & 0x7FFF;
}

static void place_food(void)
{
    for (int tries = 0; tries < 200; tries++)
    {
        food_x = (int)(rng() % GRID_W);
        food_y = (int)(rng() % GRID_H);
        bool on_snake = false;
        for (int i = 0; i < snake_len; i++)
        {
            if (snake_x[i] == food_x && snake_y[i] == food_y)
            {
                on_snake = true;
                break;
            }
        }
        if (!on_snake) return;
    }
}

static void init_game(void)
{
    snake_len = 3;
    direction = DIR_RIGHT;
    next_dir = DIR_RIGHT;
    score = 0;
    game_over = false;
    for (int i = 0; i < snake_len; i++)
    {
        snake_x[i] = 5 - i;
        snake_y[i] = GRID_H / 2;
    }
    get_random(&rng_state, sizeof(rng_state));
    place_food();
}

static void draw(void)
{
    uint32_t wid = dobui_window();
    int w = dobui_width();
    int h = dobui_height();
    int cs = cell_size();
    int gw = GRID_W * cs, gh = GRID_H * cs;
    int ox = (w - gw) / 2;
    int oy = HEADER_H;

    dobui_FillRect(wid, 0, 0, w, HEADER_H, COL_HDR);
    char info[64];
    snprintf(info, sizeof(info), " Snake   Punteggio: %d", score);
    dobui_DrawText(wid, 4, 4, info, COL_TEXT, COL_HDR);

    dobui_FillRect(wid, 0, oy, w, h - oy, COL_BG);

    for (int x = 0; x < GRID_W; x++)
    {
        for (int y = 0; y < GRID_H; y++)
        {
            dobui_DrawRect(wid, ox + x * cs, oy + y * cs, cs, cs, COL_GRID);
        }
    }

    dobui_FillRect(wid, ox + food_x * cs + 1, oy + food_y * cs + 1,
                   cs - 2, cs - 2, COL_FOOD);

    for (int i = 1; i < snake_len; i++)
    {
        dobui_FillRect(wid, ox + snake_x[i] * cs + 1,
                       oy + snake_y[i] * cs + 1, cs - 2, cs - 2, COL_SNAKE);
    }

    dobui_FillRect(wid, ox + snake_x[0] * cs + 1,
                   oy + snake_y[0] * cs + 1, cs - 2, cs - 2, COL_HEAD);

    if (game_over)
    {
        int gx = w / 2 - 80;
        int gy = oy + gh / 2 - 16;
        dobui_FillRect(wid, gx, gy, 160, 32, COL_DEAD);
        dobui_DrawText(wid, gx + 8, gy + 8, "GAME OVER", COL_TEXT, COL_DEAD);
    }

    dobui_Invalidate(wid);
}

static void step(void)
{
    if (game_over) return;
    direction = next_dir;

    int nx = snake_x[0], ny = snake_y[0];
    switch (direction)
    {
        case DIR_UP:    ny--; break;
        case DIR_DOWN:  ny++; break;
        case DIR_LEFT:  nx--; break;
        case DIR_RIGHT: nx++; break;
    }

    if (nx < 0 || nx >= GRID_W || ny < 0 || ny >= GRID_H)
    {
        game_over = true;
        return;
    }

    for (int i = 0; i < snake_len; i++)
    {
        if (snake_x[i] == nx && snake_y[i] == ny)
        {
            game_over = true;
            return;
        }
    }

    bool ate = (nx == food_x && ny == food_y);

    if (!ate)
    {
        for (int i = snake_len - 1; i > 0; i--)
        {
            snake_x[i] = snake_x[i - 1];
            snake_y[i] = snake_y[i - 1];
        }
    }
    else
    {
        if (snake_len < MAX_SNAKE) snake_len++;
        for (int i = snake_len - 1; i > 0; i--)
        {
            snake_x[i] = snake_x[i - 1];
            snake_y[i] = snake_y[i - 1];
        }
        score += 10;
        place_food();
    }
    snake_x[0] = nx;
    snake_y[0] = ny;
}

/* Event handlers */

void event_key(uint8_t k)
{
    if ((k == 'w' || k == 'W' || k == KEY_UP) && direction != DIR_DOWN)
        next_dir = DIR_UP;
    else if ((k == 's' || k == 'S' || k == KEY_DOWN) && direction != DIR_UP)
        next_dir = DIR_DOWN;
    else if ((k == 'a' || k == 'A' || k == KEY_LEFT) && direction != DIR_RIGHT)
        next_dir = DIR_LEFT;
    else if ((k == 'd' || k == 'D' || k == KEY_RIGHT) && direction != DIR_LEFT)
        next_dir = DIR_RIGHT;
}

void event_panel(int cmd_idx)
{
    (void)cmd_idx;
    init_game();
    dobui_set_tick(120);
    draw();
}

void event_resize(int w, int h)
{
    (void)w; (void)h;
    draw();
}

void event_tick(void)
{
    step();
    draw();
    /* Accelerate as score grows — next tick fires at new speed */
    int speed = 120 - score / 2;
    if (speed < 40) speed = 40;
    dobui_set_tick((uint32_t)speed);
}

void event_start(void)
{
    draw();
}

int main(void)
{
    init_game();
    dobui_set_tick(120);
    dobui_set_panel("Nuova partita");
    dobui_run("Snake", 360, 264);
    return 0;
}
