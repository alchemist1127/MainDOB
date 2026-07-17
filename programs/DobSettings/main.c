/* DobSettings.mdl -- the settings editor.
 *
 * The graphical front end of MainDOB's settings system, and the only
 * program permitted to change a setting's value.  See
 * <dob/dobsettings_protocol.h> for the protocol, the identity model
 * and the entry/field structure.
 *
 * The editor does NOT touch `.setting` files: it is a client of the
 * settings daemon (settingsd).  It calls LIST_FILES to populate its
 * file list, READ_SCHEMA to load a file, and SET_VALUE to commit an
 * edit.  For an EPS-class entry the value is read/written straight to
 * the device driver named in the schema, not through the daemon.
 *
 * Layout:
 *
 *   +----------------+--------------------------------------+
 *   |  file listbox  |  entry rectangles (scrollable panel) |
 *   |  (programs /   |                                      |
 *   |   .setting     |  +--------------------------------+  |
 *   |   files)       |  | label                          |  |
 *   |                |  |  sub  [control]                |  |
 *   |                |  |  sub  [control]                |  |
 *   |                |  +--------------------------------+  |
 *   |                |  +--------------------------------+  |
 *   |                |  | ...                            |  |
 *   +----------------+--------------------------------------+
 *
 * One rectangle == one setting (entry).  A simple setting shows a
 * single control; a composite one (e.g. screen resolution = width +
 * height) shows several, each with its sub-label, and the rectangle
 * grows to fit them.  When the rectangles outrun the panel the right
 * side scrolls.
 *
 * Panel commands: Salva (commit the current file), Ripristina default
 * (reset every entry of the current file to its declared defaults).
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <app.h>
#include <DobInterface.h>

#include <dob/ipc.h>
#include <dob/reconnect.h>
#include <dob/registry.h>
#include <dob/dobsettings_protocol.h>

#include <listview.h>
#include <checkbox.h>
#include <textbox.h>
#include <dropdown.h>
#include <label.h>
#include <button.h>

/* ====================================================================
 *  Layout constants.
 * ==================================================================== */

#define WIN_W            720
#define WIN_H            480

#define LIST_W           200      /* file listbox width                  */
#define SPLIT_X          LIST_W   /* x of the divider                     */
#define PANEL_PAD        12       /* margin around the entry panel        */
#define CARD_GAP         10       /* vertical gap between rectangles      */
#define CARD_PAD         10       /* inner padding of a rectangle         */
#define LABEL_H          20       /* entry-label band height              */
#define FIELD_H          26       /* one control row height               */
#define FIELD_GAP        6        /* gap between control rows             */
#define SUBLABEL_W       96       /* width reserved for a field sub-label */
#define CTRL_W           180      /* control width                        */
#define SCROLL_STEP      40       /* px per wheel notch                   */

#include <dobui_theme.h>
#define COL_BG           DOBUI_SURFACE
#define COL_PANEL_BG     DOBUI_SURFACE
#define COL_CARD_BG      DOBUI_INSET
#define COL_CARD_BORDER  DOBUI_RELIEF
#define COL_DIVIDER      DOBUI_DISABLED
#define COL_LABEL        DOBUI_TEXT
#define COL_SUBLABEL     DOBUI_TEXT_ALT
#define COL_HINT         DOBUI_DISABLED
#define COL_EPS_TAG      DOBUI_TEXT
#define COL_CARD_DISABLED DOBUI_DISABLED   /* greyed rectangle (condition unmet) */
#define COL_LABEL_DISABLED DOBUI_DISABLED

/* Editor-local capacities (not part of the wire contract). */
#define SETTINGS_MAX_FILES_UI    128      /* files shown in the listbox      */
#define SETTINGS_SCHEMA_UI_MAX   65536    /* local copy of a READ_SCHEMA reply */

/* ====================================================================
 *  Editor model -- a mirror of one .setting file's schema, built from a
 *  READ_SCHEMA reply, with one live UI control per field.
 * ==================================================================== */

typedef struct
{
    uint8_t  type;                                   /* SETTING_BOOL/STRING/ENUM */
    char     sublabel[SETTINGS_MAX_SUBLABEL_LEN];
    char     value[SETTINGS_MAX_VALUE_LEN];
    char     defval[SETTINGS_MAX_VALUE_LEN];

    /* Options for a SETTING_ENUM field. opt_ptr[] holds pointers into
     * opt_buf[] for dobdd_Init, which wants a const char ** array. */
    int      option_count;
    char     opt_buf[SETTINGS_MAX_OPTIONS][SETTINGS_MAX_OPTION_LEN];
    const char *opt_ptr[SETTINGS_MAX_OPTIONS];

    /* Exactly one of these is live, per `type`. */
    dob_checkbox_t cb;
    dob_textbox_t  tb;
    dob_dropdown_t dd;
} ed_field_t;

/* One invalidation rule mirrored from the schema. */
typedef struct
{
    char value[SETTINGS_MAX_VALUE_LEN];
    char targets[SETTINGS_MAX_INVAL_TARGETS][SETTINGS_MAX_KEY_LEN];
    int  target_count;
} ed_rule_t;

typedef struct
{
    char     key[SETTINGS_MAX_KEY_LEN];
    char     label[SETTINGS_MAX_LABEL_LEN];
    char     service[SETTINGS_MAX_SERVICE_LEN];
    uint8_t  setting_class;
    uint8_t  field_count;
    uint32_t eps_read_op;
    uint32_t eps_write_op;
    ed_field_t fields[SETTINGS_MAX_FIELDS];

    ed_rule_t rules[SETTINGS_MAX_INVAL_RULES];
    int       rule_count;

    dob_label_t  ui_label;          /* the rectangle's caption */
    int          card_y;            /* panel-space y of the rectangle */
    int          card_h;            /* computed rectangle height */
    bool         active;            /* false = invalidated -> greyed out */
} ed_entry_t;

/* ====================================================================
 *  Global state.
 * ==================================================================== */

static uint32_t win_id;
static int      win_w = WIN_W, win_h = WIN_H;

static uint32_t settingsd_port = 0;     /* cached daemon port, self-healing */

/* The file list (left listbox). */
static char            file_names[SETTINGS_MAX_FILES_UI][SETTINGS_MAX_NAME_LEN];
static const char     *file_name_ptr[SETTINGS_MAX_FILES_UI];
static int             file_count = 0;
static dob_listview_t  file_list;

/* The currently loaded file. */
static ed_entry_t  entries[SETTINGS_MAX_ENTRIES];
static int         entry_count = 0;
static char        cur_file[SETTINGS_MAX_NAME_LEN] = "";
static int         panel_scroll = 0;        /* px scrolled in the right panel */
static int         panel_content_h = 0;     /* total height of all rectangles */
static bool        dirty = false;           /* unsaved edits in the current file */

/* The textbox that currently has keyboard focus, or NULL.  A textbox
 * accepts typed input only while focused, so a click must hand focus
 * to the box under the pointer. */
static dob_textbox_t *focused_tb = NULL;

/* The "Applica" button, rendered at the end of the entry panel.  It
 * commits the file and asks the owning program to reload (see
 * SETTINGS_RELOAD_CALL). */
static dob_button_t  apply_btn;
static int           apply_btn_y = 0;        /* panel-space y of the button */
#define APPLY_BTN_W   140
#define APPLY_BTN_H   28

/* The READ_SCHEMA reply is parsed out of this buffer. */
static char schema_buf[SETTINGS_SCHEMA_UI_MAX];

/* ====================================================================
 *  Bounded string helpers.
 * ==================================================================== */

/* bounded name copy */
static void copy_name(char *dst, const char *src)
{
    int i = 0;
    while (src[i] && i < SETTINGS_MAX_NAME_LEN - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* bounded value copy */
static void copy_value(char *dst, const char *src)
{
    int i = 0;
    while (src[i] && i < SETTINGS_MAX_VALUE_LEN - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Give keyboard focus to one textbox (or NULL to clear), unfocusing the
 * box that previously held it.  A textbox only accepts typed input
 * while focused, so every click that lands on a control routes through
 * here. */
static void focus_textbox(dob_textbox_t *tb)
{
    if (focused_tb == tb)
        return;
    if (focused_tb)
        dobtb_SetFocus(focused_tb, false);
    focused_tb = tb;
    if (focused_tb)
        dobtb_SetFocus(focused_tb, true);
}

/* ====================================================================
 *  Daemon IPC.
 * ==================================================================== */

#define DAEMON_CALL(m, r) \
    _dob_call_reconnect(&settingsd_port, SETTINGS_SERVICE_NAME, 2000, (m), (r))

/* LIST_FILES -- fill file_names[] / file_count. */
static void load_file_list(void)
{
    dob_msg_t msg = {0}, reply = {0};
    const char *p, *end;
    int n = 0;

    file_count = 0;
    msg.code = SETTINGS_LIST_FILES;
    if (DAEMON_CALL(&msg, &reply) != DOB_OK || reply.code != (uint32_t)DOB_OK)
        return;
    if (!reply.payload || reply.payload_size == 0)
        return;

    p   = (const char *)reply.payload;
    end = p + reply.payload_size;
    while (p < end && n < SETTINGS_MAX_FILES_UI)
    {
        uint32_t len = (uint32_t)strlen(p);
        if (p + len >= end) break;
        copy_name(file_names[n], p);
        file_name_ptr[n] = file_names[n];
        n++;
        p += len + 1;
    }
    file_count = n;
}

/* Walk one NUL-terminated string from a bounded buffer. */
static const char *take_str(const char *base, uint32_t size, uint32_t *off)
{
    uint32_t i;
    const char *s;
    if (*off >= size) return 0;
    s = base + *off;
    for (i = *off; i < size && base[i] != '\0'; i++) { }
    if (i >= size) return 0;
    *off = i + 1;
    return s;
}

static const void *take_bytes(const char *base, uint32_t size,
                              uint32_t *off, uint32_t n)
{
    const void *p;
    if (*off + n > size) return 0;
    p = base + *off;
    *off += n;
    return p;
}

/* ====================================================================
 *  EPS-class value I/O -- straight to the device driver, bypassing the
 *  daemon.  The route (service, read op, write op) comes from the
 *  schema.  An EPS entry is always single-field and scalar; the value
 *  travels in arg0 as an enum index / bool, or in the payload string.
 * ==================================================================== */

/* Read an EPS field's live value into f->value. Best-effort: on any
 * failure the declared default already in f->value is kept. */
static void eps_read_value(ed_entry_t *e, ed_field_t *f)
{
    uint32_t port;
    dob_msg_t msg = {0}, reply = {0};

    if (e->service[0] == '\0' || e->eps_read_op == 0)
        return;
    port = dob_registry_wait(e->service, 2000);
    if (!port)
        return;

    msg.code = e->eps_read_op;
    if (dob_ipc_call(port, &msg, &reply) != DOB_OK)
        return;
    if (reply.code != (uint32_t)DOB_OK)
        return;

    if (reply.payload && reply.payload_size > 0)
    {
        uint32_t c = reply.payload_size;
        if (c > sizeof(f->value)) c = sizeof(f->value);
        memcpy(f->value, reply.payload, c);
        f->value[c - 1] = '\0';
    }
    else if (f->type == SETTING_ENUM)
    {
        /* Scalar reply: arg0 is the option index. */
        if (reply.arg0 < (uint32_t)f->option_count)
            copy_value(f->value, f->opt_buf[reply.arg0]);
    }
    else if (f->type == SETTING_BOOL)
    {
        copy_value(f->value, reply.arg0 ? "true" : "false");
    }
}

/* Write an EPS field's value to its device. */
static bool eps_write_value(ed_entry_t *e, ed_field_t *f, const char *value)
{
    uint32_t port;
    dob_msg_t msg = {0}, reply = {0};
    int i;

    if (e->service[0] == '\0' || e->eps_write_op == 0)
        return false;
    port = dob_registry_wait(e->service, 2000);
    if (!port)
        return false;

    msg.code = e->eps_write_op;
    if (f->type == SETTING_ENUM)
    {
        for (i = 0; i < f->option_count; i++)
            if (strcmp(value, f->opt_buf[i]) == 0) { msg.arg0 = (uint32_t)i; break; }
    }
    else if (f->type == SETTING_BOOL)
    {
        msg.arg0 = (strcmp(value, "true") == 0) ? 1u : 0u;
    }
    else
    {
        msg.payload      = (void *)value;
        msg.payload_size = (uint32_t)strlen(value) + 1u;
    }

    if (dob_ipc_call(port, &msg, &reply) != DOB_OK)
        return false;
    return reply.code == (uint32_t)DOB_OK;
}

/* ====================================================================
 *  Layout -- compute each rectangle's height and stacking y.
 * ==================================================================== */

static int field_row_count(const ed_entry_t *e)
{
    return (e->field_count < 1) ? 1 : e->field_count;
}

/* A rectangle: label band + one row per field + inner padding. */
static int card_height(const ed_entry_t *e)
{
    int rows = field_row_count(e);
    return CARD_PAD + LABEL_H
         + rows * FIELD_H + (rows - 1) * FIELD_GAP
         + CARD_PAD;
}

/* Recompute card_y / card_h for every entry, place the Apply button at
 * the end, and total the content height.  Called after a load and after
 * a resize. */
static void relayout(void)
{
    int y = PANEL_PAD;
    int i;
    for (i = 0; i < entry_count; i++)
    {
        entries[i].card_h = card_height(&entries[i]);
        entries[i].card_y = y;
        y += entries[i].card_h + CARD_GAP;
    }

    /* The Apply button sits below the last rectangle; it is part of the
     * scrolling content, so it is reachable at "the end of the page". */
    apply_btn_y = y;
    panel_content_h = (entry_count > 0)
                        ? (y + APPLY_BTN_H + PANEL_PAD)
                        : 0;

    /* Clamp the scroll offset to the new content height. */
    {
        int max_scroll = panel_content_h - win_h;
        if (max_scroll < 0) max_scroll = 0;
        if (panel_scroll > max_scroll) panel_scroll = max_scroll;
        if (panel_scroll < 0) panel_scroll = 0;
    }
}

/* Place the live controls of one entry at their absolute window
 * coordinates, given the rectangle's current on-screen y. */
static void place_entry_controls(ed_entry_t *e, int card_screen_y)
{
    int ctrl_x = SPLIT_X + PANEL_PAD + CARD_PAD + SUBLABEL_W;
    int row_y  = card_screen_y + CARD_PAD + LABEL_H;
    int f;

    e->ui_label.x = SPLIT_X + PANEL_PAD + CARD_PAD;
    e->ui_label.y = card_screen_y + CARD_PAD;

    for (f = 0; f < e->field_count; f++)
    {
        ed_field_t *fl = &e->fields[f];
        int cy = row_y + f * (FIELD_H + FIELD_GAP);

        switch (fl->type)
        {
            case SETTING_BOOL:
                fl->cb.x = ctrl_x;
                fl->cb.y = cy + (FIELD_H - DOBCB_DEFAULT_SIZE) / 2;
                break;
            case SETTING_ENUM:
                fl->dd.x = ctrl_x;
                fl->dd.y = cy + (FIELD_H - DOBDD_DEFAULT_H) / 2;
                break;
            case SETTING_STRING:
            default:
                fl->tb.x = ctrl_x;
                fl->tb.y = cy + (FIELD_H - DOBTB_DEFAULT_H) / 2;
                break;
        }
    }
}

/* ====================================================================
 *  Building the editor model from a READ_SCHEMA reply.
 * ==================================================================== */

/* Initialise the live UI control of one field from its type/value. */
static void build_field_control(ed_field_t *f)
{
    int i;

    switch (f->type)
    {
        case SETTING_BOOL:
            dobcb_Init(&f->cb, win_id, 0, 0, 0, "");
            dobcb_SetChecked(&f->cb, strcmp(f->value, "true") == 0);
            break;

        case SETTING_ENUM:
            for (i = 0; i < f->option_count; i++)
                f->opt_ptr[i] = f->opt_buf[i];
            dobdd_Init(&f->dd, win_id, 0, 0, CTRL_W, 0,
                       f->opt_ptr, f->option_count);
            f->dd.col_clear = COL_CARD_BG;
            for (i = 0; i < f->option_count; i++)
                if (strcmp(f->value, f->opt_buf[i]) == 0)
                    { dobdd_SetSelected(&f->dd, i); break; }
            break;

        case SETTING_STRING:
        case SETTING_SECRET:
        default:
            dobtb_Init(&f->tb, win_id, 0, 0, CTRL_W, DOBTB_DEFAULT_H);
            dobtb_SetText(&f->tb, f->value);
            /* SECRET: eco a pallini e clipboard-out rifiutata, gestiti
             * dentro il controllo stesso. Tutto il resto del ciclo di
             * vita (place/draw/harvest/reset) e' identico a STRING. */
            f->tb.masked = (f->type == SETTING_SECRET);
            break;
    }
}

/* Parse the whole READ_SCHEMA reply into entries[]. Returns entry
 * count, or -1 on a malformed reply. */
static int parse_schema(const char *base, uint32_t size, uint32_t want)
{
    uint32_t off = 0;
    uint32_t i;
    int n = 0;

    for (i = 0; i < want && n < SETTINGS_MAX_ENTRIES; i++)
    {
        settings_entry_hdr_t eh;
        const void *raw;
        const char *key, *label;
        ed_entry_t *e;
        int f;

        raw = take_bytes(base, size, &off, sizeof(eh));
        if (!raw) return -1;
        memcpy(&eh, raw, sizeof(eh));

        key   = take_str(base, size, &off);
        label = take_str(base, size, &off);
        if (!key || !label) return -1;

        e = &entries[n];
        memset(e, 0, sizeof(*e));
        copy_name(e->key, key);
        copy_name(e->label, label);
        e->setting_class = eh.setting_class;
        e->field_count   = eh.field_count;
        e->eps_read_op   = eh.eps_read_op;
        e->eps_write_op  = eh.eps_write_op;
        if (e->field_count < 1 || e->field_count > SETTINGS_MAX_FIELDS)
            return -1;

        if (e->setting_class == SETTING_CLASS_EPS)
        {
            const char *svc = take_str(base, size, &off);
            if (!svc) return -1;
            copy_name(e->service, svc);
        }

        for (f = 0; f < e->field_count; f++)
        {
            settings_field_hdr_t fh;
            const char *sub, *val, *def;
            ed_field_t *fl = &e->fields[f];
            int o;

            raw = take_bytes(base, size, &off, sizeof(fh));
            if (!raw) return -1;
            memcpy(&fh, raw, sizeof(fh));

            sub = take_str(base, size, &off);
            val = take_str(base, size, &off);
            def = take_str(base, size, &off);
            if (!sub || !val || !def) return -1;

            fl->type = fh.type;
            copy_name(fl->sublabel, sub);
            copy_value(fl->value, val);
            copy_value(fl->defval, def);

            fl->option_count = (fh.option_count > SETTINGS_MAX_OPTIONS)
                                 ? SETTINGS_MAX_OPTIONS : fh.option_count;
            for (o = 0; o < (int)fh.option_count; o++)
            {
                const char *opt = take_str(base, size, &off);
                if (!opt) return -1;
                if (o < fl->option_count)
                    copy_name(fl->opt_buf[o], opt);
            }

            /* For an EPS entry the schema value is just a placeholder;
             * read the device's live value before building the control. */
            if (e->setting_class == SETTING_CLASS_EPS)
                eps_read_value(e, fl);

            build_field_control(fl);
        }

        /* Invalidation-rule blocks: option_value, target_count byte,
         * then that many target keys. */
        e->rule_count = (eh.inval_rule_count > SETTINGS_MAX_INVAL_RULES)
                          ? SETTINGS_MAX_INVAL_RULES : eh.inval_rule_count;
        {
            int r, t;
            for (r = 0; r < (int)eh.inval_rule_count; r++)
            {
                const char *rv = take_str(base, size, &off);
                const void *tcraw;
                int tc;
                if (!rv) return -1;
                tcraw = take_bytes(base, size, &off, 1);
                if (!tcraw) return -1;
                tc = (int)(*(const unsigned char *)tcraw);

                if (r < e->rule_count)
                    copy_value(e->rules[r].value, rv);

                for (t = 0; t < tc; t++)
                {
                    const char *tk = take_str(base, size, &off);
                    if (!tk) return -1;
                    if (r < e->rule_count
                        && e->rules[r].target_count < SETTINGS_MAX_INVAL_TARGETS)
                        copy_name(e->rules[r].targets[e->rules[r].target_count++],
                                  tk);
                }
            }
        }

        doblbl_Init(&e->ui_label, win_id, 0, 0, e->label);
        e->ui_label.col_text = COL_LABEL;
        e->ui_label.col_bg   = COL_CARD_BG;
        n++;
    }
    return n;
}

/* READ_SCHEMA the named file and rebuild the editor model. */
static void open_file(const char *name)
{
    dob_msg_t msg = {0}, reply = {0};
    uint32_t copy;
    int n;

    entry_count  = 0;
    panel_scroll = 0;
    dirty        = false;
    focused_tb   = NULL;       /* previous file's textboxes are gone */
    copy_name(cur_file, name);

    msg.code         = SETTINGS_READ_SCHEMA;
    msg.payload      = (void *)name;
    msg.payload_size = (uint32_t)strlen(name) + 1u;

    if (DAEMON_CALL(&msg, &reply) != DOB_OK || reply.code != (uint32_t)DOB_OK)
        return;
    if (!reply.payload || reply.payload_size == 0)
        return;

    copy = reply.payload_size;
    if (copy > sizeof(schema_buf)) copy = sizeof(schema_buf);
    memcpy(schema_buf, reply.payload, copy);

    n = parse_schema(schema_buf, copy, reply.arg0);
    entry_count = (n < 0) ? 0 : n;

    /* (Re)create the Apply button for this file. */
    dobbtn_Init(&apply_btn, win_id, 0, 0, APPLY_BTN_W, APPLY_BTN_H,
                "Applica");

    relayout();
}

/* ====================================================================
 *  Committing edits.
 * ==================================================================== */

/* Pull the live control's value back into the field's value string. */
static void harvest_field(ed_field_t *f)
{
    switch (f->type)
    {
        case SETTING_BOOL:
            copy_value(f->value, f->cb.checked ? "true" : "false");
            break;
        case SETTING_ENUM:
        {
            const char *s = dobdd_GetSelectedText(&f->dd);
            if (s) copy_value(f->value, s);
            break;
        }
        case SETTING_STRING:
        default:
        {
            const char *s = dobtb_GetText(&f->tb);
            if (s) copy_value(f->value, s);
            break;
        }
    }
}

/* Commit one entry. FILE-class -> one SET_VALUE of the composite;
 * EPS-class -> a direct write to the device. Returns true on success. */
static bool commit_entry(ed_entry_t *e)
{
    int f;
    for (f = 0; f < e->field_count; f++)
        harvest_field(&e->fields[f]);

    if (e->setting_class == SETTING_CLASS_EPS)
    {
        /* Single-field by definition. */
        ed_field_t *f0 = &e->fields[0];

        /* Un campo SECRET e' un handshake, non uno stato: si spedisce
         * al servizio SOLO se l'utente lo ha davvero toccato in questa
         * sessione. Senza questo cancello, ogni Applica del file
         * ricommetterebbe anche la casella intonsa (apply_current_file
         * scorre TUTTE le entry) e il servizio riceverebbe un falso
         * tentativo a ogni salvataggio di un'altra impostazione. */
        if (f0->type == SETTING_SECRET && !f0->tb.modified)
            return true;

        bool ok = eps_write_value(e, f0, f0->value);
        if (f0->type == SETTING_SECRET && ok)
        {
            /* Consumato: il segreto non resta nel controllo (ne' nel
             * value harvestato). Su fallimento resta, cosi' l'utente
             * corregge e riprova senza ribattere tutto. */
            dobtb_SetText(&f0->tb, "");
            f0->value[0] = '\0';
        }
        return ok;
    }
    else
    {
        char composite[SETTINGS_MAX_COMPOSITE_LEN];
        char payload[SETTINGS_MAX_NAME_LEN + SETTINGS_MAX_KEY_LEN
                     + SETTINGS_MAX_COMPOSITE_LEN + 8];
        uint32_t pos = 0, cpos = 0, l;
        dob_msg_t msg = {0}, reply = {0};

        /* Join the field values with the protocol separator. */
        for (f = 0; f < e->field_count; f++)
        {
            if (f > 0 && cpos < sizeof(composite) - 1)
                composite[cpos++] = SETTINGS_FIELD_SEP;
            l = (uint32_t)strlen(e->fields[f].value);
            if (cpos + l >= sizeof(composite)) l = sizeof(composite) - 1 - cpos;
            memcpy(composite + cpos, e->fields[f].value, l);
            cpos += l;
        }
        composite[cpos] = '\0';

        /* payload = name '\0' key '\0' composite '\0' */
        #define PUTP(s) do {                              \
            uint32_t _l = (uint32_t)strlen(s) + 1u;       \
            memcpy(payload + pos, (s), _l); pos += _l;    \
        } while (0)
        PUTP(cur_file);
        PUTP(e->key);
        PUTP(composite);
        #undef PUTP

        msg.code         = SETTINGS_SET_VALUE;
        msg.payload      = payload;
        msg.payload_size = pos;

        if (DAEMON_CALL(&msg, &reply) != DOB_OK)
            return false;
        return reply.code == (uint32_t)DOB_OK;
    }
}

/* Notify the owning program that its settings changed.  Delivered as a
 * fire-and-forget POST (dob_ipc_post), not a synchronous call: the
 * editor must never block on the program's reload handler -- a slow,
 * buggy, or non-replying handler would otherwise freeze the editor.
 * The program receives it in event_request and owes no reply.
 *
 * Best-effort: if the program is not running or did not register, the
 * lookup fails and the new values simply take effect on its next
 * launch. */
static void notify_program_reload(const char *program_name)
{
    uint32_t port;
    dob_msg_t msg = {0};

    port = dob_registry_find(program_name);   /* no wait: don't block */
    if (!port)
        return;

    msg.code = SETTINGS_RELOAD_CALL;
    dob_ipc_post(port, &msg);                  /* returns immediately */
}

/* The "Applica" action: commit every entry of the current file, then
 * ask the owning program to re-read its settings. */
static void apply_current_file(void)
{
    int i, ok = 0;
    for (i = 0; i < entry_count; i++)
        if (commit_entry(&entries[i]))
            ok++;
    if (ok == entry_count)
        dirty = false;

    if (cur_file[0])
        notify_program_reload(cur_file);
}

/* Push a field's declared default into its live control. */
static void reset_field_to_default(ed_field_t *f)
{
    int i;
    copy_value(f->value, f->defval);
    switch (f->type)
    {
        case SETTING_BOOL:
            dobcb_SetChecked(&f->cb, strcmp(f->value, "true") == 0);
            break;
        case SETTING_ENUM:
            for (i = 0; i < f->option_count; i++)
                if (strcmp(f->value, f->opt_buf[i]) == 0)
                    { dobdd_SetSelected(&f->dd, i); break; }
            break;
        case SETTING_STRING:
        default:
            dobtb_SetText(&f->tb, f->value);
            break;
    }
}

/* Panel command "Ripristina default": every entry back to its
 * declared defaults.  Per the protocol this needs no dedicated opcode
 * -- it is an ordinary edit the user must still Salva to commit. */
static void reset_current_file(void)
{
    int i, f;
    for (i = 0; i < entry_count; i++)
        for (f = 0; f < entries[i].field_count; f++)
            reset_field_to_default(&entries[i].fields[f]);
    dirty = true;
}

/* ====================================================================
 *  Dependencies -- entries that invalidate others.
 * ==================================================================== */

static ed_entry_t *find_entry_by_key(const char *key)
{
    int i;
    for (i = 0; i < entry_count; i++)
        if (strcmp(entries[i].key, key) == 0)
            return &entries[i];
    return (ed_entry_t *)0;
}

/* Current live value of an entry's first field (the controlling field
 * a dependency tests). */
static void entry_live_value0(ed_entry_t *e, char *out, int cap)
{
    ed_field_t *f = &e->fields[0];
    const char *s = (const char *)0;
    int i = 0;

    out[0] = '\0';
    switch (f->type)
    {
        case SETTING_BOOL:   s = f->cb.checked ? "true" : "false"; break;
        case SETTING_ENUM:   s = dobdd_GetSelectedText(&f->dd);    break;
        case SETTING_STRING:
        default:             s = dobtb_GetText(&f->tb);           break;
    }
    while (s && s[i] && i < cap - 1) { out[i] = s[i]; i++; }
    out[i] = '\0';
}

/* Recompute every entry's `active` flag from its enable-condition.
 * An entry with no condition, or whose controlling entry is absent, is
 * active.  Called on every redraw, so greying always reflects the live
 * state of the controlling control. */
/* Recompute every entry's `active` flag.  All entries start active;
 * then each entry's rules are evaluated against its own control's live
 * value -- a matching rule greys out (deactivates) its target entries.
 * Called on every redraw, so greying always reflects live state. */
static void recompute_active(void)
{
    int i, r, t, f;

    for (i = 0; i < entry_count; i++)
        entries[i].active = true;

    /* Apply every firing rule.  A target greyed by any rule stays
     * greyed (disables win over the default-active). */
    for (i = 0; i < entry_count; i++)
    {
        ed_entry_t *c = &entries[i];
        char cur[SETTINGS_MAX_VALUE_LEN];

        if (c->rule_count == 0)
            continue;
        entry_live_value0(c, cur, sizeof(cur));

        for (r = 0; r < c->rule_count; r++)
        {
            if (strcmp(c->rules[r].value, cur) != 0)
                continue;                       /* this option not selected */
            for (t = 0; t < c->rules[r].target_count; t++)
            {
                ed_entry_t *te = find_entry_by_key(c->rules[r].targets[t]);
                if (te)
                    te->active = false;
            }
        }
    }

    /* Drop keyboard focus out of any entry that just went inactive. */
    for (i = 0; i < entry_count; i++)
        if (!entries[i].active)
            for (f = 0; f < entries[i].field_count; f++)
                if (entries[i].fields[f].type == SETTING_STRING
                    && focused_tb == &entries[i].fields[f].tb)
                    focus_textbox((dob_textbox_t *)0);
}

/* ====================================================================
 *  Drawing.
 * ==================================================================== */

static void draw_field_row(ed_entry_t *e, ed_field_t *f,
                           int row_screen_y, uint32_t card_bg)
{
    int sub_x = SPLIT_X + PANEL_PAD + CARD_PAD;

    /* Sub-label (blank for a simple single-field setting). */
    if (f->sublabel[0])
        dobui_DrawText(win_id, sub_x, row_screen_y + 5,
                       f->sublabel,
                       e->active ? COL_SUBLABEL : COL_LABEL_DISABLED, card_bg);

    switch (f->type)
    {
        case SETTING_BOOL:   dobcb_Draw(&f->cb); break;
        case SETTING_ENUM:   dobdd_Draw(&f->dd); break;   /* popup flushed later */
        case SETTING_STRING:
        default:             dobtb_Draw(&f->tb); break;
    }
}

static void draw_entry_card(ed_entry_t *e)
{
    int card_y = e->card_y - panel_scroll;
    int card_x = SPLIT_X + PANEL_PAD;
    int card_w = win_w - SPLIT_X - 2 * PANEL_PAD;
    int row_y  = card_y + CARD_PAD + LABEL_H;
    uint32_t card_bg = e->active ? COL_CARD_BG : COL_CARD_DISABLED;
    int f;

    /* Off-screen rectangles are skipped entirely. */
    if (card_y + e->card_h < 0 || card_y > win_h)
        return;

    /* An inactive entry (dependency unmet) is greyed and its controls
     * disabled, so the user can see it does not currently apply. */
    for (f = 0; f < e->field_count; f++)
    {
        ed_field_t *fl = &e->fields[f];
        switch (fl->type)
        {
            case SETTING_BOOL: dobcb_SetEnabled(&fl->cb, e->active); break;
            case SETTING_ENUM: fl->dd.enabled = e->active;           break;
            case SETTING_STRING:
            default:           fl->tb.enabled = e->active;           break;
        }
    }

    dobui_FillRect(win_id, card_x, card_y, card_w, e->card_h, card_bg);
    dobui_DrawRect(win_id, card_x, card_y, card_w, e->card_h, COL_CARD_BORDER);

    place_entry_controls(e, card_y);

    e->ui_label.col_bg   = card_bg;
    e->ui_label.col_text = e->active ? COL_LABEL : COL_LABEL_DISABLED;
    doblbl_Draw(&e->ui_label);

    /* An EPS-class entry is tagged so the user knows it acts on a
     * device directly. */
    if (e->setting_class == SETTING_CLASS_EPS)
        dobui_DrawText(win_id,
                       card_x + card_w - 11 * DOBLBL_FONT_W, card_y + CARD_PAD,
                       "[dispositivo]", COL_EPS_TAG, card_bg);

    for (f = 0; f < e->field_count; f++)
        draw_field_row(e, &e->fields[f],
                       row_y + f * (FIELD_H + FIELD_GAP), card_bg);
}

static void draw_all(void)
{
    int i;

    /* Greying reflects the live state of controlling controls. */
    recompute_active();

    /* Background + the two panes. */
    dobui_FillRect(win_id, 0, 0, win_w, win_h, COL_BG);
    dobui_FillRect(win_id, SPLIT_X, 0, win_w - SPLIT_X, win_h, COL_PANEL_BG);
    dobui_FillRect(win_id, SPLIT_X, 0, 1, win_h, COL_DIVIDER);

    /* Left: file listbox. */
    doblv_Draw(&file_list);

    /* Right: entry rectangles, or a hint when nothing is selected. */
    if (entry_count == 0)
    {
        const char *hint = (cur_file[0] == '\0')
            ? "Seleziona un programma a sinistra."
            : "Nessuna impostazione per questo programma.";
        dobui_DrawText(win_id, SPLIT_X + PANEL_PAD, PANEL_PAD,
                       hint, COL_HINT, COL_PANEL_BG);
    }
    else
    {
        for (i = 0; i < entry_count; i++)
            draw_entry_card(&entries[i]);

        /* The Apply button, at the end of the content. */
        apply_btn.x = SPLIT_X + PANEL_PAD;
        apply_btn.y = apply_btn_y - panel_scroll;
        dobbtn_Draw(&apply_btn);

        /* Dropdown popups must be flushed on top of every rectangle. */
        for (i = 0; i < entry_count; i++)
        {
            int f;
            for (f = 0; f < entries[i].field_count; f++)
                if (entries[i].fields[f].type == SETTING_ENUM)
                    dobdd_FlushPopup(&entries[i].fields[f].dd);
        }
    }

    dobui_Invalidate(win_id);
}

/* ====================================================================
 *  Event hit-testing helpers.
 * ==================================================================== */

static bool in_left_pane(int x)  { return x < SPLIT_X; }

/* Route a click in the right panel to whatever field control is under
 * it.  Returns true if a control consumed the click. */
static bool panel_click(int x, int y)
{
    int i, f;
    bool consumed = false;

    /* The Apply button, drawn at the end of the content. */
    apply_btn.x = SPLIT_X + PANEL_PAD;
    apply_btn.y = apply_btn_y - panel_scroll;
    if (dobbtn_OnClick(&apply_btn, x, y))
    {
        apply_btn.clicked = false;
        focus_textbox(NULL);
        apply_current_file();
        return true;
    }

    for (i = 0; i < entry_count; i++)
    {
        ed_entry_t *e = &entries[i];
        int card_y = e->card_y - panel_scroll;
        if (card_y + e->card_h < 0 || card_y > win_h)
            continue;
        if (!e->active)             /* dependency unmet: not editable */
            continue;

        place_entry_controls(e, card_y);
        for (f = 0; f < e->field_count; f++)
        {
            ed_field_t *fl = &e->fields[f];
            switch (fl->type)
            {
                case SETTING_BOOL:
                    if (dobcb_OnClick(&fl->cb, x, y))
                        { consumed = true; focus_textbox(NULL); }
                    break;
                case SETTING_ENUM:
                    if (dobdd_OnClick(&fl->dd, x, y))
                        { consumed = true; focus_textbox(NULL); }
                    break;
                case SETTING_STRING:
                default:
                    if (dobtb_OnClick(&fl->tb, x, y))
                    {
                        /* Hand keyboard focus to the clicked textbox --
                         * without this it accepts the click but ignores
                         * typing. */
                        consumed = true;
                        focus_textbox(&fl->tb);
                    }
                    break;
            }
        }
    }
    if (consumed) dirty = true;
    return consumed;
}

/* ====================================================================
 *  Event handlers (dobUI app framework).
 * ==================================================================== */

void event_start(void)
{
    win_id = dobui_window();
    win_w  = dobui_width();
    win_h  = dobui_height();

    doblv_Init(&file_list, win_id, 0, 0, LIST_W, win_h);

    load_file_list();
    doblv_SetItems(&file_list, file_name_ptr, file_count);

    draw_all();
}

void event_mouseclick(int x, int y, uint8_t buttons)
{
    (void)buttons;

    if (in_left_pane(x))
    {
        if (doblv_OnClick(&file_list, x, y))
        {
            const char *sel = doblv_GetSelectedText(&file_list);
            focus_textbox(NULL);                 /* leave any textbox */
            doblv_SetFocus(&file_list, true);
            if (sel) open_file(sel);
            draw_all();
        }
        return;
    }

    /* A click in the right panel takes keyboard focus away from the
     * file list. */
    doblv_SetFocus(&file_list, false);
    if (panel_click(x, y))
        draw_all();
}

void event_dblclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (!in_left_pane(x))
    {
        int i, f;
        for (i = 0; i < entry_count; i++)
        {
            ed_entry_t *e = &entries[i];
            int card_y = e->card_y - panel_scroll;
            if (!e->active)
                continue;
            place_entry_controls(e, card_y);
            for (f = 0; f < e->field_count; f++)
                if (e->fields[f].type == SETTING_STRING)
                    dobtb_OnDblClick(&e->fields[f].tb, x, y);
        }
        draw_all();
    }
}

void event_mousemove(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (focused_tb && dobtb_OnDrag(focused_tb, x, y)) draw_all();   /* drag-select in the focused field */
}

void event_mouserelease(int x, int y, uint8_t buttons)
{
    (void)x; (void)y; (void)buttons;
    if (focused_tb) dobtb_OnRelease(focused_tb);                    /* end the in-flight selection */
}

void event_key(uint8_t key)
{
    /* The left listbox has priority when it holds focus. */
    if (file_list.focused && doblv_OnKey(&file_list, key))
    {
        const char *sel = doblv_GetSelectedText(&file_list);
        if (sel) open_file(sel);
        draw_all();
        return;
    }

    /* Otherwise the keypress goes to the focused textbox, if any.  A
     * textbox accepts input only while focused, so routing it anywhere
     * else would be ignored. */
    if (focused_tb && dobtb_OnKey(focused_tb, key))
    {
        dirty = true;
        draw_all();
    }
}

void event_scroll(int delta)
{
    int max_scroll;

    /* A scroll over the right panel moves the rectangle stack. */
    panel_scroll -= delta * SCROLL_STEP;

    max_scroll = panel_content_h - win_h;
    if (max_scroll < 0) max_scroll = 0;
    if (panel_scroll > max_scroll) panel_scroll = max_scroll;
    if (panel_scroll < 0) panel_scroll = 0;

    draw_all();
}

void event_resize(int w, int h)
{
    win_w = w;
    win_h = h;

    file_list.h = h;
    relayout();
    draw_all();
}

void event_panel(int cmd_idx)
{
    switch (cmd_idx)
    {
        case 0:  reset_current_file(); break;   /* Ripristina default */
        default: break;
    }
    draw_all();
}

void event_close(void)
{
    dobui_quit();
}

int main(void)
{
    /* "Applica" is an on-page button at the end of the panel; the
     * panel keeps only the reset command. */
    dobui_set_panel("Ripristina default");
    dobui_run("Impostazioni", WIN_W, WIN_H);
    return 0;
}
