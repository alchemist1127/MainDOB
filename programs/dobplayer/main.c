/* DobPlayer.mdl — Simple Media Player
 *
 * GUI thin-client per il server `audioplayer`. Non decodifica nulla
 * in proprio: chiama DobAudioPlayer.Play/Pause/Resume/Stop/SetVolume
 * e legge Status periodicamente via event_tick per aggiornare la UI.
 *
 * UI: textbox path + Apri, bottoni Play/Pause/Stop, slider volume,
 *     progress bar posizione, label stato.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <app.h>
#include <DobInterface.h>
#include <DobAudioPlayer.h>
#include <label.h>
#include <textbox.h>
#include <button.h>
#include <slider.h>
#include <progressbar.h>
#include <focus.h>

/* Constants */

#define WIN_W           400
#define WIN_H           230

#include <dobui_theme.h>
#define COL_BG          DOBUI_INSET
#define COL_TITLE       DOBUI_TEXT
#define COL_TEXT        DOBUI_TEXT_ALT
#define COL_STATUS      DOBUI_DISABLED
#define COL_OK          DOBUI_SUCCESS
#define COL_ERR         DOBUI_DANGER
#define COL_TITLE_BG    DOBUI_SURFACE

#define DEFAULT_PATH    "/DATA/Music/quack.mp2"

/* UI Controls */

static uint32_t win_id;

static dob_label_t        lbl_title;
static dob_label_t        lbl_path;
static dob_textbox_t      tb_path;
static const char        *g_openfile = NULL;
static dob_button_t       btn_open;
static dob_button_t       btn_play;
static dob_button_t       btn_pause;
static dob_button_t       btn_stop;
static dob_label_t        lbl_volume;
static dob_slider_t       sl_volume;
static dob_progressbar_t  pb_progress;
static dob_label_t        lbl_time;
static dob_label_t        lbl_status;


/* Player state (mirrored from server) */

static dobplayer_status_t last_status;
static int                volume_committed = 80;

/* Helpers */

static const char *
format_name(uint8_t fmt)
{
    switch (fmt)
    {
        case PLAYER_FMT_WAV: return "WAV";
        case PLAYER_FMT_MP2: return "MP2";
        case PLAYER_FMT_MP3: return "MP3";
        case PLAYER_FMT_OGG: return "OGG";
        default:             return "---";
    }
}

static const char *
state_name(uint8_t st)
{
    switch (st)
    {
        case PLAYER_STATE_PLAYING: return "In riproduzione";
        case PLAYER_STATE_PAUSED:  return "In pausa";
        default:                   return "Fermo";
    }
}

static void
format_time(uint32_t ms, char *buf, int sz)
{
    uint32_t s = ms / 1000u;
    snprintf(buf, sz, "%02u:%02u", s / 60u, s % 60u);
}

/* Drawing */

static void
draw_all(void)
{
    dobui_FillRect(win_id, 0, 0, WIN_W, WIN_H, COL_BG);

    doblbl_Draw(&lbl_title);
    doblbl_Draw(&lbl_path);
    dobtb_Draw(&tb_path);
    dobbtn_Draw(&btn_open);
    dobbtn_Draw(&btn_play);
    dobbtn_Draw(&btn_pause);
    dobbtn_Draw(&btn_stop);
    doblbl_Draw(&lbl_volume);
    dobsl_Draw(&sl_volume);
    dobpb_Draw(&pb_progress);
    doblbl_Draw(&lbl_time);
    doblbl_Draw(&lbl_status);

    dobui_Invalidate(win_id);
}

/*  *  Status refresh — called by event_tick
 *
 *  Returns true iff something visible changed and the labels were
 *  actually updated. Callers use this to skip redraws and compositor
 *  invalidates when nothing moved, which is the common case: 29 ticks
 *  out of 30 during normal playback hit the same integer second.
 *
 *  Sub-blocchi:
 *    a) Query server → last_status
 *    b) Confronto con l'ultimo stato disegnato; early-exit se invariato
 *    c) Ricostruisce label time/progress/status solo se cambiato
 */

static int      last_drawn_pct      = -1;
static uint32_t last_drawn_pos_sec  = 0xFFFFFFFFu;
static uint32_t last_drawn_dur_sec  = 0xFFFFFFFFu;
static uint8_t  last_drawn_state    = 0xFF;
static uint8_t  last_drawn_format   = 0xFF;

static bool
refresh_status(void)
{
    /* (a) Query server */
    if (dobplayer.Status(&last_status) < 0) return false;

    /* (b) Compute the visible quantities and compare with last drawn */
    int pct = 0;
    if (last_status.duration_ms > 0)
    {
        pct = (int)((last_status.position_ms * 100u)
                  / last_status.duration_ms);
        if (pct > 100) pct = 100;
    }

    uint32_t pos_sec = last_status.position_ms / 1000u;
    uint32_t dur_sec = last_status.duration_ms / 1000u;

    bool dirty = (pct               != last_drawn_pct)
              || (pos_sec           != last_drawn_pos_sec)
              || (dur_sec           != last_drawn_dur_sec)
              || (last_status.state  != last_drawn_state)
              || (last_status.format != last_drawn_format);

    if (!dirty) return false;

    /* (c) Rebuild label contents only on change */
    dobpb_SetValue(&pb_progress, pct);

    char t_pos[16], t_dur[16], t_buf[48];
    format_time(last_status.position_ms, t_pos, sizeof(t_pos));
    format_time(last_status.duration_ms, t_dur, sizeof(t_dur));
    snprintf(t_buf, sizeof(t_buf), " %s / %s ", t_pos, t_dur);
    doblbl_SetText(&lbl_time, t_buf);

    char s_buf[96];
    if (last_status.state == PLAYER_STATE_IDLE)
    {
        snprintf(s_buf, sizeof(s_buf), " %s ", state_name(last_status.state));
    }
    else
    {
        snprintf(s_buf, sizeof(s_buf), " %s - %s  %u Hz  %uch ",
                 state_name(last_status.state),
                 format_name(last_status.format),
                 last_status.sample_rate,
                 (unsigned)last_status.channels);
    }
    doblbl_SetText(&lbl_status, s_buf);
    lbl_status.col_text = (last_status.state == PLAYER_STATE_PLAYING)
                          ? COL_OK : COL_STATUS;

    last_drawn_pct     = pct;
    last_drawn_pos_sec = pos_sec;
    last_drawn_dur_sec = dur_sec;
    last_drawn_state   = last_status.state;
    last_drawn_format  = last_status.format;
    return true;
}

/* Actions (bottoni) */

static void
action_play(void)
{
    const char *path = dobtb_GetText(&tb_path);
    if (!path || !path[0])
    {
        doblbl_SetText(&lbl_status, " Percorso vuoto. ");
        lbl_status.col_text = COL_ERR;
        draw_all();
        return;
    }

    int rc = dobplayer.Play(path);
    if (rc < 0)
    {
        doblbl_SetText(&lbl_status, " Errore: file non valido o non trovato. ");
        lbl_status.col_text = COL_ERR;
        draw_all();
        return;
    }

    refresh_status();
    draw_all();
}

static void
action_pause_toggle(void)
{
    if (last_status.state == PLAYER_STATE_PLAYING)
        dobplayer.Pause();
    else if (last_status.state == PLAYER_STATE_PAUSED)
        dobplayer.Resume();

    refresh_status();
    draw_all();
}

static void
action_stop(void)
{
    dobplayer.Stop();
    refresh_status();
    draw_all();
}

static void
action_volume_commit(void)
{
    int v = sl_volume.value;
    if (v != volume_committed)
    {
        dobplayer.SetVolume(v);
        volume_committed = v;
    }
}

/* event_start — init UI */

void
event_start(void)
{
    win_id = dobui_window();

    /* Title */
    doblbl_InitWithBg(&lbl_title, win_id, WIN_W / 2 - 50, 8,
                      " DobPlayer ", COL_TITLE, COL_TITLE_BG);

    int y = 38;

    /* Path row */
    doblbl_Init(&lbl_path, win_id, 12, y + 4, "File:");
    lbl_path.col_text = COL_TEXT;
    lbl_path.col_bg = COL_BG;

    dobtb_Init(&tb_path, win_id, 54, y, 256, 0);
    dobtb_SetText(&tb_path, DEFAULT_PATH);

    dobbtn_Init(&btn_open, win_id, 320, y - 1, 60, 0, "Apri");
    y += 36;

    /* Transport row */
    dobbtn_Init(&btn_play,  win_id,  12, y, 80, 0, "Play");
    dobbtn_Init(&btn_pause, win_id, 100, y, 80, 0, "Pause");
    dobbtn_Init(&btn_stop,  win_id, 188, y, 80, 0, "Stop");
    y += 36;

    /* Volume row */
    doblbl_Init(&lbl_volume, win_id, 12, y + 2, "Volume:");
    lbl_volume.col_text = COL_TEXT;
    lbl_volume.col_bg = COL_BG;

    dobsl_Init(&sl_volume, win_id, 80, y, 200, 0);
    sl_volume.min = 0;
    sl_volume.max = 100;
    sl_volume.value = volume_committed;
    sl_volume.drag_value = volume_committed;
    sl_volume.show_value = true;
    y += 30;

    /* Progress + time row */
    dobpb_Init(&pb_progress, win_id, 12, y, 240, 0);
    pb_progress.max = 100;
    dobpb_SetValue(&pb_progress, 0);

    doblbl_Init(&lbl_time, win_id, 262, y - 2, " 00:00 / 00:00 ");
    lbl_time.col_text = COL_TEXT;
    lbl_time.col_bg = COL_BG;
    y += 26;

    /* Status line */
    doblbl_Init(&lbl_status, win_id, 12, y, " Pronto. ");
    lbl_status.col_text = COL_STATUS;
    lbl_status.col_bg = COL_BG;

    /* Focus manager */
    dobfocus_set_focus(&tb_path);

    /* Initial volume */
    dobplayer.SetVolume(volume_committed);

    /* Se lanciato via file-association, il path arriva in argv[0] */
    if (g_openfile && g_openfile[0])
    {
        dobtb_SetText(&tb_path, g_openfile);
        draw_all();
        action_play();
        return;
    }

    draw_all();
}

/*  *  event_tick — poll status periodico
 *
 *  Skips both the IPC call and the redraw when nothing can have changed:
 *    - IDLE → IDLE transitions cannot happen server-side without a user
 *      action, and user actions already invoke refresh_status directly.
 *      So a cached IDLE state lets us go fully silent until the next click.
 *    - During playback, the dirty check inside refresh_status filters out
 *      the ~29/30 ticks where the displayed second hasn't rolled over.
 *
 *  Net effect: on an idle window, event_tick does zero work. On an active
 *  playback, it does ~1 full redraw per second instead of 30.
 */

void
event_tick(void)
{
    /* Cached-IDLE fast path: no IPC, no draw, no blit. */
    if (last_drawn_state == PLAYER_STATE_IDLE)
        return;

    if (!refresh_status())
        return;

    /* Full redraw each Invalidate — cmdlist reset wipes the body. */
    draw_all();
}

/* Input handlers */

static void
dispatch_focused(void *f)
{
    if (f == &btn_open || f == &btn_play)
    {
        if (f == &btn_open) btn_open.clicked = false;
        else                btn_play.clicked = false;
        action_play();
        return;
    }
    if (f == &btn_pause)
    {
        btn_pause.clicked = false;
        action_pause_toggle();
        return;
    }
    if (f == &btn_stop)
    {
        btn_stop.clicked = false;
        action_stop();
        return;
    }
    draw_all();
}

void
event_key(uint8_t key)
{
    if (dobfocus_key(key))
    {
        void *f = dobfocus_get_focused();

        /* Enter nella textbox = Play */
        if (f == &tb_path && key == '\n')
        {
            action_play();
            return;
        }

        /* Slider volume tasti frecce: committa subito */
        if (f == &sl_volume)
        {
            action_volume_commit();
            draw_all();
            return;
        }

        dispatch_focused(f);
    }
}

void
event_mouseclick(int x, int y, uint8_t buttons)
{
    (void)buttons;

    void *clicked = dobfocus_click(x, y);
    if (clicked)
    {
        dispatch_focused(clicked);
    }
}

void
event_mousemove(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (sl_volume.grabbed)
    {
        dobsl_OnDrag(&sl_volume, x, y);
        draw_all();
    }
    else if (dobfocus_drag(x, y)) draw_all();    /* drag-select in the focused text field */
}

void
event_dblclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (dobfocus_dblclick(x, y)) draw_all();      /* double-click selects word / all */
}

void
event_mouserelease(int x, int y, uint8_t buttons)
{
    (void)x; (void)y; (void)buttons;

    if (sl_volume.grabbed)
    {
        dobsl_OnRelease(&sl_volume);
        action_volume_commit();
        draw_all();
    }
    dobfocus_release();
}

int
main(int argc, char **argv)
{
    if (argc >= 1 && argv[0] && argv[0][0]) g_openfile = argv[0];
    dobui_run("DobPlayer", WIN_W, WIN_H);
    return 0;
}
