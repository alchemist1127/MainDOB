/* DobWrite -- a word processor for MainDOB.
 *
 * The app is thin: it owns a native-widget ribbon and drives the engines.
 *   - Document model  (libdobdoc)   : the continuous, editable source.
 *   - Layout          (libdoblayout): persistent, incremental column.
 *   - Page engine     (libdobpage)  : sheets with pooled RAM surfaces; it
 *                                     draws the content INTO each sheet.
 * The app blits the visible sheets, draws caret/selection as an overlay,
 * and on every edit calls df_layout_reflow + dp_notify_edit so only the
 * touched paragraph re-flows and only the dirty sheet strip repaints.
 *
 * Ribbon (built from MainDOB widgets): Font + Size dropdowns, and
 * Bold/Italic/Underline/Strike toggle picturebuttons whose icons are the
 * letters B/I/U/S rasterized from the built-in font. The font itself is
 * built in -- Times New Roman is embedded in the binary.
 *
 * v1 limits (noted): bold/italic/underline/strike and size act on the
 * selection only; the font, with no selection, arms a pending format that the
 * next typed text receives (font-from-the-caret-forward). ASCII key input; no
 * bidi / insertion-direction control yet.
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
#include <picturebutton.h>
#include <dropdown.h>
#include <button.h>
#include <cliptext.h>
#include <dob/font.h>

#include <dobdoc/dobdoc.h>
#include <dobfont/dobfont.h>
#include <dobfont/resolver.h>
#include <doblayout/doblayout.h>
#include <dobpage/dobpage.h>
#include "colorpick.h"
#include "autocorr.h"
#include "subsedit.h"
#include "findrepl.h"
#include "pagesetup.h"
#include "fontreg.h"

#define WIN_W     760
#define WIN_H     740                        /* fits a 768px-tall screen (WIN_HEADER_H + borders) so the status bar stays visible */
#define AUTOSAVE_SECS 60                      /* idle seconds after the last edit before an autosave */
#define TAB_H     26
#define RIBBON_CONTENT_H 48
#define RIBBON_H  (TAB_H + RIBBON_CONTENT_H)
#define STATUS_H  24                        /* bottom status bar height */
#define STATUS_BG 0x00484848               /* dark-grey status strip   */
#define STATUS_FG 0x00E8E8E8               /* status text              */
#define STATUS_LINE 0x00808080             /* 1px separator above bar  */
#define STATUS_BTN 0x00606060              /* zoom +/- button face     */
#define TAB_W     132
#define DESK_BG   0x00303030
#define RIBBON_BG 0x00C8C8C8
#define TABBAR_BG 0x00989898
#define TAB_INACTIVE 0x00B0B0B0
#define ACTIVE_BG 0x00A0C8F0
#define NORMAL_BTN 0x00E8E8E8
#define HILITE_R  0x3C
#define HILITE_G  0x78
#define HILITE_B  0xC8
#define ICON_W    20
#define ICON_H    20
#define DW_MULTICLICK_MS    800   /* successive double-clicks within this window escalate the selection */
#define DW_MULTICLICK_SLOP  4
#define LOAD_CHUNK 65536

static uint32_t   win;
static int        win_w = WIN_W, win_h = WIN_H;

/* status bar / zoom */
static const int  g_zoom_steps[] = { 25, 50, 75, 100, 125, 150, 200, 300, 400 };
#define NZOOM     ((int)(sizeof(g_zoom_steps) / sizeof(g_zoom_steps[0])))
static int        g_zoom_idx = 3;     /* index into g_zoom_steps; 3 = 100% */
static int        g_nwords = 0, g_nchars = 0;
static bool       g_count_dirty = false;   /* recount rimandato al tick 1 Hz */
/* Banda sporca della vista per la prossima paint (coordinate g_view,
 * [y0,y1)): -1 = vista intera. La imposta after_edit/after_format nel
 * caso caldo (reflow incrementale, niente scroll, niente shift
 * verticale); paint() la consuma e la riazzera. Fuori banda g_view e'
 * persistente e ancora valido, quindi ricostruire e RICARICARE solo la
 * banda e' corretto — e taglia il costo di un keystroke da tutta la
 * pagina alle righe del paragrafo toccato. */
static int        g_band_y0 = -1, g_band_y1 = -1;
/* Banda attiva della composite corrente ([y0,y1) in coordinate g_view):
 * i disegnatori del contenuto vi si clippano, cosi' una composite a
 * banda non tocca (ne' doppio-tinta) le righe fuori banda. Vale la
 * vista intera fuori da composite_view. */
static int        g_cv_y0 = 0, g_cv_y1 = 1 << 30;

/* Find & Replace highlight state -- render-only, never written to the doc.
 * g_fr_matches holds each match's byte start; g_fr_cur is the active one
 * (cyan), the rest orange. Cleared on edits, replace-all, and dialog close. */
#define FR_MAX_MATCHES 8192
static uint32_t   g_fr_matches[FR_MAX_MATCHES];
static int        g_fr_nmatch = 0;
static int        g_fr_nlen   = 0;
static int        g_fr_cur    = -1;
static bool       g_fr_active = false;
static char       g_fr_needle[256] = {0};
static bool       g_fr_ci = true;
static int        g_cur_page = 1;     /* topmost visible page, 1-based     */

static df_doc    *g_doc;
static df_fontset*g_fonts;
static df_face   *g_face;
static fontreg_t  g_freg;                       /* installed fonts (builtin + files) */
static const char *g_font_names[FONTREG_MAX];   /* dropdown labels, point into g_freg */
static int         g_font_count = 0;
static df_layout *g_L;
static dp_engine *g_eng;

static uint32_t   g_caret;
static int32_t    g_anchor = -1;
/* Pending character format: changing the font (etc.) with no selection must not
 * touch existing text -- instead we arm a masked format that the next typed
 * characters receive, so the choice takes effect "from the caret forward". It
 * stays armed while typing continues from g_pending_pos; the next insert landing
 * elsewhere (the caret was moved) drops it. */
static CharFmt    g_pending;            /* masked overrides for newly typed text */
static bool       g_pending_on;         /* is a pending format armed?            */
static uint32_t   g_pending_pos;        /* caret position the pending applies at */
static uint8_t    g_mods;
static bool       g_dragging;
static char       g_path[256] = "/document.dobw";
static char       g_boot_open[256] = "";   /* .dobw path passed by the file association; loaded at startup */

/* Save + autosave state. g_saved == the document is bound to a real on-disk
 * file (so Salva overwrites it and autosave has a target). The status note is
 * a small message shown at the left of the status bar. */
static bool       g_saved = false;
enum { NOTE_NONE = 0, NOTE_PENDING, NOTE_AUTOSAVED, NOTE_SAVED, NOTE_UNSAVED };
static int        g_note = NOTE_NONE;
static int        g_as_left = 0;            /* seconds left until autosave (NOTE_PENDING) */
static bool       g_tick_on = false;        /* 1 Hz autosave tick currently armed */
static void       autosave_touch(void);     /* (re)arm the countdown after an edit */

static uint32_t  *g_view;    static int g_view_n, g_view_w, g_view_h;  /* whole content area, composited then blitted once */

/* ribbon widgets */
static dob_dropdown_t     g_font_dd, g_size_dd;
static dob_picturebutton_t g_bold, g_italic, g_under, g_strike, g_open, g_save;
static uint32_t  g_ico_bold[ICON_W * ICON_H], g_ico_italic[ICON_W * ICON_H];
static uint32_t  g_ico_under[ICON_W * ICON_H], g_ico_strike[ICON_W * ICON_H];
static uint32_t  g_ico_open[ICON_W * ICON_H],  g_ico_save[ICON_W * ICON_H];
static dob_picturebutton_t g_alignL, g_alignC, g_alignR, g_alignJ;
static uint32_t  g_ico_alignL[ICON_W * ICON_H], g_ico_alignC[ICON_W * ICON_H];
static uint32_t  g_ico_alignR[ICON_W * ICON_H], g_ico_alignJ[ICON_W * ICON_H];

/* font dropdown labels come from the registry (g_font_names), built at start */
static const char *g_sizes[] = { "8","9","10","11","12","14","16","18","24","36","48","72" };
static const int   g_size_pt[] = {  8,  9, 10, 11, 12, 14, 16, 18, 24, 36, 48, 72 };
#define NSIZES (int)(sizeof(g_sizes)/sizeof(g_sizes[0]))
static int g_last_size = 4, g_last_font = 0;

/* toggle states (reflect the caret) */
static bool g_b_on, g_i_on, g_u_on, g_s_on;
static uint8_t g_align = DD_ALIGN_LEFT;   /* current paragraph alignment (dd_align) */
static uint32_t g_fontcolor = 0x00000000; /* last-picked font colour (0x00RRGGBB) */
static dob_picturebutton_t g_fcolor;
static uint32_t g_ico_fcolor[ICON_W * ICON_H];
static uint32_t g_pagebg = 0x00FFFFFF;    /* page background colour (0x00RRGGBB), default white */
static dob_picturebutton_t g_pagebg_btn;
static uint32_t g_ico_pagebg[ICON_W * ICON_H];
static uint32_t g_highlight = 0x00FFFF00;  /* last-picked highlight colour (0x00RRGGBB), yellow */
static dob_picturebutton_t g_hl_btn;
static uint32_t g_ico_hl[ICON_W * ICON_H];
static dob_picturebutton_t g_hlclear_btn;   /* remove highlight from selection */
static uint32_t g_ico_hlclear[ICON_W * ICON_H];
/* Paragraph tab: "space after paragraph" presets = the soft-wrap : hard-return
 * ratio. space_after in twips, sized off the ~13.2pt single line at 11pt. */
static dob_button_t g_para_sp[4];
static const char    *g_para_sp_lbl[4] = { "1,0", "1,15", "1,5", "2,0" };
static const uint32_t g_para_sp_tw[4]  = { 0, 40, 132, 264 };
static int g_para_sp_active = 1;        /* default 1.15 */
/* Interlinea (line spacing within a paragraph). line_spacing is absolute twips,
 * so it's computed per-paragraph from the font size (1.2 x size = single). */
static dob_button_t g_para_ls[3];
static const float g_para_ls_mult[3] = { 1.0f, 1.5f, 2.0f };
static int g_para_ls_active = 0;        /* default Singola */
static dob_button_t g_subsbtn;            /* "Cerca e sost." tab: opens the autocorrect editor */
static dob_button_t g_findbtn;            /* "Cerca e sost." tab: opens find & replace */

/* ribbon tabs. Each tab owns a set of controls drawn/hit-tested by index. */
static const char *g_tab_names[] = { "File", "Home", "Cerca e sost.", "Paragrafo", "Layout" };
#define NTABS ((int)(sizeof(g_tab_names) / sizeof(g_tab_names[0])))
#define TAB_FILE 0
#define TAB_HOME 1
#define TAB_SUBST 2
#define TAB_PARA 3
#define TAB_LAYOUT 4
static int g_active_tab = TAB_HOME;
static dob_button_t g_layout_pagebtn;   /* Layout tab: "Imposta pagina..." */
static dob_button_t g_margin_btn;       /* Layout tab: interactive-margins toggle    */
static dob_button_t g_sectbreak_btn;    /* Layout tab: insert section break           */
static bool         g_margin_edit;      /* construction lines visible + draggable     */
static int          g_drag_guide = -1;  /* 0=L 1=R 2=T 3=B while dragging, else -1     */
static int          g_drag_pageidx;     /* visible-page index being dragged over       */
static PageSetup    g_marg_ps;          /* page setup being edited (live preview)      */

/* ---- columns chooser popup (Layout tab) ------------------------------------
 * A dedicated overlay for newspaper columns: count 1..6 plus a gutter stepper.
 * Columns live in the doc-wide page setup; choosing here rebuilds the layout.
 * (Page setup no longer carries a column control -- this popup owns it.)     */
#define COLP_W   250
#define COLP_H   96
#define COLP_X   (WIN_W - COLP_W - 8)
#define COLP_Y   (RIBBON_H + 4)
static dob_button_t g_cols_btn;          /* Layout tab: open the columns popup          */
static bool         g_cols_open;         /* popup visible                                */
static dob_button_t g_cols_pick[6];      /* column-count buttons 1..6                    */
static dob_button_t g_cols_gap_dec;      /* gutter -                                     */
static dob_button_t g_cols_gap_inc;      /* gutter +                                     */

/* ---- contextual popups -----------------------------------------------------
 * Two small overlays that float to the north-east of a glyph: a formatting
 * mini-toolbar while text is selected, and an "undo this autocorrect" button
 * right after a substitution. They are painted into the document area (not
 * separate windows), so the keyboard always reaches the document -- typing
 * simply dismisses them; only mouse clicks inside their rectangle are taken. */
enum { PP_NONE = 0, PP_SEL, PP_SUBST };
static int          g_pp = PP_NONE;                 /* which popup is showing      */
static int          g_pp_x, g_pp_y, g_pp_w, g_pp_h; /* its window rectangle        */
static dob_button_t g_pp_btn[6];                    /* selection: B I U S A+ A-    */
static dob_button_t g_pp_undo;                      /* substitution: the red X     */
static uint32_t     g_subst_pos;                    /* where the swap happened     */
static char         g_subst_from[64];               /* original text, to restore   */
static int          g_subst_from_n, g_subst_to_n;   /* lengths (restore / remove)  */
static bool         g_list_undo;                    /* the pending PP_SUBST is a list marker (backspace undoes it) */

/* ---- helpers ---- */

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static uint32_t sel_lo(void) { return ((uint32_t)g_anchor < g_caret) ? (uint32_t)g_anchor : g_caret; }
static uint32_t sel_hi(void) { return ((uint32_t)g_anchor < g_caret) ? g_caret : (uint32_t)g_anchor; }
static bool     has_sel(void){ return g_anchor >= 0 && (uint32_t)g_anchor != g_caret; }
/* An open Font/Size dropdown is modal-ish: while it's up it owns keyboard and
 * mouse, and the contextual selection toolbar must stay hidden so it can't
 * overlap the list or steal its clicks. */
static bool     dd_open(void){ return g_font_dd.open || g_size_dd.open; }

static char byte_at(uint32_t pos)
{
    if (pos >= df_doc_length(g_doc)) return 0;
    char c = 0; df_doc_get_text(g_doc, pos, 1, &c, 1); return c;
}

/* rasterize a letter from the built-in font into an icon (black on transparent) */
static void make_icon(uint32_t *buf, uint32_t cp, bool bold, bool italic, bool uline, bool strike)
{
    for (int i = 0; i < ICON_W * ICON_H; i++) buf[i] = 0x00000000;   /* transparent */
    if (g_face) {
        df_raster_req req; req.px_size = 15.0f; req.embolden = bold ? 15.0f * 0.06f : 0.0f;
        req.slant = italic ? 0.20f : 0.0f; req.shift_x = 0.0f;
        uint32_t gid = df_map_codepoint(g_face, cp);
        df_bitmap bm;
        if (df_rasterize(g_face, gid, &req, &bm) == DF_OK && bm.cover) {
            int ox = (ICON_W - bm.w) / 2, oy = (ICON_H - bm.h) / 2;
            for (int r = 0; r < bm.h; r++)
                for (int c = 0; c < bm.w; c++)
                    if (bm.cover[r * bm.w + c] >= 80) {
                        int x = ox + c, y = oy + r;
                        if (x >= 0 && x < ICON_W && y >= 0 && y < ICON_H) buf[y * ICON_W + x] = 0xFF000000u;
                    }
            df_free_bitmap(&bm);
        }
    }
    if (uline)  for (int x = 3; x < ICON_W - 3; x++) buf[(ICON_H - 4) * ICON_W + x] = 0xFF000000u;
    if (strike) for (int x = 3; x < ICON_W - 3; x++) buf[(ICON_H / 2) * ICON_W + x] = 0xFF000000u;
}

/* a crude floppy-disk / open-folder mark for the file buttons */
static void make_mark(uint32_t *buf, bool save)
{
    for (int i = 0; i < ICON_W * ICON_H; i++) buf[i] = 0x00000000;
    for (int y = 4; y < ICON_H - 4; y++)
        for (int x = 4; x < ICON_W - 4; x++) {
            bool edge = (y == 4 || y == ICON_H - 5 || x == 4 || x == ICON_W - 5);
            if (edge) buf[y * ICON_W + x] = 0xFF000000u;
        }
    if (save) { for (int x = 7; x < ICON_W - 7; x++) for (int y = 5; y < 9; y++) buf[y * ICON_W + x] = 0xFF000000u; }
    else      { for (int x = 5; x < ICON_W - 5; x++) buf[(ICON_H / 2) * ICON_W + x] = 0xFF000000u; }
}

/* alignment glyph: four 2px bars, positioned to read as L/C/R/justified */
static void make_align_icon(uint32_t *buf, dd_align a)
{
    for (int i = 0; i < ICON_W * ICON_H; i++) buf[i] = 0x00000000;
    static const int lens[4] = { 14, 10, 13, 8 };
    for (int b = 0; b < 4; b++) {
        int len = (a == DD_ALIGN_JUSTIFY) ? 14 : lens[b];
        int x0;
        switch (a) {
            case DD_ALIGN_CENTER: x0 = (ICON_W - len) / 2;  break;
            case DD_ALIGN_RIGHT:  x0 = (ICON_W - 3) - len;  break;
            default:              x0 = 3;                   break;  /* LEFT, JUSTIFY */
        }
        int y = 3 + b * 4;
        for (int yy = y; yy < y + 2; yy++)
            for (int x = x0; x < x0 + len; x++)
                if (x >= 0 && x < ICON_W && yy >= 0 && yy < ICON_H) buf[yy * ICON_W + x] = 0xFF000000u;
    }
}

/* ---- caret / selection geometry in window space ---- */

/* "font colour" button glyph: an 'A' with a coloured bar underneath */
static void make_fontcolor_icon(uint32_t *buf, uint32_t rgb)
{
    for (int i = 0; i < ICON_W * ICON_H; i++) buf[i] = 0x00000000;
    if (g_face) {
        df_raster_req req; req.px_size = 14.0f; req.embolden = 14.0f * 0.06f; req.slant = 0.0f; req.shift_x = 0.0f;
        uint32_t gid = df_map_codepoint(g_face, 'A');
        df_bitmap bm;
        if (df_rasterize(g_face, gid, &req, &bm) == DF_OK && bm.cover) {
            int ox = (ICON_W - bm.w) / 2, oy = (ICON_H - 4 - bm.h) / 2; if (oy < 0) oy = 0;
            for (int r = 0; r < bm.h; r++)
                for (int c = 0; c < bm.w; c++)
                    if (bm.cover[r * bm.w + c] >= 80) {
                        int xx = ox + c, yy = oy + r;
                        if (xx >= 0 && xx < ICON_W && yy >= 0 && yy < ICON_H) buf[yy * ICON_W + xx] = 0xFF000000u;
                    }
            df_free_bitmap(&bm);
        }
    }
    uint32_t bar = 0xFF000000u | (rgb & 0x00FFFFFFu);
    for (int y = ICON_H - 3; y < ICON_H; y++)
        for (int x = 2; x < ICON_W - 2; x++) buf[y * ICON_W + x] = bar;
}

/* "page background" button glyph: a framed swatch in the current paper colour */
static void make_pagebg_icon(uint32_t *buf, uint32_t rgb)
{
    uint32_t fill = 0xFF000000u | (rgb & 0x00FFFFFFu);
    for (int y = 0; y < ICON_H; y++)
        for (int x = 0; x < ICON_W; x++) {
            bool border = (x < 3 || x >= ICON_W - 3 || y < 3 || y >= ICON_H - 3);
            buf[y * ICON_W + x] = border ? 0xFF303030u : fill;
        }
}

/* "text highlight" button glyph: dark text rows under a coloured marker band */
static void make_highlight_icon(uint32_t *buf, uint32_t rgb)
{
    uint32_t band = 0xFF000000u | (rgb & 0x00FFFFFFu);
    for (int y = 0; y < ICON_H; y++)
        for (int x = 0; x < ICON_W; x++) {
            uint32_t c = 0xFFFFFFFFu;                          /* paper */
            if (y >= 5 && y <= ICON_H - 6 && x >= 3 && x < ICON_W - 3) c = band;   /* marker band */
            buf[y * ICON_W + x] = c;
        }
    for (int row = 0; row < 3; row++) {                        /* dark text strokes over the band */
        int ty = 7 + row * 3;
        if (ty >= ICON_H - 3) break;
        for (int x = 5; x < ICON_W - 5; x++) buf[ty * ICON_W + x] = 0xFF202020u;
    }
}


static void tint_rect_rgb(uint32_t *buf, int w, int h, int x0, int y0, int x1, int y1,
                          uint32_t tr, uint32_t tg, uint32_t tb, uint32_t a)
{
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > w) x1 = w; if (y1 > h) y1 = h;
    for (int y = y0; y < y1; y++) {
        /* clip alla banda della composite: fuori banda i pixel non sono
         * stati ricostruiti — ritingerli li scurirebbe una volta in piu'. */
        if (buf == g_view && (y < g_cv_y0 || y >= g_cv_y1)) continue;
        uint32_t *row = buf + (size_t)y * w;
        for (int x = x0; x < x1; x++) {
            uint32_t d = row[x];
            uint32_t dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
            uint32_t r = (tr * a + dr * (255 - a)) / 255;
            uint32_t g = (tg * a + dg * (255 - a)) / 255;
            uint32_t b = (tb * a + db * (255 - a)) / 255;
            row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

/* ---- single-surface compositor for the content area (below the ribbon) ----
 * Per-sheet dynamic blits are a trap: clipping by source offset changes the
 * texture-pool key every scroll step (pool exhaustion -> black regions), while
 * NOT clipping lets a scrolled sheet blit above the ribbon and even outside the
 * window (the WM doesn't clip negative blit coords). So we composite every
 * visible sheet into ONE stable RAM surface the size of the content area,
 * clipping in software, and blit it once at y=RIBBON_H with a fixed (w,h,ptr)
 * key. Nothing in this path is ever drawn above the ribbon. */
static int   content_bottom(void) { return win_h - STATUS_H; }                 /* y just below the page area */
static float layout_dpi(void) { return 96.0f * (float)g_zoom_steps[g_zoom_idx] / 100.0f; }

static bool g_view_is_panel = false;   /* g_view = pannello SHM condiviso? */

static void ensure_view(void)
{
    int vw = win_w, vh = content_bottom() - RIBBON_H; if (vh < 0) vh = 0;

    /* Percorso preferito: il pannello SHM. g_view DIVENTA il buffer
     * condiviso col server — si disegna esattamente come prima, ma la
     * consegna e' un record da 9 byte invece di ~2 MB di upload a
     * segmenti piu' una texture d'appoggio. Su misura cambiata Ensure
     * rialloca (contenuto azzerato dal server: la prossima composite
     * e' comunque integrale, il resize arma la vista intera). */
    uint32_t *panel = (vw > 0 && vh > 0) ? dobui_ShmPanelEnsure(win, vw, vh)
                                         : NULL;
    if (panel)
    {
        if (!g_view_is_panel) { free(g_view); g_view_n = 0; }
        g_view = panel; g_view_is_panel = true;
        g_view_w = vw; g_view_h = vh;
        return;
    }

    /* Ripiego: buffer privato + BlitBufferDynamic (sempre corretto). */
    if (g_view_is_panel) { g_view = NULL; g_view_n = 0; g_view_is_panel = false; }
    int need = vw * vh;
    if (need > g_view_n) { free(g_view); g_view = (uint32_t *)malloc((size_t)need * 4); g_view_n = g_view ? need : 0; }
    g_view_w = vw; g_view_h = vh;
}

static inline void view_px(int x, int y, uint32_t c)
{
    if (x >= 0 && x < g_view_w && y >= 0 && y < g_view_h) g_view[(size_t)y * g_view_w + x] = c;
}

/* 1px opaque-black rectangle outline, in g_view coordinates */
static void view_frame(int x, int y, int w, int h)
{
    for (int i = 0; i < w; i++) { view_px(x + i, y, 0xFF000000u); view_px(x + i, y + h - 1, 0xFF000000u); }
    for (int j = 0; j < h; j++) { view_px(x, y + j, 0xFF000000u); view_px(x + w - 1, y + j, 0xFF000000u); }
}

/* copy a page surface (sw x sh) into g_view at (dx, dy), clipped to g_view */
static void view_blit_page(const uint32_t *src, int sw, int sh, int dx, int dy)
{
    for (int sy = 0; sy < sh; sy++) {
        int gy = dy + sy; if (gy < g_cv_y0 || gy >= g_cv_y1) continue;
        if (gy < 0 || gy >= g_view_h) continue;
        int gx = dx, sx = 0, cols = sw;
        if (gx < 0) { sx = -gx; cols += gx; gx = 0; }
        if (gx + cols > g_view_w) cols = g_view_w - gx;
        if (cols <= 0) continue;
        memcpy(g_view + (size_t)gy * g_view_w + gx, src + (size_t)sy * sw + sx, (size_t)cols * 4);
    }
}

/* Tint byte range [lo,hi) on the visible lines of sheet v with (R,G,B)@a,
 * straight into g_view over the already-blitted page. */
static void tint_byte_range(const dp_view *v, uint32_t lo, uint32_t hi,
                            uint32_t R, uint32_t G, uint32_t B, uint32_t a)
{
    if (hi <= lo) return;
    uint32_t npar = df_layout_para_count(g_L);
    for (uint32_t pi = 0; pi < npar; pi++) {
        const dl_para *pp = df_layout_para(g_L, pi);
        if (pp->byte_start + pp->byte_len < lo || pp->byte_start > hi) continue;
        for (uint32_t li = 0; li < pp->line_count; li++) {
            const dl_line *ln = &pp->lines[li];
            uint32_t le = ln->byte_start + ln->byte_len;
            if (le <= lo || ln->byte_start >= hi) continue;        /* line outside range */
            int wx, wy; dp_content_to_window(g_eng, ln->x, ln->y, &wx, &wy);
            int sy = wy - v->win_y;
            if (sy + (int)ln->height < 0 || sy >= v->h) continue;  /* not on this sheet */
            float xmin = 1e9f, xmax = -1e9f; bool any = false;
            for (uint32_t g = 0; g < ln->glyph_count; g++) {
                const dl_glyph *gl = &ln->glyphs[g];
                if (gl->byte < lo || gl->byte >= hi) continue;
                if (gl->x < xmin) xmin = gl->x;
                if (gl->x + gl->advance > xmax) xmax = gl->x + gl->advance;
                any = true;
            }
            int sx0, sx1;
            if (any) { sx0 = (int)xmin; sx1 = (int)xmax; }
            else     { sx0 = (int)ln->x; sx1 = sx0 + 4; }          /* empty / newline-only line */
            if (hi >= le) sx1 += 4;                                /* range includes the line break */
            if (sx1 <= sx0) sx1 = sx0 + 4;
            int ry0 = sy, ry1 = sy + (int)ln->height;
            if (sx0 < 0) sx0 = 0; if (sx1 > (int)v->w) sx1 = (int)v->w;   /* clip to the sheet ... */
            if (ry0 < 0) ry0 = 0; if (ry1 > (int)v->h) ry1 = (int)v->h;
            tint_rect_rgb(g_view, g_view_w, g_view_h,                     /* ... then place in g_view */
                          v->win_x + sx0, v->win_y + ry0, v->win_x + sx1, v->win_y + ry1, R, G, B, a);
        }
    }
}

/* Tint the selected lines straight into g_view (cost scales with the selection). */
static void tint_selection(const dp_view *v)
{
    tint_byte_range(v, sel_lo(), sel_hi(), HILITE_R, HILITE_G, HILITE_B, 64);
}

/* Find & Replace overlay: every match orange, the current one cyan. Purely a
 * render pass -- nothing is written into the document. */
static void tint_matches(const dp_view *v)
{
    if (!g_fr_active || g_fr_nmatch == 0 || g_fr_nlen == 0) return;
    uint32_t nlen = (uint32_t)g_fr_nlen;
    for (int k = 0; k < g_fr_nmatch; k++) {
        if (k == g_fr_cur) continue;                              /* current drawn last, on top */
        uint32_t m = g_fr_matches[k];
        tint_byte_range(v, m, m + nlen, 255, 165, 0, 96);         /* orange */
    }
    if (g_fr_cur >= 0 && g_fr_cur < g_fr_nmatch) {
        uint32_t m = g_fr_matches[g_fr_cur];
        tint_byte_range(v, m, m + nlen, 0, 200, 255, 130);        /* cyan */
    }
}

/* fill g_view (desk + every visible sheet + border) and blit it once */
static void composite_view(void)
{
    ensure_view();
    if (!g_view) return;

    /* Banda da ricostruire: [by0,by1). g_view e' persistente, quindi
     * fuori banda i pixel dell'ultima composite sono ancora corretti;
     * ricostruire e ricaricare solo la banda e' lecito quando chi ha
     * chiesto la paint garantisce che TUTTO il cambiamento vi ricade
     * (after_edit/after_format nel percorso incrementale). */
    /* Sentinella "contenuto invariato" (banda 0,0): la paint serve solo
     * al chrome (status bar del tick 1 Hz, nota autosave). Niente
     * ricostruzione e SOPRATTUTTO niente re-upload: il BlitBuffer non
     * dinamico, a parita' di (w,h,src), riusa la texture gia' caricata
     * ed emette solo il record di blit. */
    if (g_band_y0 == 0 && g_band_y1 == 0) {
        dp_set_viewport(g_eng, win_w, content_bottom() - RIBBON_H);
        if (g_view_is_panel)
            dobui_ShmPanelBlit(win, 0, RIBBON_H, g_view_w, g_view_h,
                               0xFFFF, 0xFFFF);   /* contenuto invariato */
        else
            dobui_BlitBuffer(win, 0, RIBBON_H, g_view, g_view_w, g_view_h);
        return;
    }

    int by0 = 0, by1 = g_view_h;
    bool banded = (g_band_y0 >= 0 && g_band_y1 > g_band_y0);
    if (banded) {
        by0 = g_band_y0 < 0 ? 0 : g_band_y0;
        by1 = g_band_y1 > g_view_h ? g_view_h : g_band_y1;
        if (by0 >= by1) { banded = false; by0 = 0; by1 = g_view_h; }
    }
    g_cv_y0 = by0; g_cv_y1 = by1;

    for (int y = by0; y < by1; y++) {              /* opaque desk, solo banda */
        uint32_t *row = g_view + (size_t)y * g_view_w;
        for (int x = 0; x < g_view_w; x++) row[x] = 0xFF303030u;
    }

    dp_set_viewport(g_eng, win_w, content_bottom() - RIBBON_H);
    dp_view vis[4];
    int nv = dp_visible_pages(g_eng, vis, 4);
    g_cur_page = nv > 0 ? vis[0].page_index + 1 : 1;
    bool sel = has_sel();
    for (int i = 0; i < nv; i++) {
        /* g_view (0,0) == window (0, RIBBON_H); win_x/win_y are already in that frame */
        view_blit_page(vis[i].buf, vis[i].w, vis[i].h, vis[i].win_x, vis[i].win_y);
        if (sel) tint_selection(&vis[i]);     /* tint selected lines straight into g_view */
        if (g_fr_active) tint_matches(&vis[i]);  /* find & replace overlay (orange/cyan) */
        view_frame(vis[i].win_x - 1, vis[i].win_y - 1, vis[i].w + 2, vis[i].h + 2);
    }

    if (g_view_is_panel)
        /* I pixel sono GIA' dal server (memoria condivisa): la
         * consegna e' il solo record, con la banda dichiarata — il
         * bake ricopiera' nel corpo solo quelle righe, e il damage a
         * valle (compose, presentazione) si restringe di conseguenza. */
        dobui_ShmPanelBlit(win, 0, RIBBON_H, g_view_w, g_view_h,
                           banded ? by0 : 0, banded ? (by1 - by0) : 0);
    else if (banded)
        dobui_BlitBufferDynamicRows(win, 0, RIBBON_H, g_view,
                                    g_view_w, g_view_h, by0, by1 - by0);
    else
        dobui_BlitBufferDynamic(win, 0, RIBBON_H, g_view, g_view_w, g_view_h);

    g_cv_y0 = 0; g_cv_y1 = 1 << 30;   /* i disegnatori fuori composite vedono tutto */
}

static void draw_caret(void)
{
    dl_locus loc;
    if (!df_layout_locate(g_L, g_caret, &loc)) return;
    int wx, wt, wb; dp_content_to_window(g_eng, loc.x, loc.y_top, &wx, &wt);
    dp_content_to_window(g_eng, loc.x, loc.y_bottom, &wx, &wb);
    int ay = RIBBON_H + wt, h = wb - wt;
    if (ay + h <= RIBBON_H || ay >= content_bottom()) return;
    if (ay < RIBBON_H) { h -= (RIBBON_H - ay); ay = RIBBON_H; }
    if (ay + h > content_bottom()) h = content_bottom() - ay;
    dobui_FillRect(win, wx, ay, 2, h, 0x00000000);
}

/* ---- Layout tab: interactive page margins -------------------------------
 * The four margins are drawn as draggable "construction lines" over every
 * visible sheet. Each line's position is a fraction of that sheet's on-window
 * rectangle (margin_twips / page_twips), so it stays correct at any zoom.
 * Dragging updates g_marg_ps live; the document re-flows once, on release.
 * Document-wide for now (per-page scope arrives with sections). */
#define GUIDE_COL   0x000070E0u      /* construction-line colour      */
#define GUIDE_HITPX 5                /* grab tolerance in window px    */

static void draw_vline(int x, int y, int h, uint32_t c)
{
    int y0 = y, y1 = y + h;
    if (y0 < RIBBON_H) y0 = RIBBON_H;
    if (y1 > content_bottom()) y1 = content_bottom();
    if (y1 > y0) dobui_FillRect(win, x, y0, 2, y1 - y0, c);
}
static void draw_hline(int x, int y, int w, uint32_t c)
{
    if (y < RIBBON_H || y >= content_bottom()) return;
    dobui_FillRect(win, x, y, w, 2, c);
}

static void draw_margin_guides(void)
{
    if (!g_margin_edit || g_active_tab != TAB_LAYOUT) return;
    if (g_marg_ps.width == 0 || g_marg_ps.height == 0) return;
    float fL = (float)g_marg_ps.margin_left   / (float)g_marg_ps.width;
    float fR = (float)g_marg_ps.margin_right  / (float)g_marg_ps.width;
    float fT = (float)g_marg_ps.margin_top    / (float)g_marg_ps.height;
    float fB = (float)g_marg_ps.margin_bottom / (float)g_marg_ps.height;
    dp_view vis[4];
    int nv = dp_visible_pages(g_eng, vis, 4);
    for (int i = 0; i < nv; i++) {
        int px = vis[i].win_x, py = vis[i].win_y + RIBBON_H, w = vis[i].w, h = vis[i].h;
        draw_vline(px + (int)(w * fL + 0.5f),     py, h, GUIDE_COL);   /* left   */
        draw_vline(px + w - (int)(w * fR + 0.5f), py, h, GUIDE_COL);   /* right  */
        draw_hline(px, py + (int)(h * fT + 0.5f),     w, GUIDE_COL);   /* top    */
        draw_hline(px, py + h - (int)(h * fB + 0.5f), w, GUIDE_COL);   /* bottom */
    }
}

/* Which construction line is under (mx,my)?  0=L 1=R 2=T 3=B, -1 none.
 * mx,my are window coords; page rects are desk coords (subtract RIBBON_H). */
static int guide_hit(int mx, int my, int *out_page)
{
    if (g_marg_ps.width == 0 || g_marg_ps.height == 0) return -1;
    int dy = my - RIBBON_H;
    float fL = (float)g_marg_ps.margin_left   / (float)g_marg_ps.width;
    float fR = (float)g_marg_ps.margin_right  / (float)g_marg_ps.width;
    float fT = (float)g_marg_ps.margin_top    / (float)g_marg_ps.height;
    float fB = (float)g_marg_ps.margin_bottom / (float)g_marg_ps.height;
    dp_view vis[4];
    int nv = dp_visible_pages(g_eng, vis, 4);
    for (int i = 0; i < nv; i++) {
        int px = vis[i].win_x, py = vis[i].win_y, w = vis[i].w, h = vis[i].h;
        int xL = px + (int)(w * fL + 0.5f), xR = px + w - (int)(w * fR + 0.5f);
        int yT = py + (int)(h * fT + 0.5f), yB = py + h - (int)(h * fB + 0.5f);
        if (dy >= py && dy <= py + h) {
            if (mx >= xL - GUIDE_HITPX && mx <= xL + GUIDE_HITPX) { if (out_page) *out_page = i; return 0; }
            if (mx >= xR - GUIDE_HITPX && mx <= xR + GUIDE_HITPX) { if (out_page) *out_page = i; return 1; }
        }
        if (mx >= px && mx <= px + w) {
            if (dy >= yT - GUIDE_HITPX && dy <= yT + GUIDE_HITPX) { if (out_page) *out_page = i; return 2; }
            if (dy >= yB - GUIDE_HITPX && dy <= yB + GUIDE_HITPX) { if (out_page) *out_page = i; return 3; }
        }
    }
    return -1;
}

/* Drag the grabbed line to (mx,my); clamp so the opposite margin keeps at
 * least 1/8 of the page printable. */
static void guide_drag_to(int mx, int my)
{
    if (g_drag_guide < 0) return;
    dp_view vis[4];
    int nv = dp_visible_pages(g_eng, vis, 4);
    if (g_drag_pageidx < 0 || g_drag_pageidx >= nv) return;
    int px = vis[g_drag_pageidx].win_x, py = vis[g_drag_pageidx].win_y;
    int w  = vis[g_drag_pageidx].w,     h  = vis[g_drag_pageidx].h;
    int dy = my - RIBBON_H;
    PageSetup *p = &g_marg_ps;
    if (g_drag_guide == 0) {
        float fr = (float)(mx - px) / (float)w; if (fr < 0) fr = 0;
        long v = (long)(fr * (float)p->width + 0.5f);
        long lim = (long)p->width - (long)p->width / 8 - (long)p->margin_right; if (lim < 0) lim = 0;
        if (v > lim) v = lim; p->margin_left = (uint32_t)v;
    } else if (g_drag_guide == 1) {
        float fr = (float)((px + w) - mx) / (float)w; if (fr < 0) fr = 0;
        long v = (long)(fr * (float)p->width + 0.5f);
        long lim = (long)p->width - (long)p->width / 8 - (long)p->margin_left; if (lim < 0) lim = 0;
        if (v > lim) v = lim; p->margin_right = (uint32_t)v;
    } else if (g_drag_guide == 2) {
        float fr = (float)(dy - py) / (float)h; if (fr < 0) fr = 0;
        long v = (long)(fr * (float)p->height + 0.5f);
        long lim = (long)p->height - (long)p->height / 8 - (long)p->margin_bottom; if (lim < 0) lim = 0;
        if (v > lim) v = lim; p->margin_top = (uint32_t)v;
    } else {
        float fr = (float)((py + h) - dy) / (float)h; if (fr < 0) fr = 0;
        long v = (long)(fr * (float)p->height + 0.5f);
        long lim = (long)p->height - (long)p->height / 8 - (long)p->margin_top; if (lim < 0) lim = 0;
        if (v > lim) v = lim; p->margin_bottom = (uint32_t)v;
    }
}

/* ---- paint ---- */

static void update_toggles_from_caret(void)
{
    uint32_t at = has_sel() ? sel_lo() : (g_caret > 0 ? g_caret - 1 : 0);
    CharFmt cf; df_doc_char_fmt_at(g_doc, at, &cf);
    g_b_on = cf.bold; g_i_on = cf.italic; g_u_on = cf.underline; g_s_on = cf.strike;
    /* reflect size in the dropdown */
    for (int i = 0; i < NSIZES; i++) if ((uint32_t)(g_size_pt[i] * 20) == cf.size_twips) { dobdd_SetSelected(&g_size_dd, i); g_last_size = i; break; }
    /* reflect the font family in the dropdown (leave as-is if the family isn't installed) */
    { const char *fn = df_doc_family_name(g_doc, cf.family_id);
      if (fn && *fn)
          for (int i = 0; i < g_font_count; i++)
              if (g_font_names[i] && strcmp(g_font_names[i], fn) == 0) { dobdd_SetSelected(&g_font_dd, i); g_last_font = i; break; } }
    /* reflect the paragraph alignment of the caret / selection start */
    uint32_t pidx = df_doc_para_at(g_doc, has_sel() ? sel_lo() : g_caret);
    ParaFmt pfm; df_doc_para_fmt_resolved(g_doc, pidx, &pfm); g_align = pfm.align;
}

/* Dynamic tab widths: each tab is sized to its label in the UI font, so short
 * names ("File", "Home") get a snug tab instead of a fixed 132px slot. */
#define TAB_PADX 14   /* horizontal padding inside each tab */
#define TAB_GAPX 2    /* gap between adjacent tabs          */
static int tabw(int i)
{
    return dob_text_width(g_tab_names[i], (uint32_t)strlen(g_tab_names[i])) + 2 * TAB_PADX;
}
static int tabx(int i)
{
    int x = 6;
    for (int k = 0; k < i; k++) x += tabw(k) + TAB_GAPX;
    return x;
}

static void draw_tabs(void)
{
    dobui_FillRect(win, 0, 0, win_w, TAB_H, TABBAR_BG);
    for (int i = 0; i < NTABS; i++) {
        int tx = tabx(i), tw = tabw(i);
        bool active = (i == g_active_tab);
        uint32_t bg = active ? RIBBON_BG : TAB_INACTIVE;
        dobui_FillRect(win, tx, 3, tw, TAB_H - 3, bg);
        if (active) dobui_FillRect(win, tx, TAB_H - 3, tw, 3, RIBBON_BG);  /* merge with content */
        dobui_DrawText(win, tx + TAB_PADX, 7, g_tab_names[i], 0x00202020, bg);
    }
}

/* draw the chrome (tab strip + ribbon content row + active-tab controls).
 * Called LAST in every repaint so it covers any sheet rows that spilled above
 * the ribbon while scrolling. */
static void paint_ribbon(void)
{
    draw_tabs();
    dobui_FillRect(win, 0, TAB_H, win_w, RIBBON_CONTENT_H, RIBBON_BG);   /* ribbon content row */

    if (g_active_tab == TAB_HOME) {
        g_bold.col_bg   = g_b_on ? ACTIVE_BG : NORMAL_BTN;
        g_italic.col_bg = g_i_on ? ACTIVE_BG : NORMAL_BTN;
        g_under.col_bg  = g_u_on ? ACTIVE_BG : NORMAL_BTN;
        g_strike.col_bg = g_s_on ? ACTIVE_BG : NORMAL_BTN;
        dobpbtn_Draw(&g_bold); dobpbtn_Draw(&g_italic); dobpbtn_Draw(&g_under); dobpbtn_Draw(&g_strike);
        g_alignL.col_bg = (g_align == DD_ALIGN_LEFT)    ? ACTIVE_BG : NORMAL_BTN;
        g_alignC.col_bg = (g_align == DD_ALIGN_CENTER)  ? ACTIVE_BG : NORMAL_BTN;
        g_alignR.col_bg = (g_align == DD_ALIGN_RIGHT)   ? ACTIVE_BG : NORMAL_BTN;
        g_alignJ.col_bg = (g_align == DD_ALIGN_JUSTIFY) ? ACTIVE_BG : NORMAL_BTN;
        dobpbtn_Draw(&g_alignL); dobpbtn_Draw(&g_alignC); dobpbtn_Draw(&g_alignR); dobpbtn_Draw(&g_alignJ);
        g_fcolor.col_bg = NORMAL_BTN;
        dobpbtn_Draw(&g_fcolor);
        g_pagebg_btn.col_bg = NORMAL_BTN;
        dobpbtn_Draw(&g_pagebg_btn);
        g_hl_btn.col_bg = NORMAL_BTN;
        dobpbtn_Draw(&g_hl_btn);
        g_hlclear_btn.col_bg = NORMAL_BTN;
        dobpbtn_Draw(&g_hlclear_btn);
        dobdd_Draw(&g_font_dd); dobdd_Draw(&g_size_dd);
        dobdd_FlushPopup(&g_font_dd); dobdd_FlushPopup(&g_size_dd);
    } else if (g_active_tab == TAB_FILE) {
        g_open.col_bg = NORMAL_BTN; g_save.col_bg = NORMAL_BTN;   /* default is black -- force light */
        dobpbtn_Draw(&g_open); dobpbtn_Draw(&g_save);
    } else if (g_active_tab == TAB_SUBST) {
        dobbtn_Draw(&g_findbtn);
        dobbtn_Draw(&g_subsbtn);
    } else if (g_active_tab == TAB_PARA) {
        dobui_DrawText(win, 12, TAB_H + 8,  "Interlinea:", 0x00202020, RIBBON_BG);
        dobui_DrawText(win, 12, TAB_H + 31, "Spaziatura paragrafo:", 0x00202020, RIBBON_BG);
        for (int i = 0; i < 3; i++) { g_para_ls[i].col_bg = (i == g_para_ls_active) ? ACTIVE_BG : NORMAL_BTN; dobbtn_Draw(&g_para_ls[i]); }
        for (int i = 0; i < 4; i++) { g_para_sp[i].col_bg = (i == g_para_sp_active) ? ACTIVE_BG : NORMAL_BTN; dobbtn_Draw(&g_para_sp[i]); }
    } else if (g_active_tab == TAB_LAYOUT) {
        dobbtn_Draw(&g_layout_pagebtn);
        g_margin_btn.col_bg = g_margin_edit ? ACTIVE_BG : NORMAL_BTN;
        dobbtn_Draw(&g_margin_btn);
        dobbtn_Draw(&g_sectbreak_btn);
        g_cols_btn.col_bg = g_cols_open ? ACTIVE_BG : NORMAL_BTN;
        dobbtn_Draw(&g_cols_btn);
    }
}

static void draw_popup(void);              /* contextual selection / substitution overlay */
static void position_sel_popup(void);
static void position_subst_popup(void);
static void list_renumber(void);           /* keep list markers sequential after edits */

/* Columns chooser overlay -- drawn last (on top of the ribbon), Layout tab only. */
static void draw_cols_popup(void)
{
    if (!g_cols_open) return;
    PageSetup ps; df_doc_get_page(g_doc, &ps);
    int cur = (int)(ps.columns < 1 ? 1 : ps.columns);
    dobui_FillRect(win, COLP_X, COLP_Y, COLP_W, COLP_H, 0x00ECECEC);
    dobui_FillRect(win, COLP_X, COLP_Y, COLP_W, 1, 0x00808080);
    dobui_FillRect(win, COLP_X, COLP_Y + COLP_H - 1, COLP_W, 1, 0x00808080);
    dobui_FillRect(win, COLP_X, COLP_Y, 1, COLP_H, 0x00808080);
    dobui_FillRect(win, COLP_X + COLP_W - 1, COLP_Y, 1, COLP_H, 0x00808080);
    dobui_DrawText(win, COLP_X + 10, COLP_Y + 8, "Colonne", 0x00202020, 0x00ECECEC);
    for (int i = 0; i < 6; i++) {
        g_cols_pick[i].col_bg = ((i + 1) == cur) ? ACTIVE_BG : NORMAL_BTN;
        dobbtn_Draw(&g_cols_pick[i]);
    }
    dobui_DrawText(win, COLP_X + 10, COLP_Y + 64, "Distanza", 0x00202020, 0x00ECECEC);
    dobbtn_Draw(&g_cols_gap_dec);
    char gap[24]; snprintf(gap, sizeof gap, "%d px", (int)(ps.column_gap / 15));
    dobui_DrawText(win, COLP_X + 128, COLP_Y + 64, gap, 0x00202020, 0x00ECECEC);
    dobbtn_Draw(&g_cols_gap_inc);
}

/* geometry of the two zoom buttons (right-aligned in the status bar) */
static void zoom_btn_rects(int *minus_x, int *plus_x, int *by, int *bw, int *bh)
{
    int b = 22, pctw = 50, hgt = STATUS_H - 8;
    int yb = content_bottom() + (STATUS_H - hgt) / 2;
    int px = win_w - 8 - b;                  /* "+" at the far right */
    int mx = px - 4 - pctw - 4 - b;          /* "-" left of the % slot */
    if (minus_x) *minus_x = mx;
    if (plus_x)  *plus_x  = px;
    if (by) *by = yb; if (bw) *bw = b; if (bh) *bh = hgt;
}

static void draw_zoom_btn(int x, int y, int w, int h, const char *label)
{
    dobui_FillRect(win, x, y, w, h, STATUS_BTN);
    dobui_FillRect(win, x, y, w, 1, STATUS_LINE);
    dobui_FillRect(win, x, y + h - 1, w, 1, STATUS_LINE);
    dobui_FillRect(win, x, y, 1, h, STATUS_LINE);
    dobui_FillRect(win, x + w - 1, y, 1, h, STATUS_LINE);
    int tw = dob_text_width(label, (int)strlen(label));
    dobui_DrawText(win, x + (w - tw) / 2, y + (h - 12) / 2, label, STATUS_FG, STATUS_BTN);
}

/* bottom status bar: page X of Y, word + char counts, and zoom controls. */
static void draw_statusbar(void)
{
    int y0 = content_bottom();
    dobui_FillRect(win, 0, y0, win_w, STATUS_H, STATUS_BG);
    dobui_FillRect(win, 0, y0, win_w, 1, STATUS_LINE);

    int npages = (int)dp_page_count(g_eng); if (npages < 1) npages = 1;
    int cur = g_cur_page; if (cur < 1) cur = 1; if (cur > npages) cur = npages;

    char buf[220];
    int m;
    if (has_sel()) {
        /* Selection active: show selected code points / total and the share,
         * the way a word processor does while text is highlighted. */
        uint32_t a = sel_lo(), b = sel_hi(), n = b - a;
        int selc = 0;
        char *sb = (char *)malloc(n ? n : 1);
        if (sb) {
            uint32_t got = df_doc_get_text(g_doc, a, n, sb, n);
            for (uint32_t i = 0; i < got; i++)
                if (((unsigned char)sb[i] & 0xC0) != 0x80) selc++;   /* count UTF-8 lead bytes */
            free(sb);
        } else selc = (int)n;
        int pct = (g_nchars > 0) ? (int)(((long)selc * 100 + g_nchars / 2) / g_nchars) : 0;
        m = snprintf(buf, sizeof buf, "Pagina %d di %d    Selezionati: %d / %d caratteri (%d%%)",
                     cur, npages, selc, g_nchars, pct);
    } else {
        m = snprintf(buf, sizeof buf, "Pagina %d di %d    Parole: %d    Caratteri: %d",
                     cur, npages, g_nwords, g_nchars);
    }
    if (m < 0) m = 0; if (m > (int)sizeof buf) m = (int)sizeof buf;
    switch (g_note) {
        case NOTE_PENDING:   snprintf(buf + m, sizeof buf - (size_t)m, "      Salvataggio automatico tra %ds", g_as_left); break;
        case NOTE_AUTOSAVED: snprintf(buf + m, sizeof buf - (size_t)m, "      Salvato automaticamente"); break;
        case NOTE_SAVED:     snprintf(buf + m, sizeof buf - (size_t)m, "      Salvato"); break;
        case NOTE_UNSAVED:   snprintf(buf + m, sizeof buf - (size_t)m, "      Non salvato"); break;
        default: break;
    }
    dobui_DrawText(win, 8, y0 + (STATUS_H - 12) / 2, buf, STATUS_FG, STATUS_BG);

    int mnx, plx, by, bw, bh;
    zoom_btn_rects(&mnx, &plx, &by, &bw, &bh);
    draw_zoom_btn(mnx, by, bw, bh, "-");
    draw_zoom_btn(plx, by, bw, bh, "+");
    char zb[12]; snprintf(zb, sizeof zb, "%d%%", g_zoom_steps[g_zoom_idx]);
    int slot_x = mnx + bw + 4, slot_w = plx - 4 - slot_x;
    int zw = dob_text_width(zb, (int)strlen(zb));
    dobui_DrawText(win, slot_x + (slot_w - zw) / 2, y0 + (STATUS_H - 12) / 2, zb, STATUS_FG, STATUS_BG);
}

static void paint(void)
{
    dobdd_ClearGhost(&g_font_dd); dobdd_ClearGhost(&g_size_dd);
    composite_view();
    g_band_y0 = g_band_y1 = -1;   /* la banda vale per UNA paint */   /* desk + sheets + borders -> one stable blit at RIBBON_H */
    draw_caret();       /* caret overlay (FillRect, clipped to the desk)          */
    draw_margin_guides();/* Layout-tab construction lines (no-op outside that tab) */
    draw_popup();       /* selection toolbar / autocorrect-undo button            */
    paint_ribbon();     /* chrome on top                                          */
    draw_cols_popup();  /* columns chooser, above the ribbon (Layout tab)         */
    draw_statusbar();   /* page/word/char counts + zoom controls (bottom)         */
    dobui_Invalidate(win);
}

/* Apply a new page geometry from the Imposta-pagina dialog, scoped to the
 * whole document or just the section containing the caret. */
static void on_pagesetup_apply(const PageSetup *ps, int scope, void *ud)
{
    (void)ud;
    if (scope == PAGESETUP_SCOPE_SECTION) df_doc_set_section_page_at(g_doc, g_caret, ps);
    else                                  df_doc_set_page(g_doc, ps);
    df_layout_rebuild(g_L);
    dp_relayout(g_eng);
    paint();
}

static void ensure_caret_visible(void)
{
    dl_locus loc;
    if (df_layout_locate(g_L, g_caret, &loc)) {
        dp_scroll_to_content(g_eng, loc.y_top, loc.y_bottom);
        dp_scroll_to_content_x(g_eng, loc.x, loc.x, loc.y_top);
    }
}

/* Columns popup actions: set the doc-wide column count / gutter and re-flow. */
static void apply_columns(int count)
{
    PageSetup ps; df_doc_get_page(g_doc, &ps);
    ps.columns = (uint32_t)(count < 1 ? 1 : (count > 6 ? 6 : count));
    if (ps.columns > 1 && ps.column_gap == 0) ps.column_gap = 360;   /* sane default gutter */
    df_doc_set_page(g_doc, &ps);
    df_layout_rebuild(g_L); dp_relayout(g_eng); paint();
}
static void bump_col_gap(int dtw)
{
    PageSetup ps; df_doc_get_page(g_doc, &ps);
    long g = (long)ps.column_gap + dtw;
    if (g < 0) g = 0;
    if (g > 2880) g = 2880;                                          /* clamp 0..2 inch */
    ps.column_gap = (uint32_t)g;
    df_doc_set_page(g_doc, &ps);
    df_layout_rebuild(g_L); dp_relayout(g_eng); paint();
}

/* ---- editing (drives reflow + engine notify) ---- */

/* recompute word + character counts (after every edit / load) */
static void recount(void)
{
    uint32_t n = df_doc_length(g_doc);
    g_nwords = 0; g_nchars = 0;
    if (n == 0) return;
    char *buf = (char *)malloc(n);
    if (!buf) { g_nchars = (int)n; return; }
    df_doc_get_text(g_doc, 0, n, buf, n);
    bool inword = false;
    for (uint32_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if ((c & 0xC0) != 0x80) g_nchars++;     /* UTF-8 lead bytes = code points */
        bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v');
        if (!ws) { if (!inword) { g_nwords++; inword = true; } }
        else inword = false;
    }
    free(buf);
}

/* rebuild the layout + page engine at the current zoom DPI (keeps the doc) */
static void rebuild_layout_engine(void)
{
    dp_destroy(g_eng); df_layout_destroy(g_L);
    dl_opts o; o.dpi = layout_dpi();
    g_L = df_layout_create(g_doc, g_fonts, &o);
    g_eng = dp_create(g_L);
    dp_set_viewport(g_eng, win_w, content_bottom() - RIBBON_H);
    PageSetup ps; df_doc_get_page(g_doc, &ps);
    dp_set_page_bg(g_eng, ps.bg_color ? ps.bg_color : 0xFFFFFFFFu);
}

static void apply_zoom(int delta)
{
    int ni = g_zoom_idx + delta;
    if (ni < 0) ni = 0;
    if (ni >= NZOOM) ni = NZOOM - 1;
    if (ni == g_zoom_idx) return;
    g_zoom_idx = ni;
    rebuild_layout_engine();
    ensure_caret_visible();
    paint();
}

static void tick_start(void) { if (!g_tick_on) { dobui_set_tick(1000); g_tick_on = true; } }
static void tick_stop(void)  { if (g_tick_on)  { dobui_set_tick(0);    g_tick_on = false; } }

/* Called after every edit: (re)arm the one-minute autosave countdown. Each
 * edit resets it to a full minute. A document that has never been saved has no
 * file to write to, so it's only flagged "Non salvato" -- no timer. */
static void autosave_touch(void)
{
    if (g_saved) { g_note = NOTE_PENDING; g_as_left = AUTOSAVE_SECS; tick_start(); }
    else         { g_note = NOTE_UNSAVED; }
}

/* Dal dl_update del reflow incrementale alla banda di vista per la
 * prossima paint. Condizioni per una banda stretta: (a) reflow senza
 * shift verticale (height_delta==0: nulla sotto si e' mosso), (b) lo
 * scroll non e' cambiato (ensure_caret_visible non ha spostato la
 * vista). Se una manca -> vista intera (-1). Il margine di 2 px copre
 * bordi pagina e arrotondamenti della conversione contenuto->finestra. */
static void arm_band_from_update(const dl_update *u, int sc_before, int sx_before)
{
    g_band_y0 = g_band_y1 = -1;
    if (u->height_delta != 0.0f) return;
    if (dp_scroll(g_eng) != sc_before || dp_scroll_x(g_eng) != sx_before) return;
    int wx, wt, wb;
    dp_content_to_window(g_eng, 0, u->dirty_y0, &wx, &wt);
    dp_content_to_window(g_eng, 0, u->dirty_y1, &wx, &wb);
    g_band_y0 = wt - 2;
    g_band_y1 = wb + 2;
}

static void after_edit(uint32_t pos, uint32_t old_len, uint32_t new_len)
{
    g_fr_active = false; g_fr_nmatch = 0; g_fr_cur = -1;   /* match positions are now stale */
    dl_update u; bool u_ok = false;
    int sc_before = dp_scroll(g_eng), sx_before = dp_scroll_x(g_eng);
    if (old_len > 0 && new_len > 0) {
        /* A replace (delete + insert at the same spot, e.g. typing over a
         * selection or an autocorrect swap): the incremental paragraph-splice
         * in df_layout_reflow is fragile for this case, so do a full, always-
         * correct relayout instead -- the same path undo/redo use. Pure
         * inserts / pure deletes keep the fast incremental route below. */
        df_layout_rebuild(g_L);
        dp_relayout(g_eng);
    } else {
        df_layout_reflow(g_L, pos, old_len, new_len, &u);
        dp_notify_edit(g_eng, &u);
        u_ok = true;
    }
    ensure_caret_visible();
    update_toggles_from_caret();
    if (u_ok) arm_band_from_update(&u, sc_before, sx_before);
    /* Conteggio parole DIFFERITO: recount() e' O(documento) (malloc +
     * copia + scansione integrale) e girava a OGNI tasto — su un
     * documento lungo e' il singolo costo CPU piu' alto della
     * digitazione. La status bar puo' aspettare il tick da 1 Hz. */
    g_count_dirty = true;
    tick_start();
    autosave_touch();
    paint();
}

/* Post-processing for a formatting change (bold/size/font/colour/highlight/
 * alignment/spacing). Unlike an insert or delete, the text and the paragraph
 * boundaries are UNCHANGED -- only run attributes differ -- so the layout needs
 * only the touched span [pos, pos+n) re-shaped, not a whole-document rebuild.
 *
 * This is the same incremental route a same-paragraph keystroke takes, and it
 * is actually SAFER than after_edit's replace path: there was no delete+insert,
 * so byte<->paragraph mapping is exact (the "reflow is fragile for replace"
 * caveat doesn't apply). df_layout_reflow re-runs build_para over the span,
 * which re-reads the current run attributes and re-bakes glyph colour /
 * highlight / underline / strike, then self-falls-back to a full rebuild for
 * multi-column / multi-section documents. Word/char counts are unchanged, so
 * recount() is skipped -- the whole point is to keep formatting O(touched
 * paragraphs) instead of O(document). */
static void after_format(uint32_t pos, uint32_t n)
{
    g_fr_active = false; g_fr_nmatch = 0; g_fr_cur = -1;   /* attribute change: drop the stale find overlay */
    dl_update u;
    int sc_before = dp_scroll(g_eng), sx_before = dp_scroll_x(g_eng);
    df_layout_reflow(g_L, pos, n, n, &u);
    dp_notify_edit(g_eng, &u);
    ensure_caret_visible();
    update_toggles_from_caret();
    arm_band_from_update(&u, sc_before, sx_before);
    autosave_touch();
    paint();
}

static void insert_text(const char *s, uint32_t n)
{
    if (has_sel()) {
        uint32_t a = sel_lo(), b = sel_hi();
        df_doc_delete(g_doc, a, b - a);
        df_doc_insert(g_doc, a, s, n);
        g_caret = a + n; g_anchor = -1;
        after_edit(a, b - a, n);
        list_renumber();
    } else {
        uint32_t p = g_caret;
        df_doc_insert(g_doc, p, s, n);
        if (g_pending_on && p == g_pending_pos && g_pending.mask)
            df_doc_apply_char_fmt(g_doc, p, n, &g_pending);  /* typed text gets the armed format */
        else
            g_pending_on = false;                            /* caret moved since arming -- drop it */
        g_caret = p + n; g_anchor = -1;
        g_pending_pos = g_caret;                             /* keep following the caret while armed */
        after_edit(p, 0, n);
    }
}

/* ---- clipboard: system-wide inter-process UTF-8 text ---- */
static void do_copy(void)
{
    if (!has_sel()) return;
    uint32_t a = sel_lo(), b = sel_hi(), n = b - a;
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) return;
    uint32_t got = df_doc_get_text(g_doc, a, n, buf, n + 1);
    dobui_cliptext_set(buf, (int)got);
    free(buf);
}
static void do_delete_sel(void)
{
    if (!has_sel()) return;
    uint32_t a = sel_lo(), b = sel_hi();
    df_doc_delete(g_doc, a, b - a);
    g_caret = a; g_anchor = -1;
    after_edit(a, b - a, 0);
}
static void do_cut(void)
{
    if (!has_sel()) return;
    do_copy();
    do_delete_sel();
}
static void do_paste(void)
{
    int sz = dobui_cliptext_size();
    if (sz <= 0) return;
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) return;
    int n = 0;
    if (dobui_cliptext_get(buf, sz + 1, &n) == 0 && n > 0)
        insert_text(buf, (uint32_t)n);
    free(buf);
}

static void backspace(void)
{
    if (has_sel()) { uint32_t a = sel_lo(), b = sel_hi(); df_doc_delete(g_doc, a, b - a); g_caret = a; g_anchor = -1; after_edit(a, b - a, 0); list_renumber(); return; }
    if (g_caret == 0) return;
    uint32_t prev = df_doc_prev_cp(g_doc, g_caret);
    df_doc_delete(g_doc, prev, g_caret - prev);
    uint32_t old = g_caret - prev; g_caret = prev; g_anchor = -1;
    after_edit(prev, old, 0);
    list_renumber();
}

static void forward_delete(void)
{
    if (has_sel()) { uint32_t a = sel_lo(), b = sel_hi(); df_doc_delete(g_doc, a, b - a); g_caret = a; g_anchor = -1; after_edit(a, b - a, 0); list_renumber(); return; }
    uint32_t nx = df_doc_next_cp(g_doc, g_caret);
    if (nx == g_caret) return;
    df_doc_delete(g_doc, g_caret, nx - g_caret);
    g_anchor = -1;
    after_edit(g_caret, nx - g_caret, 0);
    list_renumber();
}

/* real-time autocorrect: after a printable keystroke, if the bytes just
 * before the caret complete a known "from" pattern at a word boundary,
 * swap them for its "to" immediately. */
static void apply_autocorrect(void)
{
    if (has_sel() || g_caret == 0) return;
    char pre[64];
    int n = 0;
    uint32_t start = (g_caret > 63u) ? g_caret - 63u : 0u;
    for (uint32_t p = start; p < g_caret; p++) pre[n++] = byte_at(p);

    int mlen; const char *to;
    if (autocorr_match(pre, n, &mlen, &to)) {
        uint32_t from_pos = g_caret - (uint32_t)mlen;
        int tl = 0; while (to[tl]) tl++;
        /* remember the original bytes so the popup's X can undo the swap */
        g_subst_pos = from_pos; g_subst_to_n = tl;
        g_subst_from_n = (mlen < (int)sizeof g_subst_from) ? mlen : (int)sizeof g_subst_from;
        for (int k = 0; k < g_subst_from_n; k++) g_subst_from[k] = byte_at(from_pos + (uint32_t)k);
        df_doc_delete(g_doc, from_pos, (uint32_t)mlen);
        df_doc_insert(g_doc, from_pos, to, (uint32_t)tl);
        g_caret = from_pos + (uint32_t)tl;
        g_anchor = -1;
        after_edit(from_pos, (uint32_t)mlen, (uint32_t)tl);
        g_list_undo = false;
        g_pp = PP_SUBST; position_subst_popup();
    }
}

/* ---- list auto-continuation -------------------------------------------------
 * On Enter, if the current line starts with a list marker, the new line begins
 * with the next one: N)->N+1), a)->b), A)->B), roman->next, - stays -, and *
 * becomes the bullet. The generated marker gets the same red-X undo popup as an
 * autocorrect (backspace right after also removes it). Enter on a marker-only
 * line ends the list. Flat lists only (no nesting). */

static int lst_roman1(char c) {
    switch (c) { case 'I': case 'i': return 1;    case 'V': case 'v': return 5;
                 case 'X': case 'x': return 10;   case 'L': case 'l': return 50;
                 case 'C': case 'c': return 100;  case 'D': case 'd': return 500;
                 case 'M': case 'm': return 1000; } return 0;
}
static int lst_to_roman(int v, char *out, bool up) {
    if (v <= 0 || v > 3999) { out[0] = 0; return 0; }
    static const int val[] = { 1000,900,500,400,100,90,50,40,10,9,5,4,1 };
    static const char *U[]  = { "M","CM","D","CD","C","XC","L","XL","X","IX","V","IV","I" };
    static const char *Lo[] = { "m","cm","d","cd","c","xc","l","xl","x","ix","v","iv","i" };
    int n = 0;
    for (int i = 0; i < 13; i++) while (v >= val[i]) { const char *r = (up ? U : Lo)[i]; while (*r) out[n++] = *r++; v -= val[i]; }
    out[n] = 0; return n;
}
/* canonical roman -> value (0 if not a well-formed roman) */
static int lst_roman(const char *s, int n, bool *upper) {
    if (n <= 0 || n > 15) return 0;
    for (int i = 0; i < n; i++) if (!lst_roman1(s[i])) return 0;
    int tot = 0, prev = 0;
    for (int i = n - 1; i >= 0; i--) { int v = lst_roman1(s[i]); if (v < prev) tot -= v; else { tot += v; prev = v; } }
    if (tot <= 0 || tot > 3999) return 0;
    bool up = (s[0] >= 'A' && s[0] <= 'Z');
    char b[16]; int m = lst_to_roman(tot, b, up);
    if (m != n) return 0;
    for (int i = 0; i < n; i++) { char a = s[i], c = b[i]; if (a >= 'a') a -= 32; if (c >= 'a') c -= 32; if (a != c) return 0; }
    if (upper) *upper = up;
    return tot;
}

typedef enum { LT_NONE, LT_NUM, LT_ALPHA, LT_ROMAN, LT_DASH, LT_STAR, LT_BULLET } lst_type;
typedef struct { lst_type t; bool upper; long val; int indent; int mlen; int tok_i, tok_n; } lst_mark;

/* Parse a leading marker in s[0..n): indent + token + delimiter + one space.
 * Letters are left tentatively LT_ALPHA; lst_classify() resolves alpha vs roman. */
static bool lst_parse(const char *s, int n, lst_mark *m) {
    int i = 0; while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    m->indent = i; m->tok_i = i; m->upper = false; m->val = 0;
    if (i >= n) return false;
    unsigned char c = (unsigned char)s[i];
    if (c == '-' && i + 1 < n && s[i + 1] == ' ') { m->t = LT_DASH;   m->tok_n = 1; m->mlen = i + 2; return true; }
    if (c == '*' && i + 1 < n && s[i + 1] == ' ') { m->t = LT_STAR;   m->tok_n = 1; m->mlen = i + 2; return true; }
    if (c == 0xE2 && i + 3 < n && (unsigned char)s[i+1] == 0x80 && (unsigned char)s[i+2] == 0xA2 && s[i+3] == ' ')
        { m->t = LT_BULLET; m->tok_n = 3; m->mlen = i + 4; return true; }
    if (c >= '0' && c <= '9') {
        int j = i; long v = 0; while (j < n && s[j] >= '0' && s[j] <= '9') { v = v * 10 + (s[j] - '0'); j++; }
        if (j < n && s[j] == ')' && j + 1 < n && s[j + 1] == ' ') { m->t = LT_NUM; m->val = v; m->tok_n = j - i; m->mlen = j + 2; return true; }
        return false;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        int j = i; while (j < n && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z'))) j++;
        if (j < n && s[j] == ')' && j + 1 < n && s[j + 1] == ' ') { m->t = LT_ALPHA; m->tok_n = j - i; m->mlen = j + 2; return true; }
        return false;
    }
    return false;
}
/* Resolve a tentative LT_ALPHA into LT_ALPHA / LT_ROMAN / LT_NONE, using the
 * previous line's marker when the single letter is ambiguous (e.g. i, c, v). */
static void lst_classify(const char *s, lst_mark *m, const lst_mark *prev, const char *ps) {
    if (m->t != LT_ALPHA) return;
    const char *tok = s + m->tok_i; int tn = m->tok_n;
    bool up = false; int rv = lst_roman(tok, tn, &up);
    bool single = (tn == 1);
    char L = tok[0]; if (L >= 'a') L -= 32;
    if (prev && prev->t == LT_ALPHA && single && prev->tok_n == 1) {
        char pL = ps[prev->tok_i]; bool pu = (pL < 'a'); char pn = pL; if (pn >= 'a') pn -= 32;
        bool cu = (tok[0] < 'a');
        if (pu == cu && (pn + 1) == L) { m->t = LT_ALPHA; m->upper = cu; m->val = L - 'A' + 1; return; }
    }
    if (prev && prev->t == LT_ROMAN && rv && prev->val + 1 == rv) { m->t = LT_ROMAN; m->upper = up; m->val = rv; return; }
    if (rv && (tn >= 2 || L == 'I')) { m->t = LT_ROMAN; m->upper = up; m->val = rv; return; }
    if (single) { m->t = LT_ALPHA; m->upper = (tok[0] < 'a'); m->val = L - 'A' + 1; return; }
    m->t = LT_NONE;
}
/* Next marker (with trailing space) into out; 0 if the sequence can't advance. */
static int lst_next(const lst_mark *m, char *out) {
    int n = 0;
    switch (m->t) {
        case LT_NUM: {
            long v = m->val + 1; char t[24]; int tn = 0; if (v == 0) t[tn++] = '0';
            while (v > 0) { t[tn++] = (char)('0' + v % 10); v /= 10; }
            for (int i = tn - 1; i >= 0; i--) out[n++] = t[i];
            out[n++] = ')'; out[n++] = ' '; break;
        }
        case LT_ALPHA: {
            if (m->val + 1 > 26) return 0;
            out[n++] = (char)((m->upper ? 'A' : 'a') + m->val);
            out[n++] = ')'; out[n++] = ' '; break;
        }
        case LT_ROMAN: {
            char b[16]; int k = lst_to_roman((int)m->val + 1, b, m->upper); if (k == 0) return 0;
            for (int i = 0; i < k; i++) out[n++] = b[i];
            out[n++] = ')'; out[n++] = ' '; break;
        }
        case LT_DASH:   out[n++] = '-'; out[n++] = ' '; break;
        case LT_STAR: case LT_BULLET:
            out[n++] = (char)0xE2; out[n++] = (char)0x80; out[n++] = (char)0xA2; out[n++] = ' '; break;
        default: return 0;
    }
    out[n] = 0; return n;
}

/* Handle Enter on a list line. Returns true if it did (caller skips the plain
 * newline). */
/* Build the marker for a specific value (token + ") " + trailing space). */
static int lst_build(lst_type t, long val, bool upper, char *out) {
    int n = 0;
    if (t == LT_NUM) {
        if (val < 1) return 0;
        char tmp[24]; int tn = 0; long v = val; while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
        for (int i = tn - 1; i >= 0; i--) out[n++] = tmp[i];
    } else if (t == LT_ALPHA) {
        if (val < 1 || val > 26) return 0;
        out[n++] = (char)((upper ? 'A' : 'a') + (val - 1));
    } else if (t == LT_ROMAN) {
        char b[16]; int k = lst_to_roman((int)val, b, upper); if (!k) return 0;
        for (int i = 0; i < k; i++) out[n++] = b[i];
    } else return 0;
    out[n++] = ')'; out[n++] = ' '; out[n] = 0; return n;
}
/* Determine a block's type + starting value from its first (and second) token. */
static lst_type lst_detect(const char *t0, int n0, const char *t1, int n1, bool *upper, long *startval) {
    bool d0 = (n0 > 0); for (int i = 0; i < n0; i++) if (t0[i] < '0' || t0[i] > '9') { d0 = false; break; }
    if (d0) { long v = 0; for (int i = 0; i < n0; i++) v = v * 10 + (t0[i] - '0'); *startval = v; *upper = false; return LT_NUM; }
    bool up = (t0[0] < 'a'); bool single0 = (n0 == 1);
    int rv0 = lst_roman(t0, n0, NULL);
    if (t1 && n1 > 0) {
        if (single0 && n1 == 1) {
            char a = t0[0], b = t1[0]; bool aup = (a < 'a'), bup = (b < 'a');
            char an = a; if (an >= 'a') an -= 32; char bn = b; if (bn >= 'a') bn -= 32;
            if (aup == bup && bn == an + 1) { *upper = aup; *startval = an - 'A' + 1; return LT_ALPHA; }
        }
        int rv1 = lst_roman(t1, n1, NULL);
        if (rv0 && rv1 && rv1 == rv0 + 1) { *upper = up; *startval = rv0; return LT_ROMAN; }
    }
    if (single0 && (t0[0] == 'I' || t0[0] == 'i')) { *upper = up; *startval = 1; return LT_ROMAN; }
    if (single0) { char an = t0[0]; if (an >= 'a') an -= 32; *upper = up; *startval = an - 'A' + 1; return LT_ALPHA; }
    if (rv0) { *upper = up; *startval = rv0; return LT_ROMAN; }
    return LT_NONE;
}
/* Two lines share a renumberable family: same indent, same delimiter class
 * (digits vs letters) and, for letters, the same case. */
static bool lst_family_ok(const lst_mark *m, const char *line, int indent, bool digit, bool upper) {
    if (m->indent != indent) return false;
    if (digit) return m->t == LT_NUM;
    if (m->t != LT_ALPHA) return false;
    return ((line[m->tok_i] < 'a') == upper);
}

/* Renumber the contiguous list block the caret sits in, so inserting or
 * removing an item in the middle updates every following marker. No-op (no
 * relayout) if the caret isn't on a numeric/letter/roman item or nothing
 * changes. Bounded: reads a window around the caret in one block, caps the
 * item count. */
static void list_renumber(void)
{
    uint32_t L = df_doc_length(g_doc);
    if (!L) return;
    enum { RN = 8192, MAXIT = 2048 };
    uint32_t from = g_caret > (uint32_t)RN ? g_caret - (uint32_t)RN : 0;
    uint32_t to   = (g_caret + (uint32_t)RN < L) ? g_caret + (uint32_t)RN : L;
    uint32_t wg = to - from;
    char *w = (char *)malloc(wg + 1);
    if (!w) return;
    df_doc_get_text(g_doc, from, wg, w, wg + 1);
    uint32_t co = g_caret - from;

    uint32_t clso = co; while (clso > 0 && w[clso - 1] != '\n') clso--;
    uint32_t cleo = co; while (cleo < wg && w[cleo] != '\n') cleo++;
    lst_mark cm;
    if (!lst_parse(w + clso, (int)(cleo - clso), &cm) || (cm.t != LT_NUM && cm.t != LT_ALPHA)) { free(w); return; }
    int indent = cm.indent;
    bool digit = (cm.t == LT_NUM);
    bool upper = digit ? false : (w[clso + cm.tok_i] < 'a');

    /* walk up to the block's first line */
    uint32_t bstart = clso;
    while (bstart > 0) {
        uint32_t ple = bstart - 1, pls = ple;
        while (pls > 0 && w[pls - 1] != '\n') pls--;
        lst_mark pm2;
        if (!lst_parse(w + pls, (int)(ple - pls), &pm2)) break;
        if (!lst_family_ok(&pm2, w + pls, indent, digit, upper)) break;
        bstart = pls;
        if (pls == 0 && from != 0) break;     /* clamped at window edge */
    }
    /* collect the block's line offsets going down */
    uint32_t *lo = (uint32_t *)malloc(sizeof(uint32_t) * MAXIT);
    if (!lo) { free(w); return; }
    int count = 0; uint32_t o = bstart;
    for (;;) {
        if (count >= MAXIT) break;
        uint32_t leo = o; while (leo < wg && w[leo] != '\n') leo++;
        lst_mark m2;
        if (!lst_parse(w + o, (int)(leo - o), &m2)) break;
        if (!lst_family_ok(&m2, w + o, indent, digit, upper)) break;
        lo[count++] = o;
        if (leo >= wg) break;
        o = leo + 1;
    }
    if (count < 1) { free(lo); free(w); return; }

    /* type + starting value from the first (and second) item */
    lst_mark ma, mb;
    { uint32_t e = lo[0]; while (e < wg && w[e] != '\n') e++; lst_parse(w + lo[0], (int)(e - lo[0]), &ma); }
    bool have1 = count >= 2;
    if (have1) { uint32_t e = lo[1]; while (e < wg && w[e] != '\n') e++; lst_parse(w + lo[1], (int)(e - lo[1]), &mb); }
    long startval; bool tup;
    lst_type type = lst_detect(w + lo[0] + ma.tok_i, ma.tok_n,
                               have1 ? w + lo[1] + mb.tok_i : NULL, have1 ? mb.tok_n : 0, &tup, &startval);
    if (type == LT_NONE) { free(lo); free(w); return; }
    upper = tup;

    /* rewrite markers that differ, bottom-to-top so earlier offsets stay valid */
    uint32_t caret = g_caret, origC = g_caret; bool changed = false;
    for (int i = count - 1; i >= 0; i--) {
        uint32_t e = lo[i]; while (e < wg && w[e] != '\n') e++;
        lst_mark mi; lst_parse(w + lo[i], (int)(e - lo[i]), &mi);
        char exp[32]; int en = lst_build(type, startval + i, upper, exp);
        if (en <= 0) continue;
        int cl = mi.mlen - indent; const char *cur = w + lo[i] + indent;
        bool same = (cl == en); if (same) for (int k = 0; k < en; k++) if (cur[k] != exp[k]) { same = false; break; }
        if (same) continue;
        uint32_t pos = from + lo[i] + (uint32_t)indent;
        df_doc_delete(g_doc, pos, (uint32_t)cl);
        df_doc_insert(g_doc, pos, exp, (uint32_t)en);
        changed = true;
        int delta = en - cl;
        if (pos + (uint32_t)cl <= origC) caret += delta;
        else if (pos < origC) caret = pos + (uint32_t)en;
    }
    free(lo); free(w);
    if (changed) {
        g_caret = caret; g_anchor = -1;
        df_layout_rebuild(g_L); dp_relayout(g_eng);
        ensure_caret_visible(); update_toggles_from_caret(); recount(); paint();
    }
}


/* Handle Enter on a list line. Returns true if it did (caller skips the plain
 * newline). Reads the current line in one bounded block: a per-byte scan would
 * call df_doc_get_text (a piece-table walk) for every character, slow enough on
 * a heavily-edited document or a very long line to look like a freeze. */
static bool list_continue(void)
{
    if (has_sel()) return false;
    uint32_t L = df_doc_length(g_doc);

    /* one windowed read around the caret; if the line's start or end falls
     * outside the window the line is absurdly long -> not a list. */
    enum { LC_WIN = 512 };
    uint32_t from = g_caret > (uint32_t)LC_WIN ? g_caret - (uint32_t)LC_WIN : 0;
    uint32_t to   = (g_caret + (uint32_t)LC_WIN < L) ? g_caret + (uint32_t)LC_WIN : L;
    char w[2 * LC_WIN + 1]; uint32_t wg = to - from;
    if (wg) df_doc_get_text(g_doc, from, wg, w, sizeof w);
    uint32_t co = g_caret - from;
    uint32_t lso = co; while (lso > 0 && w[lso - 1] != '\n') lso--;
    uint32_t leo = co; while (leo < wg && w[leo] != '\n') leo++;
    if ((lso == 0 && from != 0) || (leo == wg && to != L)) return false;
    uint32_t ls = from + lso;

    char buf[128]; int bn = 0;
    for (uint32_t o = lso; o < leo && bn < (int)sizeof buf - 1; o++) buf[bn++] = w[o];
    lst_mark m; if (!lst_parse(buf, bn, &m)) return false;

    lst_mark pm; bool hasp = false; char pbuf[128];
    if (lso > 0) {                                    /* previous line is in the window */
        uint32_t pleo = lso - 1, plso = pleo;
        while (plso > 0 && w[plso - 1] != '\n') plso--;
        int pbn = 0;
        for (uint32_t o = plso; o < pleo && pbn < (int)sizeof pbuf - 1; o++) pbuf[pbn++] = w[o];
        hasp = lst_parse(pbuf, pbn, &pm);
        if (hasp) lst_classify(pbuf, &pm, NULL, NULL);
    }
    lst_classify(buf, &m, hasp ? &pm : NULL, pbuf);
    if (m.t == LT_NONE) return false;

    /* marker-only line (no content) -> terminate the list */
    bool empty = true;
    for (uint32_t o = lso + (uint32_t)m.mlen; o < leo; o++) { char c = w[o]; if (c != ' ' && c != '\t') { empty = false; break; } }
    uint32_t le = from + leo;
    if (empty) {
        df_doc_delete(g_doc, ls, le - ls);
        g_caret = ls; g_anchor = -1;
        after_edit(ls, le - ls, 0);
        g_pp = PP_NONE; g_list_undo = false;
        return true;
    }

    char nm[32]; int nl = lst_next(&m, nm);
    if (nl <= 0) return false;                       /* z / roman>3999 -> plain newline */

    uint32_t caret = g_caret;
    bool bullet = (m.t == LT_STAR);
    if (bullet) {                                    /* recognised as a bullet list: * -> bullet */
        uint32_t star = ls + (uint32_t)m.indent;
        df_doc_delete(g_doc, star, 1);
        df_doc_insert(g_doc, star, "\xE2\x80\xA2", 3);
        caret += 2;
    }
    char ins[160]; int in = 0; ins[in++] = '\n';
    for (int k = 0; k < m.indent && in < (int)sizeof ins - nl - 2; k++) ins[in++] = buf[k];
    for (int k = 0; k < nl; k++) ins[in++] = nm[k];
    df_doc_insert(g_doc, caret, ins, (uint32_t)in);

    g_subst_pos = caret + 1;                          /* just after the '\n'            */
    g_subst_from_n = 0;
    g_subst_to_n = in - 1;                            /* indent + marker + space        */
    g_list_undo = true;
    g_caret = g_subst_pos + (uint32_t)g_subst_to_n;
    g_anchor = -1;
    if (bullet) {                                    /* two edits -> one full relayout */
        df_layout_rebuild(g_L); dp_relayout(g_eng);
        ensure_caret_visible(); update_toggles_from_caret(); recount();
    } else {                                         /* single insert -> incremental, like a normal Enter */
        after_edit(caret, 0, (uint32_t)in);
    }
    g_pp = PP_SUBST; position_subst_popup(); paint();
    list_renumber();
    return true;
}

static void apply_cf_bit(uint16_t bit, int value)
{
    if (!has_sel()) { dobpopup_Info("DobWrite", "Seleziona del testo da formattare."); return; }
    uint32_t a = sel_lo(), b = sel_hi();
    CharFmt cf; memset(&cf, 0, sizeof cf); cf.mask = bit;
    if (bit == DD_CF_BOLD) cf.bold = (uint8_t)value;
    else if (bit == DD_CF_ITALIC) cf.italic = (uint8_t)value;
    else if (bit == DD_CF_UNDERLINE) cf.underline = (uint8_t)value;
    else if (bit == DD_CF_STRIKE) cf.strike = (uint8_t)value;
    df_doc_apply_char_fmt(g_doc, a, b - a, &cf);
    after_format(a, b - a);
}

static void toggle_cf(uint16_t bit)
{
    if (!has_sel()) { dobpopup_Info("DobWrite", "Seleziona del testo da formattare."); return; }
    CharFmt cf; df_doc_char_fmt_at(g_doc, sel_lo(), &cf);
    int cur = (bit == DD_CF_BOLD) ? cf.bold : (bit == DD_CF_ITALIC) ? cf.italic :
              (bit == DD_CF_UNDERLINE) ? cf.underline : cf.strike;
    apply_cf_bit(bit, cur ? 0 : 1);
}

static void apply_size(int pts)
{
    if (!has_sel()) { dobpopup_Info("DobWrite", "Seleziona del testo per cambiare la dimensione."); return; }
    uint32_t a = sel_lo(), b = sel_hi();
    CharFmt cf; memset(&cf, 0, sizeof cf); cf.mask = DD_CF_SIZE; cf.size_twips = (uint32_t)(pts * 20);
    df_doc_apply_char_fmt(g_doc, a, b - a, &cf);
    after_format(a, b - a);
}

/* Paragraph alignment formats whole paragraphs, so it works on a bare caret
 * (no selection needed). We reflow the exact paragraph span the model touched
 * -- same span math as do_set_para -- so the lines re-position immediately. */
static void reflow_para_span(uint32_t pos, uint32_t n)
{
    uint32_t p0 = df_doc_para_at(g_doc, pos);
    uint32_t p1 = df_doc_para_at(g_doc, n ? pos + n - 1 : pos);
    uint32_t s0 = 0, l0 = 0, s1 = 0, l1 = 0;
    df_doc_para_range(g_doc, p0, &s0, &l0);
    df_doc_para_range(g_doc, p1, &s1, &l1);
    uint32_t span = (s1 + l1) - s0;
    after_format(s0, span);
}

/* ---- contextual popup: positioning, drawing, and actions ---- */

static void position_sel_popup(void)
{
    static const char *lbl[6] = { "B", "I", "U", "S", "A+", "A-" };
    int bw = 26, bh = 24, pad = 4, gap = 2;
    g_pp_w = pad * 2 + 6 * bw + 5 * gap;
    g_pp_h = pad * 2 + bh;
    dl_locus loc;
    if (!df_layout_locate(g_L, sel_lo(), &loc)) { g_pp = PP_NONE; return; }
    int wx, wy; dp_content_to_window(g_eng, loc.x, loc.y_top, &wx, &wy);
    wy += RIBBON_H;
    int px = wx + 6;                              /* east of the glyph           */
    int py = wy - g_pp_h - 4;                     /* north of the glyph          */
    if (py < RIBBON_H + 2) py = wy + 20;          /* flip below if no room above */
    if (px + g_pp_w > win_w - 2) px = win_w - g_pp_w - 2;
    if (px < 2) px = 2;
    g_pp_x = px; g_pp_y = py;
    for (int i = 0; i < 6; i++)
        dobbtn_Init(&g_pp_btn[i], win, px + pad + i * (bw + gap), py + pad, bw, bh, lbl[i]);
}

static void position_subst_popup(void)
{
    int bw = 24, bh = 22, pad = 3;
    g_pp_w = pad * 2 + bw; g_pp_h = pad * 2 + bh;
    dl_locus loc;
    if (!df_layout_locate(g_L, g_subst_pos, &loc)) { g_pp = PP_NONE; return; }
    int wx, wy; dp_content_to_window(g_eng, loc.x, loc.y_top, &wx, &wy);
    wy += RIBBON_H;
    int px = wx + 6, py = wy - g_pp_h - 4;
    if (py < RIBBON_H + 2) py = wy + 20;
    if (px + g_pp_w > win_w - 2) px = win_w - g_pp_w - 2;
    if (px < 2) px = 2;
    g_pp_x = px; g_pp_y = py;
    dobbtn_Init(&g_pp_undo, win, px + pad, py + pad, bw, bh, "X");
    g_pp_undo.col_bg = 0x00D03030u;               /* red undo button */
}

static void draw_popup(void)
{
    if (g_pp == PP_NONE) return;
    dobui_FillRect(win, g_pp_x, g_pp_y, g_pp_w, g_pp_h, 0x00ECECECu);          /* face   */
    dobui_FillRect(win, g_pp_x, g_pp_y, g_pp_w, 1, 0x00808080u);              /* border */
    dobui_FillRect(win, g_pp_x, g_pp_y + g_pp_h - 1, g_pp_w, 1, 0x00808080u);
    dobui_FillRect(win, g_pp_x, g_pp_y, 1, g_pp_h, 0x00808080u);
    dobui_FillRect(win, g_pp_x + g_pp_w - 1, g_pp_y, 1, g_pp_h, 0x00808080u);
    if (g_pp == PP_SEL) { for (int i = 0; i < 6; i++) dobbtn_Draw(&g_pp_btn[i]); }
    else                { dobbtn_Draw(&g_pp_undo); }
}

static void bump_size(int dir)
{
    if (!has_sel()) return;
    int n  = (int)(sizeof g_size_pt / sizeof g_size_pt[0]);
    int ns = g_last_size + dir;
    if (ns < 0) ns = 0;
    if (ns >= n) ns = n - 1;
    if (ns != g_last_size) { g_last_size = ns; apply_size(g_size_pt[g_last_size]); }
}

/* reverse the last autocorrect swap: drop the inserted text, restore the original */
static void undo_substitution(void)
{
    uint32_t L0 = df_doc_length(g_doc);
    if (g_subst_pos + (uint32_t)g_subst_to_n > L0) { g_pp = PP_NONE; return; }
    df_doc_delete(g_doc, g_subst_pos, (uint32_t)g_subst_to_n);
    df_doc_insert(g_doc, g_subst_pos, g_subst_from, (uint32_t)g_subst_from_n);
    g_caret = g_subst_pos + (uint32_t)g_subst_from_n;
    g_anchor = -1;
    after_edit(g_subst_pos, (uint32_t)g_subst_to_n, (uint32_t)g_subst_from_n);
    g_pp = PP_NONE;
    g_list_undo = false;
    update_toggles_from_caret();
    paint();
}

static void apply_align(dd_align a)
{
    uint32_t pos, n;
    if (has_sel()) { pos = sel_lo(); n = sel_hi() - pos; }
    else           { pos = g_caret; n = 0; }
    ParaFmt pf; memset(&pf, 0, sizeof pf); pf.mask = DD_PF_ALIGN; pf.align = (uint8_t)a;
    df_doc_set_para_fmt(g_doc, pos, n, &pf);
    g_align = (uint8_t)a;
    reflow_para_span(pos, n);
}

/* Apply a paragraph-spacing preset (space after the paragraph) to the
 * selection, or to the paragraph under the caret. */
static void apply_para_spacing(int idx)
{
    uint32_t pos, n;
    if (has_sel()) { pos = sel_lo(); n = sel_hi() - pos; }
    else           { pos = g_caret; n = 0; }
    ParaFmt pf; memset(&pf, 0, sizeof pf);
    pf.mask = DD_PF_SPACE_AFTER; pf.space_after = g_para_sp_tw[idx];
    df_doc_set_para_fmt(g_doc, pos, n, &pf);
    g_para_sp_active = idx;
    reflow_para_span(pos, n);
}

/* Apply a line-spacing preset (interlinea) to each paragraph in the selection
 * (or the one under the caret). line_spacing is absolute, so it is derived from
 * each paragraph's own font size to avoid overlapping lines at large sizes. */
static void apply_line_spacing(int idx)
{
    float mult = g_para_ls_mult[idx];
    uint32_t pos, n;
    if (has_sel()) { pos = sel_lo(); n = sel_hi() - pos; }
    else           { pos = g_caret; n = 0; }
    uint32_t last = n ? (pos + n - 1) : pos;
    uint32_t p0 = df_doc_para_at(g_doc, pos), p1 = df_doc_para_at(g_doc, last);
    for (uint32_t pi = p0; pi <= p1; pi++) {
        uint32_t st, ln;
        if (df_doc_para_range(g_doc, pi, &st, &ln) != DD_OK) continue;
        ParaFmt pf; memset(&pf, 0, sizeof pf); pf.mask = DD_PF_LINE_SPACING;
        if (mult <= 1.0f) {
            pf.line_spacing = 0;                 /* natural single */
        } else {
            CharFmt cf; df_doc_char_fmt_at(g_doc, st, &cf);
            uint32_t size = cf.size_twips ? cf.size_twips : 220;
            pf.line_spacing = (uint32_t)(mult * 1.2f * (float)size + 0.5f);
        }
        df_doc_set_para_fmt(g_doc, st, 0, &pf);  /* n=0 -> just paragraph pi */
    }
    g_para_ls_active = idx;
    reflow_para_span(pos, n);
}

/* ---- caret navigation ---- */

/* colour-picker callback: remember the colour, refresh the button icon, and
 * (if there is a selection) apply it to the run as DD_CF_COLOR. */
static void on_pick_fontcolor(uint32_t rgb, void *ud)
{
    (void)ud;
    g_fontcolor = rgb & 0x00FFFFFFu;
    if (g_face) { make_fontcolor_icon(g_ico_fcolor, g_fontcolor); dobpbtn_SetImage(&g_fcolor, g_ico_fcolor, ICON_W, ICON_H); }
    if (has_sel()) {
        uint32_t a = sel_lo(), b = sel_hi();
        CharFmt cf; memset(&cf, 0, sizeof cf); cf.mask = DD_CF_COLOR; cf.color = 0xFF000000u | g_fontcolor;  /* opaque */
        df_doc_apply_char_fmt(g_doc, a, b - a, &cf);
        after_format(a, b - a);
    } else {
        paint();   /* nothing selected: just reflect the new colour on the button */
    }
}

static void open_fontcolor_picker(void)
{
    uint32_t cur = g_fontcolor;
    if (has_sel()) { CharFmt cf; df_doc_char_fmt_at(g_doc, sel_lo(), &cf); cur = cf.color & 0x00FFFFFFu; }
    colorpick_open(dobui_primary(), "Colore testo", cur, on_pick_fontcolor, NULL);
}

/* ---- Find & Replace: document side (the UI lives in findrepl.c) ---- */
static uint32_t doc_find_from(const char *needle, uint32_t from, bool ci)
{
    uint32_t L = df_doc_length(g_doc);
    int nl = 0; while (needle[nl]) nl++;
    if (nl == 0 || (uint32_t)nl > L) return (uint32_t)-1;
    for (uint32_t p = from; p + (uint32_t)nl <= L; p++) {
        int j = 0;
        for (; j < nl; j++) {
            char a = byte_at(p + (uint32_t)j), b = needle[j];
            if (ci ? (lower(a) != lower(b)) : (a != b)) break;
        }
        if (j == nl) return p;
    }
    return (uint32_t)-1;
}

/* (Re)build the match list for a needle. Non-overlapping, capped. */
static void fr_recompute(const char *needle, bool ci)
{
    g_fr_nmatch = 0; g_fr_cur = -1; g_fr_active = false; g_fr_nlen = 0;
    g_fr_needle[0] = 0;
    if (!needle || !needle[0]) return;
    int i = 0; for (; needle[i] && i < (int)sizeof g_fr_needle - 1; i++) g_fr_needle[i] = needle[i];
    g_fr_needle[i] = 0;
    g_fr_ci = ci;
    int nl = 0; while (needle[nl]) nl++;
    g_fr_nlen = nl;
    uint32_t p = 0;
    while (g_fr_nmatch < FR_MAX_MATCHES) {
        uint32_t m = doc_find_from(needle, p, ci);
        if (m == (uint32_t)-1) break;
        g_fr_matches[g_fr_nmatch++] = m;
        p = m + (uint32_t)nl;             /* non-overlapping */
    }
    g_fr_active = (g_fr_nmatch > 0);
}

/* ---- Find & Replace: host side, called by findrepl.c ----
 *
 * Each of these runs from the dialog's event handler, where the active drawing
 * context is the DIALOG. They switch it to the main window, do the work, and
 * repaint; findrepl.c switches the context back to itself afterwards. */

void findrepl_host_find_next(const char *needle, bool ci)
{
    if (!needle || !needle[0]) return;
    dobui_SetActiveWindow(win);                          /* draw the MAIN window */
    bool changed = (!g_fr_active || ci != g_fr_ci || strcmp(needle, g_fr_needle) != 0);
    if (changed) {
        fr_recompute(needle, ci);
        if (g_fr_nmatch == 0) { paint(); return; }       /* not found: clear any old highlights */
        g_fr_cur = 0;                                    /* first match at/after the caret */
        for (int i = 0; i < g_fr_nmatch; i++)
            if (g_fr_matches[i] >= g_caret) { g_fr_cur = i; break; }
    } else {
        g_fr_cur = (g_fr_cur + 1) % g_fr_nmatch;         /* advance, wrapping around */
    }
    uint32_t m = g_fr_matches[g_fr_cur];
    g_caret  = m + (uint32_t)g_fr_nlen;
    g_anchor = -1;                                        /* the cyan highlight marks it, not a selection */
    dl_locus loc;
    if (df_layout_locate(g_L, m, &loc)) dp_scroll_to_content(g_eng, loc.y_top, loc.y_bottom);
    update_toggles_from_caret();
    paint();
}

void findrepl_host_replace_all(const char *needle, const char *repl, bool ci)
{
    if (!needle || !needle[0]) return;
    dobui_SetActiveWindow(win);
    int nl = 0; while (needle[nl]) nl++;
    int rl = 0; if (repl) while (repl[rl]) rl++;
    uint32_t p = 0; int count = 0;
    for (;;) {
        uint32_t m = doc_find_from(needle, p, ci);
        if (m == (uint32_t)-1) break;
        df_doc_delete(g_doc, m, (uint32_t)nl);
        if (rl > 0) df_doc_insert(g_doc, m, repl, (uint32_t)rl);
        p = m + (uint32_t)rl;             /* skip past the replacement */
        count++;
    }
    g_fr_active = false; g_fr_nmatch = 0; g_fr_cur = -1;  /* positions are stale after editing */
    uint32_t L = df_doc_length(g_doc);
    if (g_caret > L) g_caret = L;
    g_anchor = -1;
    df_layout_rebuild(g_L);
    dp_relayout(g_eng);
    update_toggles_from_caret();
    paint();
}

void findrepl_host_closed(void)
{
    g_fr_active = false; g_fr_nmatch = 0; g_fr_cur = -1;
    dobui_SetActiveWindow(win);
    paint();
}

int findrepl_host_count(void) { return g_fr_nmatch; }

/* "remove highlight" button glyph: an empty marker band crossed by a red slash */
static void make_hlclear_icon(uint32_t *buf)
{
    for (int y = 0; y < ICON_H; y++)
        for (int x = 0; x < ICON_W; x++) {
            uint32_t c = 0xFFFFFFFFu;
            if (y >= 5 && y <= ICON_H - 6 && x >= 3 && x < ICON_W - 3) c = 0xFFE8E8E8u;  /* empty band */
            buf[y * ICON_W + x] = c;
        }
    for (int y = 2; y < ICON_H - 2; y++) {            /* red diagonal slash */
        int x = 2 + (y - 2) * (ICON_W - 4) / (ICON_H - 4);
        if (x >= 0 && x < ICON_W)     buf[y * ICON_W + x] = 0xFFD02020u;
        if (x + 1 < ICON_W)           buf[y * ICON_W + x + 1] = 0xFFD02020u;
    }
}

/* Clear the highlight on the current selection (DD_CF_HIGHLIGHT = none). */
static void clear_highlight(void)
{
    if (!has_sel()) return;
    uint32_t a = sel_lo(), b = sel_hi();
    CharFmt cf; memset(&cf, 0, sizeof cf); cf.mask = DD_CF_HIGHLIGHT; cf.highlight = 0;
    df_doc_apply_char_fmt(g_doc, a, b - a, &cf);
    after_format(a, b - a);
}

static void on_pick_pagebg(uint32_t rgb, void *ud)
{
    (void)ud;
    g_pagebg = rgb & 0x00FFFFFFu;
    dp_set_page_bg(g_eng, 0xFF000000u | g_pagebg);   /* opaque paper */
    { PageSetup ps; df_doc_get_page(g_doc, &ps); ps.bg_color = 0xFF000000u | g_pagebg; df_doc_set_page(g_doc, &ps); }
    make_pagebg_icon(g_ico_pagebg, g_pagebg);
    dobpbtn_SetImage(&g_pagebg_btn, g_ico_pagebg, ICON_W, ICON_H);
    paint();
}

static void open_pagebg_picker(void)
{
    colorpick_open(dobui_primary(), "Sfondo foglio", g_pagebg, on_pick_pagebg, NULL);
}

static void on_pick_highlight(uint32_t rgb, void *ud)
{
    (void)ud;
    g_highlight = rgb & 0x00FFFFFFu;
    if (g_face) { make_highlight_icon(g_ico_hl, g_highlight); dobpbtn_SetImage(&g_hl_btn, g_ico_hl, ICON_W, ICON_H); }
    if (has_sel()) {
        uint32_t a = sel_lo(), b = sel_hi();
        CharFmt cf; memset(&cf, 0, sizeof cf); cf.mask = DD_CF_HIGHLIGHT; cf.highlight = 0xFF000000u | g_highlight;  /* opaque */
        df_doc_apply_char_fmt(g_doc, a, b - a, &cf);
        after_format(a, b - a);
    } else {
        paint();
    }
}

static void open_highlight_picker(void)
{
    uint32_t seed = g_highlight;
    if (has_sel()) {
        CharFmt cf; df_doc_char_fmt_at(g_doc, sel_lo(), &cf);
        if ((cf.highlight >> 24) != 0) seed = cf.highlight & 0x00FFFFFFu;
    }
    colorpick_open(dobui_primary(), "Sfondo riga (evidenziatore)", seed, on_pick_highlight, NULL);
}

static void pre_move(void) { if (g_mods & DOBUI_MOD_SHIFT) { if (g_anchor < 0) g_anchor = (int32_t)g_caret; } else g_anchor = -1; }

static void move_h(int dir)
{
    pre_move();
    g_caret = dir < 0 ? df_doc_prev_cp(g_doc, g_caret) : df_doc_next_cp(g_doc, g_caret);
    ensure_caret_visible(); update_toggles_from_caret(); paint();
}

static void move_v(int dir)
{
    pre_move();
    dl_locus loc;
    if (!df_layout_locate(g_L, g_caret, &loc)) return;
    float cx = loc.x;
    float cy = dir < 0 ? loc.y_top - 1.0f : loc.y_bottom + 1.0f;
    g_caret = df_layout_hit(g_L, cx, cy);
    ensure_caret_visible(); update_toggles_from_caret(); paint();
}

/* ---- file ---- */

/* Window title = bare file name: no directory, no .dobw extension
 * (/DATA/Documents/foglio.dobw -> "foglio"). */
static void set_doc_title_from_path(const char *path)
{
    const char *b = path;
    for (const char *q = path; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
    char name[128]; size_t i = 0;
    for (; b[i] && i + 1 < sizeof name; i++) name[i] = b[i];
    name[i] = '\0';
    if (i >= 5) { char *e = name + i - 5;   /* strip a trailing ".dobw" (any case) */
        if (e[0] == '.' && (e[1]|32) == 'd' && (e[2]|32) == 'o' && (e[3]|32) == 'b' && (e[4]|32) == 'w') e[0] = '\0'; }
    dobui_SetTitle(win, name[0] ? name : "Senza nome");
}

/* Serialize g_doc and write it to `path`. On failure reports a modal error
 * unless `silent` (used by autosave, which must not interrupt typing). */
static bool write_doc_to(const char *path, bool silent)
{
    uint8_t *buf = NULL; uint32_t sz = 0;
    if (df_doc_serialize(g_doc, &buf, &sz) != DD_OK) { if (!silent) dobpopup_Error("DobWrite", "Serializzazione fallita."); return false; }
    int fd = dobfs_Open(path, FS_WRITE | FS_CREATE | FS_TRUNC);
    if (fd < 0) { free(buf); if (!silent) dobpopup_Error("DobWrite", "Apertura in scrittura fallita."); return false; }
    uint32_t off = 0; bool ok = true;
    while (off < sz) { int w = dobfs_Write(fd, (const char *)(buf + off), sz - off); if (w <= 0) { ok = false; break; } off += (uint32_t)w; }
    dobfs_Close(fd); free(buf);
    if (!ok) { if (!silent) dobpopup_Error("DobWrite", "Errore di scrittura."); return false; }
    return true;
}

/* Salva: write to the document's own file. The first save (no bound file yet)
 * asks where to put it, then binds the document to that file; afterwards Salva
 * overwrites it silently with no dialog. */
static void do_save(void)
{
    char path[256];
    if (g_saved) { strncpy(path, g_path, sizeof path - 1); path[sizeof path - 1] = 0; }
    else if (dobfiles_PickSavePath("documento.dobw", ".dobw", "/DATA/Documents", path, sizeof path) != 0) return;
    if (!write_doc_to(path, false)) return;
    strncpy(g_path, path, sizeof g_path - 1); g_path[sizeof g_path - 1] = 0;
    g_saved = true; set_doc_title_from_path(g_path);
    tick_stop(); g_note = NOTE_SAVED; g_as_left = 0;
    paint();   /* the WM resets the window cmdlist on every Invalidate, so a
                * status-bar-only redraw would blank the rest of the window --
                * the whole frame must be re-emitted. */
}

/* Salva copia: write a copy to a chosen path WITHOUT rebinding the document --
 * g_path and the autosave target stay on the original file. */
static void do_save_copy(void)
{
    char path[256];
    if (dobfiles_PickSavePath("copia.dobw", ".dobw", "/DATA/Documents", path, sizeof path) != 0) return;
    if (!write_doc_to(path, false)) return;
    dobpopup_Info("DobWrite", "Copia salvata.");
}

/* Autosave. Fires ~1x/second only while a countdown is pending; when it reaches
 * zero it writes once to the bound file and stops until the next edit. */
void event_tick(void)
{
    bool changed = false;

    /* Conteggio parole differito (vedi after_edit): al massimo una
     * scansione del documento al secondo, non una per tasto. */
    if (g_count_dirty) { recount(); g_count_dirty = false; changed = true; }

    if (g_note == NOTE_PENDING) {
        if (g_as_left > 0) g_as_left--;
        if (g_as_left <= 0)
            g_note = (g_saved && write_doc_to(g_path, true)) ? NOTE_AUTOSAVED
                                                             : NOTE_UNSAVED;
        changed = true;
    }
    /* Il tick resta armato finche' autosave O conteggio hanno lavoro. */
    if (g_note != NOTE_PENDING && !g_count_dirty) tick_stop();
    if (changed) {
        g_band_y0 = g_band_y1 = 0;   /* contenuto invariato: solo chrome */
        paint();   /* full frame: an Invalidate resets the window cmdlist, so
                    * drawing only the status bar here would white out the
                    * document + ribbon on every one-second countdown tick. */
    }
}

/* ---- font registry wiring ---- */
/* Shared, sandbox-free area: everything from /common_files on is readable by
 * any program, so the fonts directory lives under it. (DOBFS_COMMON_FONTS is
 * defined by DobFileSystem.h -- the common-files SpecialDirectories API.) */
#define FONTS_DIR  DOBFS_COMMON_FONTS

/* Read one font file from the fonts dir and register it. The blob is handed
 * to the registry (df_open keeps a pointer into it for the face's life). */
static void load_font_file(const char *fname)
{
    const char *ext = NULL;
    for (const char *p = fname; *p; p++) if (*p == '.') ext = p;
    if (!ext) return;
    /* Case-insensitive: a FAT 8.3 short name may report the extension as ".TTF". */
    bool font_ext = false;
    {
        char e[8]; int k = 0;
        for (const char *p = ext; *p && k < 7; p++) { char c = *p; if (c >= 'A' && c <= 'Z') c += 32; e[k++] = c; }
        e[k] = 0;
        font_ext = !strcmp(e, ".ttf") || !strcmp(e, ".otf") || !strcmp(e, ".dmf");
    }
    if (!font_ext) return;

    char path[320];
    snprintf(path, sizeof path, "%s/%s", FONTS_DIR, fname);
    int fd = dobfs_Open(path, FS_READ);
    if (fd < 0) return;

    uint8_t *buf = NULL; uint32_t len = 0, cap = 0; bool ok = true;
    for (;;) {
        if (len + LOAD_CHUNK > cap) { uint32_t nc = cap ? cap * 2 : LOAD_CHUNK * 2;
            uint8_t *nb = (uint8_t *)realloc(buf, nc); if (!nb) { ok = false; break; } buf = nb; cap = nc; }
        int n = dobfs_Read(fd, buf + len, LOAD_CHUNK);
        if (n < 0) { ok = false; break; } if (n == 0) break; len += (uint32_t)n;
    }
    dobfs_Close(fd);
    if (!ok || len == 0) { free(buf); return; }

    char name[FONTREG_NAME];
    size_t k = (size_t)(ext - fname); if (k >= sizeof name) k = sizeof name - 1;
    memcpy(name, fname, k); name[k] = 0;

    if (fontreg_add(&g_freg, name, buf, len, true) < 0) free(buf);  /* dup/invalid: drop */
}

static void load_fonts_from_dir(void)
{
    dobfs_Mkdir(DOBFS_COMMON_FILES);        /* parent first: Mkdir is not recursive,
                                             * and on a system where nothing created
                                             * common_files yet the nested call below
                                             * would fail, then List, then the scan —
                                             * leaving only the built-in font */
    dobfs_Mkdir(FONTS_DIR);                 /* ensure it exists (ignored if present) */
    dobfs_dirent_t ents[FONTREG_MAX];
    uint32_t cnt = 0;
    if (dobfs_List(FONTS_DIR, ents, FONTREG_MAX, &cnt) != 0) return;
    if (cnt > FONTREG_MAX) cnt = FONTREG_MAX;
    for (uint32_t i = 0; i < cnt; i++)
        if (ents[i].type == FS_TYPE_FILE) load_font_file(ents[i].name);
}

/* Register the builtin default + any installed files, then build the fontset
 * and the dropdown label list. False only if no usable font at all. */
/* Build a DMF1 monobit font from the shared 8x16 system bitmap font, so the
 * system font is a selectable document font. The system ships per-glyph ink
 * metrics (dob_glyph_l/w) for proportional spacing; we use them so the font is
 * NOT rendered fixed-width: each glyph's advance is its inked width plus a small
 * tracking (a fixed width for blanks), and each glyph's bitmap is left-aligned
 * (ink shifted to column 0) so, with the backend's zero left bearing, the ink
 * starts at the pen. Layout: 20-byte header + 256-byte advance table + 256
 * single-byte-stride 16-row bitmaps. */
static uint8_t g_sysfont_dmf[20 + DOB_FONT_COUNT + DOB_FONT_COUNT * DOB_FONT_H];

static void build_system_font(void)
{
    uint8_t *p = g_sysfont_dmf;
    p[0] = 'D'; p[1] = 'M'; p[2] = 'F'; p[3] = '1';
#define SF_PUT16(o, v) do { p[o] = (uint8_t)((v) & 0xFF); p[(o) + 1] = (uint8_t)(((v) >> 8) & 0xFF); } while (0)
    SF_PUT16(4,  DOB_FONT_W);        /* cell_w  = 8                              */
    SF_PUT16(6,  DOB_FONT_H);        /* cell_h  = 16 (== em)                     */
    SF_PUT16(8,  0);                 /* first_cp = 0 (Latin-1 byte == codepoint) */
    SF_PUT16(10, DOB_FONT_COUNT);    /* count   = 256                            */
    SF_PUT16(12, 13);                /* ascent  (baseline 13px below cell top)   */
    SF_PUT16(14, 1);                 /* flags   bit0: per-glyph advance follows  */
    SF_PUT16(16, 0);                 /* fixed_adv unused when proportional       */
    SF_PUT16(18, 0);                 /* reserved                                 */
#undef SF_PUT16
    /* per-glyph advance table (inked width + tracking, or a fixed blank width) */
    uint8_t *adv = p + 20;
    for (int g = 0; g < DOB_FONT_COUNT; g++)
        adv[g] = (uint8_t)dob_font_advance((uint8_t)g);
    /* bitmaps, ink left-aligned: shift each row left by the glyph's first inked
     * column so the ink begins at column 0. Bits left of the ink are blank, so
     * nothing is lost; with the proportional advance above this yields correct
     * inter-glyph spacing. */
    uint8_t *bm = p + 20 + DOB_FONT_COUNT;
    for (int g = 0; g < DOB_FONT_COUNT; g++) {
        int l = dob_glyph_l[(uint8_t)g];
        for (int r = 0; r < DOB_FONT_H; r++)
            bm[g * DOB_FONT_H + r] = (uint8_t)(dob_font_data[(uint8_t)g][r] << l);
    }
}

static const char *g_default_family = "Sistema";   /* resolved in install_fonts */

static bool install_fonts(void)
{
    fontreg_init(&g_freg);
    build_system_font();
    fontreg_add(&g_freg, "Sistema", g_sysfont_dmf, sizeof g_sysfont_dmf, false);
    /* Times New Roman, Arial Narrow, ... are file fonts now (shipped in the
     * shared fonts area), picked up here -- no longer embedded in the binary. */
    load_fonts_from_dir();
    if (fontreg_count(&g_freg) == 0) return false;

    g_fonts = fontreg_build_fontset(&g_freg);
    if (!g_fonts) return false;

    /* Default family: prefer Times New Roman; if its file didn't load, fall back
     * to whatever registered first (Sistema is always present), so the document
     * default always resolves to a real face. */
    int di = fontreg_find(&g_freg, "Times New Roman");
    if (di < 0) di = 0;
    g_face = fontreg_face(&g_freg, di);
    g_default_family = fontreg_name(&g_freg, di);

    g_font_count = fontreg_count(&g_freg);
    if (g_font_count > FONTREG_MAX) g_font_count = FONTREG_MAX;
    for (int i = 0; i < g_font_count; i++) g_font_names[i] = fontreg_name(&g_freg, i);
    return true;
}

/* Apply a font choice: to the selection if any, else set the default family
 * so newly typed text uses it. */
static void apply_font(int idx)
{
    const char *name = fontreg_name(&g_freg, idx);
    if (!name) return;
    uint16_t fam = df_doc_family_intern(g_doc, name);
    df_face *f = fontreg_face(&g_freg, idx);
    if (f) g_face = f;                       /* preview glyphs use the current face */

    if (has_sel()) {
        uint32_t a = sel_lo(), b = sel_hi();
        CharFmt cf; memset(&cf, 0, sizeof cf); cf.mask = DD_CF_FAMILY; cf.family_id = fam;
        df_doc_apply_char_fmt(g_doc, a, b - a, &cf);
        after_format(a, b - a);
    } else {
        /* No selection: leave existing text alone. Arm the font as a pending
         * format so it applies to text typed next, from the caret forward. */
        g_pending.mask |= DD_CF_FAMILY;
        g_pending.family_id = fam;
        g_pending_on = true;
        g_pending_pos = g_caret;
    }
}

static void do_open(void)
{
    char path[256];
    if (dobfiles_PickFile(".dobw", "/DATA/Documents", path, sizeof path) != 0) return;
    int fd = dobfs_Open(path, FS_READ);
    if (fd < 0) { dobpopup_Error("DobWrite", "File non trovato."); return; }
    uint8_t *buf = NULL; uint32_t len = 0, cap = 0; bool ok = true;
    for (;;) {
        if (len + LOAD_CHUNK > cap) { uint32_t nc = cap ? cap * 2 : LOAD_CHUNK * 2;
            uint8_t *nb = (uint8_t *)realloc(buf, nc); if (!nb) { ok = false; break; } buf = nb; cap = nc; }
        int n = dobfs_Read(fd, (char *)(buf + len), LOAD_CHUNK);
        if (n < 0) { ok = false; break; } if (n == 0) break; len += (uint32_t)n;
    }
    dobfs_Close(fd);
    df_doc *nd = NULL;
    if (ok && df_doc_load(buf, len, &nd) == DD_OK && nd) {
        dp_destroy(g_eng); df_layout_destroy(g_L); df_doc_destroy(g_doc);
        g_doc = nd; g_caret = 0; g_anchor = -1;
        dl_opts o; o.dpi = layout_dpi(); g_L = df_layout_create(g_doc, g_fonts, &o); g_eng = dp_create(g_L);
        dp_set_viewport(g_eng, win_w, content_bottom() - RIBBON_H);
        { PageSetup ps; df_doc_get_page(g_doc, &ps);
          g_pagebg = ps.bg_color & 0x00FFFFFFu;
          dp_set_page_bg(g_eng, ps.bg_color ? ps.bg_color : 0xFFFFFFFFu);
          make_pagebg_icon(g_ico_pagebg, g_pagebg);
          dobpbtn_SetImage(&g_pagebg_btn, g_ico_pagebg, ICON_W, ICON_H); }
        strncpy(g_path, path, sizeof g_path - 1); g_path[sizeof g_path - 1] = 0;
        g_saved = true; tick_stop(); g_note = NOTE_NONE; g_as_left = 0;
        set_doc_title_from_path(g_path);
        update_toggles_from_caret(); recount(); paint();
    } else dobpopup_Error("DobWrite", "File .dobw non valido.");
    free(buf);
}

/* ---- events ---- */

void event_modchange(uint8_t mods) { g_mods = mods; }

static void event_key_inner(uint8_t key)
{
    if (dd_open()) {
        /* The open dropdown owns the keyboard: arrows move the cursor, Invio
         * commits, Esc cancels. dobdd_OnKey only acts on a focused control and
         * treats '\n' (not CR) as commit, so arm focus around the call and
         * normalize Enter. */
        dob_dropdown_t *dd = g_font_dd.open ? &g_font_dd : &g_size_dd;
        if (key == 13) key = '\n';
        int prev = dd->selected;
        dd->focused = true;
        bool handled = dobdd_OnKey(dd, key);
        dd->focused = false;
        if (handled) {
            if (!dd->open && dd->selected != prev) {   /* committed a new choice */
                if (dd == &g_font_dd) { g_last_font = dd->selected; apply_font(g_last_font); }
                else                  { g_last_size = dd->selected; apply_size(g_size_pt[g_last_size]); }
            }
            paint();
        }
        return;
    }
    if (g_mods & DOBUI_MOD_CTRL) {
        char c = (key >= 1 && key <= 26) ? (char)('a' + key - 1) : lower((char)key);
        switch (c) {
            case 'b': toggle_cf(DD_CF_BOLD); break;
            case 'i': toggle_cf(DD_CF_ITALIC); break;
            case 'u': toggle_cf(DD_CF_UNDERLINE); break;
            case 'c': do_copy(); break;
            case 'x': do_cut(); break;
            case 'v': do_paste(); break;
            case 's': if (g_mods & DOBUI_MOD_SHIFT) do_save_copy(); else do_save(); break;
            case 'o': do_open(); break;
            case 'l': apply_align(DD_ALIGN_LEFT); break;
            case 'e': apply_align(DD_ALIGN_CENTER); break;
            case 'r': apply_align(DD_ALIGN_RIGHT); break;
            case 'j': apply_align(DD_ALIGN_JUSTIFY); break;
            case 'z': if (df_doc_undo(g_doc) == DD_OK) { uint32_t L0 = df_doc_length(g_doc); if (g_caret > L0) g_caret = L0; g_anchor = -1; df_layout_rebuild(g_L); dp_relayout(g_eng); update_toggles_from_caret(); paint(); } break;
            case 'y': if (df_doc_redo(g_doc) == DD_OK) { uint32_t L0 = df_doc_length(g_doc); if (g_caret > L0) g_caret = L0; g_anchor = -1; df_layout_rebuild(g_L); dp_relayout(g_eng); update_toggles_from_caret(); paint(); } break;
            case 'a': g_anchor = 0; g_caret = df_doc_length(g_doc); update_toggles_from_caret(); paint(); break;
            default: break;
        }
        return;
    }
    if (key >= 128) {
        switch (key) {
            case KEY_LEFT:  move_h(-1); break;
            case KEY_RIGHT: move_h(+1); break;
            case KEY_UP:    move_v(-1); break;
            case KEY_DOWN:  move_v(+1); break;
            case KEY_HOME:  { pre_move(); dl_locus l; if (df_layout_locate(g_L, g_caret, &l)) { const dl_para *p = df_layout_para(g_L, l.para); g_caret = p->lines[l.line].byte_start; } ensure_caret_visible(); update_toggles_from_caret(); paint(); } break;
            case KEY_END:   { pre_move(); dl_locus l; if (df_layout_locate(g_L, g_caret, &l)) { const dl_para *p = df_layout_para(g_L, l.para); uint32_t e = p->lines[l.line].byte_start + p->lines[l.line].byte_len; if (e > 0 && byte_at(e - 1) == '\n') e--; g_caret = e; } ensure_caret_visible(); update_toggles_from_caret(); paint(); } break;
            case KEY_DELETE: forward_delete(); break;
            case KEY_PGUP:  dp_scroll_by(g_eng, -(content_bottom() - RIBBON_H) * 3 / 4); paint(); break;
            case KEY_PGDN:  dp_scroll_by(g_eng,  (content_bottom() - RIBBON_H) * 3 / 4); paint(); break;
            default:
                /* The KEY_* navigation codes occupy 128..136; a byte at 0xA0 or
                 * above is a printable Latin-1 character from the keyboard layout
                 * (accented letters a/e/i/o/u, degrees, section sign, ...). The
                 * document is UTF-8, so encode the Latin-1 codepoint (== the byte
                 * value) to its two-byte UTF-8 form before inserting: valid in the
                 * file and clipboard, counted correctly, and matching the UTF-8
                 * autocorrect entries (e.g. perche` -> perche'). */
                if (key >= 0xA0) {
                    char u[2] = { (char)(0xC0 | (key >> 6)), (char)(0x80 | (key & 0x3F)) };
                    insert_text(u, 2);
                    apply_autocorrect();
                }
                break;
        }
        return;
    }
    if (key == 8 || key == 127) { backspace(); return; }
    if (key == 13 || key == 10) { if (!list_continue()) insert_text("\n", 1); return; }
    if (key == 9)               { insert_text("    ", 4); return; }
    if (key < 32 || key > 126)  return;
    char ch = (char)key; insert_text(&ch, 1); apply_autocorrect();
}

/* Public key entry point: run the editor's handling, then reconcile the
 * contextual popups. Any keystroke clears a lingering "undo" popup (an
 * autocorrect within this keystroke may re-show it); a live selection shows
 * the formatting toolbar; otherwise nothing. The inner handler already
 * painted, so repaint again only when a popup is actually involved (keeps
 * plain typing single-paint). */
void event_key(uint8_t key)
{
    int pp0 = g_pp;
    /* Backspace right after an auto-inserted list marker removes it (ends the
     * list), mirroring the popup's red X. */
    if ((key == 8 || key == 127) && pp0 == PP_SUBST && g_list_undo) {
        undo_substitution();          /* clears g_pp + g_list_undo and repaints */
        return;
    }
    if (g_pp == PP_SUBST) g_pp = PP_NONE;
    g_list_undo = false;              /* any other key commits the continuation */
    event_key_inner(key);
    if (g_pp != PP_SUBST && !dd_open()) {   /* an open dropdown keeps the toolbar hidden */
        if (has_sel()) { g_pp = PP_SEL; position_sel_popup(); }
        else g_pp = PP_NONE;
    }
    if (g_pp != PP_NONE || pp0 != PP_NONE) paint();
}

static void caret_from_window(int x, int y)
{
    float cx, cy;
    dp_window_to_content(g_eng, x, y - RIBBON_H, &cx, &cy);
    g_caret = df_layout_hit(g_L, cx, cy);
}

void event_mouseclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    /* status bar (bottom): zoom buttons; swallow any other click there. */
    if (y >= content_bottom()) {
        int mnx, plx, by, bw, bh;
        zoom_btn_rects(&mnx, &plx, &by, &bw, &bh);
        if (x >= mnx && x < mnx + bw && y >= by && y < by + bh) { apply_zoom(-1); return; }
        if (x >= plx && x < plx + bw && y >= by && y < by + bh) { apply_zoom(+1); return; }
        return;
    }
    /* columns chooser popup: clicks inside act on it; a click anywhere else
     * dismisses it (and is swallowed, standard popup behaviour). */
    if (g_cols_open) {
        if (x >= COLP_X && x < COLP_X + COLP_W && y >= COLP_Y && y < COLP_Y + COLP_H) {
            for (int i = 0; i < 6; i++)
                if (dobbtn_OnClick(&g_cols_pick[i], x, y)) { dobbtn_OnRelease(&g_cols_pick[i]); apply_columns(i + 1); return; }
            if (dobbtn_OnClick(&g_cols_gap_dec, x, y)) { dobbtn_OnRelease(&g_cols_gap_dec); bump_col_gap(-90); return; }
            if (dobbtn_OnClick(&g_cols_gap_inc, x, y)) { dobbtn_OnRelease(&g_cols_gap_inc); bump_col_gap(+90); return; }
            return;
        }
        g_cols_open = false; paint(); return;
    }
    /* An open Font/Size dropdown owns the next click: route it to the open
     * dropdown (pick an item, or dismiss it) so nothing beneath it -- the
     * contextual selection toolbar especially, which floats over the same
     * top-left area -- can steal the click. Always repaint afterwards: picking
     * a font/size with no selection arms a pending format without redrawing, so
     * without this the closed list would stay painted on screen. */
    if (dd_open()) {
        dob_dropdown_t *dd = g_font_dd.open ? &g_font_dd : &g_size_dd;
        dobdd_OnClick(dd, x, y);
        if      (g_size_dd.selected != g_last_size) { g_last_size = g_size_dd.selected; apply_size(g_size_pt[g_last_size]); }
        else if (g_font_dd.selected != g_last_font) { g_last_font = g_font_dd.selected; apply_font(g_last_font); }
        paint(); return;
    }
    /* a contextual popup takes clicks inside its rectangle; a click anywhere
     * else dismisses it and falls through to normal handling. */
    if (g_pp != PP_NONE) {
        if (x >= g_pp_x && x < g_pp_x + g_pp_w && y >= g_pp_y && y < g_pp_y + g_pp_h) {
            if (g_pp == PP_SEL) {
                if (dobbtn_OnClick(&g_pp_btn[0], x, y)) { toggle_cf(DD_CF_BOLD);      g_pp = PP_SEL; position_sel_popup(); paint(); return; }
                if (dobbtn_OnClick(&g_pp_btn[1], x, y)) { toggle_cf(DD_CF_ITALIC);    g_pp = PP_SEL; position_sel_popup(); paint(); return; }
                if (dobbtn_OnClick(&g_pp_btn[2], x, y)) { toggle_cf(DD_CF_UNDERLINE); g_pp = PP_SEL; position_sel_popup(); paint(); return; }
                if (dobbtn_OnClick(&g_pp_btn[3], x, y)) { toggle_cf(DD_CF_STRIKE);    g_pp = PP_SEL; position_sel_popup(); paint(); return; }
                if (dobbtn_OnClick(&g_pp_btn[4], x, y)) { bump_size(+1);              g_pp = PP_SEL; position_sel_popup(); paint(); return; }
                if (dobbtn_OnClick(&g_pp_btn[5], x, y)) { bump_size(-1);              g_pp = PP_SEL; position_sel_popup(); paint(); return; }
            } else if (dobbtn_OnClick(&g_pp_undo, x, y)) { undo_substitution(); return; }
            return;                       /* click in the popup chrome: swallow */
        }
        g_pp = PP_NONE;                   /* clicked outside: dismiss it        */
    }
    /* tab strip: switch the active tab */
    if (y < TAB_H) {
        for (int i = 0; i < NTABS; i++) {
            int tx = tabx(i), tw = tabw(i);
            if (x >= tx && x < tx + tw) {
                if (i != g_active_tab) { dobdd_Close(&g_font_dd); dobdd_Close(&g_size_dd); g_active_tab = i;
                    if (i != TAB_LAYOUT && g_margin_edit) { g_margin_edit = false; g_drag_guide = -1; dobui_SetCursor(win, CURSOR_DEFAULT); }
                    paint(); }
                return;
            }
        }
        return;
    }

    if (g_active_tab == TAB_HOME) {
        /* dropdowns first */
        bool consumed = dobdd_OnClick(&g_font_dd, x, y);
        if (!consumed) consumed = dobdd_OnClick(&g_size_dd, x, y);
        if (consumed) {
            if (g_size_dd.selected != g_last_size) { g_last_size = g_size_dd.selected; apply_size(g_size_pt[g_last_size]); return; }
            if (g_font_dd.selected != g_last_font) { g_last_font = g_font_dd.selected; apply_font(g_last_font); return; }
            if (dd_open()) g_pp = PP_NONE;   /* just opened the list: drop the contextual toolbar */
            paint(); return;
        }
        if (g_font_dd.open || g_size_dd.open) { dobdd_Close(&g_font_dd); dobdd_Close(&g_size_dd); paint(); return; }
        /* format buttons */
        if (dobpbtn_OnClick(&g_bold, x, y))   { toggle_cf(DD_CF_BOLD); return; }
        if (dobpbtn_OnClick(&g_italic, x, y)) { toggle_cf(DD_CF_ITALIC); return; }
        if (dobpbtn_OnClick(&g_under, x, y))  { toggle_cf(DD_CF_UNDERLINE); return; }
        if (dobpbtn_OnClick(&g_strike, x, y)) { toggle_cf(DD_CF_STRIKE); return; }
        if (dobpbtn_OnClick(&g_alignL, x, y)) { apply_align(DD_ALIGN_LEFT);    return; }
        if (dobpbtn_OnClick(&g_alignC, x, y)) { apply_align(DD_ALIGN_CENTER);  return; }
        if (dobpbtn_OnClick(&g_alignR, x, y)) { apply_align(DD_ALIGN_RIGHT);   return; }
        if (dobpbtn_OnClick(&g_alignJ, x, y)) { apply_align(DD_ALIGN_JUSTIFY); return; }
        if (dobpbtn_OnClick(&g_fcolor, x, y)) { open_fontcolor_picker(); return; }
        if (dobpbtn_OnClick(&g_pagebg_btn, x, y)) { open_pagebg_picker(); return; }
        if (dobpbtn_OnClick(&g_hl_btn, x, y)) { open_highlight_picker(); return; }
        if (dobpbtn_OnClick(&g_hlclear_btn, x, y)) { clear_highlight(); return; }
    } else if (g_active_tab == TAB_FILE) {
        if (dobpbtn_OnClick(&g_open, x, y))   { do_open(); return; }
        if (dobpbtn_OnClick(&g_save, x, y))   { do_save(); return; }
    } else if (g_active_tab == TAB_SUBST) {
        if (dobbtn_OnClick(&g_findbtn, x, y)) { dobbtn_OnRelease(&g_findbtn); findrepl_open(dobui_primary()); return; }
        if (dobbtn_OnClick(&g_subsbtn, x, y)) { dobbtn_OnRelease(&g_subsbtn); subsedit_open(dobui_primary()); return; }
    } else if (g_active_tab == TAB_PARA) {
        for (int i = 0; i < 3; i++)
            if (dobbtn_OnClick(&g_para_ls[i], x, y)) { dobbtn_OnRelease(&g_para_ls[i]); apply_line_spacing(i); return; }
        for (int i = 0; i < 4; i++)
            if (dobbtn_OnClick(&g_para_sp[i], x, y)) { dobbtn_OnRelease(&g_para_sp[i]); apply_para_spacing(i); return; }
    } else if (g_active_tab == TAB_LAYOUT) {
        if (dobbtn_OnClick(&g_layout_pagebtn, x, y)) {
            dobbtn_OnRelease(&g_layout_pagebtn);
            PageSetup ps; df_doc_section_page_at(g_doc, g_caret, &ps);
            pagesetup_open(dobui_primary(), &ps, on_pagesetup_apply, NULL);
            return;
        }
        if (dobbtn_OnClick(&g_margin_btn, x, y)) {
            dobbtn_OnRelease(&g_margin_btn);
            g_margin_edit = !g_margin_edit;
            if (g_margin_edit) df_doc_section_page_at(g_doc, g_caret, &g_marg_ps);
            else { g_drag_guide = -1; dobui_SetCursor(win, CURSOR_DEFAULT); }
            paint();
            return;
        }
        if (dobbtn_OnClick(&g_sectbreak_btn, x, y)) {
            dobbtn_OnRelease(&g_sectbreak_btn);
            if (df_doc_insert_section_break(g_doc, g_caret) == DD_OK) {
                df_layout_rebuild(g_L);
                dp_relayout(g_eng);
                ensure_caret_visible();
            }
            paint();
            return;
        }
        if (dobbtn_OnClick(&g_cols_btn, x, y)) {
            dobbtn_OnRelease(&g_cols_btn);
            g_cols_open = !g_cols_open;
            paint();
            return;
        }
    }

    /* page area: in margin-edit mode grab a construction line; else caret/selection */
    if (y >= RIBBON_H) {
        if (g_margin_edit && g_active_tab == TAB_LAYOUT) {
            int pidx = 0; int g = guide_hit(x, y, &pidx);
            if (g >= 0) { g_drag_guide = g; g_drag_pageidx = pidx; dobui_SetCursor(win, g <= 1 ? CURSOR_HSPLIT : CURSOR_VSPLIT); }
            return;
        }
        caret_from_window(x, y);
        if (g_mods & DOBUI_MOD_SHIFT) { if (g_anchor < 0) g_anchor = (int32_t)g_caret; }
        else g_anchor = (int32_t)g_caret;
        g_dragging = true;
        update_toggles_from_caret(); paint();
    }
}

/* ---- multi-click selection (word / paragraph / whole document), mirroring
 * the textbox widget: the 1st double-click selects the word under the cursor,
 * a 2nd quick double-click extends to the paragraph, a 3rd to everything. ---- */
static int dw_iabs(int v) { return v < 0 ? -v : v; }

/* A word byte: ASCII alnum / underscore, plus any byte >= 0x80 so accented
 * and multi-byte UTF-8 words (perché, città, ...) are selected whole. */
static bool dw_is_word(uint8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

/* Word/non-word run around `pos`, stopping at paragraph ends ('\n'). */
static void word_range_at(uint32_t pos, uint32_t *a_out, uint32_t *b_out)
{
    uint32_t len = df_doc_length(g_doc);
    if (pos > len) pos = len;
    bool wm = (pos < len) && dw_is_word((uint8_t)byte_at(pos));
    if (!wm && pos > 0 && dw_is_word((uint8_t)byte_at(pos - 1))) { wm = true; pos--; }
    uint32_t a = pos, b = pos;
    while (a > 0)   { uint8_t c = (uint8_t)byte_at(a - 1); if (c == '\n' || (dw_is_word(c) != wm)) break; a--; }
    while (b < len) { uint8_t c = (uint8_t)byte_at(b);     if (c == '\n' || (dw_is_word(c) != wm)) break; b++; }
    *a_out = a; *b_out = b;
}

void event_dblclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (y < RIBBON_H) return;                 /* page area only */

    /* successive-double-click counter (clock-based, like the textbox widget) */
    static uint32_t last_ms = 0;
    static int last_x = -1000, last_y = -1000, dcount = 0;
    uint32_t now = clock_ms();
    bool near = (now - last_ms) < DW_MULTICLICK_MS
             && dw_iabs(x - last_x) <= DW_MULTICLICK_SLOP
             && dw_iabs(y - last_y) <= DW_MULTICLICK_SLOP;
    dcount = near ? dcount + 1 : 1;
    last_ms = now; last_x = x; last_y = y;
    if (dcount > 3) dcount = 3;

    caret_from_window(x, y);                   /* resolve (x,y) -> g_caret byte */
    uint32_t hit = g_caret, a, b;

    if (dcount == 1) {                          /* word */
        word_range_at(hit, &a, &b);
    } else if (dcount == 2) {                   /* paragraph */
        uint32_t st, ln, idx = df_doc_para_at(g_doc, hit);
        if (df_doc_para_range(g_doc, idx, &st, &ln) == DD_OK) { a = st; b = st + ln; }
        else { a = hit; b = hit; }
    } else {                                    /* whole document */
        a = 0; b = df_doc_length(g_doc);
    }

    g_anchor   = (int32_t)a;
    g_caret    = b;
    g_dragging = false;                         /* a multi-select is not a drag */
    update_toggles_from_caret();
    paint();                                    /* leaves scroll put: select-all won't jump to the end */
}

/* Repaint during a selection drag / caret move. The WM rebuilds the window
 * from the cmdlist on every Invalidate (it does not retain pixels), so a
 * partial redraw would blank whatever it left out -- the earlier "sheets +
 * caret only" version dropped the status bar (and any open overlay) for the
 * duration of the drag. The frame is cheap enough to re-emit whole, so this is
 * just a full paint. */
static void repaint_content(void)
{
    paint();
}

void event_mousemove(int x, int y, uint8_t buttons)
{
    (void)buttons;
    /* An open Font/Size dropdown owns mouse-move while its scrollbar
     * thumb is being dragged. Feed it the move and repaint the popup;
     * either way a move over an open list never selects text. */
    if (dd_open()) {
        dob_dropdown_t *dd = g_font_dd.open ? &g_font_dd : &g_size_dd;
        if (dobdd_OnDrag(dd, x, y)) paint();
        return;
    }
    if (g_margin_edit && g_active_tab == TAB_LAYOUT) {
        if (g_drag_guide >= 0) { guide_drag_to(x, y); paint(); return; }
        int g = guide_hit(x, y, NULL);
        dobui_SetCursor(win, (g == 0 || g == 1) ? CURSOR_HSPLIT
                            : (g >= 2)          ? CURSOR_VSPLIT
                                                : CURSOR_DEFAULT);
        return;
    }
    if (!g_dragging) return;
    uint32_t old_caret = g_caret;
    caret_from_window(x, y);
    if (g_caret == old_caret) return;      /* stesso punto: niente da ridipingere */

    /* Banda del drag: cambia solo la tinta fra il vecchio e il nuovo
     * estremo della selezione (l'ancora e' ferma) — le righe da
     * ricostruire sono quelle fra le due posizioni del caret. Il resto
     * della selezione e' gia' tinto in g_view e resta com'e'. */
    dl_locus la, lb;
    if (df_layout_locate(g_L, old_caret, &la) &&
        df_layout_locate(g_L, g_caret,  &lb))
    {
        float cy0 = la.y_top    < lb.y_top    ? la.y_top    : lb.y_top;
        float cy1 = la.y_bottom > lb.y_bottom ? la.y_bottom : lb.y_bottom;
        int wx, wt, wb;
        dp_content_to_window(g_eng, 0, cy0, &wx, &wt);
        dp_content_to_window(g_eng, 0, cy1, &wx, &wb);
        g_band_y0 = wt - 2;
        g_band_y1 = wb + 2;
    }
    repaint_content();
}

void event_mouserelease(int x, int y, uint8_t buttons)
{
    (void)buttons;
    /* End any in-flight dropdown scrollbar drag. Harmless no-op when
     * none was armed. */
    dobdd_OnRelease(&g_font_dd);
    dobdd_OnRelease(&g_size_dd);
    if (g_drag_guide >= 0) {
        df_doc_set_section_page_at(g_doc, g_caret, &g_marg_ps);
        df_layout_rebuild(g_L);
        dp_relayout(g_eng);
        g_drag_guide = -1;
        paint();
        /* Re-evaluate the cursor for the current hover: without this the
         * split shape (← → or ↕) sticks after the button is released until
         * the mouse is moved off the guide. */
        int gg = (g_margin_edit && g_active_tab == TAB_LAYOUT) ? guide_hit(x, y, NULL) : -1;
        dobui_SetCursor(win, (gg == 0 || gg == 1) ? CURSOR_HSPLIT
                            : (gg >= 2)           ? CURSOR_VSPLIT
                                                  : CURSOR_DEFAULT);
    }
    g_dragging = false;
    dobpbtn_OnRelease(&g_bold); dobpbtn_OnRelease(&g_italic); dobpbtn_OnRelease(&g_under);
    dobpbtn_OnRelease(&g_strike); dobpbtn_OnRelease(&g_open); dobpbtn_OnRelease(&g_save);
    dobpbtn_OnRelease(&g_alignL); dobpbtn_OnRelease(&g_alignC); dobpbtn_OnRelease(&g_alignR); dobpbtn_OnRelease(&g_alignJ);
    dobpbtn_OnRelease(&g_fcolor);
    dobpbtn_OnRelease(&g_pagebg_btn);
    dobpbtn_OnRelease(&g_hl_btn);
    dobpbtn_OnRelease(&g_hlclear_btn);
    dobbtn_OnRelease(&g_subsbtn);
    dobbtn_OnRelease(&g_findbtn);
    dobbtn_OnRelease(&g_layout_pagebtn);
    dobbtn_OnRelease(&g_margin_btn);
    dobbtn_OnRelease(&g_sectbreak_btn);
    for (int i = 0; i < 4; i++) dobbtn_OnRelease(&g_para_sp[i]);
    for (int i = 0; i < 3; i++) dobbtn_OnRelease(&g_para_ls[i]);

    /* every mouse selection -- drag, double/triple-click, shift-click -- ends
     * with a release, so this single spot brings up the formatting toolbar.
     * Suppressed while a dropdown is open: the toolbar would float over the
     * open list and, being hit-tested first, swallow the item clicks. */
    if (g_pp != PP_SUBST && !dd_open()) {
        if (has_sel())            { g_pp = PP_SEL; position_sel_popup(); paint(); }
        else if (g_pp == PP_SEL)  { g_pp = PP_NONE;                      paint(); }
    }
}

void event_scroll(int delta)
{
    if ((g_font_dd.open && dobdd_OnScroll(&g_font_dd, delta)) ||
        (g_size_dd.open && dobdd_OnScroll(&g_size_dd, delta))) { paint(); return; }
    if (g_mods & DOBUI_MOD_CTRL) dp_scroll_x_by(g_eng, delta * 40);   /* mainDOB: Ctrl = horizontal */
    else                        dp_scroll_by(g_eng,   delta * 40);
    paint();
}

void event_resize(int w, int h) { win_w = w; win_h = h; dp_set_viewport(g_eng, w, content_bottom() - RIBBON_H); paint(); }
void event_close(void) { dobui_DestroyWindow(win); dobui_quit(); }

/* ============================================================
 *  Command bar (the mainDOB system panel)
 *
 *  A flat replica of the ribbon's actions plus the classic
 *  clipboard ops. The focused window's panel shows these labels;
 *  event_panel() dispatches the chosen index against g_panel_cmds,
 *  matched 1:1 with the list built in install_panel(). Format/
 *  clipboard actions follow the same selection rules as the ribbon
 *  (they act on the current selection). */
static void cmd_bold(void)          { toggle_cf(DD_CF_BOLD); }
static void cmd_italic(void)        { toggle_cf(DD_CF_ITALIC); }
static void cmd_underline(void)     { toggle_cf(DD_CF_UNDERLINE); }
static void cmd_strike(void)        { toggle_cf(DD_CF_STRIKE); }
static void cmd_size_up(void)       { bump_size(+1); }
static void cmd_size_down(void)     { bump_size(-1); }
static void cmd_align_left(void)    { apply_align(DD_ALIGN_LEFT); }
static void cmd_align_center(void)  { apply_align(DD_ALIGN_CENTER); }
static void cmd_align_right(void)   { apply_align(DD_ALIGN_RIGHT); }
static void cmd_align_justify(void) { apply_align(DD_ALIGN_JUSTIFY); }
static void cmd_pagesetup(void)
{
    PageSetup ps; df_doc_section_page_at(g_doc, g_caret, &ps);
    pagesetup_open(dobui_primary(), &ps, on_pagesetup_apply, NULL);
}
static void cmd_sectbreak(void)
{
    if (df_doc_insert_section_break(g_doc, g_caret) == DD_OK) {
        df_layout_rebuild(g_L); dp_relayout(g_eng); ensure_caret_visible();
    }
    paint();
}
static void cmd_findrepl(void) { findrepl_open(dobui_primary()); }
static void cmd_subsedit(void) { subsedit_open(dobui_primary()); }
static void cmd_columns(void)  { g_active_tab = TAB_LAYOUT; g_cols_open = true; paint(); }
static void cmd_margins(void)
{
    g_margin_edit = !g_margin_edit;
    if (g_margin_edit) df_doc_section_page_at(g_doc, g_caret, &g_marg_ps);
    else { g_drag_guide = -1; dobui_SetCursor(win, CURSOR_DEFAULT); }
    paint();
}

typedef struct { const char *label; void (*fn)(void); } panel_cmd_t;
static const panel_cmd_t g_panel_cmds[] = {
    { "Apri...",                 do_open },
    { "Salva",                   do_save },
    { "Salva copia",             do_save_copy },
    { "Taglia",                  do_cut },
    { "Copia",                   do_copy },
    { "Incolla",                 do_paste },
    { "Elimina",                 do_delete_sel },
    { "Grassetto",               cmd_bold },
    { "Corsivo",                 cmd_italic },
    { "Sottolineato",            cmd_underline },
    { "Barrato",                 cmd_strike },
    { "Aumenta dimensione",      cmd_size_up },
    { "Riduci dimensione",       cmd_size_down },
    { "Allinea a sinistra",      cmd_align_left },
    { "Centra",                  cmd_align_center },
    { "Allinea a destra",        cmd_align_right },
    { "Giustifica",              cmd_align_justify },
    { "Colore testo...",         open_fontcolor_picker },
    { "Evidenziazione...",       open_highlight_picker },
    { "Colore pagina...",        open_pagebg_picker },
    { "Imposta pagina...",       cmd_pagesetup },
    { "Colonne...",              cmd_columns },
    { "Margini",                 cmd_margins },
    { "Interruzione di sezione", cmd_sectbreak },
    { "Trova e sostituisci...",  cmd_findrepl },
    { "Sostituzioni...",         cmd_subsedit },
};
#define NPANEL ((int)(sizeof(g_panel_cmds) / sizeof(g_panel_cmds[0])))

static void install_panel(void)
{
    char buf[1024]; int p = 0;
    for (int i = 0; i < NPANEL && p < (int)sizeof buf; i++)
        p += snprintf(buf + p, sizeof buf - (size_t)p, "%s%s", i ? "\n" : "", g_panel_cmds[i].label);
    dobui_set_panel(buf);
}

void event_panel(int cmd_idx)
{
    if (cmd_idx >= 0 && cmd_idx < NPANEL && g_panel_cmds[cmd_idx].fn)
        g_panel_cmds[cmd_idx].fn();
}

void event_start(void)
{
    win = dobui_window();
    win_w = dobui_width(); win_h = dobui_height();

    if (!install_fonts()) {
        dobpopup_Error("DobWrite", "Nessun font disponibile."); return;
    }

    autocorr_init();
    autocorr_load(AUTOCORR_PATH);   /* saved substitutions override the defaults */

    /* If launched via the .dobw association, load that document; the welcome
     * template is only for a genuinely new document (no file passed). */
    bool opened = false;
    if (g_boot_open[0]) {
        int fd = dobfs_Open(g_boot_open, FS_READ);
        if (fd >= 0) {
            uint8_t *buf = NULL; uint32_t len = 0, cap = 0; bool ok = true;
            for (;;) {
                if (len + LOAD_CHUNK > cap) { uint32_t nc = cap ? cap * 2 : LOAD_CHUNK * 2;
                    uint8_t *nb = (uint8_t *)realloc(buf, nc); if (!nb) { ok = false; break; } buf = nb; cap = nc; }
                int n = dobfs_Read(fd, (char *)(buf + len), LOAD_CHUNK);
                if (n < 0) { ok = false; break; } if (n == 0) break; len += (uint32_t)n;
            }
            dobfs_Close(fd);
            df_doc *nd = NULL;
            if (ok && df_doc_load(buf, len, &nd) == DD_OK && nd) {
                g_doc = nd; g_caret = 0; g_anchor = -1;
                strncpy(g_path, g_boot_open, sizeof g_path - 1); g_path[sizeof g_path - 1] = 0;
                g_saved = true;
                PageSetup lps; df_doc_get_page(g_doc, &lps);
                g_pagebg = lps.bg_color & 0x00FFFFFFu;   /* paper colour follows the file */
                opened = true;
            }
            free(buf);
        }
        if (!opened) dobpopup_Error("DobWrite", "File .dobw non valido.");
    }

    if (!opened) {
        g_doc = df_doc_create();
        /* default paragraph spacing: a hard return (Enter) leaves ~1.15x the gap
         * of a soft wrap. Applied to the initial paragraph; new ones inherit it. */
        { ParaFmt pf; memset(&pf, 0, sizeof pf); pf.mask = DD_PF_SPACE_AFTER;
          pf.space_after = g_para_sp_tw[g_para_sp_active];
          df_doc_set_para_fmt(g_doc, 0, df_doc_length(g_doc), &pf); }
        PageSetup ps; df_doc_get_page(g_doc, &ps);
        ps.width = 8640; ps.height = 11160; ps.margin_left = ps.margin_right = 1080; ps.margin_top = ps.margin_bottom = 1080;
        df_doc_set_page(g_doc, &ps);
        uint16_t fam = df_doc_family_intern(g_doc, g_default_family);
        CharFmt def; df_doc_get_default_char(g_doc, &def); def.family_id = fam; def.size_twips = 240;
        df_doc_set_default_char(g_doc, &def);

        const char *t = "Benvenuto in DobWrite.\nScheda Home: font, dimensione, grassetto, corsivo, sottolineato e "
                        "barrato (seleziona prima il testo). Scheda File: apri e salva. Ctrl+S per salvare.";
        df_doc_insert(g_doc, 0, t, (uint32_t)strlen(t));
        g_caret = (uint32_t)strlen(t);
    }

    dl_opts o; o.dpi = layout_dpi();
    g_L = df_layout_create(g_doc, g_fonts, &o);
    g_eng = dp_create(g_L);
    dp_set_page_bg(g_eng, 0xFF000000u | g_pagebg);   /* opaque paper; matches the loaded doc */
    dp_set_viewport(g_eng, win_w, content_bottom() - RIBBON_H);

    /* ribbon (Home tab) -- widgets live in the content row, below the tab strip */
    dobdd_Init(&g_font_dd, win, 8, TAB_H + 12, 150, 0, g_font_names, g_font_count);
    g_font_dd.col_clear = RIBBON_BG; dobdd_SetSelected(&g_font_dd, 0);
    dobdd_Init(&g_size_dd, win, 166, TAB_H + 12, 56, 0, g_sizes, NSIZES);
    g_size_dd.col_clear = RIBBON_BG; dobdd_SetSelected(&g_size_dd, g_last_size);

    int bx = 236, by = TAB_H + 8;
    dobpbtn_Init(&g_bold, win, bx, by, 32, 32);      make_icon(g_ico_bold, 'B', true, false, false, false);  dobpbtn_SetImage(&g_bold, g_ico_bold, ICON_W, ICON_H);   bx += 36;
    dobpbtn_Init(&g_italic, win, bx, by, 32, 32);    make_icon(g_ico_italic, 'I', false, true, false, false); dobpbtn_SetImage(&g_italic, g_ico_italic, ICON_W, ICON_H); bx += 36;
    dobpbtn_Init(&g_under, win, bx, by, 32, 32);     make_icon(g_ico_under, 'U', false, false, true, false);  dobpbtn_SetImage(&g_under, g_ico_under, ICON_W, ICON_H);   bx += 36;
    dobpbtn_Init(&g_strike, win, bx, by, 32, 32);    make_icon(g_ico_strike, 'S', false, false, false, true); dobpbtn_SetImage(&g_strike, g_ico_strike, ICON_W, ICON_H);
    int ax = bx + 36 + 16;   /* small gap after the B/I/U/S group */
    dobpbtn_Init(&g_alignL, win, ax, by, 32, 32);    make_align_icon(g_ico_alignL, DD_ALIGN_LEFT);    dobpbtn_SetImage(&g_alignL, g_ico_alignL, ICON_W, ICON_H); ax += 36;
    dobpbtn_Init(&g_alignC, win, ax, by, 32, 32);    make_align_icon(g_ico_alignC, DD_ALIGN_CENTER);  dobpbtn_SetImage(&g_alignC, g_ico_alignC, ICON_W, ICON_H); ax += 36;
    dobpbtn_Init(&g_alignR, win, ax, by, 32, 32);    make_align_icon(g_ico_alignR, DD_ALIGN_RIGHT);   dobpbtn_SetImage(&g_alignR, g_ico_alignR, ICON_W, ICON_H); ax += 36;
    dobpbtn_Init(&g_alignJ, win, ax, by, 32, 32);    make_align_icon(g_ico_alignJ, DD_ALIGN_JUSTIFY); dobpbtn_SetImage(&g_alignJ, g_ico_alignJ, ICON_W, ICON_H);
    ax += 36 + 16;   /* gap after the alignment group */
    dobpbtn_Init(&g_fcolor, win, ax, by, 32, 32);    make_fontcolor_icon(g_ico_fcolor, g_fontcolor); dobpbtn_SetImage(&g_fcolor, g_ico_fcolor, ICON_W, ICON_H);
    ax += 36 + 16;
    dobpbtn_Init(&g_pagebg_btn, win, ax, by, 32, 32); make_pagebg_icon(g_ico_pagebg, g_pagebg); dobpbtn_SetImage(&g_pagebg_btn, g_ico_pagebg, ICON_W, ICON_H);
    ax += 36 + 16;
    dobpbtn_Init(&g_hl_btn, win, ax, by, 32, 32);     make_highlight_icon(g_ico_hl, g_highlight); dobpbtn_SetImage(&g_hl_btn, g_ico_hl, ICON_W, ICON_H);
    ax += 36 + 16;
    dobpbtn_Init(&g_hlclear_btn, win, ax, by, 32, 32); make_hlclear_icon(g_ico_hlclear); dobpbtn_SetImage(&g_hlclear_btn, g_ico_hlclear, ICON_W, ICON_H);
    dobbtn_Init(&g_findbtn, win,  12, TAB_H + 12, 180, 24, "Trova e sostituisci...");
    dobbtn_Init(&g_layout_pagebtn, win, 12, TAB_H + 12, 180, 24, "Imposta pagina...");
    dobbtn_Init(&g_margin_btn, win, 200, TAB_H + 12, 150, 24, "Margini");
    dobbtn_Init(&g_sectbreak_btn, win, 360, TAB_H + 12, 248, 24, "Inserisci interruzione di sezione");
    dobbtn_Init(&g_cols_btn, win, 620, TAB_H + 12, 128, 24, "Colonne");
    { static const char *cl[6] = { "1","2","3","4","5","6" };
      for (int i = 0; i < 6; i++)
          dobbtn_Init(&g_cols_pick[i], win, COLP_X + 10 + i * 38, COLP_Y + 30, 34, 22, cl[i]); }
    dobbtn_Init(&g_cols_gap_dec, win, COLP_X + 96,  COLP_Y + 58, 28, 22, "-");
    dobbtn_Init(&g_cols_gap_inc, win, COLP_X + 200, COLP_Y + 58, 28, 22, "+");
    dobbtn_Init(&g_subsbtn, win, 200, TAB_H + 12, 150, 24, "Sostituzioni...");
    for (int i = 0; i < 4; i++)
        dobbtn_Init(&g_para_sp[i], win, 190 + i * 48, TAB_H + 25, 44, 22, g_para_sp_lbl[i]);
    dobbtn_Init(&g_para_ls[0], win, 110, TAB_H + 2, 58, 22, "Singola");
    dobbtn_Init(&g_para_ls[1], win, 172, TAB_H + 2, 40, 22, "1,5");
    dobbtn_Init(&g_para_ls[2], win, 216, TAB_H + 2, 58, 22, "Doppia");
    /* File-tab buttons: their own left-aligned row (shown only on the File tab) */
    dobpbtn_Init(&g_open, win, 8,  by, 32, 32);      make_mark(g_ico_open, false); dobpbtn_SetImage(&g_open, g_ico_open, ICON_W, ICON_H);
    dobpbtn_Init(&g_save, win, 48, by, 32, 32);      make_mark(g_ico_save, true);  dobpbtn_SetImage(&g_save, g_ico_save, ICON_W, ICON_H);

    if (opened) set_doc_title_from_path(g_path);
    else        dobui_SetTitle(win, "DobWrite - Nuovo documento");
    install_panel();
    update_toggles_from_caret();
    recount();
    paint();
}

/* ---- ".ttf opened via the file association" -> offer to install it ----------
 * Opening a font with DobWrite doesn't open the editor: it asks whether to
 * install the font, and on yes copies it into the shared fonts area so the next
 * DobWrite session picks it up. */
static bool is_font_path(const char *p)
{
    const char *ext = NULL;
    for (const char *q = p; *q; q++) if (*q == '.') ext = q;
    if (!ext) return false;
    char e[8]; int k = 0;
    for (const char *q = ext; *q && k < 7; q++) { char c = *q; if (c >= 'A' && c <= 'Z') c += 32; e[k++] = c; }
    e[k] = 0;
    return !strcmp(e, ".ttf") || !strcmp(e, ".otf");
}

static const char *path_basename(const char *p)
{
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}

static void font_install_flow(const char *src)
{
    const char *base = path_basename(src);

    char msg[320];
    snprintf(msg, sizeof msg, "Installare il seguente font nel sistema?\n\n%s", base);
    if (dobpopup_YesNo("DobWrite", msg) != 1) return;          /* 1 == Si */

    /* Read the source font fully. */
    int fd = dobfs_Open(src, FS_READ);
    if (fd < 0) { dobpopup_Error("DobWrite", "Impossibile aprire il file."); return; }
    uint8_t *buf = NULL; uint32_t len = 0, cap = 0; bool ok = true;
    for (;;) {
        if (len + LOAD_CHUNK > cap) {
            uint32_t nc = cap ? cap * 2 : LOAD_CHUNK * 2;
            uint8_t *nb = (uint8_t *)realloc(buf, nc);
            if (!nb) { ok = false; break; } buf = nb; cap = nc;
        }
        int n = dobfs_Read(fd, buf + len, LOAD_CHUNK);
        if (n < 0) { ok = false; break; } if (n == 0) break; len += (uint32_t)n;
    }
    dobfs_Close(fd);
    if (!ok || len == 0) { free(buf); dobpopup_Error("DobWrite", "Lettura del font fallita."); return; }

    /* Validate it really is a font the engine can use before installing it. */
    df_face *vf;
    if (df_open(buf, len, 0, &vf) != 0) { free(buf); dobpopup_Error("DobWrite", "Il file non e' un font valido."); return; }
    df_close(vf);

    /* Write it into the shared, sandbox-free fonts area. */
    char dest[320];
    snprintf(dest, sizeof dest, "%s/%s", DOBFS_COMMON_FONTS, base);
    dobfs_Mkdir(DOBFS_COMMON_FILES);
    dobfs_Mkdir(DOBFS_COMMON_FONTS);
    int wf = dobfs_Open(dest, FS_WRITE | FS_CREATE | FS_TRUNC);
    if (wf < 0) { free(buf); dobpopup_Error("DobWrite", "Impossibile scrivere nella cartella dei font."); return; }
    uint32_t off = 0; bool wok = true;
    while (off < len) { int w = dobfs_Write(wf, buf + off, len - off); if (w <= 0) { wok = false; break; } off += (uint32_t)w; }
    dobfs_Close(wf);
    free(buf);
    if (!wok) { dobpopup_Error("DobWrite", "Scrittura del font interrotta."); return; }

    dobpopup_Info("DobWrite", "Font installato. Sara' disponibile al prossimo avvio di DobWrite.");
}

int main(int argc, char **argv)
{
    /* MainDOB passes the opened file as argv[0] (not the program path). When
     * launched on a font via the .ttf/.otf association, run the installer flow
     * instead of opening the editor. */
    const char *openf = (argc >= 1 && argv[0] && argv[0][0]) ? argv[0] : NULL;
    if (openf && is_font_path(openf)) { font_install_flow(openf); return 0; }
    /* A non-font file passed by the association is a .dobw document: remember it
     * so event_start() loads it instead of the welcome template. */
    if (openf) { strncpy(g_boot_open, openf, sizeof g_boot_open - 1); g_boot_open[sizeof g_boot_open - 1] = 0; }

    dobui_run("DobWrite - Nuovo documento", WIN_W, WIN_H);
    return 0;
}
