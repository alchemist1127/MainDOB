/* MainDOB DobInterface 2.0 — foglio: input.c
 *
 * L'input: sottoscrizione a inputd, il router degli eventi della
 * porta unificata (dispatch_one), le tre sessioni di drag
 * (MOVE / RESIZE / FILES), e l'instradamento del mouse — press,
 * release, hover, cattura del puntatore, doppio clic, cancello
 * modale, scroll.
 *
 * Architettura del ciclo (invariata dall'1.x, ora come verbo unico
 * input_pump): il main deve solo chiamare win_reap_dead_owners,
 * input_pump, e decidere il present. Dentro input_pump: receive
 * bloccante + drain del burst (fino a 64 nowait, cosi' una raffica
 * di mouse diventa UN solo delta), poi l'instradamento.
 *
 * Cattura del puntatore: dal press a bottone giu', release e drag
 * vanno alla STESSA finestra anche se il cursore ne esce — le
 * sequenze multi-bottone restano instradate li' finche' tutti i
 * bottoni non sono su.
 *
 * Cancello modale a due stadi: il percorso del press (sinistro)
 * ingoia PRIMA delle zone, cosi' un clic sul bordo non inizia un
 * resize sulla finestra bloccata; il percorso di consegna copre
 * tutti i bottoni una volta sola — ding, lampeggio, modale avanti,
 * clic NON consegnato.
 *
 * Drag di file (sessione unica, posseduta dal WM per l'intera
 * durata): la sorgente chiama BeginDrag, il WM copia il payload in
 * un buffer proprio, traccia cursore e tasto Spazio (copia/sposta),
 * Esc annulla; al release il bersaglio riceve GUI_EVT_DROP col
 * payload inline (arg3 = porta eventi della sorgente, cosi' il
 * bersaglio puo' avvisarla a operazione FINITA — il GUI_EVT_DRAG_END
 * del WM parte al commit, troppo presto perche' la sorgente veda i
 * file cancellati di una MOVE). In ogni epilogo la sorgente riceve
 * GUI_EVT_DRAG_END e il buffer si libera. */

#include "di_internal.h"

/* ================= codici IPC di inputd (ABI) ========================= */

#define INPUT_MOUSE     50
#define INPUT_KEY       51
#define INPUT_MODCHANGE 52
#define INPUT_SUBSCRIBE 1

/* ================= stato privato ====================================== */

static uint8_t mouse_buttons = 0;

/* Doppio clic — centralizzato */
#define DBLCLICK_MS     500
#define DBLCLICK_RADIUS 8
static uint32_t dblc_last_ms = 0;
static int dblc_last_x = -100, dblc_last_y = -100;

/* Modalita' di drag */
#define DRAG_NONE       0
#define DRAG_MOVE       1
#define DRAG_RESIZE     2
#define DRAG_FILES      3

static int drag_mode = DRAG_NONE;
static int drag_win = -1;
static int drag_off_x = 0, drag_off_y = 0;

/* Sessione di drag file (una sola per volta). */
static uint32_t drag_files_src_win_id = 0;    /* id finestra sorgente     */
static uint32_t drag_files_src_port   = 0;    /* porta eventi sorgente    */
static void    *drag_files_payload    = NULL; /* malloc, formato di filo  */
static uint32_t drag_files_payload_sz = 0;
static bool     drag_files_copy_mode  = false; /* Spazio = copia          */

/* ================= verbi esecutivi: sessione drag file ================ */

/* Epilogo comune della sessione: DRAG_END alla sorgente (committed
 * dice se un drop e' stato consegnato), buffer liberato, stato
 * azzerato, cursore normale. */
static void dragfiles_end_session(bool committed)
{
    if (drag_files_src_port != 0)
    {
        dob_msg_t end = {0};
        end.code = GUI_EVT_DRAG_END;
        end.arg0 = drag_files_src_win_id;
        end.arg1 = committed ? 1 : 0;
        dob_ipc_post(drag_files_src_port, &end);
    }
    if (drag_files_payload)
    {
        free(drag_files_payload);
        drag_files_payload    = NULL;
        drag_files_payload_sz = 0;
    }
    drag_files_src_win_id = 0;
    drag_files_src_port   = 0;
    drag_files_copy_mode  = false;
    drag_mode             = DRAG_NONE;
    current_cursor        = CURSOR_ARROW;
    di_mark_dirty(DIRTY_FULL);
}

/* Avvio della sessione, per conto di GUI_BEGIN_DRAG (foglio ipc).
 * Rifiuta se un drag qualunque e' gia' in corso. Ritorna false su
 * rifiuto/OOM. Il primo byte del payload di filo e' is_cut: il
 * default copia/sposta arriva da li'. */
bool dragfiles_begin(uint32_t src_win_id, uint32_t src_port,
                     const void *payload, uint32_t payload_size)
{
    if (drag_mode != DRAG_NONE || drag_files_payload != NULL) return false;
    if (!payload || payload_size == 0) return false;

    void *buf = malloc(payload_size);
    if (!buf) return false;
    memcpy(buf, payload, payload_size);

    drag_files_payload    = buf;
    drag_files_payload_sz = payload_size;
    drag_files_src_win_id = src_win_id;
    drag_files_src_port   = src_port;
    drag_files_copy_mode  = (((const uint8_t *)payload)[0] == 0);
    drag_mode             = DRAG_FILES;
    current_cursor        = CURSOR_FILE_DRAG;
    di_mark_dirty(DIRTY_FULL);
    return true;
}

/* Annullo per conto di GUI_CANCEL_DRAG (foglio ipc). No-op se non
 * c'e' una sessione file attiva. */
void dragfiles_cancel(void)
{
    if (drag_mode == DRAG_FILES)
        dragfiles_end_session(false);
}

/* Chiusura d'ufficio di OGNI sessione interattiva viva — drag di
 * finestra, resize col servo, drag di file, grab di widget. Chiamata
 * dal foglio logon all'ingresso della tenda: al risveglio il desktop
 * deve essere quieto, senza sessioni fantasma appese a un bottone che
 * per il WM risulta ancora premuto. Il MOVE non richiede epilogo (la
 * posizione e' gia' applicata live a ogni tick); il RESIZE si chiude
 * col commit del servo, la cui geometria viene pero' SCARTATA — un
 * resize interrotto dalla tenda non deve applicare mezza anteprima. */
void input_abort_sessions(void)
{
    if (drag_mode == DRAG_FILES)
    {
        dragfiles_end_session(false);   /* azzera gia' drag_mode e cursore */
    }
    else if (drag_mode == DRAG_RESIZE && servo_is_active(&win_servo))
    {
        servo_result_t result;
        servo_commit(&win_servo, &result);   /* disattiva; esito scartato */
        servo_clear_prev(&win_servo);
        servo_preview_hide();
    }

    if (drag_mode != DRAG_NONE)
        di_mark_dirty(DIRTY_FULL);

    drag_mode           = DRAG_NONE;
    drag_win            = -1;
    widget_grab_idx     = -1;
    pointer_capture_win = -1;   /* un press pre-tenda non lascia catture fantasma */
    current_cursor      = CURSOR_ARROW;
}

/* ================= terra di mezzo: gestori del mouse ================== */

static void handle_mouse_press(int mx, int my)
{
    /* Qualunque clic chiude l'overlay About. */
    if (about_overlay_active)
    {
        about_overlay_active = false;
        di_mark_dirty(DIRTY_FULL);
        return;
    }

    /* Qualunque clic archivia il toast (e il clic prosegue). */
    if (toast_active)
    {
        toast_active = false;
        toast_layer_hide();
        di_mark_dirty(DIRTY_FULL);
    }

    /* Tray: clic dentro -> inoltra al proprietario del widget (con
     * grab per il drag). Clic fuori -> chiudi, poi processa il clic
     * sottostante — tranne se era su '<': gia' chiuso, non lasciare
     * che handle_panel_click lo riapra. */
    if (widget_panel_open)
    {
        int widx = wpanel_hit_test(mx, my);
        if (widx >= 0)
        {
            widget_send_mouse(widx, mx, my, 1);
            widget_grab_idx = widx;
            di_mark_dirty(DIRTY_FULL);
            return;
        }

        widget_panel_open = false;
        widget_grab_idx = -1;
        di_mark_dirty(DIRTY_FULL);

        if (mx >= panel_x)
        {
            int pidx = hit_test_panel(my);
            if (pidx >= 0 && panel_item_command(pidx) == CMD_OPEN_WIDGETS)
                return;
        }
        /* fall-through: il clic sottostante segue il percorso normale */
    }

    /* Mission Control: clic su una miniatura la seleziona, altrove
     * esce. Durante MC il pannello risponde solo a "Mostra finestre"
     * (per uscire). */
    if (mc_active)
    {
        if (mx < panel_x)
        {
            int idx = mc_hit_test(mx, my);
            mc_exit(idx);   /* -1 = esci senza selezionare */
        }
        else
        {
            int pidx = hit_test_panel(my);
            if (pidx >= 0 && panel_item_command(pidx) == CMD_SHOW_WINDOWS)
                mc_exit(-1);
        }
        return;
    }

    if (mx >= panel_x)
    {
        int idx = hit_test_panel(my);
        if (idx >= 0) handle_panel_click(idx);
        return;
    }

    int zone = 0;
    int idx = hit_test_window(mx, my, &zone);

    if (idx < 0)
    {
        /* Nessuna finestra sotto il clic: prova le icone prima del
         * generico "clic sullo sfondo" (le icone stanno sotto le
         * finestre in z: qui il clic e' davvero su desktop nudo). */
        int icon_idx = icon_hit_test(mx, my);
        if (icon_idx >= 0)
        {
            handle_icon_click(icon_idx);
            return;
        }

        /* Desktop nudo: deseleziona l'icona (il pannello torna al
         * default), poi sfoca senza rifocalizzare. */
        if (selected_icon >= 0)
        {
            desktop_icons[selected_icon].selected = false;
            selected_icon = -1;
            di_mark_dirty(DIRTY_FULL);
        }

        win_unfocus_to_desktop();
        return;
    }

    /* Cancello modale (percorso del press): un press sinistro su una
     * finestra bloccata da un modale vivo non deve rubare il focus,
     * iniziare un drag ne' un resize. Si ingoia QUI — prima delle
     * zone, cosi' un clic sul bordo non inizia un resize sulla
     * bloccata. Ding e modale-avanti li fa, per tutti i bottoni in un
     * punto solo, il cancello di consegna nel ciclo. */
    {
        int blk = win_modal_blocker(idx);
        if (blk >= 0 && blk != idx)
            return;
    }

    win_focus(idx);

    if (zone == 2 && !windows[idx].no_resize && !windows[idx].maximized)
    {
        /* Bordo/angolo -> resize via servo. */
        window_t *w = &windows[idx];
        servo_begin(&win_servo,
                    w->x, w->y, w->width, w->height,
                    mx, my, desktop_w, SCREEN_H);
        drag_mode = DRAG_RESIZE;
        drag_win = idx;
    }
    else if (zone == 1)
    {
        /* Header -> drag di spostamento. */
        drag_mode = DRAG_MOVE;
        drag_win = idx;
        drag_off_x = mx - windows[idx].x;
        drag_off_y = my - windows[idx].y;

        /* Avvisa il client che l'utente ha toccato il CHROME (title
         * bar/bordi, non il corpo): le app lo usano per sganciare il
         * focus di un widget interno e riportare il pannello laterale
         * ai comandi base. */
        send_win_event(idx, GUI_EVT_MOUSE, 0, mouse_buttons, 7);
    }
}

static void handle_mouse_release(void)
{
    /* Release del grab widget — evento e sgancio. */
    if (widget_grab_idx >= 0)
    {
        if (widget_panel_open)
            widget_send_mouse(widget_grab_idx, cursor_x, cursor_y, 2);
        widget_grab_idx = -1;
        di_mark_dirty(DIRTY_FULL);
    }

    /* Release di una sessione file: hit-test del bersaglio e drop. */
    if (drag_mode == DRAG_FILES)
    {
        int zone = 0;
        int t_idx = hit_test_window(cursor_x, cursor_y, &zone);
        bool committed = false;

        if (t_idx >= 0 && cursor_x < panel_x)
        {
            window_t *tw = &windows[t_idx];
            /* Il drop si consegna INCONDIZIONATAMENTE — anche sulla
             * finestra sorgente. Decide il bersaglio cosa farne
             * (drop nella cartella sotto il cursore, no-op sul
             * proprio listing, ...). Centralizzare la policy nel file
             * manager tiene vivo il drag-nella-propria-cartella senza
             * un percorso speciale "drag interno". */
            if (tw->owner_port != 0)
            {
                int lx = cursor_x - tw->x;
                int ly = cursor_y - tw->y - WIN_HEADER_H;
                if (ly >= 0)
                {
                    dob_msg_t msg = {0};
                    msg.code = GUI_EVT_DROP;
                    msg.arg0 = tw->id;
                    msg.arg1 = (uint32_t)((lx & 0xFFFF) | ((ly & 0xFFFF) << 16));
                    msg.arg2 = drag_files_copy_mode ? 1 : 0;
                    msg.arg3 = drag_files_src_port;   /* vedi testa   */
                    msg.payload      = drag_files_payload;
                    msg.payload_size = drag_files_payload_sz;
                    /* Il kernel copia il payload in un buffer proprio
                     * per il bersaglio; il nostro resta nostro e si
                     * libera nell'epilogo. Post verificata: una porta
                     * riciclata non riceve il drop (win_owner_post
                     * marca orfana la finestra). */
                    win_owner_post(t_idx, &msg);
                    committed = true;
                }
            }
        }

        dragfiles_end_session(committed);
        return;
    }

    /* Resize servo attivo: commit — UNA sola applicazione di
     * geometria (e il maximize decade). */
    if (drag_mode == DRAG_RESIZE && drag_win >= 0 && servo_is_active(&win_servo))
    {
        servo_result_t result;
        servo_commit(&win_servo, &result);
        win_apply_geometry(drag_win, result.x, result.y, result.w, result.h);
        windows[drag_win].maximized = false;
        servo_clear_prev(&win_servo);
    }

    /* Dopo ogni drag, repaint pieno: ripristina pannello e stato. */
    if (drag_mode != DRAG_NONE)
        di_mark_dirty(DIRTY_FULL);

    drag_mode = DRAG_NONE;
    drag_win = -1;
}

static void handle_mouse_move(int mx, int my)
{
    /* Drag di widget — inoltra la posizione finche' in grab. */
    if (widget_grab_idx >= 0 && widget_panel_open)
    {
        widget_send_mouse(widget_grab_idx, mx, my, 6);
        di_mark_dirty(DIRTY_FULL);
    }

    /* Sessione file: cursore pinnato (resta quello del BeginDrag) e
     * niente altra logica di hover. In parallelo, GUI_EVT_DRAG_OVER
     * alla finestra sotto il cursore, cosi' evidenzia la cartella
     * bersaglio al passaggio. Si manda anche alla sorgente: il
     * drag-nella-propria-cartella e' un'operazione valida. */
    if (drag_mode == DRAG_FILES)
    {
        int zone = 0;
        int hover_idx = hit_test_window(mx, my, &zone);
        if (hover_idx >= 0 && mx < panel_x)
        {
            window_t *hw = &windows[hover_idx];
            if (hw->owner_port != 0)
            {
                int lx = mx - hw->x;
                int ly = my - hw->y - WIN_HEADER_H;
                if (ly >= 0)
                {
                    dob_msg_t om = {0};
                    om.code = GUI_EVT_DRAG_OVER;
                    om.arg0 = hw->id;
                    om.arg1 = (uint32_t)((lx & 0xFFFF) | ((ly & 0xFFFF) << 16));
                    win_owner_post(hover_idx, &om);
                }
            }
        }
        di_mark_dirty(DIRTY_FULL);
        return;
    }

    if (drag_mode == DRAG_MOVE && drag_win >= 0)
    {
        window_t *w = &windows[drag_win];
        int total_h = w->height + WIN_HEADER_H;
        int old_x = w->x;
        int old_y = w->y;

        /* Clamp bound-safe. Se la finestra e' piu' grande dello
         * schermo il limite alto normale (SCREEN_H - total_h,
         * desktop_w - width) e' negativo; clamp(lo=0, hi<0)
         * ritornerebbe l'hi negativo e il frame schizza fuori.
         * Si ORDINANO i limiti: chi entra panora in [0, gioco]; chi
         * sborda panora in [-eccedenza, 0], cosi' il suo bordo alto
         * non supera mai il bordo schermo. */
        int rx = desktop_w - w->width;
        int ry = SCREEN_H  - total_h;
        int lo_x = rx < 0 ? rx : 0, hi_x = rx < 0 ? 0 : rx;
        int lo_y = ry < 0 ? ry : 0, hi_y = ry < 0 ? 0 : ry;
        w->x = clamp(mx - drag_off_x, lo_x, hi_x);
        w->y = clamp(my - drag_off_y, lo_y, hi_y);
        w->maximized = false;

        if (w->x == old_x && w->y == old_y)
            return;

        compositor_drag_blit(drag_win);
        /* compositor_drag_blit sincronizza la posizione di ogni layer
         * nello stesso frame E consuma da se' FULL|CONTENT: niente
         * resta in sospeso in questo tick. */
    }
    else if (drag_mode == DRAG_RESIZE && servo_is_active(&win_servo))
    {
        servo_result_t result;
        servo_update(&win_servo, mx, my, &result);

        /* L'unica cosa che cambia a ogni tick e' l'anteprima
         * tratteggiata (i suoi layer a z=100): il backbuf di
         * desktop/icone/finestre/pannello e' STATICO durante il
         * resize, non c'e' nulla da ricostruire — riposiziona i layer
         * dell'anteprima e presenta un frame pulito.
         *
         * Perche' fb_flip PIENO e NON compositor_repaint_rect: l'union
         * del resize cresce fino a quasi tutto lo schermo trascinando
         * il bordo. dv_compose_rect consegnerebbe quell'union con un
         * blit scratch -> pagina VISIBILE, in-place, senza page-flip:
         * ci sta nel vblank (~784 us) solo per rect PICCOLI. Un blit
         * in-place quasi-fullscreen sfora il blank e scrive la pagina
         * mentre viene scandita -> il flash see-through visto sul
         * Mach64/E500. fb_flip invece compone nella pagina di back
         * NASCOSTA e scambia con una scrittura CRTC atomica (sempre
         * nel blank) -> zero tear sul ferro; e' lo stesso percorso gia'
         * flash-free del MOVE. In piu' la compose piena ridipinge il
         * cursore SOFTWARE a ogni tick — che il percorso scissored
         * saltava, lasciandolo congelato fino al release sui backend
         * senza cursore hardware (QEMU/BGA). */
        servo_draw_preview();
        fb_flip();
    }

    /* Tipo di cursore — ricalcolato a ogni mossa da
     * cursor_for_position, che consulta anche l'override per-finestra
     * (una finestra con CURSOR_HSPLIT sul contenuto mostra <--> non
     * appena il cursore entra nel corpo, e ARROW appena esce). */
    int new_cursor = cursor_for_position(mx, my);
    if (new_cursor != current_cursor)
        current_cursor = new_cursor;

    /* Hover del pannello. */
    int old_hover = panel_hover_idx;
    if (mx >= panel_x)
        panel_hover_idx = hit_test_panel(my);
    else
        panel_hover_idx = -1;

    if (panel_hover_idx != old_hover && drag_mode == DRAG_NONE)
        di_mark_dirty(DIRTY_PANEL);
}

/* ================= terra di mezzo: router eventi ====================== */

/* Smista un evento della porta unificata.
 * Mouse: accumula in dx/dy/scroll. Tasti: inoltro immediato alla
 * finestra in focus. Richiesta GUI sincrona: gestita e risposta
 * subito. */
static void dispatch_one(dob_msg_t *msg,
                         int *dx, int *dy, int *scroll, bool *got_mouse,
                         bool *saw_press, bool *saw_release)
{
    /* Reload "Applica" dell'editor: DobSettings lo posta (fire and
     * forget) al proprietario del file dopo un edit committato.
     * Rilettura della config: on/off e durata valgono da subito,
     * dalla prossima finestra che si apre. */
    if (msg->code == SETTINGS_RELOAD_CALL)
    {
        typed_settings_load();
        logon_settings_load();   /* timeout screensaver: vale da subito */
        return;
    }

    /* Death-watch dal kernel: la porta (arg0, arg1) di un owner e'
     * morta. Marca ORFANE tutte le finestre con quel binding esatto
     * (id+gen: anti-ABA, non tocca una finestra di un processo che ha
     * riciclato l'id). Distrutte al punto sicuro
     * (win_reap_dead_owners). Rimpiazza il vecchio poll a 500 ms:
     * event-driven per design. */
    if (msg->code == GUI_PEER_DIED)
    {
        uint32_t dead_port = msg->arg0;
        uint32_t dead_gen  = msg->arg1;
        for (int i = 0; i < MAX_WINDOWS; i++)
        {
            if (windows[i].used &&
                windows[i].owner_port == dead_port &&
                windows[i].owner_gen == dead_gen)
            {
                windows[i].owner_dead = true;
            }
        }
        return;
    }

    /* Sveglia timer (il codice 70 porta il timer_id in arg0). Routing
     * per id: le pompe periodiche di stile (typed-text, lampeggio),
     * poi i one-shot del foglio logon (inattivita', fine lockout),
     * contro il one-shot del frame pacing. Un id non riconosciuto
     * (es. un fire di pacing stantio) cade sul percorso pacing, che
     * e' un no-op con guardia quando nulla e' rinviato. */
    if (msg->code == 70 /* MSG_TIMER / TIMER_EVT_FIRED */)
    {
        if (!style_timer_fired((int)msg->arg0)
            && !logon_timer_fired((int)msg->arg0))
            pacing_run_deferred_flip();
        return;
    }

    /* Richiesta sincrona da un programma (CreateWindow, ...). */
    if (msg->type == 1)
    {
        dob_msg_t reply = {0};
        handle_gui_ipc(msg, &reply);
        dob_ipc_reply(msg->sender_tid, &reply);
        return;
    }

    /* Eventi da inputd (post async). */
    if (msg->code == INPUT_MOUSE)
    {
        logon_note_activity();   /* attivita' umana: azzera l'inattivita' */

        uint8_t new_btn = (uint8_t)msg->arg0;
        *dx += (int)(int16_t)msg->arg1;
        *dy += (int)(int16_t)msg->arg2;
        *scroll += (int)(int16_t)msg->arg3;

        /* Transizioni rilevate PER MESSAGGIO: se press+release
         * arrivano nello stesso burst, entrambi i flag si accendono
         * ed entrambi verranno processati dopo il burst — i clic non
         * si perdono quando inputd manda press e release come
         * messaggi consecutivi. */
        if ((new_btn & 1) && !(mouse_buttons & 1))
            *saw_press = true;
        if (!(new_btn & 1) && (mouse_buttons & 1))
            *saw_release = true;

        mouse_buttons = new_btn;
        *got_mouse = true;
        return;
    }
    if (msg->code == INPUT_KEY)
    {
        uint8_t key = (uint8_t)msg->arg0;

        logon_note_activity();

        /* Tenda su: OGNI tasto e' del foglio logon e muore li' —
         * niente drag, niente Stamp (screenshot), niente finestre. Il
         * tasto che risveglia lo screensaver viene ingoiato qui. */
        if (logon_visible())
        {
            logon_key(key);
            return;
        }

        /* Durante una sessione file il WM possiede la tastiera:
         * Spazio commuta copia/sposta, Esc annulla, tutto il resto
         * viene ingoiato cosi' le app non rubano il drag. */
        if (drag_mode == DRAG_FILES)
        {
            if (key == ' ')
            {
                drag_files_copy_mode = !drag_files_copy_mode;
                return;
            }
            if (key == 27)  /* Esc */
            {
                dragfiles_end_session(false);   /* nessun drop */
                return;
            }
            return;   /* ingoia ogni altro tasto */
        }

        if (key == SKEY_PRTSC)
        {
            screenshot_take();          /* MAI instradato alle finestre  */
            return;
        }
        if (focused_win >= 0 && windows[focused_win].owner_port != 0)
            send_win_event(focused_win, GUI_EVT_KEY, key, key, 0);
        return;
    }
    if (msg->code == INPUT_MODCHANGE)
    {
        logon_note_activity();
        /* Con la tenda su i modificatori non hanno destinatario. */
        if (logon_visible())
            return;
        /* Cambio dei modificatori (CTRL/SHIFT) in inputd. Si inoltra
         * solo la nuova bitmask alla finestra in focus, che ne tiene
         * copia locale e la AND-a coi clic (es. DobFiles). Postato
         * solo sulle transizioni, mai ripetuto. */
        if (focused_win >= 0 && windows[focused_win].owner_port != 0)
            send_win_event(focused_win, GUI_EVT_MODCHANGE,
                           (uint32_t)msg->arg0, 0, 0);
        return;
    }

    /* Il piano di controllo video ha spinto un cambio di modalita'
     * (nuova risoluzione da Impostazioni, o pannello nativo di un
     * driver). Il payload porta solo width/height: e' un hint, la
     * modalita' autoritativa si re-interroga in
     * video_on_mode_changed. code == 1 e' non ambiguo su gui_port:
     * ogni altro codice consegnato qui e' >= 50. */
    if (msg->code == DOBVC_EVENT_MODE_CHANGED)
    {
        video_on_mode_changed();
        return;
    }

    /* Messaggi GUI async dai programmi (Invalidate, SetPanel, ...). */
    dob_msg_t reply = {0};
    handle_gui_ipc(msg, &reply);
}

/* ================= logica ad alto livello ============================= */

/* Sottoscrizione a inputd: digli dove postare gli eventi. */
void input_subscribe(void)
{
    uint32_t inputd = dob_registry_find("inputd");
    if (!inputd)
    {
        debug_print("[dobinterface] inputd not found, retrying...\n");
        inputd = dob_registry_wait("inputd", 3000);
    }
    if (!inputd)
    {
        debug_print("[dobinterface] WARNING: inputd not found! No input.\n");
        return;
    }

    dob_msg_t m = {0}, r = {0};
    m.code = INPUT_SUBSCRIBE;
    m.arg0 = gui_port;
    dob_ipc_call(inputd, &m, &r);
    debug_print("[dobinterface] Subscribed to inputd.\n");
}

/* input_pump — un giro completo del ciclo eventi: receive bloccante,
 * drain del burst, azzeramento di frame_flipped, e l'intero
 * instradamento del mouse. Ritorna true se il cursore si e' mosso
 * (per la decisione di present del main). */
bool input_pump(void)
{
    dob_msg_t msg;
    dob_ipc_receive(gui_port, &msg);

    int dx = 0, dy = 0, scroll = 0;
    bool got_mouse = false;
    bool saw_press = false, saw_release = false;
    uint8_t buttons_before = mouse_buttons;   /* stato prima del burst */
    dispatch_one(&msg, &dx, &dy, &scroll, &got_mouse,
                 &saw_press, &saw_release);

    for (int burst = 0; burst < 64; burst++)
    {
        if (dob_ipc_receive_nowait(gui_port, &msg) != DOB_OK) break;
        dispatch_one(&msg, &dx, &dy, &scroll, &got_mouse,
                     &saw_press, &saw_release);
    }

    frame_flipped = false;
    bool cursor_moved = false;

    if (got_mouse)
    {
        int old_cx = cursor_x, old_cy = cursor_y;
        cursor_x = clamp(cursor_x + dx, 0, SCREEN_W - 1);
        cursor_y = clamp(cursor_y + dy, 0, SCREEN_H - 1);
        cursor_moved = (cursor_x != old_cx || cursor_y != old_cy);

        /* Posizione del cursore spinta subito al suo dv_layer —
         * economico (un giro di boomerang). La compose/flip avviene
         * al prossimo fb_flip; questo assicura solo che lo stato del
         * layer rifletta la posizione nuova, cosi' ogni dv_compose in
         * volo la vede. */
        if (cursor_moved)
            cursor_layer_update_pos();

        bool left_pressed  = saw_press;
        bool left_released = saw_release;

        /* Transizioni del tasto DESTRO dal confronto pre/post burst. */
        bool right_pressed  = (mouse_buttons & 2) && !(buttons_before & 2);
        bool right_released = !(mouse_buttons & 2) && (buttons_before & 2);

        /* Transizioni del tasto CENTRALE. */
        bool middle_pressed  = (mouse_buttons & 4) && !(buttons_before & 4);
        bool middle_released = !(mouse_buttons & 4) && (buttons_before & 4);

        /* any_change: QUALUNQUE bottone cambiato. */
        bool any_change = (mouse_buttons != buttons_before);

        /* Tenda su: il cursore si muove (gia' spinto al layer sopra),
         * ma NULLA viene consegnato o instradato — niente press alle
         * finestre, niente pannello, niente icone, niente scroll. In
         * L_SAVER il gate risveglia su press o mossa; l'evento muore
         * qui, mai consegnato. */
        if (logon_gate_mouse(left_pressed || right_pressed || middle_pressed,
                             cursor_moved))
            return cursor_moved;

        if (left_pressed)  handle_mouse_press(cursor_x, cursor_y);
        if (left_released) handle_mouse_release();
        handle_mouse_move(cursor_x, cursor_y);

        /* Cancello modale (percorso di consegna): se un press cade su
         * una finestra bloccata da un modale vivo — ding, modale
         * avanti (stile Windows), e clic NON consegnato. Copre tutti
         * i bottoni in un punto solo; il cancello del press ha gia'
         * fermato focus/drag/resize del sinistro. */
        int gate_zone = 0;
        int gate_hit  = (cursor_x < panel_x)
                      ? hit_test_window(cursor_x, cursor_y, &gate_zone) : -1;
        int gate_blk  = (gate_hit >= 0) ? win_modal_blocker(gate_hit) : -1;
        if (gate_blk >= 0 && (left_pressed || right_pressed || middle_pressed))
        {
            win_attention_ding();
            flash_start(gate_blk);
            win_focus(gate_blk);   /* il modale non e' bloccato: niente ding */
        }

        /* Consegna dei press: stessa guardia di prima, piu' il
         * cancello modale (gate_blk < 0). Registra il bersaglio della
         * cattura cosi' release/drag successivi vanno a QUESTA
         * finestra anche se il cursore ne esce. */
        if (any_change && cursor_x < panel_x
            && focused_win >= 0 && windows[focused_win].owner_port != 0
            && !mc_active && !widget_panel_open && drag_mode == DRAG_NONE
            && gate_blk < 0)
        {
            window_t *fw = &windows[focused_win];
            int lx = cursor_x - fw->x;
            int ly = cursor_y - fw->y - WIN_HEADER_H;
            uint32_t xy = (uint32_t)((lx & 0xFFFF) | (ly << 16));
            uint32_t etype = 0;

            if (left_pressed)
            {
                uint32_t now = (uint32_t)clock_ms();
                int adx = cursor_x - dblc_last_x;
                int ady = cursor_y - dblc_last_y;
                if (adx < 0) adx = -adx;
                if (ady < 0) ady = -ady;

                if ((now - dblc_last_ms) <= DBLCLICK_MS
                    && adx <= DBLCLICK_RADIUS && ady <= DBLCLICK_RADIUS)
                {
                    etype = 3;
                    dblc_last_ms = 0; dblc_last_x = -100; dblc_last_y = -100;
                }
                else
                {
                    etype = 1;
                    dblc_last_ms = now; dblc_last_x = cursor_x; dblc_last_y = cursor_y;
                }

                /* Stabilisci la cattura: finche' un bottone e' giu'
                 * il release corrispondente va a QUESTA finestra. */
                pointer_capture_win = focused_win;
                send_win_event(focused_win, GUI_EVT_MOUSE, xy, mouse_buttons, etype);
            }

            /* Anche destro/centrale avviano la cattura: il drag
             * cross-finestra con quei bottoni funziona come col
             * sinistro. */
            if (right_pressed)
            {
                pointer_capture_win = focused_win;
                send_win_event(focused_win, GUI_EVT_MOUSE, xy, mouse_buttons, 4);
            }
            if (middle_pressed)
            {
                pointer_capture_win = focused_win;
                send_win_event(focused_win, GUI_EVT_MOUSE, xy, mouse_buttons, 5);
            }
        }

        /* Consegna dei release: alla finestra catturata, ovunque sia
         * il cursore ora. Le coordinate sono relative alla catturata
         * (possono essere negative o oltre width/height — va bene,
         * il client serra/ignora come crede). */
        if ((left_released || right_released || middle_released)
            && pointer_capture_win >= 0
            && windows[pointer_capture_win].used
            && windows[pointer_capture_win].owner_port != 0
            && !mc_active && !widget_panel_open && drag_mode == DRAG_NONE)
        {
            window_t *fw = &windows[pointer_capture_win];
            int lx = cursor_x - fw->x;
            int ly = cursor_y - fw->y - WIN_HEADER_H;
            uint32_t xy = (uint32_t)((lx & 0xFFFF) | (ly << 16));

            if (left_released)
                send_win_event(pointer_capture_win, GUI_EVT_MOUSE,
                               xy, mouse_buttons, 2);
            if (right_released)
                send_win_event(pointer_capture_win, GUI_EVT_MOUSE,
                               xy, mouse_buttons, 2);
            if (middle_released)
                send_win_event(pointer_capture_win, GUI_EVT_MOUSE,
                               xy, mouse_buttons, 2);
        }

        /* La cattura cade quando TUTTI i bottoni sono su. Finche'
         * uno resta giu' si tiene: le sequenze multi-bottone
         * (sinistro giu', clic destro, sinistro su) restano
         * instradate alla stessa finestra. */
        if (mouse_buttons == 0)
            pointer_capture_win = -1;

        /* Evento drag (etype=6): cursore mosso a bottone premuto.
         * Instradato al bersaglio della cattura se attivo, altrimenti
         * alla finestra in focus. */
        if (cursor_moved && mouse_buttons != 0
            && !mc_active && !widget_panel_open && drag_mode == DRAG_NONE)
        {
            int target = (pointer_capture_win >= 0
                          && windows[pointer_capture_win].used
                          && windows[pointer_capture_win].owner_port != 0)
                         ? pointer_capture_win
                         : ((cursor_x < panel_x && focused_win >= 0
                             && windows[focused_win].owner_port != 0)
                            ? focused_win : -1);

            if (target >= 0)
            {
                window_t *fw = &windows[target];
                int lx = cursor_x - fw->x;
                int ly = cursor_y - fw->y - WIN_HEADER_H;
                uint32_t xy = (uint32_t)((lx & 0xFFFF) | (ly << 16));
                send_win_event(target, GUI_EVT_MOUSE, xy, mouse_buttons, 6);
            }
        }

        /* Scroll del tray — consumato se il mouse e' dentro. */
        if (scroll != 0 && widget_panel_open
            && wpanel_contains(cursor_x, cursor_y))
        {
            if (wpanel_scroll_by(scroll * WIDGET_SCROLL_STEP))
                di_mark_dirty(DIRTY_FULL);
        }
        else if (scroll != 0 && !mc_active
            && focused_win >= 0 && windows[focused_win].owner_port != 0
            && cursor_x < panel_x)
        {
            send_win_event(focused_win, GUI_EVT_SCROLL,
                           (uint32_t)(int32_t)scroll, 0, 0);
        }

        /* Scroll di Mission Control (rotella o bordi). */
        if (mc_active && mc_handle_scroll(scroll, cursor_y))
            di_mark_dirty(DIRTY_FULL);
    }

    return cursor_moved;
}
