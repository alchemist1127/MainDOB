/* MainDOB Editor — text editor (dobmt_ based).
 *
 * Thin shell around dob_multitextbox_t: file I/O + title + panel.
 * Selection, drag-to-select, multi-double-click expansion, clipboard
 * (Ctrl+A/C/X/V), scroll, focus, line numbers — all from libdobui.
 *
 * Panel commands:
 *   No file open:   Apri, Salva copia
 *   File open:      Apri, Salva, Salva copia
 * When the editor has focus, the focus manager appends contextual
 * clipboard commands.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <app.h>
#include <DobInterface.h>
#include <DobFileSystem.h>
#include <DobPopup.h>
#include <DobFiles.h>
#include <dob/types.h>
#include <textbox.h>
#include <focus.h>

#define WIN_W           600
#define WIN_H           450
#define HEADER_H        24
#define MAX_PATH_LEN    256

#include <dobui_theme.h>
#define COL_HEADER_BG   DOBUI_RELIEF
#define COL_HEADER_TEXT DOBUI_TEXT_ALT

static uint32_t win_id = 0;
static int win_w = WIN_W;
static int win_h = WIN_H;

static dob_multitextbox_t mt;

static char file_path[MAX_PATH_LEN] = "";
static const char *g_openfile = NULL;
static bool has_file = false;

/* Last modified flag we reflected in the title, so we only re-publish
 * the title when it actually flips. */
static bool last_modified_shown = false;

/* --- Title + panel ------------------------------------------------ */

static void update_title(void)
{
    char title[128];
    const char *name = "Nuovo documento";
    if (has_file)
    {
        name = file_path;
        for (const char *p = file_path; *p; p++)
            if (*p == '/') name = p + 1;
    }
    snprintf(title, sizeof(title), "Editor - %s%s",
             mt.modified ? "*" : "", name);
    dobui_SetTitle(win_id, title);
    last_modified_shown = mt.modified;
}

static void update_panel(void)
{
    /* Static — dobfocus_set_base_panel stores the pointer (no copy),
     * so the buffer must outlive this function call. */
    static char cmds[64];
    const char *wrap_label = dobmt_GetWordWrap(&mt) ? "A capo: Si" : "A capo: No";
    if (has_file)
        snprintf(cmds, sizeof(cmds), "Apri\nSalva\nSalva copia\n%s", wrap_label);
    else
        snprintf(cmds, sizeof(cmds), "Apri\nSalva copia\n%s", wrap_label);
    dobfocus_set_base_panel(cmds);
}

/* --- File I/O ----------------------------------------------------- */

#define LOAD_CHUNK   2048

static bool load_file(const char *path)
{
    int fd = dobfs_Open(path, FS_READ);
    if (fd < 0)
    {
        char errmsg[384];
        snprintf(errmsg, sizeof(errmsg), "Impossibile aprire:\n%s", path);
        dobpopup_Error("Errore", errmsg);
        return false;
    }

    /* Slurp into a growable heap buffer, then hand to mt via SetText
     * (which copies into the widget's own storage). */
    char *buf  = NULL;
    int   cap  = 0;
    int   len  = 0;
    int   n;
    do
    {
        if (len + LOAD_CHUNK + 1 > cap)
        {
            int new_cap = cap ? cap * 2 : (LOAD_CHUNK + 1);
            while (new_cap < len + LOAD_CHUNK + 1) new_cap *= 2;
            char *nb = (char *)realloc(buf, (uint32_t)new_cap);
            if (!nb)
            {
                free(buf); dobfs_Close(fd);
                dobpopup_Error("Errore", "Memoria esaurita.");
                return false;
            }
            buf = nb; cap = new_cap;
        }
        n = dobfs_Read(fd, buf + len, LOAD_CHUNK);
        if (n > 0) len += n;
    } while (n > 0);
    dobfs_Close(fd);

    if (buf) buf[len] = '\0';
    dobmt_SetText(&mt, buf ? buf : "", len);
    free(buf);

    strncpy(file_path, path, MAX_PATH_LEN - 1);
    file_path[MAX_PATH_LEN - 1] = '\0';
    has_file = true;
    mt.modified = false;
    update_title();
    update_panel();
    return true;
}

static bool save_file(const char *path)
{
    int         len;
    const char *text = dobmt_GetText(&mt, &len);

    int fd = dobfs_Open(path, FS_WRITE | FS_CREATE | FS_TRUNC);
    if (fd < 0)
    {
        char errmsg[384];
        snprintf(errmsg, sizeof(errmsg), "Impossibile salvare:\n%s", path);
        dobpopup_Error("Errore", errmsg);
        return false;
    }

    uint32_t offset = 0;
    while (offset < (uint32_t)len)
    {
        uint32_t remaining = (uint32_t)len - offset;
        uint32_t chunk = remaining < 2048u ? remaining : 2048u;
        int written = dobfs_Write(fd, text + offset, chunk);
        if (written <= 0) break;
        offset += (uint32_t)written;
    }
    dobfs_Close(fd);

    if (offset < (uint32_t)len)
    {
        dobpopup_Warning("Attenzione", "Scrittura parziale del file.");
        return false;
    }
    return true;
}

/* --- Panel commands ----------------------------------------------- */

static void cmd_open(void)
{
    if (mt.modified)
    {
        int choice = dobpopup_YesNo("Salva modifiche?",
            "Il documento ha modifiche non salvate. Salvare prima di aprire?");
        if (choice == 0 && has_file)
            save_file(file_path);
    }

    char path[MAX_PATH_LEN];
    int result = dobfiles_PickFile(".txt|.md|.c|.h|.cfg",
                                    "/DATA/Documents", path, sizeof(path));
    if (result != 0)
        result = dobpopup_InputBox("Apri file", "Percorso del file:",
                                    "/DATA/Documents/", path, sizeof(path));
    if (result != 0 || path[0] == '\0') return;
    load_file(path);
}

static void cmd_save(void)
{
    if (!has_file) return;
    if (save_file(file_path))
    {
        mt.modified = false;
        update_title();
    }
}

static void cmd_save_copy(void)
{
    char suggested[128] = "documento.txt";
    if (has_file)
    {
        const char *name = file_path;
        for (const char *p = file_path; *p; p++)
            if (*p == '/') name = p + 1;
        snprintf(suggested, sizeof(suggested), "copia_%s", name);
    }

    char path[MAX_PATH_LEN];
    int result = dobfiles_PickSavePath(suggested, ".txt|.md|.c|.h",
                                        "/DATA/Documents",
                                        path, sizeof(path));
    if (result != 0)
        result = dobpopup_InputBox("Salva copia", "Percorso di destinazione:",
                                    "/DATA/Documents/", path, sizeof(path));
    if (result != 0 || path[0] == '\0') return;

    if (save_file(path))
    {
        int len; dobmt_GetText(&mt, &len);
        char okmsg[384];
        snprintf(okmsg, sizeof(okmsg), "Salvato in:\n%s\n(%d byte)", path, len);
        dobpopup_Info("Salvato", okmsg);
    }
}

/* --- Drawing ------------------------------------------------------ */

static void draw_header(void)
{
    dobui_FillRect(win_id, 0, 0, win_w, HEADER_H, COL_HEADER_BG);
    char info[96];
    int  len;
    dobmt_GetText(&mt, &len);
    snprintf(info, sizeof(info), " Riga %d  Col %d  Len %d",
             dobmt_CursorLine(&mt) + 1,
             dobmt_CursorColumn(&mt) + 1,
             len);
    dobui_DrawText(win_id, 8, 4, info, COL_HEADER_TEXT, COL_HEADER_BG);
}

static void redraw(void)
{
    /* Full re-emit every Invalidate (cmdlist resets server-side). */
    draw_header();
    dobmt_Draw(&mt);
    dobui_Invalidate(win_id);

    if (mt.modified != last_modified_shown)
        update_title();
}

/* --- Event handlers ---------------------------------------------- */

void event_key(uint8_t key)
{
    /* ESC unconditionally drops focus so the panel returns to base. */
    if (key == 27)
    {
        dobfocus_clear_focus();
        redraw();
        return;
    }
    if (dobfocus_key(key))
        redraw();
}

void event_mouseclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    dobfocus_click(x, y);    /* clears focus internally if outside any widget */
    redraw();
}

void event_mouserelease(int x, int y, uint8_t buttons)
{
    (void)x; (void)y; (void)buttons;
    dobfocus_release();
    redraw();
}

void event_mousemove(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (dobfocus_drag(x, y))
        redraw();
}

void event_dblclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (dobfocus_dblclick(x, y))
        redraw();
}

void event_scroll(int delta)
{
    if (dobfocus_scroll(delta))
        redraw();
}

void event_panel(int cmd_idx)
{
    /* Focus manager consumes contextual (clipboard) commands first. */
    if (dobfocus_panel(cmd_idx))
    {
        redraw();
        return;
    }

    /* Base commands; index matches the order set in update_panel. */
    if (has_file)
    {
        switch (cmd_idx)
        {
            case 0: cmd_open();      break;
            case 1: cmd_save();      break;
            case 2: cmd_save_copy(); break;
            case 3:
                dobmt_SetWordWrap(&mt, !dobmt_GetWordWrap(&mt));
                update_panel();
                break;
        }
    }
    else
    {
        switch (cmd_idx)
        {
            case 0: cmd_open();      break;
            case 1: cmd_save_copy(); break;
            case 2:
                dobmt_SetWordWrap(&mt, !dobmt_GetWordWrap(&mt));
                update_panel();
                break;
        }
    }
    redraw();
}

void event_close(void)
{
    if (mt.modified)
    {
        int choice = dobpopup_YesNo("Chiudi",
            "Il documento ha modifiche non salvate. Chiudere senza salvare?");
        if (choice != 0) return;
    }
    dobui_DestroyWindow(win_id);
    _exit(0);
}

void event_resize(int w, int h)
{
    win_w = w;
    win_h = h;
    dobmt_SetSize(&mt, w, h);
    redraw();
}

void event_start(void)
{
    win_id = dobui_window();

    dobmt_InitFill(&mt, win_id, 0, HEADER_H);
    dobmt_SetSize(&mt, win_w, win_h);
    mt.show_line_numbers = true;
    dobmt_SetWordWrap(&mt, true);

    /* Attach panel BEFORE the first publish.  Do NOT set focus on
     * the mt here — that would lock contextual commands on at boot
     * and hide the app's base commands.  The user clicks the mt to
     * activate clipboard ops; clicks outside to return to base. */
    dobfocus_attach_panel(win_id, "Apri\nSalva copia");

    char startup_path[MAX_PATH_LEN];
    if (g_openfile && g_openfile[0])
    {
        strncpy(startup_path, g_openfile, MAX_PATH_LEN - 1);
        startup_path[MAX_PATH_LEN - 1] = '\0';
        load_file(startup_path);
    }

    update_title();
    update_panel();
    redraw();
}

int main(int argc, char **argv)
{
    if (argc >= 1 && argv[0] && argv[0][0]) g_openfile = argv[0];
    dobui_run("Editor - Nuovo documento", WIN_W, WIN_H);
    return 0;
}
