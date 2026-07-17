/* MainDOB DobInterface 2.0 — foglio: compositor.c
 *
 * Il compositor: finestre-come-layer e la scena del desktop.
 *
 * Modello (fix storici diventati architettura):
 *  - ogni finestra possiede una SURFACE del corpo in RAM di sistema
 *    (SYSRAM: zero VRAM, zero quota) e un layer del driver; il drag
 *    e' un update di dst_rect (zero rendering), lo z un update di z,
 *    show/hide un toggle di visibilita';
 *  - il corpo si disegna UNA volta per cambiamento (win_bake: chrome
 *    + replay del cmdbuf in draw DIRETTI); la compose blitta pixel
 *    finali — mai piu' replay di record nel buffer visibile, mai
 *    piu' tetti di capacita' da far crescere;
 *  - lo sfondo del corpo e' bianco e OPACO prima del replay: i pixel
 *    che il client non copre restano definiti (nel modello a cmdlist
 *    il non-disegnato lasciava trasparire i layer sotto: uno dei
 *    view-through storici);
 *  - il backbuf (surface fullscreen) e' contemporaneamente piano di
 *    disegno e sfondo desktop: clear a COLOR_BG in testa a ogni
 *    rebuild, niente layer bg separato da rigiocare a ogni compose;
 *  - overlay fullscreen (MC, About): bump dello z del backbuf a 50,
 *    sopra le finestre (10..) e sotto tray (120), toast (500) e
 *    cursore (999). NON coperti dal bump, per lezione appresa:
 *    l'anteprima resize (una volta bumpava e "tutte le finestre
 *    sparivano durante il resize"), il toast e il tray — ognuno sul
 *    proprio layer dedicato. */

#include "di_internal.h"

/* Lo stato "da presentare" del desktop (ex needs_*) vive ora nel
 * foglio dirty (dirty.c), stato privato dietro di_mark_dirty /
 * di_dirty / di_dirty_clear. */

/* ================= verbi esecutivi: video per-finestra ================ */

/* win_alloc_video — crea surface del corpo (SYSRAM) + body_layer per
 * la finestra. Da win_create e da win_apply_geometry sul resize. Il
 * fallimento lascia entrambi DV_HANDLE_NONE e la finestra resta
 * invisibile finche' un retry non riesce. */
void win_alloc_video(window_t *w)
{
    w->body_layer = DV_HANDLE_NONE;
    w->body_surf  = DV_HANDLE_NONE;
    w->panel_synced = false;   /* corpo nuovo: la prossima banda non basta */

    int sw = win_surf_w(w);
    int sh = win_surf_h(w);

    dv_surface_desc_t sd = {
        .width  = (uint32_t)sw,
        .height = (uint32_t)sh,
        .format = DV_FMT_BGRA8888,
        .flags  = DV_SURF_FLAG_SYSRAM,
    };
    int32_t rc = dv_surface_create(g_vproc, &sd, &w->body_surf);
    if (rc != DV_OK)
    {
        char line[80];
        sprintf(line, "[dobinterface] win surface alloc FAILED (rc=%d). "
                      "Window will be invisible.\n", rc);
        debug_print(line);
        w->body_surf = DV_HANDLE_NONE;
        return;
    }

    dv_layer_desc_t ld = {
        .source           = w->body_surf,
        .z                = win_layer_z(w),
        .alpha            = 255,
        .visible          = false,          /* mostrata dal compositor quando ready */
        .use_pixel_alpha  = false,
        .src_rect         = { 0, 0, (uint32_t)sw, (uint32_t)sh },
        .dst_rect         = { win_layer_x(w), win_layer_y(w),
                              (uint32_t)sw, (uint32_t)sh },
        .cmdlist          = DV_HANDLE_NONE,
    };
    if (dv_layer_create(g_vproc, &ld, &w->body_layer) != DV_OK)
    {
        dv_surface_destroy(w->body_surf);
        w->body_surf  = DV_HANDLE_NONE;
        w->body_layer = DV_HANDLE_NONE;
    }
}

/* win_free_video — smonta layer e surface del corpo.
 *
 * NOTA: last_cmdbuf deliberatamente NON viene liberato qui. E' il
 * contenuto logico della finestra (l'ultimo frame del client), non
 * una risorsa video: win_bake deve poter ricostruire il corpo dopo
 * un release/realloc del video SENZA aspettare un nuovo cmdbuf dal
 * client. Liberato in win_destroy. (Con le surface SYSRAM il release
 * sotto pressione VRAM non ha piu' motivo di esistere per i corpi
 * finestra: resta per compatibilita' col percorso di recovery, che
 * oggi non costa nulla evitare di toccare.)
 *
 * Il TEX POOL, per la stessa ragione, NON viene toccato: e' contenuto
 * logico (le icone della toolbar del client) che win_bake deve poter
 * ri-blittare al ripristino, e col backing SYSRAM tenerlo vivo costa
 * RAM, non VRAM. Chi DEVE distruggerlo (resize, destroy) chiama
 * win_tex_pool_free. */
void win_free_video(window_t *w)
{
    if (w->body_layer != DV_HANDLE_NONE)
    {
        dv_layer_destroy(w->body_layer);
        w->body_layer = DV_HANDLE_NONE;
    }
    if (w->body_surf != DV_HANDLE_NONE)
    {
        dv_surface_destroy(w->body_surf);
        w->body_surf = DV_HANDLE_NONE;
    }
}

/* win_tex_pool_free — distruzione del tex pool di una finestra. SOLO
 * per i percorsi in cui il client sa (o sapra') che le sue texture
 * sono morte: il resize (contratto: lo stub azzera il proprio mirror
 * su GUI_EVT_RESIZE e ri-alloca alla prima BlitBuffer) e la
 * distruzione della finestra. MAI su hide/minimizza: il client non
 * riceve alcun evento, il suo mirror resterebbe caldo (last_src==src
 * => niente re-upload) e i BLIT_TEX del last_cmdbuf punterebbero a
 * handle morti — icone della toolbar sparite per sempre dopo
 * riduci-a-icona + ripristino. */
void win_tex_pool_free(window_t *w)
{
    for (uint32_t i = 0; i < w->tex_pool_count; i++)
    {
        if (w->tex_pool[i].handle != DV_HANDLE_NONE)
            dv_texture_destroy(w->tex_pool[i].handle);
        w->tex_pool[i].handle = DV_HANDLE_NONE;
        free(w->tex_pool[i].cpu);
        w->tex_pool[i].cpu = NULL;
    }
    w->tex_pool_count = 0;
}

/* win_update_layer_pos — al movimento (drag, snap,
 * maximize/ripristino) o al cambio di readiness. Aggiorna dst_rect,
 * z e visibilita' del layer. Atomico alla prossima dv_compose.
 * Nessun rendering. */
void win_update_layer_pos(window_t *w)
{
    if (w->body_layer == DV_HANDLE_NONE) return;
    int sw = win_surf_w(w);
    int sh = win_surf_h(w);
    dv_layer_desc_t ld = {
        .source           = w->body_surf,
        .z                = win_layer_z(w),
        .alpha            = 255,
        .visible          = w->visible && w->ready,
        .use_pixel_alpha  = false,
        .src_rect         = { 0, 0, (uint32_t)sw, (uint32_t)sh },
        .dst_rect         = { win_layer_x(w), win_layer_y(w),
                              (uint32_t)sw, (uint32_t)sh },
        .cmdlist          = DV_HANDLE_NONE,
    };
    dv_layer_update(w->body_layer, &ld);
}

/* ================= terra di mezzo: bake del corpo ===================== */

/* Chrome della finestra sulla sua surface: bordo alto, sfondo header,
 * glifi del titolo, bordi laterali (che sovradisegnano la prima e
 * l'ultima colonna dell'header: e' la continuazione voluta del
 * bordo), bordo basso. Il colore del bordo segue focus e lampeggio
 * modale; l'header segue il focus. */
static void win_bake_chrome(window_t *w, int sw, int sh,
                            uint32_t border_col, uint32_t hdr_col,
                            uint32_t white_col)
{
    /* Bordo alto (1 px, larghezza piena). */
    surf_hline(w->body_surf, 0, 0, sw, border_col);

    /* Sfondo header: un solo fill a larghezza interna piena. */
    surf_fill_rect(w->body_surf, 1, 1, sw - 2, WIN_HEADER_H, hdr_col);

    /* Titolo — stesso layout proporzionale del testo generale
     * (string_to_glyphs, foglio font): un solo punto calcola gli
     * avanzamenti, cosi' titolo e corpo non possono divergere [PX]. */
    if (g_glyph_atlas != DV_HANDLE_NONE && w->title[0])
    {
        dv_glyph_t glyphs[64];
        int txt_y = 1 + (WIN_HEADER_H - FONT_H) / 2;
        uint32_t n = string_to_glyphs(w->title, 1 + 6, txt_y, glyphs, 64);
        if (n > 0)
            dv_draw_glyphs(w->body_surf, g_glyph_atlas, glyphs, n,
                           dv_color_from_u32(white_col));
    }

    /* Bordi laterali, alti quanto il corpo. */
    surf_vline(w->body_surf, 0,      1, sh - 2, border_col);
    surf_vline(w->body_surf, sw - 1, 1, sh - 2, border_col);
}

/* Sfondo del corpo — bianco di default, PRIMA del replay: i pixel che
 * il client non copre restano definiti e OPACHI.
 *
 * Fill PERIMETRALE quando c'e' un pannello SHM: il bianco serve solo
 * dove il pannello non arriva — le quattro fasce attorno al suo
 * rettangolo. Dentro, i pixel sono del pannello: copia integrale se
 * non sincronizzato, banda valida altrimenti. Cosi' il fill non
 * cancella cio' che il blit a banda NON ricopiera' (il baco dell'area
 * di lavoro bianca) e non marca sporco tutto il contenuto a ogni bake
 * (stesso principio dello skip_clear della compose). Nessun pannello
 * nel cmdbuf => fill pieno, comportamento storico. */
static void win_bake_body_background(window_t *w, dv_color_t white_c)
{
    shm_rect_scan_t pr = { .found = false };
    if (w->panel_shm_id >= 0 && w->panel_ptr && w->last_cmdbuf)
        cmdbuf_shmpanel_rect(w->last_cmdbuf, w->last_cmdbuf_size, &pr);
    if (pr.found)
    {
        /* Serra il rettangolo del pannello all'area contenuto
         * (coordinate body-content: 0,0 = dentro il chrome). */
        int px0 = pr.x < 0 ? 0 : pr.x;
        int py0 = pr.y < 0 ? 0 : pr.y;
        int px1 = pr.x + pr.w > (int)w->width  ? (int)w->width  : pr.x + pr.w;
        int py1 = pr.y + pr.h > (int)w->height ? (int)w->height : pr.y + pr.h;
        if (px0 >= px1 || py0 >= py1) { pr.found = false; }
        else
        {
            int bx = 1, by = 1 + WIN_HEADER_H;    /* origine contenuto */
            if (py0 > 0)                              /* fascia sopra    */
                dv_fill_rect(w->body_surf, (dv_rect_t){ bx, by,
                    (uint32_t)w->width, (uint32_t)py0 }, white_c);
            if (py1 < (int)w->height)                 /* fascia sotto    */
                dv_fill_rect(w->body_surf, (dv_rect_t){ bx, by + py1,
                    (uint32_t)w->width, (uint32_t)((int)w->height - py1) },
                    white_c);
            if (px0 > 0)                              /* fascia sinistra */
                dv_fill_rect(w->body_surf, (dv_rect_t){ bx, by + py0,
                    (uint32_t)px0, (uint32_t)(py1 - py0) }, white_c);
            if (px1 < (int)w->width)                  /* fascia destra   */
                dv_fill_rect(w->body_surf, (dv_rect_t){ bx + px1, by + py0,
                    (uint32_t)((int)w->width - px1), (uint32_t)(py1 - py0) },
                    white_c);
        }
    }
    if (!pr.found)
        dv_fill_rect(w->body_surf,
            (dv_rect_t){ 1, 1 + WIN_HEADER_H,
                         (uint32_t)w->width, (uint32_t)w->height },
            white_c);
}

/* win_bake — disegna la finestra nella sua surface: chrome, sfondo
 * del corpo, poi replay dell'ultimo cmdbuf del client in draw DIRETTI
 * (visitor win_v_*, in cmdbuf.c). Una volta per cambiamento; la
 * compose si limita a blittare pixel finali. */
void win_bake(window_t *w)
{
    if (w->body_surf == DV_HANDLE_NONE) return;

    int sw = win_surf_w(w);
    int sh = win_surf_h(w);
    uint32_t border_col = w->focused ? COLOR_CYAN : COLOR_BLACK;
    if (w->flash_active && flash_is_red((int)(w - windows)))
        border_col = COLOR_WIN_FLASH;
    uint32_t hdr_col    = w->focused ? COLOR_WIN_HEAD_ACTIVE : COLOR_WIN_HEAD;
    dv_color_t white_c  = dv_color_from_u32(COLOR_WHITE);   /* per il body background */

    win_bake_chrome(w, sw, sh, border_col, hdr_col, COLOR_WHITE);
    win_bake_body_background(w, white_c);

    /* Bordo basso. */
    surf_hline(w->body_surf, 0, sh - 1, sw, border_col);

    /* Replay dell'ultimo cmdbuf del client (se c'e'). L'header
     * (magic + reserved) e' gia' stato validato prima di memorizzare
     * last_cmdbuf: qui si salta la validazione.
     *
     * Reveal typed-text: finche' la finestra anima, passa la frazione
     * di progresso (trascorso / g_typed_ms) cosi' le run del corpo
     * vengono troncate da win_v_draw_text. reveal_den == 0 = "rendi
     * pieno" — sia a regime sia sull'ultimo frame a budget esaurito
     * (la pompa azzera type_anim_active subito dopo). */
    if (w->last_cmdbuf && w->last_cmdbuf_size > DOBUI_CMDBUF_HDR_SIZE)
    {
        uint32_t rv_num = 0, rv_den = 0;   /* 0/0 => nessun troncamento */
        if (w->type_anim_active)
        {
            uint32_t elapsed = (uint32_t)clock_ms() - w->type_anim_start_ms;
            if (elapsed < g_typed_ms)
            {
                rv_num = elapsed;
                rv_den = g_typed_ms;
            }
        }
        win_replay_cmdbuf(w, w->last_cmdbuf, w->last_cmdbuf_size,
                          rv_num, rv_den);
    }
}

/* Sincronizza lo stato lato driver di una finestra: rendering in
 * win_bake (solo se surface_dirty), posizione/z/visibilita' in
 * win_update_layer_pos. Saltare il rebuild quando non e' sporca E'
 * il fast path critico di focus, giri di z e drag. */
void compositor_blit_window(window_t *w)
{
    if (w->body_layer == DV_HANDLE_NONE) return;
    if (w->surface_dirty)
    {
        win_bake(w);
        w->surface_dirty = false;
    }
    win_update_layer_pos(w);
}

/* Overlay About — reso a ogni frame da compositor_rebuild finche'
 * about_overlay_active e' vero. Si chiude a qualunque clic (il
 * gestore input azzera il flag e chiede il repaint). */
static void about_draw(void)
{
    uint32_t ver[5] = {0};
    syscall1(SYS_GETVERSION, (int)ver);
    char version[32];
    sprintf(version, "%u.%u.%u.%u.%u", ver[0], ver[1], ver[2], ver[3], ver[4]);

    int aw = 320, ah = 140;
    int ax = (desktop_w - aw) / 2;
    int ay = (SCREEN_H - ah) / 2;

    fb_fill_rect(ax, ay, aw, ah, 0x000000AA);     /* sfondo blu    */
    fb_draw_rect(ax, ay, aw, ah, COLOR_WHITE, 1); /* bordo bianco  */

    font_draw_string(ax + 20, ay + 16, "MainDOB", COLOR_WHITE, 0x000000AA);

    char vline[80];
    sprintf(vline, "Versione: %s", version);
    font_draw_string(ax + 20, ay + 40, vline, COLOR_WHITE, 0x000000AA);

    font_draw_string(ax + 20, ay + 64,
                     "Copyright (C) Dob Systems & Technologies",
                     COLOR_CYAN, 0x000000AA);
    font_draw_string(ax + 20, ay + 100,
                     "[Clicca per chiudere]",
                     COLOR_TEXT_MUTE, 0x000000AA);
}

/* ================= logica ad alto livello ============================= */

/* compositor_rebuild — ricostruisce l'intero backbuf (sfondo, icone,
 * finestre, pannello, overlay) SENZA presentarlo. La presentazione e'
 * separata cosi' i chiamanti scelgono un fb_flip pieno o un
 * fb_flip_rect scissored per una regione sporca nota. */
static void compositor_rebuild(void)
{
    /* ===== Ramo tenda (foglio logon) =====
     * Con la tenda su, il backbuf E' il sipario: pulizia a NERO,
     * la sola UI del logon sopra, e z a LOGON_LAYER_Z — sopra
     * finestre (10..73), servo (100), tray (120) e toast (500),
     * sotto il solo cursore (999). NIENT'ALTRO viene disegnato:
     * non un pixel del desktop (icone, finestre, pannello) deve
     * trapelare. Il reflow da panel_width_changed resta in coda
     * (il flag non si consuma qui) e verra' servito dal primo
     * rebuild pieno dopo l'uscita dalla tenda. */
    if (logon_visible())
    {
        if (g_backbuf_surf != DV_HANDLE_NONE)
            dv_surface_clear(g_backbuf_surf, dv_color_from_u32(COLOR_BLACK));
        backbuf_set_z(LOGON_LAYER_Z);
        logon_draw();
        return;
    }

    /* Clear della surface backbuf a COLOR_BG in testa a ogni repaint
     * pieno: reset del piano di disegno e sfondo desktop insieme. */
    if (g_backbuf_surf != DV_HANDLE_NONE)
    {
        dv_surface_clear(g_backbuf_surf, dv_color_from_u32(COLOR_BG));
    }

    /* Se la larghezza del pannello e' cambiata, sistema le finestre
     * che sbordano dal nuovo desktop. Deve avvenire qui (non in
     * panel_recalculate): e' il compositor a possedere la scena. */
    if (panel_width_changed)
    {
        panel_width_changed = false;
        for (int i = 0; i < MAX_WINDOWS; i++)
        {
            if (!windows[i].used) continue;
            if (windows[i].maximized)
            {
                windows[i].x = 0;
                windows[i].y = 0;
                windows[i].width = desktop_w;
                windows[i].height = SCREEN_H - WIN_HEADER_H;
            }
            else if (windows[i].x + windows[i].width > desktop_w)
            {
                windows[i].x = desktop_w - windows[i].width;
                if (windows[i].x < 0) windows[i].x = 0;
            }
        }
    }

    /* z del backbuf: bump sopra le finestre quando un overlay
     * fullscreen reso sul backbuf deve stare in cima; 0 altrimenti.
     * Coperti: MC e About. NON coperti (lezioni in testa al foglio):
     * anteprima resize, toast, tray — layer dedicati. */
    bool overlay_active = mc_active
                       || about_overlay_active;
    backbuf_set_z(overlay_active ? 50 : 0);

    if (mc_active)
    {
        mc_draw();
        panel_draw();
    }
    else
    {
        /* Lo sfondo desktop e' il clear in testa: si parte dalle icone. */
        icon_draw_all();
        sort_windows_by_z();
        for (int i = 0; i < sorted_count; i++)
            compositor_blit_window(&windows[sorted_wins[i]]);

        servo_draw_preview();
        /* Il toast si compone da se' sul suo dv_layer (z=500). */
        panel_draw();
    }

    /* Tray: layer dedicato (z=120). Chiamato incondizionatamente:
     * quando il tray e' chiuso — incluso il frame in cui mc/about lo
     * chiudono — il layer viene nascosto. Da chiuso e' un solo check
     * di visibilita'. */
    wpanel_draw();

    /* About sopra tutto tranne mc (che e' il suo overlay fullscreen).
     * Dopo panel/wpanel cosi' nessuno lo taglia; lo z=50 fissato
     * sopra lo tiene sopra le finestre normali. */
    if (about_overlay_active)
        about_draw();
}

/* Repaint pieno: rebuild + present dell'intero schermo. Un present
 * pieno consuma tutte le ragioni di contenuto: e' l'unico azzeratore
 * di FULL|PANEL|CONTENT (main non ne ripulisce piu' per conto suo). */
void compositor_repaint(void)
{
    compositor_rebuild();
    fb_flip();
    di_dirty_clear(DIRTY_FULL | DIRTY_PANEL | DIRTY_CONTENT);
}

/* Presentazione parziale: oggi la catena di repaint promuove tutto a
 * full (eredita' delle cmdlist non editabili). Con le superfici il
 * dirty-rect torna praticabile: questa e' la base del prossimo
 * stadio. Tenuta fuori dal grafo delle chiamate di proposito. */
__attribute__((unused))
void compositor_repaint_rect(int x, int y, int w, int h)
{
    compositor_rebuild();
    fb_flip_rect(x, y, w, h);
    di_dirty_clear(DIRTY_FULL | DIRTY_PANEL | DIRTY_CONTENT);
}

/* compositor_drag_blit — sincronizza i layer durante il drag di una
 * finestra: il layer della trascinata viene ripuntato, e ANCHE ogni
 * altro layer vivo viene risincronizzato. Il resync previene il
 * flash "stratificazione cede": win_focus ha rinumerato gli z_order
 * ma solo lo z del layer trascinato e' stato spinto lato driver — le
 * altre finestre portano per un attimo z stantii che possono pareggiare
 * o invertirsi. Costo: al massimo MAX_WINDOWS boomerang extra
 * (sotto i 100 us). */
void compositor_drag_blit(int drag_idx)
{
    window_t *dw = &windows[drag_idx];
    if (dw->body_layer == DV_HANDLE_NONE) return;

    win_update_layer_pos(dw);

    for (int i = 0; i < MAX_WINDOWS; i++)
    {
        window_t *w = &windows[i];
        if (!w->used) continue;
        if (w->body_layer == DV_HANDLE_NONE) continue;
        if (w->surface_dirty)
        {
            win_bake(w);
            w->surface_dirty = false;
            w->content_dirty = false;
        }
        if (w != dw)
            win_update_layer_pos(w);
    }
    fb_flip();
    /* Il drag ha presentato: consuma le ragioni che copre (FULL e
     * CONTENT). PANEL resta intatto — un repaint pannello pendente
     * durante il drag non e' affare di questo present. */
    di_dirty_clear(DIRTY_FULL | DIRTY_CONTENT);
}

/* compositor_content_blit — present per un cambio di SOLO contenuto
 * (DIRTY_CONTENT senza FULL/PANEL): ri-baka le finestre col corpo
 * sporco e fa il flip PIENO, SENZA ricostruire il backbuf.
 *
 * Perche' e' corretto e sicuro:
 *  - le finestre sono layer separati dal backbuf; ri-bakare un corpo
 *    aggiorna solo la sua surface, la compose la ripesca. Il backbuf
 *    (sfondo, icone, pannello, overlay) NON e' cambiato in un tick di
 *    solo contenuto: saltarne il rebuild e' lecito, non un dirty-rect
 *    scissored (quello — blit in-place nella pagina visibile — resta
 *    ritirato per il flash su ferro vero);
 *  - il flip resta PIENO (fb_flip, page-flip): l'unico modello di
 *    present validato flash-free. Il risparmio e' il rebuild saltato
 *    (clear + icon_draw_all + panel_draw), non la compose;
 *  - stessa forma esatta del re-bake di compositor_drag_blit, gia' in
 *    esercizio: il pattern e' collaudato.
 *
 * Il chiamante (loop di main) garantisce che nessun overlay/anteprima
 * resize sia attivo — quei casi disegnano sul backbuf e vogliono il
 * rebuild pieno. */
void compositor_content_blit(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
    {
        window_t *w = &windows[i];
        if (!w->used) continue;
        if (w->body_layer == DV_HANDLE_NONE) continue;
        if (w->surface_dirty)
        {
            win_bake(w);
            w->surface_dirty = false;
            w->content_dirty = false;
        }
        /* Posizione/z stabili in un tick di solo contenuto (move,
         * resize, z e visibilita' passano tutti da DIRTY_FULL): niente
         * win_update_layer_pos. */
    }
    fb_flip();
    di_dirty_clear(DIRTY_CONTENT);
}
