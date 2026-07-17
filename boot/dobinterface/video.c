/* MainDOB DobInterface 2.0 — foglio: video.c
 *
 * L'unica area che parla con la scheda video per lo schermo intero:
 * attach del vproc e quota VRAM, backbuffer fullscreen, compose e
 * page-flip con pacing a ~60 Hz, z del backbuf per gli overlay,
 * cambio di modalita' video (relayout dell'intero desktop).
 *
 * Struttura a blocchi: verbi esecutivi in alto, raccoglitori nella
 * terra di mezzo, i due algoritmi ad alto livello (video_init e
 * video_relayout_to) in fondo al foglio.
 *
 * Fix 1.x inglobati nel design (non piu' toppe):
 *  - quota VRAM risincronizzata da dv_vram_info (il default del
 *    driver, total/4, non basta a una finestra media su stdvga);
 *  - backbuf come SURFACE SYSRAM, non cmdlist: disegnato una volta
 *    per cambiamento, la compose blitta pixel finali; fa anche da
 *    sfondo desktop (clear a COLOR_BG), niente layer bg separato;
 *  - pacing a FRAME_INTERVAL_MS con timer one-shot: il VRETRACE
 *    rotto dello stdvga QEMU non fa piu' tearing, no-op su BGA vero;
 *  - flip del puntatore senza vsync (un bordo strappato su un 12x17
 *    e' invisibile, 16.6 ms a mossa si accumulano);
 *  - al cambio modo la surface del backbuf viene ricreata con ordine
 *    crea -> rebind -> distruggi, cosi' il layer non resta mai senza
 *    sorgente. */

#include "di_internal.h"

/* ================= stato condiviso (proprieta' di questo foglio) ====== */

/* Modalita' corrente. Default 1024x768: vale finche' il piano di
 * controllo video non spinge una modalita' diversa (video_on_mode_changed). */
int g_screen_w = 1024;
int g_screen_h = 768;

/* ================= stato del foglio (privato) ========================= */

dv_vproc_t   g_vproc        = DV_HANDLE_NONE;   /* condiviso (header)    */
dv_surface_t g_backbuf_surf = DV_HANDLE_NONE;   /* condiviso (header)    */
bool         frame_flipped  = false;            /* condiviso (header)    */

static dv_layer_t g_backbuf_layer = DV_HANDLE_NONE;

/* z corrente del layer backbuf. Con un overlay fullscreen attivo
 * (MC, About) il backbuf sale sopra i layer finestra (z=10+z_order,
 * tipicamente 10..20) cosi' l'overlay copre le finestre; 0 quando
 * nessun overlay e' attivo. Il puntatore sta a z=999; il bump da
 * overlay usa z=50. Tracciato per non riempire dv_layer_update di
 * scritture no-op. */
static int g_backbuf_z_current = 0;

/* --- Frame pacing ---
 * Lo stdvga di QEMU ha il bit VRETRACE rotto: il wait_for_vblank del
 * driver BGA ritorna subito invece di attendere ~16.6 ms, e i
 * dv_page_flip senza freno producono tearing visibile. Gate a ~60 Hz
 * su clock_ms(): un flip arrivato troppo presto alza flip_deferred e
 * arma un timer one-shot; un flip vero successivo lo abbassa e lo
 * scatto del timer diventa un no-op. Su hardware BGA reale (vblank
 * funzionante) l'intero blocco e' un no-op. */
#define FRAME_INTERVAL_MS   16
static uint32_t pacing_last_flip_ms = 0;
static int      pacing_timer_id     = -1;
static bool     flip_deferred       = false;

/* Pool di scratch texture lato server per i record OP_BLIT_INLINE.
 *
 * Ogni blit inline carica il proprio raster in una scratch e accoda
 * un blit differito che la referenzia. L'esecuzione della cmdlist
 * avviene DOPO, quindi una singola scratch condivisa si rompe quando
 * piu' blit inline si accumulano in un frame: ogni blit accodato
 * legge l'ultimo raster caricato (tutte le icone identiche —
 * osservato sul ribbon di DobFiles). Round-robin di
 * BLIT_SCRATCH_COUNT slot: le letture accodate atterrano su texture
 * distinte e la race upload-poi-accoda sparisce.
 *
 * Il percorso inline non e' piu' usato in esercizio (la soglia
 * client e' 0: ogni BlitBuffer passa dal texture pool), quindi UNO
 * slot basta; l'opcode resta per diagnostica o casi speciali futuri.
 * Era 48 quando il percorso portava il traffico icone: liberare i
 * 192 KiB e' la mossa giusta su un adapter da 4 MiB. */
#define BLIT_SCRATCH_DIM     32u
#define BLIT_SCRATCH_COUNT   1u
static dv_texture_t blit_scratch_tex[BLIT_SCRATCH_COUNT];
static uint32_t     blit_scratch_next  = 0;
static bool         blit_scratch_ready = false;

/* ================= verbi esecutivi ==================================== */

/* Attach del vproc. Richiesta iniziale piccola e generosa (16 MiB):
 * vale finche' la risincronizzazione quota non subentra. Fatale. */
static void vproc_attach_or_die(void)
{
    dv_vproc_attach_desc_t vd = {
        .vram_quota_bytes = 16ull * 1024 * 1024,
        .max_vthreads     = 4,
        .flags            = 0,
    };
    int rc = dv_vproc_attach(&vd, &g_vproc);
    if (rc != DV_OK)
    {
        char line[96];
        sprintf(line, "[dobinterface] FATAL: dv_vproc_attach failed (rc=%d).\n", rc);
        debug_print(line);
        _exit(1);
    }
}

/* Quota dal VRAM realmente libero. Il driver alloca le sue pagine di
 * scanout prima del nostro attach, quindi vi.free_bytes le esclude
 * gia' ed e' corretto a singolo o doppio buffer. Reclamiamo tutto
 * meno un piccolo pad; su fallimento resta il grant iniziale. */
static void vram_quota_sync(void)
{
    dv_vram_info_t vi = { 0 };
    if (dv_vram_info(g_vproc, &vi) != DV_OK || vi.free_bytes == 0) return;

    uint64_t pad  = 256ull * 1024;
    uint64_t want = (vi.free_bytes > pad) ? vi.free_bytes - pad
                                          : vi.free_bytes;
    int qrc = dv_vproc_set_quota(g_vproc, want);
    char line[96];
    sprintf(line, "[dobinterface] VRAM free=%u KiB, quota=%u KiB (rc=%d).\n",
            (unsigned)(vi.free_bytes / 1024),
            (unsigned)(want / 1024), qrc);
    debug_print(line);
}

/* Crea la surface fullscreen del backbuf (SYSRAM) gia' pulita a
 * COLOR_BG. Ritorna DV_HANDLE_NONE su fallimento. */
static dv_surface_t backbuf_surface_create(int *rc_out)
{
    dv_surface_desc_t sd = {
        .width  = SCREEN_W,
        .height = SCREEN_H,
        .format = DV_FMT_BGRA8888,
        .flags  = DV_SURF_FLAG_SYSRAM,
    };
    dv_surface_t s = DV_HANDLE_NONE;
    int rc = dv_surface_create(g_vproc, &sd, &s);
    if (rc_out) *rc_out = rc;
    if (rc != DV_OK) return DV_HANDLE_NONE;
    dv_surface_clear(s, dv_color_from_u32(COLOR_BG));
    return s;
}

/* Descrittore del layer backbuf per lo z dato (unico punto in cui la
 * forma del layer e' scritta: create, set_z e relayout la riusano). */
static dv_layer_desc_t backbuf_layer_desc(int z)
{
    dv_layer_desc_t ld = {
        .source          = g_backbuf_surf,
        .z               = z,
        .alpha           = 255,
        .visible         = true,
        .use_pixel_alpha = false,
        .src_rect        = { 0, 0, SCREEN_W, SCREEN_H },
        .dst_rect        = { 0, 0, SCREEN_W, SCREEN_H },
        .cmdlist         = DV_HANDLE_NONE,
    };
    return ld;
}

/* Inizializza il pool di scratch per i blit inline. Best-effort:
 * ready anche se qualche slot fallisce — win_v_blit_inline guarda
 * DV_HANDLE_NONE per slot, e un pool parziale batte nessun pool. */
static void blit_scratch_pool_init(void)
{
    if (blit_scratch_ready) return;

    for (uint32_t i = 0; i < BLIT_SCRATCH_COUNT; i++)
        blit_scratch_tex[i] = DV_HANDLE_NONE;

    dv_texture_desc_t td = {
        .width      = BLIT_SCRATCH_DIM,
        .height     = BLIT_SCRATCH_DIM,
        .format     = DV_FMT_BGRA8888,
        .mip_levels = 1,
        /* SYSRAM: questa texture viene LETTA dalla CPU quando la si
         * blitta nei corpi finestra (che sono SYSRAM). Su hardware
         * vero le letture dall'aperture VRAM viaggiano a 10-25 MB/s
         * — in RAM di sistema la stessa lettura e' centinaia di
         * MB/s. Vale per tutte le texture consumate dal bake. */
        .flags      = DV_TEX_FLAG_DYNAMIC | DV_SURF_FLAG_SYSRAM,
    };
    for (uint32_t i = 0; i < BLIT_SCRATCH_COUNT; i++)
        if (dv_texture_create(g_vproc, &td, &blit_scratch_tex[i]) != DV_OK)
            blit_scratch_tex[i] = DV_HANDLE_NONE;

    blit_scratch_ready = true;
}

/* Prossima scratch del round-robin per un blit inline (verbo usato
 * da cmdbuf.c; DV_HANDLE_NONE se il pool manca del tutto). Cammina
 * l'anello in avanti fino a COUNT slot per trovarne uno vivo: in
 * pratica gira solo la prima iterazione — il giro conta solo se
 * qualche slot ha fallito l'allocazione all'avvio. Il pool resta
 * incapsulato qui: nell'1.x era una tripla di globali sparse. */
dv_texture_t video_blit_scratch_next(uint32_t *dim_out)
{
    if (dim_out) *dim_out = BLIT_SCRATCH_DIM;
    if (!blit_scratch_ready) return DV_HANDLE_NONE;
    for (uint32_t tries = 0; tries < BLIT_SCRATCH_COUNT; tries++)
    {
        uint32_t idx = blit_scratch_next;
        blit_scratch_next = (blit_scratch_next + 1u) % BLIT_SCRATCH_COUNT;
        if (blit_scratch_tex[idx] != DV_HANDLE_NONE)
            return blit_scratch_tex[idx];
    }
    return DV_HANDLE_NONE;
}

/* Referto VRAM post-init (diagnostica di avvio). */
static void vram_report(const char *when)
{
    dv_vram_info_t vi = { 0 };
    if (dv_vram_info(g_vproc, &vi) != DV_OK) return;
    char line[96];
    sprintf(line, "[dobinterface] VRAM %s: total=%u KiB, free=%u KiB.\n",
            when, (unsigned)(vi.total_bytes / 1024),
            (unsigned)(vi.free_bytes  / 1024));
    debug_print(line);
}

/* ================= terra di mezzo: compose e pacing =================== */

/* Compose + present con gate di pacing. vsync=false e' il percorso
 * del puntatore (vedi nota di testa). */
static void fb_flip_internal(bool vsync)
{
    uint32_t now_ms = (uint32_t)clock_ms();
    uint32_t since  = now_ms - pacing_last_flip_ms;

    /* Difensivo contro un timer id stantio (drenato ma mai armato). */
    if (since > 1000) pacing_timer_id = -1;

    if (since < FRAME_INTERVAL_MS)
    {
        flip_deferred = true;
        if (pacing_timer_id < 0)
        {
            uint32_t delay = FRAME_INTERVAL_MS - since;
            int tid = timer_set(gui_port, delay, 0);
            if (tid >= 0) pacing_timer_id = tid;
        }
        return;
    }

    flip_deferred = false;
    pacing_timer_id = -1;

    cursor_upload_if_needed();
    cursor_layer_update_pos();

    dv_compose(0, DV_HANDLE_NONE);
    dv_page_flip(0, vsync ? DV_FLIP_VSYNC : 0, DV_HANDLE_NONE);
    frame_flipped = true;
    pacing_last_flip_ms = (uint32_t)clock_ms();
}

/* ================= verbi pubblici del foglio ========================== */

void fb_flip(void)          { fb_flip_internal(true);  }

/* Solo-puntatore: nessun cambio di contenuto, ricomposizione col
 * layer del cursore alla nuova posizione, senza vsync. */
void fb_flip_no_vsync(void) { fb_flip_internal(false); }

/* Dal loop eventi, quando scatta il timer di pacing. */
void pacing_run_deferred_flip(void)
{
    if (!flip_deferred) return;
    flip_deferred = false;
    pacing_timer_id = -1;
    /* Retrodata last_flip cosi' il gate passa subito ma il PROSSIMO
     * flip resta distanziato di un intervallo pieno. */
    pacing_last_flip_ms = (uint32_t)clock_ms() - FRAME_INTERVAL_MS;
    fb_flip_internal(true);
}

/* Present del SOLO rettangolo (x,y,w,h) via compose parziale
 * scissored del driver. Per le interazioni che cambiano una regione
 * piccola e nota (un bottone cliccato, la barra comandi scrollata,
 * l'unione di un resize): il driver tocca solo quei pixel e il
 * pannello non scansiona mai un frame mezzo ricomposto — cio' che
 * uccide il flash see-through su hardware vero (il percorso
 * dv_compose pieno resta per i cambi frame-interi).
 *
 * Niente pacing: un update parziale e' economico e l'obiettivo e'
 * l'immediatezza. Il puntatore (HW cursor su mach64) e' indipendente
 * dalla compose; cursor_upload_if_needed tiene aggiornato lo sprite
 * se il bitmap e' cambiato. Le coordinate le clampa il driver:
 * passare un rect che sborda dallo schermo e' sicuro. */
void fb_flip_rect(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    cursor_upload_if_needed();
    dv_compose_rect(0, x, y, (uint32_t)w, (uint32_t)h, DV_HANDLE_NONE);
    frame_flipped = true;
}

/* z del layer backbuf (bump da overlay). No-op se invariato. */
void backbuf_set_z(int new_z)
{
    if (g_backbuf_layer == DV_HANDLE_NONE) return;
    if (new_z == g_backbuf_z_current) return;
    g_backbuf_z_current = new_z;
    dv_layer_desc_t ld = backbuf_layer_desc(new_z);
    dv_layer_update(g_backbuf_layer, &ld);
}

/* ================= logica ad alto livello ============================= */

/* video_relayout_to — rilayout dell'intero desktop per una nuova
 * risoluzione. Chiamata solo da video_on_mode_changed, che ha gia'
 * validato le dimensioni e verificato che siano davvero cambiate.
 *
 * Copre le risorse a misura di schermo possedute dal compositor:
 * scratch di raster, i layer fullscreen (backbuf, toast, tray), le
 * strip dell'anteprima servo, la geometria pannello/desktop e il
 * puntatore. Le finestre aperte NON vengono ri-clampate o
 * ridimensionate qui (vedi nota in video_on_mode_changed): su un
 * grow restano valide, ed e' il caso che il modello push produce in
 * pratica. */
static void video_relayout_to(int new_w, int new_h)
{
    char line[96];
    sprintf(line, "[dobinterface] relayout %dx%d -> %dx%d.\n",
            g_screen_w, g_screen_h, new_w, new_h);
    debug_print(line);

    int old_px = (g_screen_w + 2) * (g_screen_h + WIN_HEADER_H + 2);
    int new_px = (new_w     + 2) * (new_h     + WIN_HEADER_H + 2);

    g_screen_w = new_w;
    g_screen_h = new_h;

    /* (1) Cresci lo scratch di raster se il desktop nuovo e' piu'
     * grande. I percorsi raster finestra/icona assumono che copra una
     * surface a tutto desktop; un buffer rimasto piccolo
     * sborderebbe. Su fallimento di allocazione si tiene il vecchio
     * piuttosto che lasciare un NULL. */
    if (new_px > old_px)
    {
        uint32_t *ns = (uint32_t *)malloc((uint32_t)new_px * 4);
        if (ns) { free(g_win_scratch); g_win_scratch = ns; }
        else debug_print("[dobinterface] WARN: scratch grow failed; "
                         "large-window raster may clip.\n");
    }

    /* (2) Geometria pannello + desktop. panel_recalculate aggiorna
     * desktop_w/panel_x solo quando panel_w stesso cambia (e'
     * content-driven e su un puro cambio di risoluzione di solito
     * non cambia): fissali prima esplicitamente, poi lasciale
     * riapplicare il cap SCREEN_W/4. wpanel_calc_geometry ricolloca
     * il tray. */
    desktop_w = SCREEN_W - panel_w;
    panel_x   = desktop_w;
    panel_recalculate();
    panel_sync_context();
    wpanel_calc_geometry();

    /* Ri-slotta le icone desktop per la nuova geometria: il numero
     * di righe dipende da SCREEN_H, e icone impacchettate per il
     * raster vecchio finirebbero fuori schermo (o ammucchiate male)
     * dopo il cambio modo. */
    icon_repack_slots();

    /* (3) Ridimensiona sul posto i layer a misura di schermo. La
     * surface del backbuf ha dimensioni fisse: va RICREATA
     * all'estensione nuova (ordine: crea -> rebind -> distruggi
     * vecchia, cosi' il layer non resta mai senza sorgente). Se la
     * create fallisce si tiene la vecchia: schermo coerente anche se
     * non a misura. Toast e tray sono layer a cmdlist: basta
     * aggiornare il dst_rect (delegato ai fogli proprietari). */
    if (g_backbuf_layer != DV_HANDLE_NONE)
    {
        dv_surface_t ns = backbuf_surface_create(NULL);
        if (ns != DV_HANDLE_NONE)
        {
            dv_surface_t old = g_backbuf_surf;
            g_backbuf_surf = ns;
            if (old != DV_HANDLE_NONE) dv_surface_destroy(old);
        }
        dv_layer_desc_t ld = backbuf_layer_desc(g_backbuf_z_current);
        dv_layer_update(g_backbuf_layer, &ld);
    }
    toast_layer_relayout();
    wpanel_layer_relayout();

    /* (4) Le strip dell'anteprima servo sono a misura di schermo:
     * ricostruiscile. */
    servo_preview_teardown();
    servo_preview_init_video();

    /* (5) Tieni il puntatore sullo schermo. */
    cursor_x = clamp(cursor_x, 0, SCREEN_W - 1);
    cursor_y = clamp(cursor_y, 0, SCREEN_H - 1);
    cursor_layer_update_pos();

    /* (6) Repaint pieno alla misura nuova. */
    di_mark_dirty(DIRTY_FULL);
    compositor_repaint();
}

/* video_on_mode_changed — e' arrivato DOBVC_EVENT_MODE_CHANGED.
 * L'evento porta larghezza/altezza ma il control plane e'
 * autoritativo: re-interroga. Valida contro il range che sappiamo
 * reggere e ignora i no-op; solo un cambio genuino guida il
 * relayout. */
void video_on_mode_changed(void)
{
    dv_mode_t m;
    if (dobvc_ModeGet(0, &m) != DV_OK) return;

    int w = (int)m.width;
    int h = (int)m.height;

    if (w < SCREEN_MIN_W || h < SCREEN_MIN_H ||
        w > SCREEN_MAX_DIM || h > SCREEN_MAX_DIM)
    {
        char l[96];
        sprintf(l, "[dobinterface] ignoring out-of-range mode %dx%d.\n", w, h);
        debug_print(l);
        return;
    }
    if (w == g_screen_w && h == g_screen_h) return;  /* niente da fare */

    video_relayout_to(w, h);
}

/* video_init — porta su la pipeline dv_*. Le risorse core (vproc,
 * surface e layer del backbuf) sono obbligatorie: fallire e' fatale.
 * Le secondarie (puntatore, atlante glifi, scratch blit, toast,
 * anteprima servo, tray) sono best-effort. */
void video_init(void)
{
    vproc_attach_or_die();
    vram_quota_sync();

    int rc = 0;
    g_backbuf_surf = backbuf_surface_create(&rc);
    if (g_backbuf_surf == DV_HANDLE_NONE)
    {
        char line[96];
        sprintf(line, "[dobinterface] FATAL: dv_surface_create(backbuf) "
                      "failed (rc=%d).\n", rc);
        debug_print(line);
        _exit(1);
    }

    dv_layer_desc_t ld = backbuf_layer_desc(0);
    rc = dv_layer_create(g_vproc, &ld, &g_backbuf_layer);
    if (rc != DV_OK)
    {
        char line[96];
        sprintf(line, "[dobinterface] FATAL: dv_layer_create(backbuf) "
                      "failed (rc=%d).\n", rc);
        debug_print(line);
        _exit(1);
    }

    debug_print("[dobinterface] dv_* pipeline up.\n");

    /* Risorse secondarie, best-effort, nell'ordine dell'1.x. */
    cursor_init_video();
    glyph_atlas_init();
    blit_scratch_pool_init();
    toast_init_video();
    servo_preview_init_video();
    wpanel_init_video();

    vram_report("after fixed surfaces");
}
