/* MainDOB keymap -- keyboard layout tray applet.
 *
 * A widget-only program (no main window) that lives in the panel's
 * widget tray. It shows a dropdown of the keyboard layouts found in
 * its own folder; picking one:
 *
 *   1. parses the .kbl data file into a kbd_layout_t,
 *   2. ships it to inputd via INPUT_SETLAYOUT (IPC payload),
 *   3. persists the choice in the config key input.keyboard_layout.
 *
 * At startup it reads that config key and applies the saved layout,
 * so the user's choice survives reboots.
 *
 * Layouts are pure data: a layout has no code, only three scancode
 * tables (normal / shift / altgr). Adding a language = dropping a new
 * <id>.kbl file in this folder -- no recompilation.
 *
 * .kbl file format (see us.kbl / it.kbl):
 *   # comment line
 *   name=<display name>
 *   <scancode> <normal> <shift> <altgr> [<altgr_shift>]
 * where each character field is:
 *   'X'    a quoted literal character
 *   -      no character at this modifier level
 *   0xNN   a numeric codepoint (decimal also accepted)
 * <scancode> is a PS/2 set-1 make code (0..127).  The fifth field
 * <altgr_shift> (AltGr+Shift level) is OPTIONAL: lines with only the
 * first four fields keep that level empty, so older 4-column files
 * stay valid.  It carries e.g. the Italian braces { } which sit on
 * AltGr+Shift of the [ ] keys.
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <DobInterface.h>
#include <DobFileSystem.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/types.h>
#include <dob/input_layout.h>
#include <DobSettings.h>
#include <dropdown.h>

/* GUI event codes (from dobinterface's protocol) */
#define GUI_EVT_WIDGET_CLICK    220

/* Widget geometry */
#define W           210
#define H           200
#define PAD         12
#define TITLE_Y     14
#define DD_Y        44
#define DD_H        24

/* Colors */
#define COL_BG      0x00242430
#define COL_TEXT    0x00FFFFFF
#define COL_DIM     0x00888899

/* Where this program (and its layout files) live */
#define KEYMAP_DIR  "/SYSTEM/PROGRAMS/keymap"

/* Limits */
#define MAX_LAYOUTS 12
#define FBUF_CAP    8192

/* A discovered layout: file id (basename without .kbl) + display name */
typedef struct
{
    char id[32];
    char name[48];
} layout_t;

/* State */
static uint32_t event_port;
static uint32_t inputd_port;
static uint32_t widget_id;

static dobui_widget_fb_ctx_t wctx;
static dob_dropdown_t        dd;

static layout_t    layouts[MAX_LAYOUTS];
static int         layout_count = 0;
static const char *item_ptrs[MAX_LAYOUTS + 1];  /* +1 for ENUM NULL terminator */

/* ------------------------------------------------------------------ */
/* .kbl parsing                                                       */
/* ------------------------------------------------------------------ */

/* Parse one character field. Returns 0..255, or -1 if malformed.
 *   'X'  -> the byte X (closing quote optional)
 *   -    -> 0 (no character)
 *   0xNN / decimal -> the numeric value */
static int parse_char_tok(const char *t)
{
    if (t[0] == '\'')
    {
        if (t[1] == '\0') return -1;
        return (int)(unsigned char)t[1];
    }
    if (t[0] == '-' && t[1] == '\0')
        return 0;

    char *end = NULL;
    long v = strtol(t, &end, 0);
    if (end == t || v < 0 || v > 255)
        return -1;
    return (int)v;
}

/* Parse a whole .kbl buffer.
 *   tbl  : if non-NULL, filled with the scancode tables (zeroed first).
 *   name : if non-NULL, receives the display name from the name= line.
 * Malformed mapping lines are skipped individually -- a bad line never
 * aborts the whole file. */
static void parse_kbl(const char *buf, kbd_layout_t *tbl,
                      char *name, int name_cap)
{
    if (tbl) memset(tbl, 0, sizeof(*tbl));
    if (name && name_cap > 0) name[0] = '\0';

    const char *p = buf;
    while (*p)
    {
        /* Pull one line. */
        char line[160];
        int li = 0;
        while (*p && *p != '\n' && li < (int)sizeof(line) - 1)
            line[li++] = *p++;
        line[li] = '\0';
        if (*p == '\n') p++;

        /* Skip leading whitespace; ignore blanks and comments. */
        char *s = line;
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        if (*s == '\0' || *s == '#')
            continue;

        /* name= line. */
        if (strncmp(s, "name=", 5) == 0)
        {
            if (name && name_cap > 0)
            {
                const char *v = s + 5;
                int i = 0;
                while (v[i] && v[i] != '\r' && i < name_cap - 1)
                {
                    name[i] = v[i];
                    i++;
                }
                while (i > 0 && (name[i - 1] == ' ' || name[i - 1] == '\t'))
                    i--;
                name[i] = '\0';
            }
            continue;
        }

        /* Mapping line: <scancode> <normal> <shift> <altgr>
         * [<altgr_shift>] -- the fifth field is optional. */
        if (!tbl)
            continue;

        char tok[5][32];
        int nt = 0;
        const char *q = s;
        while (nt < 5)
        {
            while (*q == ' ' || *q == '\t') q++;
            /* Stop at end of line or at an inline '#' comment -- the
             * comment must not be picked up as the 5th field. */
            if (*q == '\0' || *q == '\r' || *q == '#') break;
            int ti = 0;
            while (*q && *q != ' ' && *q != '\t' && *q != '\r'
                   && ti < (int)sizeof(tok[0]) - 1)
                tok[nt][ti++] = *q++;
            tok[nt][ti] = '\0';
            nt++;
        }
        if (nt < 4)
            continue;   /* need at least the four base fields */

        char *end = NULL;
        long sc = strtol(tok[0], &end, 0);
        if (end == tok[0] || sc < 0 || sc >= KBD_LAYOUT_KEYS)
            continue;

        int n  = parse_char_tok(tok[1]);
        int sh = parse_char_tok(tok[2]);
        int ag = parse_char_tok(tok[3]);
        int as = (nt >= 5) ? parse_char_tok(tok[4]) : 0;
        if (n < 0 || sh < 0 || ag < 0 || as < 0)
            continue;

        tbl->normal     [sc] = (uint8_t)n;
        tbl->shift      [sc] = (uint8_t)sh;
        tbl->altgr      [sc] = (uint8_t)ag;
        tbl->altgr_shift[sc] = (uint8_t)as;
    }
}

/* Read an entire file into buf (NUL-terminated). Returns bytes read,
 * or -1 on open failure. */
static int slurp(const char *path, char *buf, int cap)
{
    int fd = dobfs_Open(path, FS_READ);
    if (fd < 0)
        return -1;

    int total = 0, n;
    while (total < cap - 1 &&
           (n = dobfs_Read(fd, buf + total, (uint32_t)(cap - 1 - total))) > 0)
        total += n;
    dobfs_Close(fd);

    buf[total] = '\0';
    return total;
}

/* ------------------------------------------------------------------ */
/* Layout discovery                                                   */
/* ------------------------------------------------------------------ */

/* Scan KEYMAP_DIR for *.kbl files and populate layouts[]. */
static void discover_layouts(void)
{
    static dobfs_dirent_t ents[24];
    uint32_t count = 0;

    int rc = dobfs_List(KEYMAP_DIR, ents, 24, &count);
    if (rc < 0)
        return;

    for (uint32_t i = 0; i < count && layout_count < MAX_LAYOUTS; i++)
    {
        const char *nm = ents[i].name;
        int len = (int)strlen(nm);
        if (len < 5 || strcmp(nm + len - 4, ".kbl") != 0)
            continue;

        layout_t *L = &layouts[layout_count];

        /* id = filename without the .kbl suffix */
        int idl = len - 4;
        if (idl > (int)sizeof(L->id) - 1)
            idl = (int)sizeof(L->id) - 1;
        memcpy(L->id, nm, (uint32_t)idl);
        L->id[idl] = '\0';
        L->name[0] = '\0';

        /* Read the name= line out of the file. */
        char path[160];
        sprintf(path, "%s/%s", KEYMAP_DIR, nm);

        static char fbuf[FBUF_CAP];
        if (slurp(path, fbuf, sizeof(fbuf)) >= 0)
            parse_kbl(fbuf, NULL, L->name, sizeof(L->name));

        if (L->name[0] == '\0')
        {
            strncpy(L->name, L->id, sizeof(L->name) - 1);
            L->name[sizeof(L->name) - 1] = '\0';
        }

        layout_count++;
    }
}

/* ------------------------------------------------------------------ */
/* Apply a layout                                                     */
/* ------------------------------------------------------------------ */

/* Load layouts[idx], push it to inputd, and persist the choice. */
static void apply_layout(int idx)
{
    if (idx < 0 || idx >= layout_count)
        return;

    char path[160];
    sprintf(path, "%s/%s.kbl", KEYMAP_DIR, layouts[idx].id);

    static char fbuf[FBUF_CAP];
    if (slurp(path, fbuf, sizeof(fbuf)) < 0)
        return;

    static kbd_layout_t tbl;
    parse_kbl(fbuf, &tbl, NULL, 0);

    /* Hand the table to inputd. */
    if (inputd_port)
    {
        dob_msg_t m, r;
        memset(&m, 0, sizeof(m));
        memset(&r, 0, sizeof(r));
        m.code         = INPUT_SETLAYOUT;
        m.payload      = &tbl;
        m.payload_size = sizeof(tbl);
        dob_ipc_call(inputd_port, &m, &r);
    }

    /* Persistence: a USER-initiated layout change is written back to the
     * keyboard.layout setting by the caller (the tray dropdown handler in
     * main), via writeSetting -- so the choice now survives a reboot.
     * apply_layout itself only applies the table to inputd; it is also
     * called at boot to apply the saved layout, and that path must NOT
     * write (it would rewrite the file on every boot). */
}

/* ------------------------------------------------------------------ */
/* Drawing                                                            */
/* ------------------------------------------------------------------ */

static void draw_widget(void)
{
    dobui_WidgetRestoreContext(&wctx);

    dobdd_ClearGhost(&dd);
    dobui_FillRect(widget_id, 0, 0, W, H, COL_BG);
    dobui_DrawText(widget_id, PAD, TITLE_Y, "Tastiera", COL_TEXT, COL_BG);

    if (layout_count == 0)
        dobui_DrawText(widget_id, PAD, DD_Y, "Nessun layout",
                       COL_DIM, COL_BG);
    else
        dobdd_Draw(&dd);

    dobdd_FlushPopup(&dd);
    dobui_WidgetInvalidate(widget_id);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    event_port = (uint32_t)port_create();

    /* One instance only -- the applet is launched from Startup_modules. */
    if (dob_registry_find("keymap"))
        _exit(0);

    /* The widget tray lives in dobinterface; inputd is the layout sink. */
    dob_registry_wait("dobinterface", 5000);
    inputd_port = dob_registry_find("inputd");
    if (!inputd_port)
        inputd_port = dob_registry_wait("inputd", 3000);

    dob_registry_register("keymap", event_port);

    discover_layouts();

    widget_id = dobui_CreateWidget(W, H, event_port);
    if (widget_id == 0)
        _exit(1);
    dobui_WidgetSaveContext(&wctx);

    /* Build the dropdown over the discovered layout names. */
    for (int i = 0; i < layout_count; i++)
        item_ptrs[i] = layouts[i].name;
    dobdd_Init(&dd, widget_id, PAD, DD_Y, W - 2 * PAD, DD_H,
               item_ptrs, layout_count);
    dd.col_clear = COL_BG;

    /* Declare the layout choice to settingsd so it appears in the
     * DobSettings editor and persists there. The option list is the
     * discovered layout NAMES (NULL-terminated); the default is the
     * first one found. settingsd owns the persisted value -- the user
     * changes it in the editor. The tray dropdown below still applies a
     * layout instantly for the session, but settingsd is the source of
     * truth on the next boot. */
    if (layout_count > 0)
    {
        /* item_ptrs already holds the names; append a NULL terminator
         * for the ENUM option array (one slot beyond layout_count). */
        item_ptrs[layout_count] = 0;
        declareSetting("keyboard.layout", SETTING_ENUM,
                       "Layout tastiera", layouts[0].name,
                       item_ptrs);
    }

    /* Select and apply the layout from settingsd (default: first). */
    if (layout_count > 0)
    {
        int sel = 0;
        const char *cur = getSetting("keyboard.layout");
        if (cur)
        {
            for (int i = 0; i < layout_count; i++)
            {
                /* settingsd stores the NAME (the ENUM option). */
                if (strcmp(cur, layouts[i].name) == 0)
                {
                    sel = i;
                    break;
                }
            }
        }
        dobdd_SetSelected(&dd, sel);
        apply_layout(sel);
    }

    draw_widget();

    /* Event loop: the only events a widget-only program gets are
     * widget clicks. arg3 is the event type (1 = click). */
    for (;;)
    {
        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        dob_ipc_receive(event_port, &msg);

        if (msg.code != GUI_EVT_WIDGET_CLICK)
            continue;
        if (msg.arg3 != 0 && msg.arg3 != 1)
            continue;   /* ignore drag/release */

        int rx = (int)msg.arg1;
        int ry = (int)msg.arg2;

        int before = dd.selected;
        bool consumed = dobdd_OnClick(&dd, rx, ry);
        if (!consumed && dd.open)
            dobdd_Close(&dd);

        if (dd.selected != before)
        {
            apply_layout(dd.selected);
            /* User-initiated change from the tray: persist it so it
             * survives a reboot.  settingsd stores the ENUM option,
             * which is the layout's display NAME (see the boot read
             * above).  Only this user-driven path writes -- the boot
             * apply_layout() call must not, or every boot would rewrite
             * the file. */
            if (dd.selected >= 0 && dd.selected < layout_count)
                writeSetting("keyboard.layout", layouts[dd.selected].name);
        }

        draw_widget();
    }

    return 0;
}
