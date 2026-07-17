/* DobUITools — Focus Manager Implementation
 *
 * Pure routing: the focus manager never draws. It tracks which
 * widget is focused, dispatches events to it, and — when the app
 * has handed over panel management via dobfocus_AttachPanel — keeps
 * the window's command panel in sync with the focused widget's
 * contextual state.
 *
 * Panel refresh runs at every point where context could change
 * (focus transitions, events dispatched to a text widget, panel
 * commands). The refresh is cheap when nothing actually changed —
 * a signature hash short-circuits the SetPanelCommands IPC — so
 * being aggressive about calling it is harmless.
 */

#include "focus.h"
#include "button.h"
#include "picturebutton.h"
#include "textbox.h"
#include "cliptext.h"
#include "dropdown.h"
#include "checkbox.h"
#include "switch.h"
#include "slider.h"
#include "radiogroup.h"
#include "listview.h"
#include "checked_listview.h"
#include "table.h"
#include <DobInterface.h>
#include <string.h>
#include <stdio.h>

/* Init / registration */

void dobfocus_Init(dob_focus_t *fm)
{
    memset(fm, 0, sizeof(*fm));
    fm->focused = -1;
}

int dobfocus_Register(dob_focus_t *fm, void *ctrl, dob_ctrl_type_t type)
{
    if (fm->count >= DOBFOCUS_MAX) return -1;
    int idx = fm->count;
    fm->entries[idx].ctrl = ctrl;
    fm->entries[idx].type = type;
    fm->count++;
    return idx;
}

/* Per-type SetFocus dispatch */

static void ctrl_set_focus(dob_focus_entry_t *e, bool val)
{
    switch (e->type)
    {
        case DOB_CTRL_BUTTON:        dobbtn_SetFocus(e->ctrl, val);  break;
        case DOB_CTRL_PICTUREBUTTON: dobpbtn_SetFocus(e->ctrl, val); break;
        case DOB_CTRL_TEXTBOX:       dobtb_SetFocus(e->ctrl, val);   break;
        case DOB_CTRL_MULTITEXTBOX:  dobmt_SetFocus(e->ctrl, val);   break;
        case DOB_CTRL_DROPDOWN:      dobdd_SetFocus(e->ctrl, val);   break;
        case DOB_CTRL_CHECKBOX:      dobcb_SetFocus(e->ctrl, val);   break;
        case DOB_CTRL_SWITCH:        dobsw_SetFocus(e->ctrl, val);   break;
        case DOB_CTRL_SLIDER:        dobsl_SetFocus(e->ctrl, val);   break;
        case DOB_CTRL_RADIOGROUP:    dobrg_SetFocus(e->ctrl, val);   break;
        case DOB_CTRL_LISTVIEW:      doblv_SetFocus(e->ctrl, val);   break;
        case DOB_CTRL_CHECKED_LISTVIEW: dobclv_SetFocus(e->ctrl, val); break;
        case DOB_CTRL_TABLE:         dobtbl_SetFocus(e->ctrl, val);  break;
        default: break;
    }
}

/*  *  Contextual panel model
 *
 *  Each command the focus manager can publish has a stable id, a
 *  label and a handler. The live list is rebuilt every Refresh.
 */

typedef enum
{
    CTX_PASTE = 0,
    CTX_CLEAR,
    CTX_COPY_ALL,
    CTX_COPY,
    CTX_CUT,
    CTX_COUNT_
} ctx_cmd_id_t;

static const char *const CTX_LABEL[CTX_COUNT_] =
{
    "Incolla",
    "Pulisci",
    "Copia tutto",
    "Copia",
    "Taglia",
};

typedef struct
{
    int          n;
    ctx_cmd_id_t ids[CTX_COUNT_];
} ctx_active_t;

static ctx_active_t g_active;

static void ctx_build(dob_focus_t *fm)
{
    g_active.n = 0;
    if (fm->focused < 0) return;

    dob_focus_entry_t *e = &fm->entries[fm->focused];
    bool has_sel, has_text;
    if (e->type == DOB_CTRL_TEXTBOX)
    {
        dob_textbox_t *tb = e->ctrl;
        has_sel  = dobtb_HasSelection(tb);
        has_text = tb->len > 0;
    }
    else if (e->type == DOB_CTRL_MULTITEXTBOX)
    {
        dob_multitextbox_t *mt = e->ctrl;
        has_sel  = dobmt_HasSelection(mt);
        has_text = mt->len > 0;
    }
    else
    {
        return;
    }

    /* cliptext_size is O(1) on the hot path (mapped SHM read). */
    if (dobui_cliptext_size() > 0) g_active.ids[g_active.n++] = CTX_PASTE;
    if (has_text)                  g_active.ids[g_active.n++] = CTX_CLEAR;
    if (has_text)                  g_active.ids[g_active.n++] = CTX_COPY_ALL;
    if (has_sel)                   g_active.ids[g_active.n++] = CTX_COPY;
    if (has_sel)                   g_active.ids[g_active.n++] = CTX_CUT;
}

static void ctx_serialize(char *buf, int bufsize)
{
    int off = 0;
    for (int i = 0; i < g_active.n && off < bufsize - 1; i++)
    {
        const char *s = CTX_LABEL[g_active.ids[i]];
        int n = (int)strlen(s);
        if (i > 0 && off < bufsize - 1) buf[off++] = '\n';
        if (off + n > bufsize - 1) n = bufsize - 1 - off;
        memcpy(buf + off, s, (uint32_t)n);
        off += n;
    }
    buf[off] = '\0';
}

/* FNV-1a 32-bit — detects whether the serialized panel actually
 * changed, lets us skip redundant SetPanelCommands IPC. */
static uint32_t ctx_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

void dobfocus_AttachPanel(dob_focus_t *fm, uint32_t win_id,
                          const char *base_cmds)
{
    fm->panel_win_id   = win_id;
    fm->panel_base     = base_cmds;
    fm->panel_attached = true;
    fm->panel_sig      = 0;
    dobfocus_RefreshPanel(fm);
}

void dobfocus_SetBasePanel(dob_focus_t *fm, const char *base_cmds)
{
    fm->panel_base = base_cmds;
    dobfocus_RefreshPanel(fm);
}

/* Count the entries in a newline-separated panel string. */
static int panel_base_count(const char *base)
{
    if (!base || !*base) return 0;
    int n = 1;
    for (const char *p = base; *p; p++) if (*p == '\n') n++;
    return n;
}

void dobfocus_RefreshPanel(dob_focus_t *fm)
{
    if (!fm->panel_attached || fm->panel_win_id == 0) return;

    ctx_build(fm);

    /* Publish the base navigation ALWAYS, with any contextual clipboard
     * commands APPENDED after it — never replacing it. Replacing the base
     * panel while a textbox was focused removed Indietro/Avanti/Annulla
     * entirely, and on a screen where defocus-by-background-click was hard
     * to hit (Parametri sistema), the user was trapped: all three nav
     * buttons gone, clipboard ops in their place. Appending keeps nav at
     * stable indices 0..base-1 and puts clipboard ops at base.. so the
     * wizard is always escapable. */
    char        ctx_buf[256];
    char        full[512];
    const char *base = fm->panel_base ? fm->panel_base : "";

    if (g_active.n > 0)
    {
        ctx_serialize(ctx_buf, sizeof(ctx_buf));
        if (*base)
            snprintf(full, sizeof(full), "%s\n%s", base, ctx_buf);
        else
            snprintf(full, sizeof(full), "%s", ctx_buf);
    }
    else
    {
        snprintf(full, sizeof(full), "%s", base);
    }

    uint32_t sig = ctx_hash(full);
    if (sig == fm->panel_sig) return;
    fm->panel_sig = sig;
    dobui_SetPanelCommands(fm->panel_win_id, full);
}

bool dobfocus_OnPanel(dob_focus_t *fm, int cmd_idx)
{
    if (!fm->panel_attached) return false;

    ctx_build(fm);
    if (g_active.n <= 0)                       return false;
    if (fm->focused < 0)                       return false;

    /* Clipboard commands now live AFTER the base nav (see RefreshPanel).
     * Indices below the base count are real nav buttons — let them fall
     * through to the app's handler. Only indices in the appended clipboard
     * range are ours to consume. */
    int base_n = panel_base_count(fm->panel_base);
    if (cmd_idx < base_n)                      return false;
    int ctx_idx = cmd_idx - base_n;
    if (ctx_idx < 0 || ctx_idx >= g_active.n)  return false;

    dob_focus_entry_t *e = &fm->entries[fm->focused];
    ctx_cmd_id_t id = g_active.ids[ctx_idx];

    if (e->type == DOB_CTRL_TEXTBOX)
    {
        dob_textbox_t *tb = e->ctrl;
        switch (id)
        {
            case CTX_PASTE:    dobtb_Paste(tb);    break;
            case CTX_CLEAR:    dobtb_Clear(tb);    break;
            case CTX_COPY_ALL: dobtb_CopyAll(tb);  break;
            case CTX_COPY:     dobtb_Copy(tb);     break;
            case CTX_CUT:      dobtb_Cut(tb);      break;
            default: break;
        }
    }
    else if (e->type == DOB_CTRL_MULTITEXTBOX)
    {
        dob_multitextbox_t *mt = e->ctrl;
        switch (id)
        {
            case CTX_PASTE:    dobmt_Paste(mt);    break;
            case CTX_CLEAR:    dobmt_Clear(mt);    break;
            case CTX_COPY_ALL: dobmt_CopyAll(mt);  break;
            case CTX_COPY:     dobmt_Copy(mt);     break;
            case CTX_CUT:      dobmt_Cut(mt);      break;
            default: break;
        }
    }
    else
    {
        return false;
    }

    dobfocus_RefreshPanel(fm);
    return true;
}

/* Focus management */

void dobfocus_SetFocus(dob_focus_t *fm, void *ctrl)
{
    if (fm->focused >= 0 && fm->focused < fm->count)
        ctrl_set_focus(&fm->entries[fm->focused], false);

    fm->focused = -1;
    for (int i = 0; i < fm->count; i++)
    {
        if (fm->entries[i].ctrl == ctrl)
        {
            fm->focused = i;
            ctrl_set_focus(&fm->entries[i], true);
            break;
        }
    }
    dobfocus_RefreshPanel(fm);
}

void dobfocus_ClearFocus(dob_focus_t *fm)
{
    if (fm->focused >= 0 && fm->focused < fm->count)
        ctrl_set_focus(&fm->entries[fm->focused], false);
    fm->focused = -1;
    dobfocus_RefreshPanel(fm);
}

void *dobfocus_GetFocused(dob_focus_t *fm)
{
    if (fm->focused >= 0 && fm->focused < fm->count)
        return fm->entries[fm->focused].ctrl;
    return NULL;
}

dob_ctrl_type_t dobfocus_GetFocusedType(dob_focus_t *fm)
{
    if (fm->focused >= 0 && fm->focused < fm->count)
        return fm->entries[fm->focused].type;
    return DOB_CTRL_NONE;
}

/* Event routing */

bool dobfocus_OnKey(dob_focus_t *fm, uint8_t key)
{
    if (fm->focused < 0) return false;
    dob_focus_entry_t *e = &fm->entries[fm->focused];

    /* Ctrl+C/X/V on text widgets go through the clipboard add-on
     * first. Linked into every program that pulls DOBUI_OBJS. */
    if (e->type == DOB_CTRL_TEXTBOX
        && dobtb_OnKeyClip(e->ctrl, key))
    {
        dobfocus_RefreshPanel(fm);
        return true;
    }
    if (e->type == DOB_CTRL_MULTITEXTBOX
        && dobmt_OnKeyClip(e->ctrl, key))
    {
        dobfocus_RefreshPanel(fm);
        return true;
    }

    bool consumed = false;
    switch (e->type)
    {
        case DOB_CTRL_BUTTON:        consumed = dobbtn_OnKey(e->ctrl, key);  break;
        case DOB_CTRL_PICTUREBUTTON: consumed = dobpbtn_OnKey(e->ctrl, key); break;
        case DOB_CTRL_TEXTBOX:       consumed = dobtb_OnKey(e->ctrl, key);   break;
        case DOB_CTRL_MULTITEXTBOX:  consumed = dobmt_OnKey(e->ctrl, key);   break;
        case DOB_CTRL_DROPDOWN:      consumed = dobdd_OnKey(e->ctrl, key);   break;
        case DOB_CTRL_CHECKBOX:      consumed = dobcb_OnKey(e->ctrl, key);   break;
        case DOB_CTRL_SWITCH:        consumed = dobsw_OnKey(e->ctrl, key);   break;
        case DOB_CTRL_SLIDER:        consumed = dobsl_OnKey(e->ctrl, key);   break;
        case DOB_CTRL_RADIOGROUP:    consumed = dobrg_OnKey(e->ctrl, key);   break;
        case DOB_CTRL_LISTVIEW:      consumed = doblv_OnKey(e->ctrl, key);   break;
        case DOB_CTRL_CHECKED_LISTVIEW: consumed = dobclv_OnKey(e->ctrl, key); break;
        case DOB_CTRL_TABLE:         consumed = dobtbl_OnKey(e->ctrl, key);  break;
        default: break;
    }

    if (consumed
        && (e->type == DOB_CTRL_TEXTBOX || e->type == DOB_CTRL_MULTITEXTBOX))
        dobfocus_RefreshPanel(fm);
    return consumed;
}

bool dobfocus_OnScroll(dob_focus_t *fm, int delta)
{
    if (fm->focused < 0) return false;
    dob_focus_entry_t *e = &fm->entries[fm->focused];

    switch (e->type)
    {
        case DOB_CTRL_MULTITEXTBOX: return dobmt_OnScroll(e->ctrl, delta);
        case DOB_CTRL_DROPDOWN:     return dobdd_OnScroll(e->ctrl, delta);
        case DOB_CTRL_LISTVIEW:     return doblv_OnScroll(e->ctrl, delta);
        case DOB_CTRL_CHECKED_LISTVIEW: return dobclv_OnScroll(e->ctrl, delta);
        case DOB_CTRL_TABLE:        return dobtbl_OnScroll(e->ctrl, delta);
        default: break;
    }
    return false;
}

/* Click routing — hit-test, set focus, dispatch */

static bool ctrl_on_click(dob_focus_entry_t *e, int x, int y)
{
    switch (e->type)
    {
        case DOB_CTRL_BUTTON:        return dobbtn_OnClick(e->ctrl, x, y);
        case DOB_CTRL_PICTUREBUTTON: return dobpbtn_OnClick(e->ctrl, x, y);
        case DOB_CTRL_TEXTBOX:       return dobtb_OnClick(e->ctrl, x, y);
        case DOB_CTRL_MULTITEXTBOX:  return dobmt_OnClick(e->ctrl, x, y);
        case DOB_CTRL_DROPDOWN:      return dobdd_OnClick(e->ctrl, x, y);
        case DOB_CTRL_CHECKBOX:      return dobcb_OnClick(e->ctrl, x, y);
        case DOB_CTRL_SWITCH:        return dobsw_OnClick(e->ctrl, x, y);
        case DOB_CTRL_SLIDER:        return dobsl_OnClick(e->ctrl, x, y);
        case DOB_CTRL_RADIOGROUP:    return dobrg_OnClick(e->ctrl, x, y);
        case DOB_CTRL_LISTVIEW:      return doblv_OnClick(e->ctrl, x, y);
        case DOB_CTRL_CHECKED_LISTVIEW: return dobclv_OnClick(e->ctrl, x, y);
        case DOB_CTRL_TABLE:         return dobtbl_OnClick(e->ctrl, x, y);
        default: break;
    }
    return false;
}

void *dobfocus_OnClick(dob_focus_t *fm, int x, int y)
{
    for (int i = 0; i < fm->count; i++)
    {
        if (ctrl_on_click(&fm->entries[i], x, y))
        {
            if (fm->focused != i)
            {
                if (fm->focused >= 0)
                    ctrl_set_focus(&fm->entries[fm->focused], false);
                fm->focused = i;
                ctrl_set_focus(&fm->entries[i], true);
            }
            dobfocus_RefreshPanel(fm);
            return fm->entries[i].ctrl;
        }
    }
    /* Click landed outside every registered widget — drop focus so
     * the panel returns to the app's base commands.  Cheap no-op
     * when there was no focus to begin with. */
    if (fm->focused >= 0)
        dobfocus_ClearFocus(fm);
    return NULL;
}

void *dobfocus_OnDblClick(dob_focus_t *fm, int x, int y)
{
    for (int i = 0; i < fm->count; i++)
    {
        dob_focus_entry_t *e = &fm->entries[i];
        bool hit = false;
        if (e->type == DOB_CTRL_TEXTBOX)
            hit = dobtb_OnDblClick(e->ctrl, x, y);
        else if (e->type == DOB_CTRL_MULTITEXTBOX)
            hit = dobmt_OnDblClick(e->ctrl, x, y);
        if (hit)
        {
            if (fm->focused != i)
            {
                if (fm->focused >= 0)
                    ctrl_set_focus(&fm->entries[fm->focused], false);
                fm->focused = i;
                ctrl_set_focus(&fm->entries[i], true);
            }
            dobfocus_RefreshPanel(fm);
            return e->ctrl;
        }
    }
    return NULL;
}

/* Release — end any in-flight press or drag */

void *dobfocus_OnRelease(dob_focus_t *fm)
{
    void *released_slider = NULL;

    for (int i = 0; i < fm->count; i++)
    {
        dob_focus_entry_t *e = &fm->entries[i];
        switch (e->type)
        {
            case DOB_CTRL_BUTTON:        dobbtn_OnRelease(e->ctrl);  break;
            case DOB_CTRL_PICTUREBUTTON: dobpbtn_OnRelease(e->ctrl); break;
            case DOB_CTRL_SLIDER:
            {
                dob_slider_t *sl = e->ctrl;
                if (sl->grabbed) released_slider = sl;
                dobsl_OnRelease(sl);
                break;
            }
            case DOB_CTRL_TEXTBOX:       dobtb_OnRelease(e->ctrl);   break;
            case DOB_CTRL_MULTITEXTBOX:  dobmt_OnRelease(e->ctrl);   break;
            case DOB_CTRL_DROPDOWN:      dobdd_OnRelease(e->ctrl);   break;
            case DOB_CTRL_LISTVIEW:      doblv_OnRelease(e->ctrl);   break;
            case DOB_CTRL_CHECKED_LISTVIEW: dobclv_OnRelease(e->ctrl); break;
            case DOB_CTRL_TABLE:         dobtbl_OnRelease(e->ctrl);  break;
            default: break;
        }
    }
    dobfocus_RefreshPanel(fm);
    return released_slider;
}

/* Drag — route mouse move to draggable controls */

bool dobfocus_OnDrag(dob_focus_t *fm, int x, int y)
{
    bool any = false;
    for (int i = 0; i < fm->count; i++)
    {
        dob_focus_entry_t *e = &fm->entries[i];
        switch (e->type)
        {
            case DOB_CTRL_SLIDER:
                if (dobsl_OnDrag(e->ctrl, x, y))          any = true;
                break;
            case DOB_CTRL_DROPDOWN:
                if (dobdd_OnDrag(e->ctrl, x, y))          any = true;
                break;
            case DOB_CTRL_TEXTBOX:
                if (dobtb_OnDrag(e->ctrl, x, y))          any = true;
                break;
            case DOB_CTRL_MULTITEXTBOX:
                if (dobmt_OnDrag(e->ctrl, x, y))          any = true;
                break;
            case DOB_CTRL_LISTVIEW:
                if (doblv_OnDrag(e->ctrl, x, y))          any = true;
                break;
            case DOB_CTRL_CHECKED_LISTVIEW:
                if (dobclv_OnDrag(e->ctrl, x, y))         any = true;
                break;
            case DOB_CTRL_TABLE:
                if (dobtbl_OnDrag(e->ctrl, x, y))         any = true;
                break;
            default: break;
        }
    }
    if (any) dobfocus_RefreshPanel(fm);
    return any;
}

/*  *  Global singleton
 *
 *  A library-owned dob_focus_t that widgets self-register into at
 *  _Init time. Apps get focus tracking for free: no explicit
 *  declaration, no Register() calls.
 */

static dob_focus_t g_focus;
static bool        g_focus_ready = false;

dob_focus_t *dobfocus_global(void)
{
    if (!g_focus_ready)
    {
        dobfocus_Init(&g_focus);
        g_focus_ready = true;
    }
    return &g_focus;
}

void dobfocus_auto_register(void *ctrl, dob_ctrl_type_t type)
{
    if (!ctrl) return;
    dob_focus_t *fm = dobfocus_global();
    /* Skip duplicates — _Init may be called more than once on the
     * same instance, or two widgets may share storage transiently. */
    for (int i = 0; i < fm->count; i++)
        if (fm->entries[i].ctrl == ctrl) return;
    dobfocus_Register(fm, ctrl, type);
}

void dobfocus_auto_unregister(void *ctrl)
{
    if (!ctrl || !g_focus_ready) return;
    dob_focus_t *fm = &g_focus;
    for (int i = 0; i < fm->count; i++)
    {
        if (fm->entries[i].ctrl == ctrl)
        {
            /* Shift the tail down. Preserve focused-index semantics. */
            for (int j = i; j < fm->count - 1; j++)
                fm->entries[j] = fm->entries[j + 1];
            fm->count--;
            if (fm->focused == i)      fm->focused = -1;
            else if (fm->focused > i)  fm->focused--;
            return;
        }
    }
}

/* Wrappers: one-liner forwarders that use the singleton. */

void             dobfocus_set_focus(void *ctrl)
                 { dobfocus_SetFocus(dobfocus_global(), ctrl); }
void             dobfocus_clear_focus(void)
                 { dobfocus_ClearFocus(dobfocus_global()); }
void            *dobfocus_get_focused(void)
                 { return dobfocus_GetFocused(dobfocus_global()); }
dob_ctrl_type_t  dobfocus_get_focused_type(void)
                 { return dobfocus_GetFocusedType(dobfocus_global()); }
bool             dobfocus_key(uint8_t key)
                 { return dobfocus_OnKey(dobfocus_global(), key); }
void            *dobfocus_click(int x, int y)
                 { return dobfocus_OnClick(dobfocus_global(), x, y); }
void            *dobfocus_dblclick(int x, int y)
                 { return dobfocus_OnDblClick(dobfocus_global(), x, y); }
void            *dobfocus_release(void)
                 { return dobfocus_OnRelease(dobfocus_global()); }
bool             dobfocus_drag(int x, int y)
                 { return dobfocus_OnDrag(dobfocus_global(), x, y); }
bool             dobfocus_scroll(int delta)
                 { return dobfocus_OnScroll(dobfocus_global(), delta); }
bool             dobfocus_panel(int cmd_idx)
                 { return dobfocus_OnPanel(dobfocus_global(), cmd_idx); }
void             dobfocus_attach_panel(uint32_t win_id, const char *base)
                 { dobfocus_AttachPanel(dobfocus_global(), win_id, base); }
void             dobfocus_set_base_panel(const char *base)
                 { dobfocus_SetBasePanel(dobfocus_global(), base); }
void             dobfocus_refresh_panel(void)
                 { dobfocus_RefreshPanel(dobfocus_global()); }
