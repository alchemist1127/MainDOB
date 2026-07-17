/* MainDOB DobInterface 2.0 — foglio: font.c
 *
 * Font a cella 8x16 (la tabella glifi vive in libdob, <dob/font.h>):
 * costruzione e upload dell'atlante glifi nel driver, conversione
 * stringa -> glifi con avanzamento proporzionale, resa testo sul
 * backbuffer.
 *
 * Convenzione BGA dell'atlante: striscia verticale di 256 celle
 * (DOB_FONT_W x DOB_FONT_H = 8x16), cella `i` a (0, i*16, 8, 16).
 * I pixel con alpha > 0 vengono disegnati (ricolorati al draw);
 * alpha == 0 e' saltato. Le celle 32..126 tengono l'ASCII
 * stampabile, il resto resta a zero. 128 KiB, write-once. */

#include "di_internal.h"

/* ================= stato del foglio =================================== */

dv_texture_t g_glyph_atlas = DV_HANDLE_NONE;    /* condiviso (header) */

#define GLYPH_ATLAS_W   DOB_FONT_W
#define GLYPH_ATLAS_H   (DOB_FONT_H * 256)

/* ================= verbi esecutivi ==================================== */

/* Stringa NUL-terminata -> array dv_glyph_t per dv_cmdlist_draw_glyphs.
 * glyph_index e' il byte carattere grezzo (atlante indicizzato
 * 0..255); il decode e' UTF-8 aware (dob_font_decode), i fuori-range
 * cadono su spazio — stesso fallback di font_draw_string. Il
 * chiamante alloca `out` con capienza >= lunghezza stringa. Ritorna
 * il numero di glifi. */
uint32_t string_to_glyphs(const char *str, int x, int y,
                          dv_glyph_t *out, uint32_t out_cap)
{
    uint32_t n = 0, bi = 0;
    uint32_t len = (uint32_t)strlen(str);
    int penx = 0;
    while (bi < len && n < out_cap)
    {
        uint8_t g;
        bi += dob_font_decode((const uint8_t *)str, len, bi, &g);
        out[n].glyph_index = (uint32_t)g;
        out[n].x = x + penx - dob_font_left(g);   /* avanzamento proporzionale */
        out[n].y = y;
        n++;
        penx += dob_font_advance(g);
    }
    return n;
}

/* font_draw_string — un fill_rect per lo sfondo + un draw_glyphs per
 * il testo (celle dell'atlante indicizzate dal byte). Troncata a 256
 * caratteri per il buffer glifi in stack; stringhe piu' lunghe sono
 * rare nella UI MainDOB. */
void font_draw_string(int x, int y, const char *str, uint32_t fg, uint32_t bg)
{
    if (g_backbuf_surf == DV_HANDLE_NONE
        || g_glyph_atlas == DV_HANDLE_NONE || !str) return;

    size_t len = 0;
    while (str[len] && len < 256) len++;
    if (len == 0) return;

    dv_rect_t bgrect = {
        (int32_t)x, (int32_t)y,
        (uint32_t)dob_text_width(str, (uint32_t)len), (uint32_t)FONT_H,
    };
    dv_fill_rect(g_backbuf_surf, bgrect, dv_color_from_u32(bg));

    dv_glyph_t glyphs[256];
    uint32_t n = string_to_glyphs(str, x, y, glyphs, 256);
    dv_draw_glyphs(g_backbuf_surf, g_glyph_atlas,
                   glyphs, n, dv_color_from_u32(fg));
}

/* ================= logica ad alto livello ============================= */

/* glyph_atlas_init — alloca la texture (DOB_FONT_W x DOB_FONT_H*256)
 * BGRA e carica le righe dei glifi: nelle celle ASCII stampabili i
 * pixel "accesi" hanno alpha=0xFF, il resto 0; l'RGB e' irrilevante
 * (dv_draw_glyphs ricolora al draw). Best-effort: su fallimento
 * g_glyph_atlas resta DV_HANDLE_NONE e il testo semplicemente non
 * rende. Riusa g_win_scratch come staging (con guardia di misura). */
void glyph_atlas_init(void)
{
    if (g_glyph_atlas != DV_HANDLE_NONE) return;
    if (!g_win_scratch) return;

    /* Guardia: lo scratch deve contenere l'atlante in staging. */
    size_t need = (size_t)GLYPH_ATLAS_W * GLYPH_ATLAS_H;
    if (need > (size_t)WIN_SCRATCH_PX)
    {
        debug_print("[dobinterface] glyph atlas: scratch too small.\n");
        return;
    }

    /* Staging BGRA nello scratch. Layout: striscia verticale di 256
     * celle, ognuna DOB_FONT_W x DOB_FONT_H. */
    for (size_t i = 0; i < need; i++) g_win_scratch[i] = 0;

    for (int code = DOB_FONT_FIRST; code <= DOB_FONT_LAST; code++)
    {
        int idx = code - DOB_FONT_FIRST;
        for (int row = 0; row < DOB_FONT_H; row++)
        {
            uint8_t bits = dob_font_data[idx][row];
            int dst_y = code * DOB_FONT_H + row;
            uint32_t *r = &g_win_scratch[(uint32_t)dst_y * GLYPH_ATLAS_W];
            for (int col = 0; col < DOB_FONT_W; col++)
            {
                if (bits & (uint8_t)(0x80u >> col))
                    r[col] = 0xFFFFFFFFu;     /* alpha=FF, RGB=bianco */
                /* else resta 0 = trasparente */
            }
        }
    }

    dv_texture_desc_t td = {
        .width  = GLYPH_ATLAS_W,
        .height = GLYPH_ATLAS_H,
        .format = DV_FMT_BGRA8888,
        /* SYSRAM: il testo nei corpi finestra (SYSRAM) campiona
         * l'atlante pixel per pixel dalla CPU — in VRAM ogni campione
         * e' una lettura non cacheata dall'aperture, e il testo e'
         * OVUNQUE. */
        .flags  = DV_SURF_FLAG_SYSRAM,
    };
    if (dv_texture_create(g_vproc, &td, &g_glyph_atlas) != DV_OK)
    {
        debug_print("[dobinterface] glyph atlas: texture_create failed.\n");
        g_glyph_atlas = DV_HANDLE_NONE;
        return;
    }
    dv_rect_t whole = { 0, 0, GLYPH_ATLAS_W, GLYPH_ATLAS_H };
    int32_t rc = dv_texture_update_region(g_glyph_atlas, whole,
                                          g_win_scratch,
                                          GLYPH_ATLAS_W * 4);
    if (rc != DV_OK)
    {
        debug_print("[dobinterface] glyph atlas: upload failed.\n");
        dv_texture_destroy(g_glyph_atlas);
        g_glyph_atlas = DV_HANDLE_NONE;
        return;
    }
    debug_print("[dobinterface] glyph atlas uploaded.\n");
}
