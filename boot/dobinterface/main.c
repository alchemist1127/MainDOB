/* MainDOB DobInterface 2.0 — foglio: main.c
 *
 * L'algoritmo di boot del desktop e il ciclo eventi, in forma alta:
 * ogni passo e' un verbo di un foglio. L'ordine di boot e'
 * contratto:
 *
 *   1. priorita' alta (il compositor non deve mai essere affamato
 *      dai programmi utente);
 *   2. scratch di rendering (fatale se manca);
 *   3. video su (fatale sui core: vproc, backbuf — startup_modules
 *      col needs:video ci parcheggia finche' il driver BGA non e'
 *      registrato, quindi qui driver e slot boomerang sono vivi);
 *   4. pannello, finestre, servo;
 *   5. porta GUI + registry ("dobinterface");
 *   6. sottoscrizioni: inputd, hotplug (icone device), e SOLO POI
 *      settingsd (best-effort: un settingsd lento o assente non deve
 *      ritardare le connessioni sensibili alla latenza) e il piano
 *      di controllo video (opt-in e additivo: senza provider
 *      "DobVideoControl" si resta al default 1024x768, identico alle
 *      build precedenti);
 *   7. primo paint, poi il ciclo.
 *
 * Ciclo: PURO event-driven. Blocca su receive, si sveglia su
 * qualunque evento, drena il burst, processa, presenta, riblocca.
 * Zero polling, zero sleep_ms — zero CPU da fermo. Anche la morte
 * degli owner e' un evento (GUI_PEER_DIED dal death-watch del
 * kernel) mietuto al punto sicuro di inizio giro.
 *
 * La decisione di present interroga il dirty state (foglio dirty), in
 * ordine di forza:
 *   - di_dirty(FULL|PANEL) -> compositor_repaint (rebuild backbuf +
 *     flip pieno): il backbuf (sfondo, icone, pannello, overlay,
 *     geometria, z) e' cambiato. Il present consuma FULL|PANEL|CONTENT;
 *   - altrimenti di_dirty(CONTENT) -> compositor_content_blit: solo il
 *     corpo di una finestra e' cambiato. Le finestre sono layer
 *     separati dal backbuf: si ri-bakano solo loro e si fa il flip
 *     PIENO, saltando il rebuild del backbuf (il risparmio del
 *     keystroke). Il flip resta pieno perche' e' l'unico modello
 *     validato flash-free: il dirty-rect scissored (blit in-place
 *     nella pagina visibile) resta ritirato per il flash su ferro
 *     vero. Con overlay/anteprima resize attivi si ricade sul rebuild
 *     pieno (quei pixel vivono sul backbuf);
 *   - altrimenti, solo il cursore: col cursore hardware la mossa e'
 *     gia' nei registri (niente ricomposizione — e' tutto il senso
 *     dell'HW cursor); una forma nuova a mouse fermo
 *     (DIRTY_CURSOR) fa il solo upload. Col cursore software
 *     si ristampa via flip senza vsync — sia sulla mossa geometrica
 *     sia sulla forma cambiata da fermo, o l'utente dovrebbe
 *     scuotere il mouse per vederla. */

#include "di_internal.h"

/* ================= stato condiviso (proprieta' di questo foglio) ====== */

uint32_t gui_port = 0;   /* porta IPC di DobInterface (i programmi chiamano qui) */

int main(void)
{
    set_priority(1);
    debug_print("[dobinterface] Starting Dob_Interface desktop module...\n");

    /* Scratch di rendering — dimensionato al caso peggiore (desktop
     * pieno + bordo + chrome) cosi' ogni corpo finestra puo'
     * ri-renderizzare qui senza allocazioni per-finestra. Usato dal
     * raster delle icone e dall'upload dell'atlante glifi. */
    g_win_scratch = (uint32_t *)malloc((uint32_t)WIN_SCRATCH_PX * 4);
    if (!g_win_scratch)
    {
        debug_print("[dobinterface] FATAL: No win_scratch.\n");
        _exit(1);
    }

    video_init();

    /* Smoke test dv_*: un singolo dv_fill_rect subito dopo
     * video_init. Economico, una volta per boot, canarino che il
     * percorso boomerang + dispatch del driver e' sano. */
    {
        dv_rect_t r  = { 0, 0, 1, 1 };
        dv_color_t c = { 0, 0, 0, 0xFF };
        int rc = dv_fill_rect(g_backbuf_surf, r, c);
        char line[80];
        sprintf(line, "[dobinterface] dv_* smoke test: rc=%d.\n", rc);
        debug_print(line);
    }

    panel_init_items();
    panel_sync_context();

    memset(windows, 0, sizeof(windows));
    memset(widgets, 0, sizeof(widgets));

    servo_init(&win_servo, WIN_MIN_SIZE, WIN_HEADER_H, RESIZE_GRAB);

    gui_port = (uint32_t)port_create();
    dob_registry_register("dobinterface", gui_port);
    debug_print("[dobinterface] Registered in registry.\n");

    input_subscribe();

    /* hotplug possiede il database DAS e ci posta GUI_DEVICE_ATTACH
     * gia' iconificati. Noi siamo display muto: riceviamo icone e
     * rimandiamo i clic a hotplug. */
    {
        uint32_t hp = dob_registry_find("hotplug");
        if (hp)
        {
            dob_msg_t sub = {0};
            sub.code = HOTPLUG_SUBSCRIBE;
            sub.arg0 = gui_port;
            dob_ipc_post(hp, &sub);
            debug_print("[dobinterface] Subscribed to hotplug.\n");
        }
        else
        {
            debug_print("[dobinterface] hotplug service not found -- "
                        "no device icons this session.\n");
        }
    }

    /* Config del typed-text (dichiara ui.window_typing e legge on/off
     * + durata; i live edit arrivano poi via SETTINGS_RELOAD_CALL). */
    typed_settings_load();

    /* Config del foglio logon: timeout di inattivita' (FILE-class) e
     * la entry EPS SECRET del cambio password. Stesso ciclo di vita
     * del typed-text: dichiara idempotente, legge, live-reload
     * all'Applica. Arma anche il timer di inattivita'. */
    logon_settings_load();

    /* Piano di controllo video per i cambi modalita' live. Anche
     * contro un provider a modalita' fissa (mach64) la sottoscrizione
     * e' innocua: non emette mai MODE_CHANGED. Solo un provider che
     * annuncia una modalita' custom (pannello nativo x3100, o BGA
     * dopo uno switch da Impostazioni) innesca il relayout. */
    {
        uint32_t vc = dob_registry_find("DobVideoControl");
        if (vc)
        {
            if (dobvc_Subscribe(gui_port, DOBVC_EVENT_MODE_CHANGED) == DV_OK)
                debug_print("[dobinterface] Subscribed to DobVideoControl "
                            "(MODE_CHANGED).\n");
            else
                debug_print("[dobinterface] DobVideoControl subscribe failed "
                            "-- mode stays at default.\n");
        }
        else
        {
            debug_print("[dobinterface] No DobVideoControl provider -- "
                        "mode fixed at 1024x768 default.\n");
        }
    }

    /* Password di accesso: se /SYSTEM/CONFIG/Logon_password.dat
     * esiste, la tenda sale ORA — il primissimo frame che l'utente
     * vede e' la schermata di accesso, mai il desktop. La ISO live
     * non contiene il file: si va dritti al demo. */
    logon_boot_check();

    compositor_repaint();

    debug_print("[dobinterface] Desktop ready.\n");

    for (;;)
    {
        /* Punto sicuro: distruggi le finestre orfane scoperte dalla
         * consegna verificata nel giro precedente. */
        win_reap_dead_owners();

        /* Receive bloccante + drain del burst + instradamento mouse
         * completo (foglio input). */
        bool cursor_moved = input_pump();

        if (frame_flipped)
            continue;   /* un drag blit / flip ha gia' presentato questo tick */

        /* Il backbuf (sfondo, icone, pannello, overlay, geometria, z)
         * e' cambiato: rebuild pieno + flip. compositor_repaint
         * consuma da se' FULL|PANEL|CONTENT. */
        if (di_dirty(DIRTY_FULL | DIRTY_PANEL))
        {
            compositor_repaint();
            for (int i = 0; i < MAX_WINDOWS; i++)
                windows[i].content_dirty = false;
        }
        /* Solo il corpo di una finestra e' cambiato (keystroke, redraw
         * client): le finestre sono layer separati dal backbuf, quindi
         * si ri-bakano solo loro e si fa il flip PIENO, saltando il
         * rebuild del backbuf. Eccezione: con un overlay o l'anteprima
         * resize attivi quei pixel vivono sul backbuf e serve il
         * rebuild pieno. */
        else if (di_dirty(DIRTY_CONTENT))
        {
            if (mc_active || about_overlay_active || servo_is_active(&win_servo))
                compositor_repaint();
            else
                compositor_content_blit();
            for (int i = 0; i < MAX_WINDOWS; i++)
                windows[i].content_dirty = false;
        }
        else if (cursor_moved || di_dirty(DIRTY_CURSOR))
        {
            if (cursor_is_hw())
            {
                /* Mossa gia' applicata nei registri; una forma nuova
                 * a mouse fermo fa il solo upload dello sprite. */
                if (di_dirty(DIRTY_CURSOR))
                {
                    cursor_upload_if_needed();
                    di_dirty_clear(DIRTY_CURSOR);
                }
            }
            else
            {
                /* Ristampa del cursore software (mossa geometrica o
                 * forma nuova da fermo). */
                fb_flip_no_vsync();
                di_dirty_clear(DIRTY_CURSOR);
            }
        }
    }

    return 0;
}
