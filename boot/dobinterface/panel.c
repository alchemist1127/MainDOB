/* MainDOB DobInterface 2.0 — foglio: panel.c
 *
 * La barra dei comandi (pannello destro): il cuore dell'interfaccia
 * MainDOB. Slot pre-allocati (zero crescita a runtime), header
 * condiviso (titolo della finestra in focus o "MainDOB"), comandi
 * finestra a visibilita' condizionata, sezione contesto data-driven
 * (dal programma in focus, dal menu MainDOB, o dal menu DAS di
 * un'icona), footer ancorato in basso.
 *
 * Incapsulamento 2.0: il motore finestre non tocca piu' panel_items[]
 * a mani nude — passa dai verbi panel_focus_window /
 * panel_focus_desktop / panel_set_maximize_*. Le etichette, i colori
 * e le metriche sono specifica [PX]: centratura delle voci, hover
 * COLOR_PANEL_HOVER, titolo in giallo, mute per i disabilitati. */

#include "di_internal.h"

/* ================= stato condiviso (proprieta' di questo foglio) ====== */

int  panel_w   = 0;        /* calcolato a runtime      */
int  desktop_w = 0;        /* SCREEN_W - panel_w       */
int  panel_x   = 0;        /* inizio pannello          */
bool panel_width_changed = false;
bool maindob_menu_open     = false;
bool about_overlay_active  = false;
int  panel_hover_idx = -1; /* voce sotto il mouse (dal foglio input) */

/* ================= stato privato ====================================== */

static panel_item_t panel_items[MAX_PANEL_ITEMS];
static int panel_item_count = 0;

/* Voci fisse — indici per accesso rapido */
static int idx_header       = -1;
static int idx_close        = -1;
static int idx_maximize     = -1;
static int idx_hide         = -1;
static int idx_show_windows = -1;
static int idx_widgets      = -1;
static int idx_ctx_start    = -1;   /* primo slot SECTION_CONTEXT */

static int ctx_item_count_vis = 0;

/* ================= verbi esecutivi ==================================== */

static int panel_add_item(uint8_t section, const char *label,
                          bool enabled, uint32_t cmd_id)
{
    if (panel_item_count >= MAX_PANEL_ITEMS) return -1;
    int idx = panel_item_count++;
    panel_items[idx].section = section;
    strncpy(panel_items[idx].label, label, 63);
    panel_items[idx].label[63] = '\0';
    panel_items[idx].command_id = cmd_id;
    panel_items[idx].visible = true;
    panel_items[idx].enabled = enabled;
    panel_items[idx].hit_y = 0;
    panel_items[idx].hit_h = 0;
    return idx;
}

/* Lo slot header e' condiviso: col focus mostra il titolo della
 * finestra; sul desktop nudo torna alla voce "MainDOB" — che e'
 * l'unica via a build info e spegnimento, raggiungibili quindi solo
 * dal desktop. */
static void panel_header_show_window_title(const char *title)
{
    strncpy(panel_items[idx_header].label, title, 63);
    panel_items[idx_header].label[63]  = '\0';
    panel_items[idx_header].enabled    = false;  /* etichetta, non bottone */
    panel_items[idx_header].command_id = 0;
}

static void panel_header_show_maindob(void)
{
    strncpy(panel_items[idx_header].label, "MainDOB", 63);
    panel_items[idx_header].label[63]  = '\0';
    panel_items[idx_header].enabled    = true;
    panel_items[idx_header].command_id = CMD_MAINDOB_MENU;
}

/* ================= verbi pubblici: focus e comandi ==================== */

void panel_set_maximize_label(bool maximized)
{
    strncpy(panel_items[idx_maximize].label,
            maximized ? "Finestra" : "Ingrandisci", 63);
}

void panel_set_maximize_visible(bool visible)
{
    panel_items[idx_maximize].visible = visible;
}

/* Il pannello segue il focus su una finestra: titolo nell'header,
 * comandi finestra visibili (Ingrandisci secondo policy), etichetta
 * del maximize dallo stato corrente. */
void panel_focus_window(const window_t *w)
{
    panel_header_show_window_title(w->title);
    panel_items[idx_close].visible    = true;
    panel_items[idx_maximize].visible = !w->no_maximize;
    panel_items[idx_hide].visible     = true;
    panel_set_maximize_label(w->maximized);
}

/* Il pannello torna allo stato desktop: header "MainDOB", comandi
 * finestra nascosti. */
void panel_focus_desktop(void)
{
    panel_header_show_maindob();
    panel_items[idx_close].visible    = false;
    panel_items[idx_maximize].visible = false;
    panel_items[idx_hide].visible     = false;
}

/* Contesto a insieme fisso di etichette + command ID. Usa solo gli
 * slot pre-allocati — zero allocazioni a runtime. */
void panel_set_context_items(const char *labels[], const uint32_t cmds[],
                             int count)
{
    if (count > MAX_WIN_CTX_CMDS) count = MAX_WIN_CTX_CMDS;

    ctx_item_count_vis = 0;
    for (int i = 0; i < MAX_WIN_CTX_CMDS; i++)
    {
        int slot = idx_ctx_start + i;
        if (i < count)
        {
            strncpy(panel_items[slot].label, labels[i], 63);
            panel_items[slot].label[63] = '\0';
            panel_items[slot].command_id = cmds[i];
            panel_items[slot].visible = true;
            panel_items[slot].enabled = true;
            ctx_item_count_vis++;
        }
        else
        {
            panel_items[slot].visible = false;
        }
    }
}

/* ================= terra di mezzo: layout e contesto ================== */

/* Ricalcola la larghezza dal contenuto (voce piu' larga + padding),
 * con minimo 140 e cap a SCREEN_W/4. Aggiorna la geometria desktop
 * solo se panel_w cambia davvero. */
void panel_recalculate(void)
{
    uint32_t max_w = 0;
    for (int i = 0; i < panel_item_count; i++)
    {
        if (!panel_items[i].visible) continue;
        uint32_t w = font_string_width(panel_items[i].label);
        if (w > max_w) max_w = w;
    }

    int new_panel_w = (int)max_w + PANEL_PAD_L + PANEL_PAD_R;
    if (new_panel_w < 140) new_panel_w = 140;
    if (new_panel_w > SCREEN_W / 4) new_panel_w = SCREEN_W / 4;

    if (new_panel_w != panel_w)
    {
        panel_w = new_panel_w;
        desktop_w = SCREEN_W - panel_w;
        panel_x = desktop_w;
        panel_width_changed = true;
        di_mark_dirty(DIRTY_FULL);
    }
}

/* Ricostruisce le voci SECTION_CONTEXT: menu MainDOB se aperto,
 * default desktop senza focus, altrimenti i comandi registrati dal
 * programma in focus. Solo slot pre-allocati. */
void panel_sync_context(void)
{
    /* Il menu MainDOB prevale su tutto. */
    if (maindob_menu_open)
    {
        const char *labels[] = { "Spegni", "Riavvia", "Blocca",
                                 "Oscura schermo", "Info" };
        const uint32_t cmds[] = { CMD_SHUTDOWN, CMD_REBOOT, CMD_LOCK,
                                  CMD_DARKEN, CMD_ABOUT };
        panel_set_context_items(labels, cmds, 5);
        panel_recalculate();
        return;
    }

    /* Nessuna finestra in focus: comandi desktop di default. */
    if (focused_win < 0 || !windows[focused_win].used)
    {
        const char *labels[] = { "Apri File", "Apri Moduli", "Apri Impostazioni" };
        const uint32_t cmds[] = { CMD_OPEN_FILES, CMD_OPEN_MODULES, CMD_OPEN_SETTINGS };
        panel_set_context_items(labels, cmds, 3);
        panel_recalculate();
        return;
    }

    /* Finestra in focus: i suoi comandi di contesto. */
    window_t *w = &windows[focused_win];
    int count = w->ctx_count;
    if (count > MAX_WIN_CTX_CMDS) count = MAX_WIN_CTX_CMDS;

    const char *labels[MAX_WIN_CTX_CMDS];
    uint32_t cmds[MAX_WIN_CTX_CMDS];
    for (int i = 0; i < count; i++)
    {
        labels[i] = w->ctx_labels[i];
        cmds[i] = CMD_CTX_BASE + (uint32_t)i;
    }
    panel_set_context_items(labels, cmds, count);
    panel_recalculate();
}

/* ================= disegno e hit-test ================================= */

/* Disegna una voce centrata alla y data e ne registra la hit zone.
 * [PX] Le voci sono CENTRATE (la barra e' il cuore della UI, le
 * etichette leggono meglio centrate che allineate come testo di
 * corpo), con ripiego sul pad sinistro se l'etichetta sborda. */
static void panel_draw_item(int i, int y)
{
    panel_item_t *item = &panel_items[i];

    if (i == panel_hover_idx && item->enabled)
        fb_fill_rect(panel_x, y, panel_w, ITEM_HEIGHT, COLOR_PANEL_HOVER);

    uint32_t color;
    if (i == idx_header && !item->enabled)
        color = COLOR_YELLOW;          /* titolo della finestra in focus */
    else if (!item->enabled)
        color = COLOR_TEXT_MUTE;
    else if (i == panel_hover_idx)
        color = COLOR_WHITE;
    else
        color = COLOR_CYAN;

    int tw = (int)font_string_width(item->label);
    int tx = panel_x + (panel_w - tw) / 2;
    if (tx < panel_x + PANEL_PAD_L) tx = panel_x + PANEL_PAD_L;
    int ty = y + ITEM_PAD_TOP;
    font_draw_string(tx, ty, item->label, color,
                     (i == panel_hover_idx && item->enabled)
                         ? COLOR_PANEL_HOVER : COLOR_PANEL_BG);

    item->hit_y = y;
    item->hit_h = ITEM_HEIGHT;
}

void panel_draw(void)
{
    fb_fill_rect(panel_x, 0, panel_w, SCREEN_H, COLOR_PANEL_BG);

    int y = PANEL_MARGIN_TOP;
    uint8_t last_section = 255;

    /* Sezioni alte (header, finestra, contesto). */
    for (int i = 0; i < panel_item_count; i++)
    {
        panel_item_t *item = &panel_items[i];
        if (!item->visible) continue;
        if (item->section == SECTION_FOOTER) continue;  /* in basso */

        /* Separatore di sezione. */
        if (item->section != last_section && last_section != 255)
        {
            y += 4;
            fb_draw_hline(panel_x + 8, y, panel_w - 16, COLOR_STRUCT_LINE);
            y += SEPARATOR_H - 4;
        }
        last_section = item->section;

        panel_draw_item(i, y);
        y += ITEM_HEIGHT;
    }

    /* Footer — ancorato in basso, dal fondo. */
    int footer_y = SCREEN_H - PANEL_MARGIN_TOP;
    for (int i = panel_item_count - 1; i >= 0; i--)
    {
        panel_item_t *item = &panel_items[i];
        if (!item->visible || item->section != SECTION_FOOTER) continue;
        footer_y -= ITEM_HEIGHT;
        panel_draw_item(i, footer_y);
    }
}

/* Hit test: indice della voce o -1. */
int hit_test_panel(int my)
{
    for (int i = 0; i < panel_item_count; i++)
    {
        panel_item_t *item = &panel_items[i];
        if (!item->visible || !item->enabled) continue;
        if (my >= item->hit_y && my < item->hit_y + item->hit_h)
            return i;
    }
    return -1;
}

/* command_id della voce (per i test speciali del foglio input:
 * "il clic e' su '<'?", "e' su Mostra finestre?"). */
uint32_t panel_item_command(int idx)
{
    if (idx < 0 || idx >= panel_item_count) return 0;
    return panel_items[idx].command_id;
}

/* ================= logica ad alto livello ============================= */

void panel_init_items(void)
{
    panel_item_count = 0;

    idx_header = panel_add_item(SECTION_HEADER, "MainDOB", true, CMD_MAINDOB_MENU);
    idx_close = panel_add_item(SECTION_WINDOW, "Chiudi", true, CMD_WIN_CLOSE);
    panel_items[idx_close].visible = false;
    idx_maximize = panel_add_item(SECTION_WINDOW, "Ingrandisci", true, CMD_WIN_MAXIMIZE);
    panel_items[idx_maximize].visible = false;
    idx_hide = panel_add_item(SECTION_WINDOW, "Nascondi", true, CMD_WIN_HIDE);
    panel_items[idx_hide].visible = false;

    /* Tutti gli slot di contesto pre-allocati subito: MAX_WIN_CTX_CMDS
     * copre il caso peggiore (16 comandi). I default desktop ne usano
     * 3, il menu MainDOB 3 — entrambi entrano. panel_item_count e'
     * FISSO da qui in poi — zero crescita a runtime. */
    idx_ctx_start = panel_item_count;
    for (int i = 0; i < MAX_WIN_CTX_CMDS; i++)
    {
        int s = panel_add_item(SECTION_CONTEXT, "", true, 0);
        panel_items[s].visible = false;
    }

    idx_widgets = panel_add_item(SECTION_FOOTER, "<", true, CMD_OPEN_WIDGETS);
    idx_show_windows = panel_add_item(SECTION_FOOTER, "Mostra finestre", true, CMD_SHOW_WINDOWS);
}

/* handle_panel_click — esegue il comando della voce cliccata.
 * L'algoritmo della barra: chiudi menu/tray se il clic e' altrove,
 * esegui, risincronizza il contesto. */
void handle_panel_click(int item_idx)
{
    if (item_idx < 0) return;
    panel_item_t *item = &panel_items[item_idx];

    /* Cliccare qualunque cosa che non sia l'header MainDOB chiude il
     * menu (senza resync qui: prima esegue il comando, il resync
     * arriva in coda). */
    if (item->command_id != CMD_MAINDOB_MENU && maindob_menu_open)
    {
        maindob_menu_open = false;
    }

    /* Cliccare qualunque voce che non sia '<' chiude il tray. */
    if (item->command_id != CMD_OPEN_WIDGETS && widget_panel_open)
    {
        widget_panel_open = false;
        di_mark_dirty(DIRTY_FULL);
    }

    switch (item->command_id)
    {
        case CMD_MAINDOB_MENU:
            maindob_menu_open = !maindob_menu_open;
            panel_sync_context();
            di_mark_dirty(DIRTY_FULL);
            return;  /* early return — niente doppio resync in coda */

        case CMD_WIN_CLOSE:
            if (focused_win >= 0)
            {
                /* Con un owner vivo: richiesta di chiusura, non
                 * esecuzione capitale. */
                if (windows[focused_win].owner_port != 0)
                {
                    send_win_event(focused_win, GUI_EVT_CLOSE_REQ, 0, 0, 0);
                }
                else
                {
                    win_destroy(focused_win);
                    win_unfocus_all();
                }
            }
            break;

        case CMD_WIN_MAXIMIZE:
            if (focused_win >= 0 && !windows[focused_win].no_maximize)
                win_toggle_maximize(focused_win);
            break;

        case CMD_WIN_HIDE:
            if (focused_win >= 0)
            {
                windows[focused_win].visible = false;
                /* Libera layer+surface per rilasciare RAM del driver
                 * da nascosta. RAISE rialloca e rigioca last_cmdbuf. */
                win_free_video(&windows[focused_win]);
                z_sort_valid = false;
                win_unfocus_all();
            }
            break;

        case CMD_SHOW_WINDOWS:
            mc_enter();
            break;

        case CMD_OPEN_WIDGETS:
        {
            if (widget_panel_open)
            {
                widget_panel_open = false;
                di_mark_dirty(DIRTY_FULL);
                break;
            }

            int wcount = 0;
            for (int i = 0; i < MAX_WIDGETS; i++)
            {
                if (widgets[i].used) wcount++;
            }
            if (wcount == 0)
            {
                toast_show("Nessun widget disponibile");
                break;
            }

            wpanel_open();
            di_mark_dirty(DIRTY_FULL);
            break;
        }

        case CMD_OPEN_FILES:
            if (spawn_file("/SYSTEM/PROGRAMS/DobFiles/DobFiles.mdl", NULL) < 0)
                toast_show("Impossibile avviare DobFiles");
            break;

        case CMD_OPEN_MODULES:
            if (spawn_file("/SYSTEM/PROGRAMS/modules/modules.mdl", NULL) < 0)
                toast_show("Impossibile avviare Moduli");
            break;

        case CMD_OPEN_SETTINGS:
            if (spawn_file("/SYSTEM/PROGRAMS/DobSettings/DobSettings.mdl", NULL) < 0)
                toast_show("Impossibile avviare Impostazioni");
            break;

        case CMD_SHUTDOWN:
        {
            debug_print("[dobinterface] Shutdown requested.\n");

            /* Spegnimento ACPI mediato dal kernel (i port write girano
             * in ring 0: niente PRIV_DRIVER qui). Non ritorna. */
            power_off();

            /* Se mai si arrivasse qui, il kernel non conosce
             * SYS_SHUTDOWN (build libc/kernel disallineate). Uscita
             * pulita: il processo muore, il respawn del primary ci
             * riporta su e l'utente puo' riprovare invece di restare
             * incastrato. Soprattutto: NESSUN `cli; hlt` — lo
             * userspace non deve mai contenere opcode privilegiati,
             * nemmeno come fallback "difensivo". In un microkernel lo
             * userspace che muore e' recuperabile; il kernel fermo no. */
            _exit(-1);
            break;
        }

        case CMD_REBOOT:
        {
            debug_print("[dobinterface] Reboot requested.\n");

            /* Reset 8042 mediato dal kernel. dobinterface e' un primary
             * auto-promosso, che il kernel accetta per SYS_REBOOT.
             * Non ritorna in caso di successo. */
            reboot();

            /* Raggiunto solo se la syscall e' irraggiungibile o
             * rifiutata (build disallineata / privilegio perso).
             * Uscita pulita, mai opcode privilegiati da userspace. */
            _exit(-1);
            break;
        }

        case CMD_ABOUT:
            /* L'overlay About lo rende compositor_repaint finche'
             * about_overlay_active e' vero. Disegnare qui direttamente
             * verrebbe spazzato al prossimo frame, quando il repaint
             * resetta il backbuf prima di ridisegnare il desktop. */
            about_overlay_active = true;
            di_mark_dirty(DIRTY_FULL);
            break;

        case CMD_LOCK:
            /* Con una password: schermata di accesso. Senza: degrada
             * al solo oscuramento (identico a Oscura) — mai uno stato
             * bloccato senza via d'uscita. La resa e' del compositor
             * (ramo tenda), come per About. */
            logon_lock();
            break;

        case CMD_DARKEN:
            /* Screensaver immediato, con o senza password. Al
             * risveglio vale la regola unica: password -> prompt,
             * altrimenti desktop. */
            logon_darken();
            break;

        default:
            /* Voce del menu icona (CMD_ICON_MENU_BASE..+MAX): inoltra a
             * hotplug. La sezione menu {} del DAS e' l'unica fonte di
             * verita' — dobinterface non sa ne' gli importa cosa
             * faccia ogni voce. */
            if (item->command_id >= CMD_ICON_MENU_BASE
                && item->command_id < CMD_ICON_MENU_BASE + DEV_MENU_MAX_ITEMS)
            {
                icon_menu_activate(item->command_id - CMD_ICON_MENU_BASE);
            }
            /* Comando di contesto? Inoltra al programma proprietario. */
            else if (item->command_id >= CMD_CTX_BASE && focused_win >= 0)
            {
                int ctx_idx = (int)(item->command_id - CMD_CTX_BASE);
                send_win_event(focused_win, GUI_EVT_PANEL_CMD, (uint32_t)ctx_idx, 0, 0);
            }
            break;
    }

    /* Dopo l'esecuzione, risincronizza il contesto (menu chiuso). */
    panel_sync_context();
    di_mark_dirty(DIRTY_FULL);
}
