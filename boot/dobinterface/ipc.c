/* MainDOB DobInterface 2.0 — foglio: ipc.c
 *
 * Il gestore delle richieste GUI: l'ABI con cui i programmi creano
 * finestre e disegnano. I codici, gli arg e le reply sono CONGELATI
 * byte-per-byte (vedi di_internal.h): ogni programma MainDOB
 * esistente deve girare identico contro questo foglio.
 *
 * Convenzioni ereditate e mantenute:
 *  - reply.arg1 di WIN_CREATE resta -1 (era l'id SHM del modello a
 *    framebuffer condiviso; riservato per compat ABI);
 *  - la consegna degli eventi agli owner passa SEMPRE da
 *    win_owner_post (anti-ABA);
 *  - il death-watch si arma alla CREATE: il client e' vivo e bloccato
 *    in attesa della reply, quindi la sua porta ha generazione
 *    stabile — catturarla ora e' privo di TOCTOU. */

#include "di_internal.h"

/* ================= verbi esecutivi ==================================== */

/* Spezza il testo dei comandi contestuali ('\n'-separati) nelle
 * etichette pre-allocate della finestra. */
static void parse_ctx_commands(window_t *w, const char *cmds, uint32_t len)
{
    w->ctx_count = 0;
    const char *p = cmds;
    const char *end = cmds + len;

    while (p < end && w->ctx_count < MAX_WIN_CTX_CMDS)
    {
        const char *nl = p;
        while (nl < end && *nl != '\n' && *nl != '\0') nl++;

        uint32_t slen = (uint32_t)(nl - p);
        if (slen > 0 && slen < 64)
        {
            memcpy(w->ctx_labels[w->ctx_count], p, slen);
            w->ctx_labels[w->ctx_count][slen] = '\0';
            w->ctx_count++;
        }

        p = nl + 1;
    }
}

/* ================= logica ad alto livello ============================= */

void handle_gui_ipc(dob_msg_t *msg, dob_msg_t *reply)
{
    switch (msg->code)
    {
        case GUI_WIN_CREATE:
        {
            int ww = (int)msg->arg0;
            int wh = (int)msg->arg1;
            uint32_t owner_port = msg->arg2;
            const char *title = msg->payload ? (const char *)msg->payload : "Untitled";

            debug_print("[dobui] WIN_CREATE: ");
            debug_print(title);
            debug_print("\n");

            /* Centra nel desktop. */
            int x = (desktop_w - ww) / 2;
            int y = (SCREEN_H - wh - WIN_HEADER_H) / 2;
            if (x < 10) x = 10;
            if (y < 10) y = 10;

            int idx = win_create(title, ww, wh, x, y);
            if (idx >= 0)
            {
                windows[idx].owner_pid = msg->sender_pid;
                windows[idx].owner_port = owner_port;
                /* Cattura della generazione: senza TOCTOU, vedi testa.
                 * Ogni inoltro successivo la verifica (win_owner_post):
                 * un id riciclato da un altro processo fallisce invece
                 * di consegnargli l'input di questa finestra. */
                windows[idx].owner_gen =
                    (owner_port != 0) ? dob_ipc_port_generation(owner_port)
                                      : 0;
                /* Death-watch event-driven al posto del poll: quando la
                 * porta (owner_port, owner_gen) muore, il kernel ci
                 * posta GUI_PEER_DIED. Deduplicato lato kernel: piu'
                 * finestre dello stesso owner = un solo watch. Se e'
                 * gia' morta proprio ora (ritorno 1), mieti al prossimo
                 * punto sicuro. */
                if (owner_port != 0 && windows[idx].owner_gen != 0)
                {
                    if (dob_port_watch(owner_port, windows[idx].owner_gen,
                                       gui_port, GUI_PEER_DIED) == 1)
                    {
                        windows[idx].owner_dead = true;
                    }
                }
                win_focus(idx);
                reply->arg0 = windows[idx].id;
                /* arg1 = id SHM, riservato a -1 per compat ABI. */
                reply->arg1 = (uint32_t)-1;
            }
            else
            {
                /* Tetto MAX_WINDOWS raggiunto — log chiaro perche' il
                 * programma nuovo dell'utente e' appena uscito. */
                debug_print("[dobinterface] WIN_CREATE FAIL: MAX_WINDOWS "
                            "(64) reached.\n");
                toast_show("Limite finestre raggiunto");
                reply->arg0 = 0;
                reply->arg1 = (uint32_t)-1;  /* id SHM esplicitamente invalido */
            }
            break;
        }

        case GUI_WIN_DESTROY:
        {
            int idx = win_find_by_id(msg->arg0);
            if (idx >= 0)
            {
                /* win_destroy scende in cascata sulle figlie e azzera
                 * focused_win se libera lo slot in focus — quindi un
                 * focused_win negativo dopo significa che il focus era
                 * dentro il sottoalbero distrutto (es. un modale caduto
                 * con la madre). Rifocalizza la superstite piu' in
                 * alto, che per un modale chiuso e' la madre appena
                 * sbloccata. */
                win_destroy(idx);
                if (focused_win < 0)
                    win_unfocus_all();
            }
            break;
        }

        case GUI_WIN_SET_TITLE:
        {
            int idx = win_find_by_id(msg->arg0);
            if (idx >= 0 && msg->payload)
            {
                strncpy(windows[idx].title, (const char *)msg->payload, 63);
                windows[idx].title[63] = '\0';
                if (idx == focused_win)
                {
                    panel_focus_window(&windows[idx]);
                    panel_recalculate();
                    di_mark_dirty(DIRTY_PANEL);
                }
                /* Barra header cambiata — reblit della banda della
                 * finestra, non tutto lo schermo. */
                windows[idx].content_dirty = true;
                di_mark_dirty(DIRTY_CONTENT);
            }
            break;
        }

        case GUI_WIN_RAISE:
        {
            int idx = win_find_by_id(msg->arg0);
            if (idx >= 0)
            {
                /* Rialloca surface+layer se HIDE li aveva liberati. */
                if (windows[idx].body_layer == DV_HANDLE_NONE)
                {
                    win_alloc_video(&windows[idx]);
                    windows[idx].surface_dirty = true;
                }
                windows[idx].visible = true;
                z_sort_valid = false;
                win_focus(idx);
                win_update_layer_pos(&windows[idx]);
            }
            break;
        }

        case GUI_WIN_HIDE:
        {
            int idx = win_find_by_id(msg->arg0);
            if (idx >= 0)
            {
                windows[idx].visible = false;
                /* Libera surface+layer come CMD_WIN_HIDE — rilascia
                 * risorse driver da nascosta, ricreate pigre allo
                 * show. Il tex pool NON si tocca (vedi
                 * win_tex_pool_free per il perche'). */
                win_free_video(&windows[idx]);
                z_sort_valid = false;
                if (focused_win == idx)
                    win_unfocus_all();
                di_mark_dirty(DIRTY_FULL);
            }
            break;
        }

        case GUI_WIN_SET_FLAGS:
        {
            int idx = win_find_by_id(msg->arg0);
            if (idx >= 0)
            {
                uint32_t flags = msg->arg1;
                windows[idx].no_resize   = (flags & GUI_WIN_FLAG_NORESIZE)   != 0;
                windows[idx].no_maximize = (flags & GUI_WIN_FLAG_NOMAXIMIZE) != 0;
                /* La finestra a questo punto e' gia' in focus (la
                 * CREATE la focalizza prima che l'owner riceva la
                 * reply e ci richiami), quindi la voce Ingrandisci e'
                 * viva — allineane la visibilita' al flag nuovo. */
                if (focused_win == idx)
                {
                    panel_set_maximize_visible(!windows[idx].no_maximize);
                    panel_sync_context();
                    di_mark_dirty(DIRTY_FULL);
                }
            }
            break;
        }

        case GUI_WIN_SET_PARENT:
        {
            int cidx = win_find_by_id(msg->arg0);   /* figlia */
            if (cidx >= 0)
            {
                if (msg->arg1 == 0)
                {
                    /* Sgancio -> torna top-level, non piu' modale. */
                    windows[cidx].parent_id = 0;
                    windows[cidx].modal     = false;
                }
                else
                {
                    int pidx = win_find_by_id(msg->arg1);   /* madre */
                    if (pidx >= 0 && pidx != cidx)
                    {
                        windows[cidx].parent_id = windows[pidx].id;
                        windows[cidx].modal =
                            (msg->arg2 & GUI_WIN_PARENT_MODAL) != 0;
                        /* Solleva subito la figlia sopra la madre. Per
                         * un modale questo inizia anche a bloccarla.
                         * win_focus -> win_restack_for alza l'intero
                         * gruppo owner e inchioda il modale in cima. */
                        win_focus(cidx);
                    }
                }
            }
            break;
        }

        case GUI_WIN_INVALIDATE:
        {
            int idx = win_find_by_id(msg->arg0);
            if (idx >= 0)
            {
                window_t *w = &windows[idx];

                /* Riassemblaggio dei segmenti (foglio cmdbuf): i
                 * segmenti intermedi vengono assorbiti qui e il resto
                 * dell'handler gira solo a cmdbuf COMPLETO.
                 * Legacy/segmento unico passano dritti, zero copie. */
                const uint8_t *asm_buf  = NULL;
                uint32_t       asm_size = 0;
                if (!cmdbuf_reasm_ingest(&w->reasm, msg,
                                         &asm_buf, &asm_size))
                {
                    break;
                }

                /* Ingest del payload. Valida il magic, poi copia in
                 * w->last_cmdbuf cosi' il corpo si puo' ricostruire
                 * sui cambi chrome-only (focus, titolo) senza
                 * round-trip col client.
                 *
                 * Guardia payload-vuoto: una Invalidate header-only e'
                 * un doppio flush spurio — scarta il payload ma
                 * rinfresca comunque il chrome. Senza guardia la
                 * finestra diventa bianca ("tutto bianco a fine
                 * test"). */
                const uint8_t *src   = asm_buf;
                uint32_t       sz    = asm_size;
                bool           valid = false;
                bool           empty = false;
                if (src && sz == DOBUI_CMDBUF_HDR_SIZE)
                {
                    /* Header-only: verifica comunque il magic, cosi'
                     * un messaggio davvero malformato non passa. */
                    uint32_t magic = (uint32_t)src[0]
                                   | ((uint32_t)src[1] <<  8)
                                   | ((uint32_t)src[2] << 16)
                                   | ((uint32_t)src[3] << 24);
                    if (magic == DOBUI_CMDBUF_MAGIC) empty = true;
                }
                else if (src && sz > DOBUI_CMDBUF_HDR_SIZE)
                {
                    uint32_t magic = (uint32_t)src[0]        |
                                     ((uint32_t)src[1] <<  8) |
                                     ((uint32_t)src[2] << 16) |
                                     ((uint32_t)src[3] << 24);
                    if (magic == DOBUI_CMDBUF_MAGIC) valid = true;
                }
                if (valid)
                {
                    /* Cresci il buffer se serve. Le app con redraw
                     * stabili orbitano alla stessa misura; il realloc
                     * e' un no-op raggiunto l'high-water. */
                    if (sz > w->last_cmdbuf_cap)
                    {
                        uint8_t *nb = (uint8_t *)realloc(w->last_cmdbuf, sz);
                        if (nb)
                        {
                            w->last_cmdbuf     = nb;
                            w->last_cmdbuf_cap = sz;
                        }
                        else
                        {
                            /* OOM: perdi il payload di QUESTO frame
                             * (il contenuto precedente resta). */
                            valid = false;
                        }
                    }
                    if (valid)
                    {
                        memcpy(w->last_cmdbuf, src, sz);
                        w->last_cmdbuf_size = sz;
                    }
                }
                /* Payload vuoto/invalido: last_cmdbuf resta intatto,
                 * cosi' un rebuild chrome-only mostra ancora il corpo
                 * precedente. */
                (void)empty;

                /* Cancello del primo draw: la finestra passa da
                 * "appena creata, in bootstrap" a "pronta". La prima
                 * volta serve un repaint PIENO (non solo
                 * content_dirty): bordo, header e occlusione del
                 * desktop attorno alla neo-rivelata vanno ricalcolati. */
                if (!w->ready)
                {
                    w->ready = true;
                    z_sort_valid = false;
                    di_mark_dirty(DIRTY_FULL);
                    /* La finestra si sta aprendo: parte il reveal
                     * typed-text. Usa il cmdbuf appena ingerito; no-op
                     * se il corpo non ha testo. */
                    type_anim_start(w);
                }
                w->content_dirty = true;
                w->surface_dirty = true;        /* win_bake al prossimo blit */
                di_mark_dirty(DIRTY_CONTENT);
            }
            break;
        }

        case GUI_WIN_TEX_ALLOC:
        {
            /* Pool di texture lato server per le BlitBuffer dei client
             * oltre la soglia inline. Alloc pigra; vive fino alla
             * distruzione della finestra o alla free esplicita. */
            int idx = win_find_by_id(msg->arg0);
            reply->arg0 = 0;
            if (idx < 0) break;
            window_t *w = &windows[idx];
            if (w->tex_pool_count >= WIN_TEX_POOL_SIZE) break;   /* pool pieno */
            uint32_t tw = msg->arg1, th = msg->arg2;
            /* Rifiuta zero e oltre-schermo: una BlitBuffer non puo'
             * utilmente eccedere il display, e una texture smisurata
             * (il vecchio cap 4096x4096 = 64 MiB) brucerebbe l'intero
             * budget VRAM di un adattatore da 4 MiB. */
            if (tw == 0 || th == 0
                || tw > (uint32_t)SCREEN_W || th > (uint32_t)SCREEN_H) break;

            dv_texture_desc_t td = {
                .width      = tw,
                .height     = th,
                .format     = DV_FMT_BGRA8888,
                .mip_levels = 1,
                /* SYSRAM — il punto piu' caldo di tutti: la pagina di
                 * un editor (~2 MB) viene ri-blittata nel corpo SYSRAM
                 * a OGNI bake della finestra. Con la texture in VRAM
                 * il bake la rileggerebbe dall'aperture a 10-25 MB/s:
                 * 80-190 ms per keystroke solo di letture. In RAM:
                 * pochi ms. */
                .flags      = DV_TEX_FLAG_DYNAMIC | DV_SURF_FLAG_SYSRAM,
            };
            dv_texture_t t = DV_HANDLE_NONE;
            if (dv_texture_create(g_vproc, &td, &t) != DV_OK) break;
            w->tex_pool[w->tex_pool_count].handle = t;
            w->tex_pool[w->tex_pool_count].w      = (uint16_t)tw;
            w->tex_pool[w->tex_pool_count].h      = (uint16_t)th;
            /* Specchio CPU (best-effort): abilita screenshot e
             * miniature fedeli. Se manca (OOM), quelle ricomposizioni
             * degradano — il percorso video non ne dipende. */
            w->tex_pool[w->tex_pool_count].cpu    =
                (uint32_t *)malloc((size_t)tw * th * 4u);
            w->tex_pool_count++;
            reply->arg0 = (uint32_t)t;
            break;
        }

        case GUI_WIN_TEX_UPDATE:
        {
            /* Upload a BANDE DI RIGHE INTERE: arg2 = riga iniziale,
             * arg3 = numero di righe (0 = legacy: texture intera in un
             * messaggio). Il buffer IPC del ricevente strippa i
             * payload oltre il suo tetto (vedi dobui_cmdbuf.h): una
             * pagina di DobWrite (~6 MiB) non puo' viaggiare
             * monoblocco. Ogni banda si applica subito con
             * dv_texture_update_region: nessuno stato di
             * riassemblaggio, una banda persa lascia solo una striscia
             * stantia che il prossimo upload risana. */
            int idx = win_find_by_id(msg->arg0);
            if (idx < 0) break;
            window_t *w = &windows[idx];
            int pi = tex_pool_find(w, (dv_texture_t)msg->arg1);
            if (pi < 0) break;
            uint16_t tw = w->tex_pool[pi].w;
            uint16_t th = w->tex_pool[pi].h;
            uint32_t row0  = msg->arg2;
            uint32_t nrows = msg->arg3;
            if (nrows == 0)                     /* legacy monoblocco   */
            {
                row0  = 0;
                nrows = th;
            }
            if (row0 >= th || nrows > th - row0) break;
            uint32_t need = nrows * (uint32_t)tw * 4u;
            if (!msg->payload || msg->payload_size < need) break;
            dv_rect_t r = { 0, (int32_t)row0, tw, nrows };
            dv_texture_update_region((dv_texture_t)w->tex_pool[pi].handle, r,
                                     msg->payload, (uint32_t)tw * 4u);
            if (w->tex_pool[pi].cpu != NULL)
            {
                memcpy(w->tex_pool[pi].cpu + (size_t)row0 * tw,
                       msg->payload, need);   /* banda nello specchio */
            }
            break;
        }

        case GUI_WIN_TEX_FREE:
        {
            int idx = win_find_by_id(msg->arg0);
            if (idx < 0) break;
            window_t *w = &windows[idx];
            int pi = tex_pool_find(w, (dv_texture_t)msg->arg1);
            if (pi < 0) break;
            dv_texture_destroy(w->tex_pool[pi].handle);
            free(w->tex_pool[pi].cpu);
            w->tex_pool[pi].cpu = NULL;
            /* Compatta: l'ultima voce scende nello slot liberato.
             * L'ordine non e' osservabile dal client. */
            w->tex_pool_count--;
            if ((uint32_t)pi != w->tex_pool_count)
            {
                w->tex_pool[pi] = w->tex_pool[w->tex_pool_count];
            }
            w->tex_pool[w->tex_pool_count].handle = DV_HANDLE_NONE;
            break;
        }

        case GUI_WIN_SHM_ENSURE:
        {
            /* Pannello SHM del contenuto: crea (o rialloca a nuova
             * misura) un framebuffer condiviso legato alla finestra.
             * Reply: arg0 = rc (0 ok), arg1 = shm_id da mappare lato
             * client. Limiti come il tex pool: mai oltre lo schermo. */
            int idx = win_find_by_id(msg->arg0);
            reply->arg0 = (uint32_t)-1;
            reply->arg1 = (uint32_t)-1;
            if (idx < 0) break;
            window_t *w = &windows[idx];
            uint32_t pw = msg->arg1, ph = msg->arg2;
            if (pw == 0 || ph == 0
                || pw > (uint32_t)SCREEN_W || ph > (uint32_t)SCREEN_H) break;

            if (w->panel_shm_id >= 0
                && w->panel_w == (uint16_t)pw && w->panel_h == (uint16_t)ph)
            {
                reply->arg0 = 0;
                reply->arg1 = (uint32_t)w->panel_shm_id;   /* gia' pronta */
                break;
            }
            if (w->panel_shm_id >= 0)
            {
                shm_unmap(w->panel_shm_id);
                w->panel_shm_id = -1;
                w->panel_ptr    = NULL;
            }
            uint32_t vaddr = 0;   /* la libc passa il vaddr come intero */
            int sid = shm_create(pw * ph * 4u, &vaddr);
            if (sid < 0 || !vaddr) break;
            memset((void *)(uintptr_t)vaddr, 0, (size_t)pw * ph * 4u);
            w->panel_shm_id = sid;
            w->panel_ptr    = (uint32_t *)(uintptr_t)vaddr;
            w->panel_synced = false;      /* primo bake: copia integrale */
            w->panel_w      = (uint16_t)pw;
            w->panel_h      = (uint16_t)ph;
            reply->arg0 = 0;
            reply->arg1 = (uint32_t)sid;
            break;
        }

        case GUI_PANEL_SET_CMDS:
        {
            int idx = win_find_by_id(msg->arg0);
            if (idx >= 0 && msg->payload)
            {
                parse_ctx_commands(&windows[idx],
                                   (const char *)msg->payload,
                                   msg->payload_size);
                if (idx == focused_win)
                    panel_sync_context();
                /* Solo contenuto del pannello — il desktop non cambia.
                 * panel_recalculate accende DIRTY_FULL se la
                 * larghezza cambia. */
                di_mark_dirty(DIRTY_PANEL);
            }
            break;
        }

        case GUI_PANEL_CLEAR_CMDS:
        {
            int idx = win_find_by_id(msg->arg0);
            if (idx >= 0)
            {
                windows[idx].ctx_count = 0;
                if (idx == focused_win)
                    panel_sync_context();
                di_mark_dirty(DIRTY_PANEL);
            }
            break;
        }

        case GUI_SET_CURSOR:
        {
            /* arg0 = id finestra, arg1 = tipo cursore (o
             * CURSOR_DEFAULT castato a unsigned per "togli
             * l'override").
             *
             * Override per-finestra: il cursore prende la forma
             * richiesta SOLO col mouse dentro il corpo di questa
             * finestra (cursor_for_position consulta il campo). Fuori
             * — pannello, corpo altrui, bordo schermo — decide il WM.
             * Alla distruzione l'override muore con la finestra: il
             * client non deve fare pulizia sulla propria morte.
             *
             * Event-driven: i chiamanti sparano al press (HSPLIT) e
             * al release (DEFAULT). Nessun IPC per-frame. */
            int idx = win_find_by_id(msg->arg0);
            if (idx >= 0)
            {
                int requested = (int)msg->arg1;
                /* Accetta CURSOR_DEFAULT (sentinella) o un CURSOR_*
                 * valido. Il resto si rifiuta in silenzio: un client
                 * bacato non puo' far cadere il compositor. */
                if (requested == CURSOR_DEFAULT
                    || (requested >= 0 && requested < CURSOR_COUNT))
                {
                    windows[idx].cursor_override = requested;
                    /* Rinfresca il cursore SUBITO anche a mouse fermo
                     * — o l'utente dovrebbe scuoterlo prima di vedere
                     * la forma nuova. */
                    int new_cursor = cursor_for_position(cursor_x, cursor_y);
                    if (new_cursor != current_cursor)
                    {
                        current_cursor = new_cursor;
                        di_mark_dirty(DIRTY_CURSOR);
                    }
                }
            }
            break;
        }

        /* ---- IPC del tray ---- */

        case GUI_WIDGET_CREATE:
        {
            int ww = (int)msg->arg0;
            int wh = (int)msg->arg1;
            uint32_t owner_port = msg->arg2;

            int idx = widget_create(ww, wh, msg->sender_pid, owner_port);
            if (idx >= 0)
            {
                reply->arg0 = widgets[idx].id;
                reply->arg1 = (uint32_t)widgets[idx].shm_id;
                /* Geometria del tray da rifare se e' aperto. */
                if (widget_panel_open)
                {
                    wpanel_calc_geometry();
                    di_mark_dirty(DIRTY_FULL);
                }
            }
            else
            {
                reply->arg0 = 0;
                reply->arg1 = (uint32_t)-1;
            }
            break;
        }

        case GUI_WIDGET_DESTROY:
        {
            int idx = widget_find_by_id(msg->arg0);
            if (idx >= 0)
            {
                widget_destroy(idx);
                if (widget_panel_open)
                {
                    wpanel_calc_geometry();
                    di_mark_dirty(DIRTY_FULL);
                }
            }
            break;
        }

        case GUI_WIDGET_INVALIDATE:
        {
            int idx = widget_find_by_id(msg->arg0);
            if (idx >= 0)
            {
                widget_slot_t *ws = &widgets[idx];

                /* Riassemblaggio dei segmenti (foglio cmdbuf); gli
                 * intermedi vengono assorbiti qui. */
                const uint8_t *src = NULL;
                uint32_t       sz  = 0;
                if (!cmdbuf_reasm_ingest(&ws->reasm, msg, &src, &sz))
                {
                    break;
                }

                /* Parse del cmdbuf e render nello SHM del widget via
                 * primitive software. wpanel_draw poi blitta il
                 * risultato nella cmdlist del tray. */
                if (src && sz >= DOBUI_CMDBUF_HDR_SIZE && ws->buffer)
                {
                    uint32_t magic = (uint32_t)src[0]
                                   | ((uint32_t)src[1] <<  8)
                                   | ((uint32_t)src[2] << 16)
                                   | ((uint32_t)src[3] << 24);
                    if (magic == DOBUI_CMDBUF_MAGIC)
                        widget_replay_cmdbuf_to_shm(ws, src, sz);
                }

                widgets[idx].content_dirty = true;
                if (widget_panel_open)
                    di_mark_dirty(DIRTY_FULL);
            }
            break;
        }

        case GUI_SPAWN_DRIVER:
        {
            /* Layout del payload prodotto da dobui_SpawnDriver:
             *   [0 .. arg0]            path + NUL
             *   [arg0 .. payload_size] blob argv (header + stringhe) o vuoto
             * arg0 == 0 = "niente argv", solo il path.
             *
             * Il blob non si deserializza qui — la
             * spawn_file_driver_with_blob del kernel lo prende cosi'
             * com'e'. Spawn ASINCRONO: il load ELF gira in un thread
             * di sfondo e il worker fa make_driver dopo il load; il
             * ciclo del compositor continua a servire eventi durante
             * lo spawn. Il PID non e' ancora noto alla reply:
             * reply.arg0 = 1 a sottomissione ok, 0 su fallimento. */
            const char *path  = msg->payload ? (const char *)msg->payload : "";
            uint32_t    off   = msg->arg0;
            const void *blob  = NULL;
            uint32_t    bsize = 0;
            if (off > 0 && off < msg->payload_size)
            {
                blob  = (const char *)msg->payload + off;
                bsize = msg->payload_size - off;
            }
            int ok = spawn_file_driver_with_blob(path, blob, bsize);
            reply->arg0 = (ok > 0) ? 1 : 0;
            break;
        }

        case GUI_LAUNCH_PROGRAM:
        {
            /* Lancio di un programma GUI per conto di un chiamante
             * (motore azioni DAS). Payload = una stringa comando
             * separata da spazi: "path [arg1 .. argN]". Tokenizzata
             * qui e lanciata con la stessa spawn_file del launcher da
             * desktop — l'unico percorso di lancio programmi
             * esercitato (e provato) ogni giorno. Regola argv di
             * MainDOB: argv[0] e' il PRIMO ARGOMENTO, non il nome del
             * programma. */
            if (!msg->payload || msg->payload_size == 0)
            {
                reply->arg0 = 0;
                break;
            }
            static char cmd[192];
            uint32_t n = msg->payload_size;
            if (n > sizeof(cmd) - 1) n = sizeof(cmd) - 1;
            memcpy(cmd, msg->payload, n);
            cmd[n] = '\0';

            const char *cargv[9];
            int   cn = 0;
            char *path2 = NULL;
            {
                /* Split manuale sugli spazi (questa libc non ha
                 * strtok). */
                char *p = cmd;
                while (*p && (path2 == NULL || cn < 8))
                {
                    while (*p == ' ') *p++ = '\0';
                    if (!*p) break;
                    if (!path2) path2 = p;
                    else        cargv[cn++] = p;
                    while (*p && *p != ' ') p++;
                }
            }
            cargv[cn] = NULL;

            if (!path2)
            {
                reply->arg0 = 0;
                break;
            }
            {
                char l[128];
                snprintf(l, sizeof(l),
                         "[dobinterface] launch_program: %s (%d args)\n",
                         path2, cn);
                debug_print(l);
            }
            reply->arg0 = (spawn_file(path2, cn ? cargv : NULL) >= 0)
                              ? 1 : 0;
            break;
        }

        case GUI_BEGIN_DRAG:
        {
            /* La sorgente deve esistere (serve la sua porta eventi). */
            uint32_t src_win_id = msg->arg0;
            int src_idx = win_find_by_id(src_win_id);
            if (src_idx < 0)
            {
                reply->arg0 = (uint32_t)-1;
                break;
            }

            /* La sessione la possiede il foglio input: rifiuta da se'
             * se un drag qualunque e' gia' in corso, o su payload
             * vuoto/OOM. */
            reply->arg0 = dragfiles_begin(src_win_id,
                                          windows[src_idx].owner_port,
                                          msg->payload, msg->payload_size)
                              ? 0 : (uint32_t)-1;
            break;
        }

        case GUI_CANCEL_DRAG:
        {
            dragfiles_cancel();
            reply->arg0 = 0;
            break;
        }

        /* ===== Password di accesso (foglio logon) =====
         * I due opcode della entry EPS SECRET dell'editor
         * Impostazioni. Sincroni: l'editor attende la reply. Tutta la
         * semantica (verifica, tenda, toast) e' del foglio logon —
         * qui c'e' solo l'instradamento. */
        case GUI_LOGON_PW_READ:
        {
            logon_eps_read(reply);
            break;
        }

        case GUI_LOGON_PW_WRITE:
        {
            logon_eps_write(msg, reply);
            break;
        }

        case GUI_DEVICE_ATTACH:
        {
            if (msg->payload
                && msg->payload_size >= sizeof(gui_device_attach_t))
            {
                const gui_device_attach_t *p =
                    (const gui_device_attach_t *)msg->payload;
                icon_add(p);
            }
            break;
        }

        case GUI_SHOW_TOAST:
        {
            /* Il messaggio di un componente senza finestra diventa un
             * banner. Payload = testo NUL-terminato; arg0 porta la
             * severita' (riservata a colore/suono futuri). Solo post. */
            if (msg->payload && msg->payload_size > 0)
            {
                char tbuf[128];
                uint32_t n = msg->payload_size;
                if (n > sizeof(tbuf) - 1) n = sizeof(tbuf) - 1;
                memcpy(tbuf, msg->payload, n);
                tbuf[n] = '\0';
                toast_show(tbuf);
            }
            break;
        }

        case GUI_DEVICE_DETACH:
        {
            if (msg->payload
                && msg->payload_size >= sizeof(gui_device_detach_t))
            {
                const gui_device_detach_t *p =
                    (const gui_device_detach_t *)msg->payload;
                icon_remove_by_id(p->device_id);
            }
            break;
        }

        case GUI_LIST_DEVICES:
        {
            /* Query sincrona: dump delle desktop_icons[] come header
             * gui_device_list_t + N voci impacchettate. Usata dalla
             * vista Monta di DobFiles per specchiare in finestra i
             * device montabili del desktop. Dimensionata al caso
             * peggiore (64 voci) in un buffer statico — entra comoda
             * nel payload IPC. */
            static struct {
                gui_device_list_t       hdr;
                gui_device_list_entry_t entries[DEV_LIST_MAX_ENTRIES];
            } out;
            uint32_t n = 0;
            for (int i = 0; i < MAX_DESKTOP_ICONS
                            && n < DEV_LIST_MAX_ENTRIES; i++)
            {
                if (!desktop_icons[i].in_use) continue;
                gui_device_list_entry_t *e = &out.entries[n++];
                e->device_id = desktop_icons[i].device_id;
                e->kind      = desktop_icons[i].kind;
                memcpy(e->label,        desktop_icons[i].label,
                       sizeof(e->label));
                memcpy(e->service_name, desktop_icons[i].service_name,
                       sizeof(e->service_name));
                e->bitmap = desktop_icons[i].bitmap;
            }
            out.hdr.count = n;
            reply->arg0 = 0;
            reply->payload = &out;
            reply->payload_size = (uint32_t)(sizeof(out.hdr)
                                  + n * sizeof(out.entries[0]));
            break;
        }
    }
}
