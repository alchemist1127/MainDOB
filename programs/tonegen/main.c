/* tonegen.mdl — PC Speaker Tone Generator (test)
 *
 * Minimal audio test: generates a square wave tone via the PC Speaker
 * using PIT channel 2. Requires driver privileges for I/O port access.
 *
 * UI: frequency textbox (default 432 Hz) + "Genera tono" button.
 * The speaker plays for 1 second, blocking the event loop.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <app.h>
#include <DobInterface.h>
#include <label.h>
#include <textbox.h>
#include <button.h>
#include <focus.h>

/* Constants */

#define WIN_W       300
#define WIN_H       130

#include <dobui_theme.h>
#define COL_BG      DOBUI_INSET
#define COL_TITLE   DOBUI_TEXT
#define COL_STATUS  DOBUI_DISABLED
#define COL_OK      DOBUI_SUCCESS
#define COL_ERR     DOBUI_DANGER

/* PIT (Programmable Interval Timer) */
#define PIT_BASE_FREQ   1193180
#define PIT_CH2_DATA    0x42
#define PIT_CMD         0x43
#define SPEAKER_PORT    0x61

#define TONE_DURATION   1000    /* ms */
#define FREQ_MIN        20
#define FREQ_MAX        20000

/* UI Controls */

static uint32_t win_id;

static dob_label_t      lbl_title;
static dob_label_t      lbl_freq;
static dob_textbox_t    tb_freq;
static dob_button_t     btn_play;
static dob_label_t      lbl_status;


/* PC Speaker */

static void speaker_on(uint32_t freq_hz)
{
    uint32_t divisor = PIT_BASE_FREQ / freq_hz;

    /* PIT channel 2: lobyte/hibyte, square wave generator */
    io_outb(PIT_CMD, 0xB6);
    io_outb(PIT_CH2_DATA, (uint8_t)(divisor & 0xFF));
    io_outb(PIT_CH2_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    /* Enable speaker: set bits 0 (gate) and 1 (speaker data) */
    uint8_t spk = io_inb(SPEAKER_PORT);
    io_outb(SPEAKER_PORT, spk | 0x03);
}

static void speaker_off(void)
{
    uint8_t spk = io_inb(SPEAKER_PORT);
    io_outb(SPEAKER_PORT, spk & ~0x03);
}

/* Drawing */

static void draw_all(void)
{
    dobui_FillRect(win_id, 0, 0, WIN_W, WIN_H, COL_BG);

    doblbl_Draw(&lbl_title);
    doblbl_Draw(&lbl_freq);
    dobtb_Draw(&tb_freq);
    dobbtn_Draw(&btn_play);
    doblbl_Draw(&lbl_status);

    dobui_Invalidate(win_id);
}

/* Tone generation */

static void do_play(void)
{
    const char *text = dobtb_GetText(&tb_freq);
    int freq = atoi(text);

    if (freq < FREQ_MIN || freq > FREQ_MAX)
    {
        char msg[64];
        snprintf(msg, sizeof(msg), " Errore: %d-%d Hz ", FREQ_MIN, FREQ_MAX);
        doblbl_SetText(&lbl_status, msg);
        lbl_status.col_text = COL_ERR;
        draw_all();
        return;
    }

    /* Update status */
    char msg[64];
    snprintf(msg, sizeof(msg), " Riproduzione: %d Hz... ", freq);
    doblbl_SetText(&lbl_status, msg);
    lbl_status.col_text = COL_OK;
    draw_all();

    /* Play tone */
    speaker_on((uint32_t)freq);
    sleep_ms(TONE_DURATION);
    speaker_off();

    /* Done */
    doblbl_SetText(&lbl_status, " Completato. ");
    lbl_status.col_text = COL_STATUS;
    draw_all();
}

/* Event handlers */

void event_start(void)
{
    win_id = dobui_window();
    int y = 10;

    /* Title */
    doblbl_InitWithBg(&lbl_title, win_id, WIN_W / 2 - 80, y,
                      " Generatore di Tono ", COL_TITLE, 0x00CCCCFF);
    y += 30;

    /* Frequency input */
    doblbl_Init(&lbl_freq, win_id, 16, y + 3, "Frequenza (Hz):");
    lbl_freq.col_text = COL_TITLE;
    lbl_freq.col_bg = COL_BG;

    dobtb_Init(&tb_freq, win_id, 140, y, 70, 0);
    dobtb_SetText(&tb_freq, "432");

    dobbtn_Init(&btn_play, win_id, 220, y - 1, 0, 0, "Genera tono");
    y += 32;

    /* Status */
    doblbl_Init(&lbl_status, win_id, 16, y + 5, " Pronto. ");
    lbl_status.col_text = COL_STATUS;
    lbl_status.col_bg = COL_BG;

    /* Focus manager */
    dobfocus_set_focus(&tb_freq);

    /* Initial draw */
    dobui_FillRect(win_id, 0, 0, WIN_W, WIN_H, COL_BG);
    draw_all();
}

void event_key(uint8_t key)
{
    if (dobfocus_key(key))
    {
        void *f = dobfocus_get_focused();

        if (f == &btn_play)
        {
            btn_play.clicked = false;
            do_play();
            return;
        }

        /* Enter in textbox → play */
        if (f == &tb_freq && key == '\n')
        {
            do_play();
            return;
        }

        draw_all();
    }
}

void event_mouseclick(int x, int y, uint8_t buttons)
{
    (void)buttons;

    void *clicked = dobfocus_click(x, y);
    if (clicked)
    {
        if (clicked == &btn_play)
        {
            btn_play.clicked = false;
            do_play();
            return;
        }
        draw_all();
    }
}

void event_mouserelease(int x, int y, uint8_t buttons)
{
    (void)x; (void)y; (void)buttons;
    dobfocus_release();
}

void event_mousemove(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (dobfocus_drag(x, y)) draw_all();       /* drag-select in the focused field */
}

void event_dblclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (dobfocus_dblclick(x, y)) draw_all();    /* double-click selects word / all */
}

int main(void)
{
    dobui_run("Tonegen", WIN_W, WIN_H);
    return 0;
}
