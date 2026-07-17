/* MainDOB DobInterface 2.0 — foglio: icons.c
 *
 * Icone device sul desktop, 100% data-driven dal DAS: popolate via
 * GUI_DEVICE_ATTACH/_DETACH da hotplug (che possiede il database),
 * layout in colonne dal basso-destra (vicino al pannello = minimo
 * tragitto del puntatore), bitmap 1bpp spedite con l'ATTACH e
 * rasterizzate pigramente in surface BGRA a per-pixel alpha.
 *
 * Contratto di z-order: le icone si disegnano DOPO il fill blu del
 * desktop e PRIMA di ogni finestra, cosi' le finestre le coprono
 * naturalmente. dobinterface non ha ALCUNA conoscenza delle azioni:
 * doppio clic e voci di menu vengono solo inoltrati a hotplug.
 *
 * Fix 1.x inglobato: alla rimozione (e al cambio risoluzione) gli
 * slot vengono RICOMPATTATI preservando l'ordine visivo — senza, la
 * cella liberata restava un buco permanente ("quando si disinserisce
 * un dispositivo hotplug, rimane la cella vuota"). */

#include "di_internal.h"

/* ================= stato condiviso (proprieta' di questo foglio) ====== */

desktop_icon_t desktop_icons[MAX_DESKTOP_ICONS];
int            desktop_icon_count = 0;
int            selected_icon = -1;

/* ================= stato privato ====================================== */

/* Tracker doppio-clic dedicato alle icone — il dblc_* del foglio
 * input e' scopato all'inoltro dei doppi clic nelle finestre e non
 * va toccato qui. */
static uint32_t dblc_icon_last_ms  = 0;
static int      dblc_icon_last_idx = -1;

/* ================= verbi esecutivi ==================================== */

int icon_find_by_device_id(uint32_t id)
{
    for (int i = 0; i < MAX_DESKTOP_ICONS; i++)
    {
        if (desktop_icons[i].in_use && desktop_icons[i].device_id == id)
            return i;
    }
    return -1;
}

/* Primo slot (col,row) libero in scansione antioraria dal
 * basso-destra: prima la colonna piu' a destra, dal basso in alto;
 * poi una colonna a sinistra e via cosi'. false = nessuno libero. */
static bool icon_find_next_layout_slot(int *out_col, int *out_row)
{
    int max_cols = (desktop_w  - ICON_MARGIN_RIGHT)  / (ICON_W + ICON_GAP);
    int max_rows = (SCREEN_H   - ICON_MARGIN_BOTTOM) / (ICON_H + ICON_GAP);
    if (max_cols < 1) max_cols = 1;
    if (max_rows < 1) max_rows = 1;

    for (int col = 0; col < max_cols; col++)
    {
        for (int row = 0; row < max_rows; row++)
        {
            bool taken = false;
            for (int i = 0; i < MAX_DESKTOP_ICONS; i++)
            {
                if (desktop_icons[i].in_use
                    && desktop_icons[i].slot_col == col
                    && desktop_icons[i].slot_row == row)
                {
                    taken = true;
                    break;
                }
            }
            if (!taken)
            {
                *out_col = col;
                *out_row = row;
                return true;
            }
        }
    }
    return false;
}

/* (col,row) -> pixel alto-sinistro della cella. */
static void icon_slot_to_xy(int col, int row, int *out_x, int *out_y)
{
    *out_x = panel_x - ICON_MARGIN_RIGHT - ICON_W
             - col * (ICON_W + ICON_GAP);
    *out_y = SCREEN_H - ICON_MARGIN_BOTTOM - ICON_H
             - row * (ICON_H + ICON_GAP);
}

/* Hit-test: icona sotto (mx,my) o -1. */
int icon_hit_test(int mx, int my)
{
    for (int i = 0; i < MAX_DESKTOP_ICONS; i++)
    {
        if (!desktop_icons[i].in_use) continue;
        int x, y;
        icon_slot_to_xy(desktop_icons[i].slot_col,
                        desktop_icons[i].slot_row,
                        &x, &y);
        if (mx >= x && mx < x + ICON_W
            && my >= y && my < y + ICON_H)
            return i;
    }
    return -1;
}

/* Rasterizzazione pigra della bitmap 1bpp in una piccola surface
 * BGRA a per-pixel alpha, con upload. Cache sull'icona per i paint
 * successivi (si ri-rasterizza solo se la bitmap cambia — cosa che
 * oggi non accade mai). */
static void icon_rasterize_to_surface(desktop_icon_t *ic)
{
    if (ic->icon_surf != DV_HANDLE_NONE) return;
    if (!g_win_scratch) return;
    if (ic->bitmap.format != ICON_FMT_1BPP_MASK) return;

    int bmp_w = ic->bitmap.width;
    int bmp_h = ic->bitmap.height;
    if (bmp_w > ICON_W)                bmp_w = ICON_W;
    if (bmp_h > ICON_H - ICON_LABEL_H) bmp_h = ICON_H - ICON_LABEL_H;
    if (bmp_w <= 0 || bmp_h <= 0) return;

    /* fg impacchettato BGRA con alpha = FF dove il bit e' acceso. */
    uint32_t fg = 0xFF000000u
                | ((uint32_t)ic->bitmap.fg_r << 16)
                | ((uint32_t)ic->bitmap.fg_g << 8)
                |  (uint32_t)ic->bitmap.fg_b;

    /* Staging nello scratch — bmp_w * bmp_h pixel contigui. */
    if ((size_t)bmp_w * (size_t)bmp_h > (size_t)WIN_SCRATCH_PX) return;
    for (int i = 0; i < bmp_w * bmp_h; i++) g_win_scratch[i] = 0;

    int stride = (ic->bitmap.width + 7) / 8;
    for (int py = 0; py < bmp_h; py++)
    {
        const uint8_t *row = &ic->bitmap.data[py * stride];
        uint32_t      *dst = &g_win_scratch[py * bmp_w];
        for (int px = 0; px < bmp_w; px++)
        {
            if (row[px >> 3] & (uint8_t)(1u << (7 - (px & 7))))
                dst[px] = fg;
        }
    }

    dv_surface_desc_t sd = {
        .width  = (uint32_t)bmp_w,
        .height = (uint32_t)bmp_h,
        .format = DV_FMT_BGRA8888,
        .flags  = 0,
    };
    if (dv_surface_create(g_vproc, &sd, &ic->icon_surf) != DV_OK)
    {
        ic->icon_surf = DV_HANDLE_NONE;
        return;
    }
    dv_rect_t whole = { 0, 0, (uint32_t)bmp_w, (uint32_t)bmp_h };
    if (dv_texture_update_region((dv_texture_t)ic->icon_surf, whole,
                                 g_win_scratch, (uint32_t)bmp_w * 4) != DV_OK)
    {
        dv_surface_destroy(ic->icon_surf);
        ic->icon_surf = DV_HANDLE_NONE;
        return;
    }
    ic->icon_surf_w = (uint16_t)bmp_w;
    ic->icon_surf_h = (uint16_t)bmp_h;
}

/* ================= terra di mezzo: disegno ============================ */

static void icon_draw_one(int idx)
{
    desktop_icon_t *ic = &desktop_icons[idx];
    int cell_x, cell_y;
    icon_slot_to_xy(ic->slot_col, ic->slot_row, &cell_x, &cell_y);

    int bmp_area_h = ICON_H - ICON_LABEL_H;

    /* Bitmap centrata nella regione alta (ICON_W x bmp_area_h). */
    int bmp_w = ic->bitmap.width;
    int bmp_h = ic->bitmap.height;
    if (bmp_w > ICON_W)     bmp_w = ICON_W;
    if (bmp_h > bmp_area_h) bmp_h = bmp_area_h;

    int bmp_x = cell_x + (ICON_W - bmp_w) / 2;
    int bmp_y = cell_y + (bmp_area_h - bmp_h) / 2;

    /* Bitmap: blit per-pixel-alpha DIRETTO nel backbuf (il raster
     * mette alpha 0xFF sui pixel accesi della maschera 1bpp, 0 sugli
     * spenti). Formati non supportati: nessuna bitmap, la label si
     * disegna comunque. */
    if (g_backbuf_surf != DV_HANDLE_NONE
        && ic->bitmap.format == ICON_FMT_1BPP_MASK)
    {
        icon_rasterize_to_surface(ic);
        if (ic->icon_surf != DV_HANDLE_NONE && bmp_w > 0 && bmp_h > 0)
        {
            dv_rect_t  sr = { 0, 0, ic->icon_surf_w, ic->icon_surf_h };
            dv_point_t dp = { bmp_x, bmp_y };
            dv_blit_pixel_alpha(ic->icon_surf, sr, g_backbuf_surf, dp);
        }
    }

    /* Label centrata sotto la bitmap. */
    int label_w = (int)font_string_width(ic->label);
    int label_x = cell_x + (ICON_W - label_w) / 2;
    int label_y = cell_y + bmp_area_h + 2;
    if (label_x < 0) label_x = cell_x;
    if (label_y + FONT_H <= SCREEN_H
        && cell_x >= 0 && cell_x + ICON_W <= desktop_w)
    {
        font_draw_string(label_x, label_y, ic->label, COLOR_WHITE, COLOR_BG);
    }

    /* Contorno di selezione — sottile rettangolo ciano attorno alla cella. */
    if (ic->selected)
    {
        int ox = cell_x - 2;
        int oy = cell_y - 2;
        int ow = ICON_W + 4;
        int oh = ICON_H + 4;
        if (ox >= 0 && oy >= 0
            && ox + ow <= desktop_w && oy + oh <= SCREEN_H)
        {
            fb_draw_rect(ox, oy, ow, oh, COLOR_CYAN, 2);
        }
    }
}

void icon_draw_all(void)
{
    for (int i = 0; i < MAX_DESKTOP_ICONS; i++)
    {
        if (desktop_icons[i].in_use)
            icon_draw_one(i);
    }
}

/* Solo le icone la cui cella interseca la banda verticale [y0,y1).
 * Compagna di compositor_repaint_rect: riservata allo stadio
 * dirty-rect (tenuta fuori dal grafo delle chiamate di proposito). */
__attribute__((unused))
void icon_draw_band(int y0, int y1)
{
    for (int i = 0; i < MAX_DESKTOP_ICONS; i++)
    {
        if (!desktop_icons[i].in_use) continue;
        int x, y;
        icon_slot_to_xy(desktop_icons[i].slot_col,
                        desktop_icons[i].slot_row,
                        &x, &y);
        /* Espansa di poco per il contorno di selezione da 2 px, che
         * non va tagliato ai bordi banda. */
        int top    = y - 2;
        int bottom = y + ICON_H + 2;
        if (bottom <= y0 || top >= y1) continue;
        icon_draw_one(i);
    }
}

/* Pannello contestuale dal menu {} DAS del device: dobinterface non
 * sa cosa significhino le voci — rende le etichette e inoltra la
 * scelta via MENU_ACTIVATED. */
static void icon_show_context_panel(int idx)
{
    if (idx < 0 || idx >= MAX_DESKTOP_ICONS) return;
    desktop_icon_t *ic = &desktop_icons[idx];
    if (ic->menu_count == 0) return;

    const char *labels[DEV_MENU_MAX_ITEMS];
    uint32_t    cmds  [DEV_MENU_MAX_ITEMS];
    int n = ic->menu_count;
    if (n > DEV_MENU_MAX_ITEMS) n = DEV_MENU_MAX_ITEMS;
    for (int i = 0; i < n; i++)
    {
        labels[i] = ic->menu_items[i];
        cmds  [i] = CMD_ICON_MENU_BASE + (uint32_t)i;
    }
    panel_set_context_items(labels, cmds, n);
    panel_recalculate();
    di_mark_dirty(DIRTY_PANEL);
}

/* ================= logica ad alto livello ============================= */

/* Nuova icona da un payload GUI_DEVICE_ATTACH. */
int icon_add(const gui_device_attach_t *p)
{
    if (icon_find_by_device_id(p->device_id) >= 0)
        return -1;      /* gia' presente — duplicato ignorato */

    int slot = -1;
    for (int i = 0; i < MAX_DESKTOP_ICONS; i++)
    {
        if (!desktop_icons[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -1;

    int col, row;
    if (!icon_find_next_layout_slot(&col, &row))
        return -1;

    desktop_icon_t *ic = &desktop_icons[slot];
    memset(ic, 0, sizeof(*ic));
    ic->in_use    = true;
    ic->device_id = p->device_id;
    ic->kind      = p->kind;
    strncpy(ic->label,        p->label,        sizeof(ic->label) - 1);
    strncpy(ic->service_name, p->service_name, sizeof(ic->service_name) - 1);
    ic->slot_col  = col;
    ic->slot_row  = row;
    ic->selected  = false;
    ic->bitmap    = p->bitmap;
    ic->icon_surf = DV_HANDLE_NONE;     /* alloc pigra al primo draw */

    /* Snapshot del menu DAS: cosi' il pannello contestuale si
     * ridisegna senza re-interrogare il provider. */
    ic->menu_count = p->menu_count;
    if (ic->menu_count > DEV_MENU_MAX_ITEMS)
        ic->menu_count = DEV_MENU_MAX_ITEMS;
    for (int i = 0; i < ic->menu_count; i++)
    {
        memcpy(ic->menu_items[i], p->menu_items[i], DEV_MENU_LABEL_LEN);
        ic->menu_items[i][DEV_MENU_LABEL_LEN - 1] = '\0';
    }

    desktop_icon_count++;
    di_mark_dirty(DIRTY_FULL);
    return slot;
}

/* Ricompatta le icone vive in slot consecutivi preservando l'ordine
 * visivo corrente (column-major dal basso-destra, lo stesso ordine
 * di riempimento). Chiamata anche al cambio risoluzione, dove il
 * numero di righe cambia e icone non ricompattate finirebbero fuori
 * schermo. */
void icon_repack_slots(void)
{
    int max_rows = (SCREEN_H - ICON_MARGIN_BOTTOM) / (ICON_H + ICON_GAP);
    if (max_rows < 1) max_rows = 1;

    /* Raccogli le vive e ordinale per posizione visiva (chiave
     * column-major). Insertion sort: n minuscolo. */
    int order[MAX_DESKTOP_ICONS];
    int n = 0;
    for (int i = 0; i < MAX_DESKTOP_ICONS; i++)
        if (desktop_icons[i].in_use) order[n++] = i;

    for (int a = 1; a < n; a++)
    {
        int idx = order[a];
        int key = desktop_icons[idx].slot_col * max_rows
                + desktop_icons[idx].slot_row;
        int b = a - 1;
        while (b >= 0 &&
               (desktop_icons[order[b]].slot_col * max_rows
                + desktop_icons[order[b]].slot_row) > key)
        {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1] = idx;
    }

    /* Riassegna slot consecutivi nello stesso ordine di enumerazione. */
    for (int k = 0; k < n; k++)
    {
        desktop_icons[order[k]].slot_col = k / max_rows;
        desktop_icons[order[k]].slot_row = k % max_rows;
    }
}

/* Rimozione per device_id. Ritorna true se rimossa. */
bool icon_remove_by_id(uint32_t device_id)
{
    int idx = icon_find_by_device_id(device_id);
    if (idx < 0) return false;

    bool was_selected = (idx == selected_icon);

    /* Rilascia la surface dv_* se era stata allocata pigramente
     * (prima del memset, finche' l'handle c'e' ancora). */
    if (desktop_icons[idx].icon_surf != DV_HANDLE_NONE)
    {
        dv_surface_destroy(desktop_icons[idx].icon_surf);
        desktop_icons[idx].icon_surf = DV_HANDLE_NONE;
    }

    memset(&desktop_icons[idx], 0, sizeof(desktop_icons[idx]));
    desktop_icon_count--;

    /* Chiudi il buco: fai scorrere le superstiti in celle consecutive
     * mantenendo l'ordine relativo. */
    icon_repack_slots();

    if (was_selected)
    {
        selected_icon = -1;
        panel_sync_context();    /* ripristina il pannello di default */
    }

    di_mark_dirty(DIRTY_FULL);
    return true;
}

/* Inoltra un'attivazione (doppio clic o "Apri" dal pannello) a
 * hotplug: e' lui che possiede il DAS del device ed esegue la
 * sequenza di azioni — dobinterface resta fuori dalla policy,
 * consegna solo "l'utente ha attivato questo id". */
void send_icon_activated(uint32_t device_id)
{
    uint32_t hp = dob_registry_find("hotplug");
    if (!hp) return;

    static icon_activated_t req;
    req.device_id          = device_id;
    /* Doppio clic da desktop: nessun bersaglio hijack — l'OpenMount
     * in coda alla catena apre una finestra nuova. */
    req.hijack_target_port = 0;

    dob_msg_t m;
    memset(&m, 0, sizeof(m));
    m.code         = ICON_ACTIVATED;
    m.payload      = &req;
    m.payload_size = sizeof(req);
    dob_ipc_post(hp, &m);
}

/* Scelta di una voce del menu DAS dell'icona selezionata (dal
 * pannello): inoltra a hotplug via MENU_ACTIVATED. */
void icon_menu_activate(uint32_t menu_idx)
{
    if (selected_icon < 0) return;
    uint32_t hp = dob_registry_find("hotplug");
    if (!hp) return;

    static menu_activated_t req;
    req.device_id = desktop_icons[selected_icon].device_id;
    req.menu_idx  = menu_idx;

    dob_msg_t m;
    memset(&m, 0, sizeof(m));
    m.code         = MENU_ACTIVATED;
    m.payload      = &req;
    m.payload_size = sizeof(req);
    dob_ipc_post(hp, &m);
}

/* Clic su un'icona: doppio -> attivazione via hotplug; singolo ->
 * selezione + pannello contestuale dal menu DAS. */
void handle_icon_click(int idx)
{
    uint32_t now = (uint32_t)clock_ms();
    bool is_double = (idx == dblc_icon_last_idx)
                  && ((now - dblc_icon_last_ms) <= ICON_DBLCLICK_MS);

    if (is_double)
    {
        dblc_icon_last_ms  = 0;
        dblc_icon_last_idx = -1;
        send_icon_activated(desktop_icons[idx].device_id);
        return;
    }

    dblc_icon_last_ms  = now;
    dblc_icon_last_idx = idx;

    /* Deseleziona la precedente, seleziona questa. */
    for (int i = 0; i < MAX_DESKTOP_ICONS; i++)
        desktop_icons[i].selected = false;
    desktop_icons[idx].selected = true;
    selected_icon = idx;

    icon_show_context_panel(idx);
    di_mark_dirty(DIRTY_FULL);
}
