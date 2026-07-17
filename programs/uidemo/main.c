/* MainDOB UI Controls Demo
 *
 * Full showcase for DobUITools: all 10 control types.
 * Not a real application — purely a visual/interactive test. */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <app.h>
#include <DobInterface.h>
#include <dob/types.h>

#include <label.h>
#include <button.h>
#include <textbox.h>
#include <dropdown.h>
#include <checkbox.h>
#include <progressbar.h>
#include <radiogroup.h>
#include <slider.h>
#include <listview.h>
#include <separator.h>
#include <focus.h>

#define WIN_W   640
#define WIN_H   740

#include <dobui_theme.h>
#define COL_BG      DOBUI_SURFACE
#define COL_DARK    DOBUI_INSET
#define COL_WHITE   DOBUI_TEXT_ALT
#define COL_BLUE    0x00FF8800
#define COL_GREEN   0x0000AA00
#define COL_RED     0x000000DD
#define COL_ORANGE  0x000088FF
#define COL_CYAN    0x00FFFF00
#define COL_PURPLE  0x00FF00AA

static uint32_t win_id;
static int win_w = WIN_W, win_h = WIN_H;

/* Controls */

/* Labels */
static dob_label_t lbl_title;
static dob_label_t lbl_plain;
static dob_label_t lbl_blue;
static dob_label_t lbl_green;
static dob_label_t lbl_red;
static dob_label_t lbl_status;

/* Buttons */
static dob_button_t btn_hello;
static dob_button_t btn_count;
static dob_button_t btn_toggle;
static dob_button_t btn_disabled;
static dob_button_row_t btn_row;

/* Textbox */
static dob_label_t lbl_input;
static dob_textbox_t tb_input;
static dob_label_t lbl_echo;

/* Dropdowns */
static dob_label_t lbl_dd;
static dob_dropdown_t dd_fruit;
static dob_dropdown_t dd_color;
static dob_label_t lbl_dd_result;

/* Separators */
static dob_separator_t sep[6];

/* Checkboxes */
static dob_label_t lbl_checks;
static dob_checkbox_t cb_notif;
static dob_checkbox_t cb_dark;
static dob_checkbox_t cb_disabled;

/* Slider */
static dob_label_t lbl_slider;
static dob_slider_t sl_volume;

/* RadioGroup */
static dob_label_t lbl_radio;
static dob_radiogroup_t rg_theme;

/* ProgressBars */
static dob_label_t lbl_progress;
static dob_progressbar_t pb_linked;
static dob_progressbar_t pb_static;
static dob_label_t lbl_pb_linked;
static dob_label_t lbl_pb_static;

/* ListView */
static dob_label_t lbl_list;
static dob_listview_t lv_files;
static dob_label_t lbl_lv_sel;

/* Multi-line */
static dob_label_t lbl_editor;
static dob_multitextbox_t mt_editor;

/* Data */
static const char *fruits[] = {
    "Mela", "Banana", "Ciliegia", "Dattero", "Fico",
    "Uva", "Kiwi", "Limone", "Mango", "Cocco"
};
static const char *colors_list[] = { "Rosso", "Verde", "Blu", "Giallo", "Viola" };
static const char *file_list[] = {
    "documento.txt", "foto_vacanza.png", "presentazione.pptx",
    "bilancio_2025.xlsx", "musica.mp3", "video_corso.mp4",
    "backup.tar.gz", "note_riunione.md", "progetto.zip",
    "screenshot_01.png", "relazione.pdf", "database.db"
};

/* State */
static int click_count = 0;
static bool toggle_state = false;

/* Init */

static void init_controls(void)
{
    int y = 8;

    /* Title */
    doblbl_InitWithBg(&lbl_title, win_id, 8, y, " DobUITools Demo — Tutti i controlli ",
                       COL_WHITE, COL_DARK);
    lbl_title.pad = 6;
    y += 30;

    /* === Labels === */
    doblbl_Init(&lbl_plain, win_id, 16, y, "Label semplice (no sfondo)");
    lbl_plain.col_text = COL_DARK;
    lbl_plain.col_bg = COL_BG;
    y += 22;

    doblbl_InitWithBg(&lbl_blue, win_id, 16, y, " Label blu ", COL_WHITE, COL_BLUE);
    y += 28;

    doblbl_InitWithBg(&lbl_green, win_id, 16, y, " Label verde ", COL_WHITE, COL_GREEN);
    doblbl_InitWithBg(&lbl_red, win_id,
                       16 + doblbl_GetWidth(&lbl_green) + 8, y,
                       " Label rossa ", COL_WHITE, COL_RED);
    doblbl_InitWithBg(&lbl_status, win_id,
                       16 + doblbl_GetWidth(&lbl_green) + 8
                          + doblbl_GetWidth(&lbl_red) + 8, y,
                       " Stato ", COL_DARK, COL_ORANGE);
    y += 30;

    /* sep 0 */
    dobsep_Init(&sep[0], win_id, 16, y, win_w - 32, false);
    y += 8;

    /* === Buttons === */
    dobbtn_Init(&btn_hello, win_id, 16, y, 0, 0, "Cliccami!");
    dobbtn_Init(&btn_count, win_id, 16 + btn_hello.w + 8, y, 120, 0, "Contatore: 0");
    dobbtn_Init(&btn_toggle, win_id, 16 + btn_hello.w + 8 + 128, y, 0, 0, "Toggle: OFF");
    dobbtn_Init(&btn_disabled, win_id,
                16 + btn_hello.w + 8 + 128 + btn_toggle.w + 8, y, 0, 0, "Disabilitato");
    dobbtn_SetEnabled(&btn_disabled, false);
    y += 30;

    dobbtn_RowInit(&btn_row, win_id, win_w / 2, y);
    dobbtn_RowAdd(&btn_row, "Primo");
    dobbtn_RowAdd(&btn_row, "Secondo");
    dobbtn_RowAdd(&btn_row, "Terzo");
    dobbtn_RowAdd(&btn_row, "Quarto");
    dobbtn_RowLayout(&btn_row);
    y += 30;

    /* sep 1 */
    dobsep_Init(&sep[1], win_id, 16, y, win_w - 32, false);
    y += 8;

    /* === Textbox + Dropdown === */
    doblbl_Init(&lbl_input, win_id, 16, y + 3, "Input:");
    lbl_input.col_text = COL_DARK; lbl_input.col_bg = COL_BG;
    dobtb_Init(&tb_input, win_id, 70, y, 240, 0);
    dobtb_SetText(&tb_input, "Scrivi qui...");
    doblbl_InitWithBg(&lbl_echo, win_id, 320, y, " Echo: --- ", COL_DARK, COL_CYAN);
    y += 28;

    doblbl_Init(&lbl_dd, win_id, 16, y + 3, "Dropdown:");
    lbl_dd.col_text = COL_DARK; lbl_dd.col_bg = COL_BG;
    dobdd_Init(&dd_fruit, win_id, 100, y, 150, 0, fruits, 10);
    dd_fruit.col_clear = COL_BG;
    dobdd_Init(&dd_color, win_id, 260, y, 130, 0, colors_list, 5);
    dd_color.col_clear = COL_BG;
    doblbl_InitWithBg(&lbl_dd_result, win_id, 400, y, " --- ", COL_DARK, COL_ORANGE);
    y += 28;

    /* sep 2 */
    dobsep_Init(&sep[2], win_id, 16, y, win_w - 32, false);
    y += 8;

    /* === Checkbox + Slider (left/right) === */
    doblbl_Init(&lbl_checks, win_id, 16, y, "Checkbox:");
    lbl_checks.col_text = COL_DARK; lbl_checks.col_bg = COL_BG;
    doblbl_Init(&lbl_slider, win_id, 340, y, "Slider:");
    lbl_slider.col_text = COL_DARK; lbl_slider.col_bg = COL_BG;
    y += 20;

    dobcb_Init(&cb_notif, win_id, 16, y, 0, "Notifiche attive");
    cb_notif.col_bg = COL_BG;
    cb_notif.checked = true;

    dobsl_Init(&sl_volume, win_id, 340, y, 200, 0);
    sl_volume.value = 50;
    sl_volume.show_value = true;
    sl_volume.col_text_bg = COL_BG;
    y += 22;

    dobcb_Init(&cb_dark, win_id, 16, y, 0, "Modalita' scura");
    cb_dark.col_bg = COL_BG;

    y += 22;

    dobcb_Init(&cb_disabled, win_id, 16, y, 0, "Checkbox disabilitata");
    cb_disabled.col_bg = COL_BG;
    dobcb_SetEnabled(&cb_disabled, false);
    cb_disabled.checked = true;
    y += 24;

    /* sep 3 */
    dobsep_Init(&sep[3], win_id, 16, y, win_w - 32, false);
    y += 8;

    /* === RadioGroup (left) + ProgressBars (right) === */
    doblbl_Init(&lbl_radio, win_id, 16, y, "RadioGroup:");
    lbl_radio.col_text = COL_DARK; lbl_radio.col_bg = COL_BG;
    doblbl_Init(&lbl_progress, win_id, 340, y, "ProgressBar:");
    lbl_progress.col_text = COL_DARK; lbl_progress.col_bg = COL_BG;
    y += 20;

    dobrg_Init(&rg_theme, win_id, 16, y, 0);
    dobrg_AddItem(&rg_theme, "Chiaro");
    dobrg_AddItem(&rg_theme, "Scuro");
    dobrg_AddItem(&rg_theme, "Sistema");
    rg_theme.selected = 0;
    rg_theme.col_bg = COL_BG;

    /* Progress bars on the right */
    doblbl_Init(&lbl_pb_linked, win_id, 340, y, "Volume:");
    lbl_pb_linked.col_text = COL_DARK; lbl_pb_linked.col_bg = COL_BG;
    dobpb_Init(&pb_linked, win_id, 410, y + 2, 160, 12);
    pb_linked.show_text = true;
    pb_linked.col_text_bg = COL_BG;
    pb_linked.value = 50;

    doblbl_Init(&lbl_pb_static, win_id, 340, y + 24, "Batteria:");
    lbl_pb_static.col_text = COL_DARK; lbl_pb_static.col_bg = COL_BG;
    dobpb_Init(&pb_static, win_id, 410, y + 26, 160, 12);
    pb_static.show_text = true;
    pb_static.col_text_bg = COL_BG;
    pb_static.col_fill = COL_GREEN;
    pb_static.value = 75;

    y += dobrg_GetHeight(&rg_theme) + 6;

    /* sep 4 */
    dobsep_Init(&sep[4], win_id, 16, y, win_w - 32, false);
    y += 8;

    /* === ListView === */
    doblbl_Init(&lbl_list, win_id, 16, y, "ListView:");
    lbl_list.col_text = COL_DARK; lbl_list.col_bg = COL_BG;
    doblbl_InitWithBg(&lbl_lv_sel, win_id, 240, y, " (nessuna selezione) ",
                       COL_DARK, COL_CYAN);
    y += 20;

    doblv_Init(&lv_files, win_id, 16, y, win_w - 32, 100);
    doblv_SetItems(&lv_files, file_list, 12);
    y += 108;

    /* sep 5 */
    dobsep_Init(&sep[5], win_id, 16, y, win_w - 32, false);
    y += 8;

    /* === Multi-line editor === */
    doblbl_Init(&lbl_editor, win_id, 16, y, "Multi-line editor:");
    lbl_editor.col_text = COL_DARK; lbl_editor.col_bg = COL_BG;
    y += 20;

    dobmt_Init(&mt_editor, win_id, 16, y, win_w - 32, win_h - y - 8);
    dobmt_SetText(&mt_editor,
        "Benvenuto nel DobUITools Demo!\n"
        "\n"
        "Tutti i 10 controlli sono mostrati sopra.\n"
        "Prova a interagire con ciascuno.\n"
        "\n"
        "Il slider 'Volume' aggiorna la barra\n"
        "di progresso in tempo reale.\n",
        -1);

    /* Register all focusable controls with the focus manager */
}

/* Drawing */

static void draw_all(void)
{
    /* Re-emit body background every frame — cmdlist semantics
     * don't preserve pixels across Invalidates. */
    dobui_FillRect(win_id, 0, 0, win_w, win_h, COL_BG);

    dobdd_ClearGhost(&dd_fruit);
    dobdd_ClearGhost(&dd_color);

    /* Labels */
    doblbl_Draw(&lbl_title);
    doblbl_Draw(&lbl_plain);
    doblbl_Draw(&lbl_blue);
    doblbl_Draw(&lbl_green);
    doblbl_Draw(&lbl_red);
    doblbl_Draw(&lbl_status);

    /* Separators */
    for (int i = 0; i < 6; i++) dobsep_Draw(&sep[i]);

    /* Buttons */
    dobbtn_Draw(&btn_hello);
    dobbtn_Draw(&btn_count);
    dobbtn_Draw(&btn_toggle);
    dobbtn_Draw(&btn_disabled);
    dobbtn_RowDraw(&btn_row);

    /* Textbox */
    doblbl_Draw(&lbl_input);
    dobtb_Draw(&tb_input);
    doblbl_Draw(&lbl_echo);

    /* Dropdowns */
    doblbl_Draw(&lbl_dd);
    dobdd_Draw(&dd_fruit);
    dobdd_Draw(&dd_color);
    doblbl_Draw(&lbl_dd_result);

    /* Checkboxes */
    doblbl_Draw(&lbl_checks);
    dobcb_Draw(&cb_notif);
    dobcb_Draw(&cb_dark);
    dobcb_Draw(&cb_disabled);

    /* Slider */
    doblbl_Draw(&lbl_slider);
    dobsl_Draw(&sl_volume);

    /* RadioGroup */
    doblbl_Draw(&lbl_radio);
    dobrg_Draw(&rg_theme);

    /* ProgressBars */
    doblbl_Draw(&lbl_progress);
    doblbl_Draw(&lbl_pb_linked);
    dobpb_Draw(&pb_linked);
    doblbl_Draw(&lbl_pb_static);
    dobpb_Draw(&pb_static);

    /* ListView */
    doblbl_Draw(&lbl_list);
    doblbl_Draw(&lbl_lv_sel);
    doblv_Draw(&lv_files);

    /* Multi-line */
    doblbl_Draw(&lbl_editor);
    dobmt_Draw(&mt_editor);

    /* Dropdown popups ON TOP */
    dobdd_FlushPopup(&dd_fruit);
    dobdd_FlushPopup(&dd_color);

    dobui_Invalidate(win_id);
}

static void draw_quick_mt(void)
{
    /* Each Invalidate resets the cmdlist, so the incremental
     * QuickDraw path can't skip widgets — emit all of them. */
    draw_all();
}

static void draw_quick_tb(void)
{
    char echo[128];
    snprintf(echo, sizeof(echo), " Echo: %.40s ", dobtb_GetText(&tb_input));
    doblbl_SetText(&lbl_echo, echo);
    draw_all();
}

/* Helpers */

static void update_dd_result(void)
{
    char res[128];
    const char *f = dobdd_GetSelectedText(&dd_fruit);
    const char *c = dobdd_GetSelectedText(&dd_color);
    snprintf(res, sizeof(res), " %s + %s ", f ? f : "?", c ? c : "?");
    doblbl_SetText(&lbl_dd_result, res);
}

static void update_lv_sel(void)
{
    const char *sel = doblv_GetSelectedText(&lv_files);
    char buf[128];
    if (sel)
        snprintf(buf, sizeof(buf), " Selezionato: %s ", sel);
    else
        snprintf(buf, sizeof(buf), " (nessuna selezione) ");
    doblbl_SetText(&lbl_lv_sel, buf);
}

static void update_pb_from_slider(void)
{
    pb_linked.value = sl_volume.value;
}

static void set_status(const char *text, uint32_t col)
{
    doblbl_SetText(&lbl_status, text);
    lbl_status.col_bg = col;
}

/* Event handlers — routed through focus manager */

void event_key(uint8_t key)
{
    /* Route to focused control first */
    if (dobfocus_key(key))
    {
        /* Post-processing for specific controls */
        void *f = dobfocus_get_focused();
        dob_ctrl_type_t ft = dobfocus_get_focused_type();

        if (f == &btn_hello)
        {
            btn_hello.clicked = false;
            set_status(" Ciao! ", COL_PURPLE);
        }
        else if (f == &btn_count)
        {
            btn_count.clicked = false;
            click_count++;
            char buf[32]; snprintf(buf, sizeof(buf), "Contatore: %d", click_count);
            dobbtn_SetLabel(&btn_count, buf);
        }
        else if (f == &btn_toggle)
        {
            btn_toggle.clicked = false;
            toggle_state = !toggle_state;
            dobbtn_SetLabel(&btn_toggle, toggle_state ? "Toggle: ON" : "Toggle: OFF");
            btn_toggle.col_bg = toggle_state ? COL_GREEN : DOBBTN_COL_BG;
        }
        else if (f == &dd_fruit || f == &dd_color)
        {
            update_dd_result();
        }
        else if (f == &cb_notif)
        {
            set_status(cb_notif.checked ? " Notifiche ON " : " Notifiche OFF ", COL_BLUE);
        }
        else if (f == &cb_dark)
        {
            set_status(cb_dark.checked ? " Dark mode ON " : " Dark mode OFF ", COL_BLUE);
        }
        else if (f == &sl_volume)
        {
            update_pb_from_slider();
            char s[32]; snprintf(s, sizeof(s), " Volume: %d ", sl_volume.value);
            set_status(s, COL_BLUE);
        }
        else if (f == &rg_theme)
        {
            const char *themes[] = { "Chiaro", "Scuro", "Sistema" };
            char s[64]; snprintf(s, sizeof(s), " Tema: %s ", themes[rg_theme.selected]);
            set_status(s, COL_GREEN);
        }
        else if (f == &lv_files)
        {
            update_lv_sel();
        }
        else if (ft == DOB_CTRL_TEXTBOX)
        {
            /* Quick draw for textbox typing */
            draw_quick_tb();
            return;
        }
        else if (ft == DOB_CTRL_MULTITEXTBOX)
        {
            bool structural = (key == '\n' || key == '\b' || key == 134);
            if (structural || key >= 128) { draw_all(); } else { draw_quick_mt(); }
            return;
        }

        draw_all();
        return;
    }

    /* Button row (not managed by focus — always responds if focused) */
    int ri = dobbtn_RowOnKey(&btn_row, key);
    if (ri >= 0)
    {
        const char *n[] = { "Primo!", "Secondo!", "Terzo!", "Quarto!" };
        char s[64]; snprintf(s, sizeof(s), " Cliccato: %s ", n[ri]);
        set_status(s, COL_ORANGE);
        draw_all(); return;
    }
}

void event_mouseclick(int x, int y, uint8_t buttons)
{
    (void)buttons;

    /* Button row first (not in focus manager) */
    int ri = dobbtn_RowOnClick(&btn_row, x, y);
    if (ri >= 0)
    {
        const char *n[] = { "Primo!", "Secondo!", "Terzo!", "Quarto!" };
        char s[64]; snprintf(s, sizeof(s), " Cliccato: %s ", n[ri]);
        set_status(s, COL_ORANGE);
        draw_all(); return;
    }

    /* Focus manager handles hit-test, focus switch, and OnClick */
    void *clicked = dobfocus_click(x, y);
    if (clicked)
    {
        if (clicked == &btn_hello)
        {
            btn_hello.clicked = false;
            set_status(" Ciao! ", COL_PURPLE);
        }
        else if (clicked == &btn_count)
        {
            btn_count.clicked = false;
            click_count++;
            char buf[32]; snprintf(buf, sizeof(buf), "Contatore: %d", click_count);
            dobbtn_SetLabel(&btn_count, buf);
        }
        else if (clicked == &btn_toggle)
        {
            btn_toggle.clicked = false;
            toggle_state = !toggle_state;
            dobbtn_SetLabel(&btn_toggle, toggle_state ? "Toggle: ON" : "Toggle: OFF");
            btn_toggle.col_bg = toggle_state ? COL_GREEN : DOBBTN_COL_BG;
        }
        else if (clicked == &dd_fruit || clicked == &dd_color)
        {
            update_dd_result();
        }
        else if (clicked == &cb_notif)
        {
            set_status(cb_notif.checked ? " Notifiche ON " : " Notifiche OFF ", COL_BLUE);
        }
        else if (clicked == &cb_dark)
        {
            set_status(cb_dark.checked ? " Dark mode ON " : " Dark mode OFF ", COL_BLUE);
        }
        else if (clicked == &sl_volume)
        {
            update_pb_from_slider();
            char s[32]; snprintf(s, sizeof(s), " Volume: %d ", sl_volume.value);
            set_status(s, COL_BLUE);
        }
        else if (clicked == &rg_theme)
        {
            const char *themes[] = { "Chiaro", "Scuro", "Sistema" };
            char s[64]; snprintf(s, sizeof(s), " Tema: %s ", themes[rg_theme.selected]);
            set_status(s, COL_GREEN);
        }
        else if (clicked == &lv_files)
        {
            update_lv_sel();
        }

        draw_all(); return;
    }

    draw_all();
}

void event_mouserelease(int x, int y, uint8_t buttons)
{
    (void)x; (void)y; (void)buttons;

    void *released = dobfocus_release();
    for (int i = 0; i < btn_row.count; i++)
        dobbtn_OnRelease(&btn_row.buttons[i]);

    if (released == &sl_volume)
    {
        update_pb_from_slider();
        char s[32]; snprintf(s, sizeof(s), " Volume: %d ", sl_volume.value);
        set_status(s, COL_BLUE);
        draw_all();
    }
}

void event_mousemove(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (dobfocus_drag(x, y))
        draw_all();
}

void event_dblclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (dobfocus_dblclick(x, y))
        draw_all();
}

void event_scroll(int delta)
{
    if (dobfocus_scroll(delta))
        draw_all();
}

void event_panel(int cmd_idx)
{
    /* Contextual clipboard commands on text widgets are handled by
     * the focus manager; fall through to our own command only when
     * no contextual widget is focused. */
    if (dobfocus_panel(cmd_idx))
    {
        draw_all();
        return;
    }
    click_count = 0;
    dobbtn_SetLabel(&btn_count, "Contatore: 0");
    set_status(" Reset! ", COL_GREEN);
    draw_all();
}

void event_resize(int w, int h)
{
    win_w = w;
    win_h = h;
    mt_editor.w = win_w - 32;
    mt_editor.h = win_h - mt_editor.y - 8;
    lv_files.w = win_w - 32;
    btn_row.center_x = win_w / 2;
    dobbtn_RowLayout(&btn_row);
    for (int i = 0; i < 6; i++) sep[i].length = win_w - 32;
    dobui_FillRect(win_id, 0, 0, win_w, win_h, COL_BG);
    draw_all();
}

void event_start(void)
{
    win_id = dobui_window();
    init_controls();
    dobfocus_attach_panel(win_id, "Reset contatore");
    dobfocus_set_focus(&mt_editor);
    dobui_FillRect(win_id, 0, 0, win_w, win_h, COL_BG);
    draw_all();
}

int main(void)
{
    dobui_set_panel("Reset contatore");
    dobui_run("DobUITools Demo", WIN_W, WIN_H);
    return 0;
}
