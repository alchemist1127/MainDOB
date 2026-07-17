/* MainDOB Calculator
 *
 * A calculator with basic operations (+, -, ×, ÷) plus advanced
 * functions (built in):
 *   x²   — square
 *   x³   — cube
 *   √x   — square root
 *   cos  — cosine (degrees) */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <app.h>
#include <DobInterface.h>
#include <dob/types.h>
#include "math_operations.h"

/* Window */
static uint32_t win_id;
static int win_w = 280, win_h = 380;

/* Colors — MainDOB blue theme */
#define COL_BG          0x001A1A2E
#define COL_DISPLAY_BG  0x000D1B2A
#define COL_DISPLAY_FG  0x0000FFCC
#define COL_DISPLAY_LBL 0x00667788
#define COL_BTN_NUM     0x002A2A40
#define COL_BTN_OP      0x00003366
#define COL_BTN_FN      0x00004488
#define COL_BTN_EQ      0x000055AA
#define COL_BTN_CLR     0x00662222
#define COL_BTN_TEXT    0x00FFFFFF
#define COL_BTN_BORDER  0x00444466

/* Calculator state */
#define DISPLAY_MAX 16

static char display[DISPLAY_MAX + 1];
static int  display_len;
static int  accumulator;
static int  current_op;     /* 0=none, '+'=add, '-'=sub, '*'=mul, '/'=div */
static bool new_input;      /* True after pressing an op: next digit starts fresh */

#define OP_NONE 0
#define OP_ADD  '+'
#define OP_SUB  '-'
#define OP_MUL  '*'
#define OP_DIV  '/'

/* Number formatting — handled via libc snprintf/atoi.
 * (Display lengths are clamped via DISPLAY_MAX in the buttons, no need
 * for custom formatting helpers.) */

/* Calculator logic */

static void calc_reset(void)
{
    display[0] = '0';
    display[1] = '\0';
    display_len = 1;
    accumulator = 0;
    current_op = OP_NONE;
    new_input = true;
}

static void calc_append_digit(char digit)
{
    if (new_input)
    {
        display[0] = digit;
        display[1] = '\0';
        display_len = 1;
        new_input = false;
        return;
    }

    /* Don't allow leading zeros */
    if (display_len == 1 && display[0] == '0' && digit == '0')
    {
        return;
    }
    if (display_len == 1 && display[0] == '0' && digit != '0')
    {
        display[0] = digit;
        return;
    }

    if (display_len < DISPLAY_MAX)
    {
        display[display_len] = digit;
        display_len++;
        display[display_len] = '\0';
    }
}

static void calc_negate(void)
{
    int val = atoi(display);
    val = -val;
    snprintf(display, DISPLAY_MAX + 1, "%d", val);
    display_len = strlen(display);
}

static int calc_execute_op(int a, int op, int b)
{
    switch (op)
    {
        case OP_ADD: return a + b;
        case OP_SUB: return a - b;
        case OP_MUL: return a * b;
        case OP_DIV: return (b != 0) ? a / b : 0;
        default:     return b;
    }
}

static void calc_press_op(int op)
{
    int current_val = atoi(display);

    if (current_op != OP_NONE && !new_input)
    {
        accumulator = calc_execute_op(accumulator, current_op, current_val);
    }
    else
    {
        accumulator = current_val;
    }

    snprintf(display, DISPLAY_MAX + 1, "%d", accumulator);
    display_len = strlen(display);
    current_op = op;
    new_input = true;
}

static void calc_press_equals(void)
{
    if (current_op == OP_NONE) return;

    int current_val = atoi(display);
    int result = calc_execute_op(accumulator, current_op, current_val);

    snprintf(display, DISPLAY_MAX + 1, "%d", result);
    display_len = strlen(display);
    accumulator = result;
    current_op = OP_NONE;
    new_input = true;
}

/* Scientific functions */
static void calc_fn_square(void)
{
    int val = atoi(display);
    int result = math_square(val);
    snprintf(display, DISPLAY_MAX + 1, "%d", result);
    display_len = strlen(display);
    new_input = true;
}

static void calc_fn_cube(void)
{
    int val = atoi(display);
    int result = math_cube(val);
    snprintf(display, DISPLAY_MAX + 1, "%d", result);
    display_len = strlen(display);
    new_input = true;
}

static void calc_fn_sqrt(void)
{
    int val = atoi(display);
    int result = math_sqrt(val);
    snprintf(display, DISPLAY_MAX + 1, "%d", result);
    display_len = strlen(display);
    new_input = true;
}

static void calc_fn_cos(void)
{
    int degrees = atoi(display);
    int result = math_cos_fp(degrees);

    /* Format fixed-point result: result is cos * 1000
     * Show as "X.YYY" or "-X.YYY" */
    bool neg = (result < 0);
    if (neg) result = -result;

    int whole = result / 1000;
    int frac = result % 1000;

    char tmp[DISPLAY_MAX + 1];
    int pos = 0;

    if (neg)
    {
        tmp[pos++] = '-';
    }
    tmp[pos++] = '0' + whole;
    tmp[pos++] = '.';
    tmp[pos++] = '0' + (frac / 100);
    tmp[pos++] = '0' + ((frac / 10) % 10);
    tmp[pos++] = '0' + (frac % 10);
    tmp[pos] = '\0';

    memcpy(display, tmp, pos + 1);
    display_len = pos;
    new_input = true;
}

/* Drawing */

/* Button layout: 5 rows × 4 columns */
typedef struct
{
    const char *label;
    uint32_t    color;
    void        (*action)(void);
    char        key;        /* Keyboard shortcut */
} btn_def_t;

static void act_0(void) { calc_append_digit('0'); }
static void act_1(void) { calc_append_digit('1'); }
static void act_2(void) { calc_append_digit('2'); }
static void act_3(void) { calc_append_digit('3'); }
static void act_4(void) { calc_append_digit('4'); }
static void act_5(void) { calc_append_digit('5'); }
static void act_6(void) { calc_append_digit('6'); }
static void act_7(void) { calc_append_digit('7'); }
static void act_8(void) { calc_append_digit('8'); }
static void act_9(void) { calc_append_digit('9'); }
static void act_add(void) { calc_press_op(OP_ADD); }
static void act_sub(void) { calc_press_op(OP_SUB); }
static void act_mul(void) { calc_press_op(OP_MUL); }
static void act_div(void) { calc_press_op(OP_DIV); }
static void act_eq(void)  { calc_press_equals(); }
static void act_clr(void) { calc_reset(); }
static void act_neg(void) { calc_negate(); }

/* 6 rows × 4 columns */
#define BTN_ROWS 6
#define BTN_COLS 4

static const btn_def_t buttons[BTN_ROWS][BTN_COLS] =
{
    /* Row 0: scientific functions */
    { {"x\xFD",  COL_BTN_FN, calc_fn_square, 0},       /* x² */
      {"x\xFD""3",COL_BTN_FN, calc_fn_cube,   0},       /* x³ */
      {"\xFBx",  COL_BTN_FN, calc_fn_sqrt,   0},       /* √x */
      {"cos",    COL_BTN_FN, calc_fn_cos,    0} },

    /* Row 1: Clear, negate, ÷ */
    { {"C",   COL_BTN_CLR, act_clr, 'c'},
      {"+/-", COL_BTN_OP,  act_neg, 0},
      {"/",   COL_BTN_OP,  act_div, '/'},
      {"x",   COL_BTN_OP,  act_mul, '*'} },

    /* Row 2: 7 8 9 - */
    { {"7", COL_BTN_NUM, act_7, '7'},
      {"8", COL_BTN_NUM, act_8, '8'},
      {"9", COL_BTN_NUM, act_9, '9'},
      {"-", COL_BTN_OP,  act_sub, '-'} },

    /* Row 3: 4 5 6 + */
    { {"4", COL_BTN_NUM, act_4, '4'},
      {"5", COL_BTN_NUM, act_5, '5'},
      {"6", COL_BTN_NUM, act_6, '6'},
      {"+", COL_BTN_OP,  act_add, '+'} },

    /* Row 4: 1 2 3 = */
    { {"1", COL_BTN_NUM, act_1, '1'},
      {"2", COL_BTN_NUM, act_2, '2'},
      {"3", COL_BTN_NUM, act_3, '3'},
      {"=", COL_BTN_EQ,  act_eq, '='} },

    /* Row 5: 0 (wide) */
    { {"0", COL_BTN_NUM, act_0, '0'},
      {NULL, 0, NULL, 0},
      {NULL, 0, NULL, 0},
      {NULL, 0, NULL, 0} },
};

static void draw(void)
{
    if (dobui_IsResizePending()) return;

    int pad = 8;
    int display_h = 60;
    int btn_gap = 4;

    /* Background */
    dobui_FillRect(win_id, 0, 0, win_w, win_h, COL_BG);

    /* Display area */
    dobui_FillRect(win_id, pad, pad, win_w - 2 * pad, display_h, COL_DISPLAY_BG);
    dobui_DrawRect(win_id, pad, pad, win_w - 2 * pad, display_h, COL_BTN_BORDER);

    /* Display value — right-aligned */
    int text_w = display_len * 8;
    int text_x = win_w - pad - 8 - text_w;
    int text_y = pad + display_h / 2 - 4;
    if (text_x < pad + 8) text_x = pad + 8;
    dobui_DrawText(win_id, text_x, text_y, display, COL_DISPLAY_FG, COL_DISPLAY_BG);

    /* Operation indicator */
    if (current_op != OP_NONE)
    {
        char op_str[2] = { (char)current_op, '\0' };
        dobui_DrawText(win_id, pad + 6, text_y, op_str, COL_DISPLAY_LBL, COL_DISPLAY_BG);
    }

    /* Buttons */
    int btn_area_y = pad + display_h + pad;
    int btn_area_h = win_h - btn_area_y - pad;
    int btn_w = (win_w - 2 * pad - (BTN_COLS - 1) * btn_gap) / BTN_COLS;
    int btn_h = (btn_area_h - (BTN_ROWS - 1) * btn_gap) / BTN_ROWS;

    for (int row = 0; row < BTN_ROWS; row++)
    {
        for (int col = 0; col < BTN_COLS; col++)
        {
            const btn_def_t *btn = &buttons[row][col];
            if (!btn->label) continue;

            int bx = pad + col * (btn_w + btn_gap);
            int by = btn_area_y + row * (btn_h + btn_gap);
            int bw = btn_w;

            /* Row 5, col 0: "0" button spans 2 columns */
            if (row == 5 && col == 0)
            {
                bw = btn_w * 2 + btn_gap;
            }

            dobui_FillRect(win_id, bx, by, bw, btn_h, btn->color);
            dobui_DrawRect(win_id, bx, by, bw, btn_h, COL_BTN_BORDER);

            /* Center text in button */
            int lbl_len = strlen(btn->label);
            int lbl_x = bx + (bw - lbl_len * 8) / 2;
            int lbl_y = by + (btn_h - 12) / 2;
            dobui_DrawText(win_id, lbl_x, lbl_y, btn->label, COL_BTN_TEXT, btn->color);
        }
    }

    dobui_Invalidate(win_id);
}

/* Input handling */

static void handle_key(uint8_t key)
{
    /* Check keyboard shortcuts */
    for (int row = 0; row < BTN_ROWS; row++)
    {
        for (int col = 0; col < BTN_COLS; col++)
        {
            const btn_def_t *btn = &buttons[row][col];
            if (btn->key && btn->key == (char)key && btn->action)
            {
                btn->action();
                return;
            }
        }
    }

    /* Enter key → equals */
    if (key == '\n' || key == '\r')
    {
        calc_press_equals();
    }
    /* 'C' or 'c' → clear */
    else if (key == 'C' || key == 'c')
    {
        calc_reset();
    }
}

static void handle_click(int mx, int my)
{
    int pad = 8;
    int display_h = 60;
    int btn_gap = 4;
    int btn_area_y = pad + display_h + pad;
    int btn_area_h = win_h - btn_area_y - pad;
    int btn_w = (win_w - 2 * pad - (BTN_COLS - 1) * btn_gap) / BTN_COLS;
    int btn_h = (btn_area_h - (BTN_ROWS - 1) * btn_gap) / BTN_ROWS;

    for (int row = 0; row < BTN_ROWS; row++)
    {
        for (int col = 0; col < BTN_COLS; col++)
        {
            const btn_def_t *btn = &buttons[row][col];
            if (!btn->label || !btn->action) continue;

            int bx = pad + col * (btn_w + btn_gap);
            int by = btn_area_y + row * (btn_h + btn_gap);
            int bw = btn_w;

            if (row == 5 && col == 0)
            {
                bw = btn_w * 2 + btn_gap;
            }

            if (mx >= bx && mx < bx + bw && my >= by && my < by + btn_h)
            {
                btn->action();
                return;
            }
        }
    }
}

/* Event handlers */

void event_key(uint8_t key)
{
    handle_key(key);
    draw();
}

void event_mouseclick(int x, int y, uint8_t buttons)
{
    if (buttons & 1)
    {
        handle_click(x, y);
        draw();
    }
}

void event_panel(int cmd_idx)
{
    (void)cmd_idx;
    calc_reset();
    draw();
}

void event_resize(int w, int h)
{
    win_w = w;
    win_h = h;
    draw();
}

void event_close(void)
{
    dobui_quit();
}

void event_start(void)
{
    win_id = dobui_window();
    draw();
}

int main(void)
{
    calc_reset();
    dobui_set_panel("Clear");
    dobui_run("Calculator", 280, 380);
    return 0;
}
