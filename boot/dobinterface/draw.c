/* MainDOB DobInterface 2.0 — foglio: draw.c
 *
 * Primitive di disegno: verbi esecutivi puri, senza stato proprio
 * (l'unico dato del foglio e' lo scratch CPU condiviso, che vive qui
 * perche' il disegno e' il suo dominio naturale).
 *
 * DUE STRATI, per il riciclo:
 *  - surf_*  primitive su una SURFACE qualunque (parametro esplicito):
 *            e' il livello esecutivo puro, senza dipendenze da stato
 *            globale. Le usa il compositor sul corpo finestra
 *            (w->body_surf) tanto quanto il desktop sul backbuf; sono
 *            il nucleo promuovibile a libdobui (disegno su SHM panel
 *            lato client);
 *  - fb_*    binding di convenienza degli stessi verbi al backbuf
 *            desktop (g_backbuf_surf): il vocabolario naturale del
 *            chrome di desktop (icone, pannello, overlay), invariato
 *            per tutti i chiamanti storici.
 *
 * Regole di resa ereditate come specifica [PX] dall'1.x:
 *  - le linee da 1 px sono fill_rect di altezza/larghezza 1, cosi'
 *    passano da dv_fill_rect senza overhead Bresenham per-pixel;
 *  - il contorno di rettangolo e' quattro fill sottili per anello
 *    (spessore = anelli concentrici), identico al tratto 1.x;
 *  - ogni verbo e' un no-op sicuro se la surface non esiste o la
 *    misura e' degenere: i chiamanti non si difendono. */

#include "di_internal.h"

/* Scratch CPU condiviso (atlante glifi, raster icone, screenshot):
 * allocato da main all'avvio a WIN_SCRATCH_PX, ricresciuto dal
 * relayout se il desktop si allarga. */
uint32_t *g_win_scratch = NULL;

/* ================= verbi esecutivi: primitive su surface ============== */

int clamp(int val, int lo, int hi)
{
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

void surf_hline(dv_surface_t surf, int x, int y, int w, uint32_t color)
{
    if (w <= 0 || surf == DV_HANDLE_NONE) return;
    dv_rect_t r = { (int32_t)x, (int32_t)y, (uint32_t)w, 1 };
    dv_fill_rect(surf, r, dv_color_from_u32(color));
}

void surf_vline(dv_surface_t surf, int x, int y, int h, uint32_t color)
{
    if (h <= 0 || surf == DV_HANDLE_NONE) return;
    dv_rect_t r = { (int32_t)x, (int32_t)y, 1, (uint32_t)h };
    dv_fill_rect(surf, r, dv_color_from_u32(color));
}

void surf_fill_rect(dv_surface_t surf, int x, int y, int w, int h, uint32_t color)
{
    if (surf == DV_HANDLE_NONE) return;
    dv_rect_t r = { (int32_t)x, (int32_t)y, (uint32_t)w, (uint32_t)h };
    dv_fill_rect(surf, r, dv_color_from_u32(color));
}

/* ================= terra di mezzo: contorno su surface =============== */

/* Contorno di rettangolo come quattro fill sottili per anello. */
void surf_draw_rect(dv_surface_t surf, int x, int y, int w, int h,
                    uint32_t color, int thick)
{
    for (int t = 0; t < thick; t++)
    {
        surf_hline(surf, x + t,         y + t,         w - 2 * t, color);
        surf_hline(surf, x + t,         y + h - 1 - t, w - 2 * t, color);
        surf_vline(surf, x + t,         y + t,         h - 2 * t, color);
        surf_vline(surf, x + w - 1 - t, y + t,         h - 2 * t, color);
    }
}

/* ================= binding di convenienza al backbuf desktop ========== */

void fb_draw_hline(int x, int y, int w, uint32_t color)
{
    surf_hline(g_backbuf_surf, x, y, w, color);
}

void fb_draw_vline(int x, int y, int h, uint32_t color)
{
    surf_vline(g_backbuf_surf, x, y, h, color);
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    surf_fill_rect(g_backbuf_surf, x, y, w, h, color);
}

void fb_draw_rect(int x, int y, int w, int h, uint32_t color, int thick)
{
    surf_draw_rect(g_backbuf_surf, x, y, w, h, color, thick);
}
