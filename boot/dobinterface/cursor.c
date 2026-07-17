/* MainDOB DobInterface 2.0 — foglio: cursor.c
 *
 * Il puntatore. Due percorsi, scelti a init:
 *
 *  - HARDWARE (DV_CAP_HW_CURSOR, es. mach64): lo sprite e' un overlay
 *    che il CRTC compone allo scanout; muoverlo e' una scrittura di
 *    registro via dv_cursor_set_position, SENZA dv_compose — il frame
 *    a buffer singolo non viene mai smontato solo per muovere il
 *    mouse.
 *
 *  - LAYER (es. BGA): surface CURSOR_W x CURSOR_H BGRA con alpha=0
 *    sui pixel trasparenti, layer a z=999 con use_pixel_alpha. Il
 *    cambio di forma ri-renderizza la surface; la mossa e' un
 *    dv_layer_update col nuovo dst.
 *
 * Gli SPRITE sono copiati VERBATIM dal main.c 1.x (b140), pixel per
 * pixel: sono specifica visiva [PX], non codice da riscrivere.
 * Valori: 0 = trasparente, 1 = nero (contorno), 2 = bianco (corpo).
 *
 * In fondo, la logica ad alto livello: cursor_for_position, l'albero
 * decisionale che sceglie la forma dalla posizione. */

#include "di_internal.h"

/* ================= stato condiviso (proprieta' di questo foglio) ====== */

int cursor_x = SCREEN_W_DEFAULT / 2;   /* ri-centrati/clampati al relayout */
int cursor_y = SCREEN_H_DEFAULT / 2;
int current_cursor = CURSOR_ARROW;

/* ================= stato privato ====================================== */

static dv_surface_t g_cursor_surf  = DV_HANDLE_NONE;
static dv_layer_t   g_cursor_layer = DV_HANDLE_NONE;

/* Vero quando il driver dichiara DV_CAP_HW_CURSOR (vedi testa). */
static bool g_hw_cursor          = false;
static int  g_hw_cursor_uploaded = -1;
static int  g_cursor_uploaded    = -1;   /* ultima forma caricata (layer) */

/* Staging BGRA dello sprite corrente. */
static uint32_t g_cursor_pixels[CURSOR_W * CURSOR_H];

/* ================= sprite (VERBATIM dall'1.x) ========================= */

static uint8_t cursor_bitmaps[CURSOR_COUNT][CURSOR_H][CURSOR_W] =
{
    /* CURSOR_ARROW */
    {
        { 1,0,0,0,0,0,0,0,0,0,0,0 },
        { 1,1,0,0,0,0,0,0,0,0,0,0 },
        { 1,2,1,0,0,0,0,0,0,0,0,0 },
        { 1,2,2,1,0,0,0,0,0,0,0,0 },
        { 1,2,2,2,1,0,0,0,0,0,0,0 },
        { 1,2,2,2,2,1,0,0,0,0,0,0 },
        { 1,2,2,2,2,2,1,0,0,0,0,0 },
        { 1,2,2,2,2,2,2,1,0,0,0,0 },
        { 1,2,2,2,2,2,2,2,1,0,0,0 },
        { 1,2,2,2,2,2,2,2,2,1,0,0 },
        { 1,2,2,2,2,2,1,1,1,1,1,0 },
        { 1,2,2,1,2,2,1,0,0,0,0,0 },
        { 1,2,1,0,1,2,2,1,0,0,0,0 },
        { 1,1,0,0,1,2,2,1,0,0,0,0 },
        { 1,0,0,0,0,1,2,2,1,0,0,0 },
        { 0,0,0,0,0,1,2,2,1,0,0,0 },
        { 0,0,0,0,0,0,1,1,0,0,0,0 },
    },
    /* CURSOR_RESIZE */
    {
        { 0,0,0,0,0,0,1,1,1,1,1,0 },
        { 0,0,0,0,0,0,0,1,2,2,1,0 },
        { 0,0,0,0,0,0,1,2,2,1,0,0 },
        { 0,0,0,0,0,1,2,2,1,0,0,0 },
        { 0,0,0,0,1,2,2,1,0,0,0,0 },
        { 0,0,0,1,2,2,1,0,0,0,0,0 },
        { 0,0,1,2,2,1,0,0,0,0,0,0 },
        { 0,1,2,2,1,0,0,0,0,0,0,0 },
        { 1,2,2,1,0,0,0,0,0,0,0,0 },
        { 1,2,1,0,0,0,0,0,0,0,0,0 },
        { 1,1,1,1,1,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0,0,0,0 },
    },
    /* CURSOR_FILE_DRAG -- documento con angolo alto-destro piegato */
    {
        { 1,1,1,1,1,1,1,0,0,0,0,0 },
        { 1,2,2,2,2,2,1,1,0,0,0,0 },
        { 1,2,2,2,2,2,2,1,0,0,0,0 },
        { 1,2,2,2,2,2,1,1,1,0,0,0 },
        { 1,2,2,2,2,2,2,2,1,0,0,0 },
        { 1,2,2,2,2,2,2,2,1,0,0,0 },
        { 1,2,2,1,1,1,2,2,1,0,0,0 },
        { 1,2,2,2,2,2,2,2,1,0,0,0 },
        { 1,2,2,1,1,1,2,2,1,0,0,0 },
        { 1,2,2,2,2,2,2,2,1,0,0,0 },
        { 1,2,2,1,1,1,2,2,1,0,0,0 },
        { 1,2,2,2,2,2,2,2,1,0,0,0 },
        { 1,2,2,2,2,2,2,2,1,0,0,0 },
        { 1,2,2,2,2,2,2,2,1,0,0,0 },
        { 1,2,2,2,2,2,2,2,1,0,0,0 },
        { 1,1,1,1,1,1,1,1,1,0,0,0 },
        { 0,0,0,0,0,0,0,0,0,0,0,0 },
    },
    /* CURSOR_HSPLIT -- doppia freccia orizzontale <--> per splitter di
     * colonna e ogni elemento UI che divide spazio orizzontale. Hot
     * spot al centro geometrico della freccia (circa riga 8, col 6),
     * cosi' il "punto di presa" apparente cade esattamente sulla
     * linea del divisore. */
    {
        { 0,0,0,0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0,0,0,0 },
        { 0,0,0,1,0,0,0,0,1,0,0,0 },
        { 0,0,1,2,1,0,0,1,2,1,0,0 },
        { 0,1,2,2,1,0,0,1,2,2,1,0 },
        { 1,2,2,2,1,1,1,1,2,2,2,1 },
        { 1,2,2,2,2,2,2,2,2,2,2,1 },
        { 1,2,2,2,1,1,1,1,2,2,2,1 },
        { 0,1,2,2,1,0,0,1,2,2,1,0 },
        { 0,0,1,2,1,0,0,1,2,1,0,0 },
        { 0,0,0,1,0,0,0,0,1,0,0,0 },
        { 0,0,0,0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0,0,0,0,0 },
    },
    /* CURSOR_VSPLIT -- doppia freccia verticale per splitter di riga /
     * margine (margini pagina alto e basso). Specchio di HSPLIT
     * ruotato di 90 gradi, stesso ancoraggio alto-sinistro (hotspot
     * 0,0) cosi' il punto di presa apparente cade sulla linea del
     * divisore, coerente col cursore di split orizzontale. */
    {
        { 0,0,0,0,0,1,1,0,0,0,0,0 },
        { 0,0,0,0,1,2,2,1,0,0,0,0 },
        { 0,0,0,1,2,2,2,2,1,0,0,0 },
        { 0,0,1,2,2,2,2,2,2,1,0,0 },
        { 0,1,2,2,2,2,2,2,2,2,1,0 },
        { 1,2,2,2,2,2,2,2,2,2,2,1 },
        { 1,1,1,1,2,2,2,2,1,1,1,1 },
        { 0,0,0,1,2,2,2,2,1,0,0,0 },
        { 0,0,0,1,2,2,2,2,1,0,0,0 },
        { 0,0,0,1,2,2,2,2,1,0,0,0 },
        { 1,1,1,1,2,2,2,2,1,1,1,1 },
        { 1,2,2,2,2,2,2,2,2,2,2,1 },
        { 0,1,2,2,2,2,2,2,2,2,1,0 },
        { 0,0,1,2,2,2,2,2,2,1,0,0 },
        { 0,0,0,1,2,2,2,2,1,0,0,0 },
        { 0,0,0,0,1,2,2,1,0,0,0,0 },
        { 0,0,0,0,0,1,1,0,0,0,0,0 },
    },
};

/* ================= verbi esecutivi ==================================== */

/* Rasterizza lo sprite `type` in g_cursor_pixels (0 -> trasparente,
 * 1 -> nero opaco, 2 -> bianco opaco). Unico punto di conversione:
 * nell'1.x il doppio ciclo era duplicato nei due percorsi. */
static void cursor_rasterize(int type)
{
    const uint8_t (*bmp)[CURSOR_W] = cursor_bitmaps[type];
    for (int row = 0; row < CURSOR_H; row++)
        for (int col = 0; col < CURSOR_W; col++)
        {
            uint8_t p = bmp[row][col];
            uint32_t v;
            if (p == 0)      v = 0;                       /* trasparente */
            else if (p == 1) v = 0xFF000000u | COLOR_BLACK;
            else             v = 0xFF000000u | COLOR_WHITE;
            g_cursor_pixels[row * CURSOR_W + col] = v;
        }
}

/* Descrittore del layer puntatore alla posizione corrente (unico
 * punto in cui la forma del layer e' scritta: init e mossa). */
static dv_layer_desc_t cursor_layer_desc(void)
{
    dv_layer_desc_t ld = {
        .source           = g_cursor_surf,
        .z                = 999,
        .alpha            = 255,
        .visible          = true,
        .use_pixel_alpha  = true,
        .src_rect         = { 0, 0, CURSOR_W, CURSOR_H },
        .dst_rect         = { cursor_x, cursor_y, CURSOR_W, CURSOR_H },
    };
    return ld;
}

/* ================= verbi pubblici del foglio ========================== */

/* Carica la forma corrente se e' cambiata dall'ultimo upload.
 * Percorso HW: dv_cursor_set_bitmap; percorso layer: update_region
 * sulla surface del puntatore. No-op se la forma e' invariata. */
void cursor_upload_if_needed(void)
{
    if (g_hw_cursor)
    {
        if (current_cursor == g_hw_cursor_uploaded) return;  /* invariata */
        if (current_cursor < 0 || current_cursor >= CURSOR_COUNT) return;
        cursor_rasterize(current_cursor);
        dv_cursor_desc_t cd = {
            .width = CURSOR_W, .height = CURSOR_H,
            .hotspot_x = 0, .hotspot_y = 0,
            .format = DV_FMT_BGRA8888,
            .pixels = g_cursor_pixels,
        };
        if (dv_cursor_set_bitmap(0, &cd) == DV_OK)
            g_hw_cursor_uploaded = current_cursor;
        return;
    }

    if (g_cursor_surf == DV_HANDLE_NONE) return;
    if (current_cursor == g_cursor_uploaded) return;         /* invariata */
    if (current_cursor < 0 || current_cursor >= CURSOR_COUNT) return;

    cursor_rasterize(current_cursor);
    dv_rect_t r = { 0, 0, CURSOR_W, CURSOR_H };
    dv_texture_update_region((dv_texture_t)g_cursor_surf, r,
                             g_cursor_pixels, CURSOR_W * 4);
    g_cursor_uploaded = current_cursor;
}

/* Muove il puntatore a (cursor_x, cursor_y). Economico: registro sul
 * percorso HW, dv_layer_update col nuovo dst sul percorso layer.
 * Chiamata da handle_mouse_move e ovunque cursor_x/y cambino. */
void cursor_layer_update_pos(void)
{
    if (g_hw_cursor)
    {
        cursor_upload_if_needed();   /* la forma puo' essere cambiata in hover */
        dv_cursor_set_position(0, cursor_x, cursor_y);
        return;
    }
    if (g_cursor_layer == DV_HANDLE_NONE) return;
    dv_layer_desc_t ld = cursor_layer_desc();
    dv_layer_update(g_cursor_layer, &ld);
}

/* Porta su il puntatore. Da video_init, dopo surface+layer del
 * backbuf. Il fallimento rende il puntatore invisibile sotto
 * windows-as-layers: si logga e si continua (brutto, non fatale). */
void cursor_init_video(void)
{
    /* Preferisci il puntatore hardware quando il driver lo offre:
     * elimina del tutto la ricomposizione per-mossa (overlay CRTC
     * allo scanout). */
    uint64_t caps = 0;
    if (dv_cap_query(&caps) == DV_OK && (caps & DV_CAP_HW_CURSOR))
    {
        g_hw_cursor = true;
        cursor_upload_if_needed();   /* spinge il bitmap via set_bitmap */
        dv_cursor_set_position(0, cursor_x, cursor_y);
        dv_cursor_show(0);
        debug_print("[dobinterface] hardware cursor active.\n");
        return;
    }

    dv_surface_desc_t sd = {
        .width  = CURSOR_W,
        .height = CURSOR_H,
        .format = DV_FMT_BGRA8888,
        .flags  = DV_SURF_FLAG_RENDERTARGET,
    };
    if (dv_surface_create(g_vproc, &sd, &g_cursor_surf) != DV_OK)
    {
        debug_print("[dobinterface] cursor surface create failed.\n");
        g_cursor_surf = DV_HANDLE_NONE;
        return;
    }

    dv_layer_desc_t ld = cursor_layer_desc();
    if (dv_layer_create(g_vproc, &ld, &g_cursor_layer) != DV_OK)
    {
        dv_surface_destroy(g_cursor_surf);
        g_cursor_surf  = DV_HANDLE_NONE;
        g_cursor_layer = DV_HANDLE_NONE;
        return;
    }

    cursor_upload_if_needed();
    debug_print("[dobinterface] cursor layer up (z=999, pixel-alpha).\n");
}

/* ================= logica ad alto livello ============================= */

/* cursor_for_position — quale forma a (mx, my). Albero decisionale:
 * dal contesto piu' esterno (pannello, overlay, tray) verso il piu'
 * interno (finestra, zona, override del client). */
int cursor_for_position(int mx, int my)
{
    if (mx >= panel_x)
        return CURSOR_ARROW;

    /* Gli overlay modali possiedono il puntatore. In Mission Control
     * (mc_active) o About (about_overlay_active) le finestre sotto
     * sono nascoste dall'overlay ma ancora presenti nella lista —
     * hit_test_window riporterebbe allegramente i loro bordi resize,
     * facendo tremolare il puntatore in CURSOR_RESIZE quando passa su
     * una miniatura che capita sopra il bordo di una finestra
     * sepolta. Sopprimi tutta la logica per-finestra in quei modi;
     * l'overlay gestisce da se' le sue affordance di hover se gli
     * servono. */
    if (mc_active || about_overlay_active)
        return CURSOR_ARROW;

    /* Il tray sta SOPRA le finestre (z maggiore) ma, come gli overlay
     * sopra, non e' nella lista finestre. Quando e' aperto e il
     * puntatore e' sul suo rettangolo, le finestre sotto sono coperte
     * — eppure hit_test_window riporterebbe il bordo resize di una
     * finestra sepolta, girando il puntatore in CURSOR_RESIZE sopra
     * il tray. Sopprimi la logica per-finestra dentro il pannello;
     * fuori, le finestre sono davvero visibili e i loro bordi tengono
     * il normale cursore di resize. */
    if (widget_panel_open && wpanel_contains(mx, my))
        return CURSOR_ARROW;

    if (servo_is_active(&win_servo))
        return CURSOR_RESIZE;

    int zone = 0;
    int idx = hit_test_window(mx, my, &zone);
    if (idx >= 0 && zone == 2 && !windows[idx].no_resize
        && !windows[idx].maximized)
        return CURSOR_RESIZE;

    /* Override per-finestra: il client puo' chiedere un cursore
     * custom (es. CURSOR_HSPLIT per uno splitter di colonna) valido
     * solo col mouse nel CORPO della finestra. Header (zona 1) e
     * bordi resize (zona 2) tengono i default del WM — quelle zone
     * appartengono al window manager, non al programma.
     *
     * Ciclo di vita: il campo si imposta via GUI_SET_CURSOR e si
     * azzera con lo stesso opcode (passando CURSOR_DEFAULT). Alla
     * distruzione della finestra lo slot si azzera e l'override
     * sparisce con lei — nessun cleanup esplicito lato client, anche
     * se il processo e' crashato a meta' drag. */
    if (idx >= 0 && zone == 0
        && windows[idx].cursor_override != CURSOR_DEFAULT)
        return windows[idx].cursor_override;

    return CURSOR_ARROW;
}

/* Il present decide diversamente col cursore hardware (registro, non
 * framebuffer): il main interroga questo verbo. */
bool cursor_is_hw(void)
{
    return g_hw_cursor;
}

/* Pixel dello sprite per le ricomposizioni software (screenshot):
 * 0 = trasparente, 1 = nero, 2 = bianco. Fuori range = 0. */
uint8_t cursor_sprite_pixel(int type, int row, int col)
{
    if (type < 0 || type >= CURSOR_COUNT) return 0;
    if (row < 0 || row >= CURSOR_H || col < 0 || col >= CURSOR_W) return 0;
    return cursor_bitmaps[type][row][col];
}
