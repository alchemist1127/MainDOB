/* MainDOB DobInterface 2.0 — foglio: window.c
 *
 * Il motore finestre: ciclo di vita degli slot, gerarchia owner
 * (sub-window, dialoghi, modali), z-order come singola sorgente di
 * verita', focus, hit-test e consegna verificata degli eventi al
 * programma proprietario.
 *
 * Invarianti dello stacking (validi per costruzione, non per patch):
 *  - le figlie stanno SOPRA le madri;
 *  - i modali stanno sopra i fratelli non modali;
 *  - il bersaglio del focus sopra i pari di livello;
 *  - l'intero gruppo owner sale in cima COME UNITA';
 *  - gli z restano contigui in [0, n_used-1], sotto i layer fissi
 *    (backbuf overlay z=50, tray z=120, toast z=500, cursore z=999).
 *
 * Fix 1.x inglobati nel design:
 *  - clamp di spawn: una finestra piu' alta/larga dello schermo
 *    invertiva il clamp del move e il primo drag strappava la title
 *    bar fuori dallo schermo ("window stretch") — misura serrata
 *    PRIMA della posizione, mai il contrario;
 *  - death-watch anti-ABA: la consegna passa SOLO da win_owner_post
 *    (porta + generazione); un DOB_ERR_DEAD marca la finestra orfana
 *    e la distruzione avviene al punto sicuro del ciclo, mai dentro
 *    hit-test o dispatch;
 *  - le finestre non-ready (mai invalidate) restano fuori dallo
 *    z-sort: nessun lampo di rettangolo vuoto prima del primo frame
 *    del programma. */

#include "di_internal.h"

/* ================= stato condiviso (proprieta' di questo foglio) ====== */

window_t windows[MAX_WINDOWS];
int      focused_win         = -1;
int      pointer_capture_win = -1;
int      sorted_wins[MAX_WINDOWS];
int      sorted_count        = 0;
bool     z_sort_valid        = false;

/* ================= stato privato ====================================== */

static uint32_t next_win_id = 1;

/* ================= verbi esecutivi ==================================== */

int win_find_by_id(uint32_t id)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].used && windows[i].id == id) return i;
    return -1;
}

/* Primo slot libero, o -1 (parco pieno). */
static int win_find_free_slot(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!windows[i].used) return i;
    return -1;
}

/* z sopra ogni finestra viva (il nuovo arrivato nasce in cima). */
static int win_next_top_z(void)
{
    int max_z = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].used && windows[i].z_order >= max_z)
            max_z = windows[i].z_order + 1;
    return max_z;
}

/* Serra misura e posizione di spawn allo schermo utilizzabile.
 * ORDINE OBBLIGATO: prima la misura, poi la posizione. Una finestra
 * piu' alta (o larga) del display non si mostra intera e, peggio,
 * inverte il clamp del move: SCREEN_H - (h + WIN_HEADER_H) va
 * negativo e clamp(lo=0, hi<0) ritorna l'hi negativo — al primo drag
 * della title bar il frame schizza in alto e barra e tab spariscono
 * dal bordo superiore (il baco "window stretch"). */
static void win_clamp_spawn_geometry(int *x, int *y, int *w, int *h)
{
    if (*w < WIN_MIN_SIZE) *w = WIN_MIN_SIZE;
    if (*h < WIN_MIN_SIZE) *h = WIN_MIN_SIZE;

    if (desktop_w > 0 && *w > desktop_w) *w = desktop_w;
    {
        int max_h = SCREEN_H - WIN_HEADER_H;
        if (max_h >= WIN_MIN_SIZE && *h > max_h) *h = max_h;
    }
    {
        int total_h = *h + WIN_HEADER_H;
        if (desktop_w > 0 && *x > desktop_w - *w) *x = desktop_w - *w;
        if (*x < 0) *x = 0;
        if (*y > SCREEN_H - total_h) *y = SCREEN_H - total_h;
        if (*y < 0) *y = 0;
    }
}

/* Suona il ding "hai cliccato una finestra bloccata". Stub per ora:
 * il ding vero DEVE essere fire-and-forget (PLAYER_CMD_PLAY a post);
 * il dobplayer.Play sincrono bloccherebbe il loop input del WM,
 * congelando il desktop per il round-trip IPC. */
void win_attention_ding(void)
{
    /* TODO(dang): suono UI asincrono via servizio audioplayer. */
}

/* Lista z-crescente delle finestre visibili e ready. Cache: si
 * ricostruisce solo dopo un'invalidazione (z_sort_valid). Le
 * non-ready restano fuori: blittare un framebuffer vuoto farebbe
 * lampeggiare un rettangolo bianco/zero fino al primo frame del
 * programma. */
void sort_windows_by_z(void)
{
    if (z_sort_valid) return;

    sorted_count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
    {
        if (windows[i].used && windows[i].visible && windows[i].ready)
            sorted_wins[sorted_count++] = i;
    }
    /* Insertion sort per z_order: n piccolo, stabile, zero alloc. */
    for (int i = 1; i < sorted_count; i++)
    {
        int key = sorted_wins[i];
        int j = i - 1;
        while (j >= 0 && windows[sorted_wins[j]].z_order > windows[key].z_order)
        {
            sorted_wins[j + 1] = sorted_wins[j];
            j--;
        }
        sorted_wins[j + 1] = key;
    }

    z_sort_valid = true;
}

/* ================= terra di mezzo: gerarchia owner ==================== */

/* Antenato top-level di `idx` (segui parent_id fino a 0 o a un link
 * penzolante). Guardia anti-ciclo. */
int win_top_ancestor(int idx)
{
    int guard = 0;
    while (idx >= 0 && windows[idx].used &&
           windows[idx].parent_id != 0 && guard++ < MAX_WINDOWS)
    {
        int p = win_find_by_id(windows[idx].parent_id);
        if (p < 0) break;           /* madre penzolante -> trattala da top */
        idx = p;
    }
    return idx;
}

/* Profondita' di `idx` nel suo albero owner (0 = top-level). */
int win_depth(int idx)
{
    int d = 0, guard = 0;
    while (idx >= 0 && windows[idx].used &&
           windows[idx].parent_id != 0 && guard++ < MAX_WINDOWS)
    {
        int p = win_find_by_id(windows[idx].parent_id);
        if (p < 0) break;
        idx = p; d++;
    }
    return d;
}

/* Se `idx` e' bloccata da un modale vivo, ritorna l'indice del modale
 * bloccante PIU' IN ALTO (discende la catena per i modali annidati);
 * altrimenti -1. Una finestra e' bloccata sse ha una discendente
 * modale visibile — cliccare la madre del modale (o qualunque cosa
 * sopra di lei nella catena) deve rimbalzare sul modale. */
int win_modal_blocker(int idx)
{
    if (idx < 0 || !windows[idx].used) return -1;

    int cur = idx, blocker = -1, guard = 0;
    for (; guard++ < MAX_WINDOWS;)
    {
        uint32_t cur_id = windows[cur].id;
        int child = -1;
        for (int i = 0; i < MAX_WINDOWS; i++)
        {
            if (windows[i].used && windows[i].visible &&
                windows[i].modal && windows[i].parent_id == cur_id)
            {
                child = i;          /* al piu' un modale per madre */
                break;
            }
        }
        if (child < 0) break;
        blocker = child;
        cur = child;                /* discendi per i modali annidati */
    }
    return blocker;
}

/* Predicato d'ordine per il restack di gruppo. True quando `a` deve
 * stare SOTTO `b`, cioe' quando la chiave di a e' strettamente
 * minore. Chiave (crescente = piu' in basso): profondita', poi
 * modale, poi il bersaglio del focus, poi lo z precedente (stabile).
 * Quindi: figlie sopra madri, modali sopra fratelli non modali, la
 * cliccata sopra i pari non modali del suo livello. */
static bool win_stack_below(int a, int b, int target)
{
    int da = win_depth(a), db = win_depth(b);
    if (da != db) return da < db;
    int ma = windows[a].modal ? 1 : 0, mb = windows[b].modal ? 1 : 0;
    if (ma != mb) return ma < mb;
    int ta = (a == target) ? 1 : 0, tb = (b == target) ? 1 : 0;
    if (ta != tb) return ta < tb;
    return windows[a].z_order < windows[b].z_order;
}

/* ================= terra di mezzo: consegna eventi ==================== */

/* Consegna verificata all'owner di una finestra. Usa la post
 * anti-ABA: se la porta e' morta o riciclata (generazione diversa),
 * DOB_ERR_DEAD marca la finestra come orfana — che verra' distrutta
 * al prossimo punto sicuro (win_reap_dead_owners), event-driven,
 * senza mai instradare l'input al processo sbagliato. Unico punto da
 * cui la GUI posta agli owner: ogni inoltro passa di qui. */
bool win_owner_post(int win_idx, dob_msg_t *msg)
{
    window_t *w = &windows[win_idx];
    if (!w->used || w->owner_port == 0 || w->owner_dead) return false;

    dob_status_t rc = dob_ipc_post_checked(w->owner_port, w->owner_gen, msg);
    if (rc == DOB_ERR_DEAD)
    {
        /* Porta chiusa o id riciclato: l'owner di QUESTA finestra non
         * esiste piu'. Marca e rimanda la distruzione al punto sicuro
         * (siamo potenzialmente dentro l'hit-test/dispatch). */
        w->owner_dead = true;
        return false;
    }
    return (rc == DOB_OK);
}

/* Evento al programma proprietario. Fire-and-forget verificato:
 * async, nessuna reply, mai bloccante. */
void send_win_event(int win_idx, uint32_t code, uint32_t a0, uint32_t a1,
                    uint32_t a2)
{
    window_t *w = &windows[win_idx];
    if (!w->used || w->owner_port == 0) return;

    dob_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.code = code;
    msg.arg0 = w->id;
    msg.arg1 = a0;
    msg.arg2 = a1;
    msg.arg3 = a2;

    win_owner_post(win_idx, &msg);
}

/* ================= logica ad alto livello ============================= */

/* win_create — nuovo slot finestra: geometria serrata, id fresco,
 * z in cima, video best-effort. Ritorna l'indice o -1 (parco pieno).
 * Se il pool cmdlist BGA e' pieno, win_alloc_video lascia gli handle
 * a DV_HANDLE_NONE e la finestra resta invisibile finche' un retry
 * non riesce. */
int win_create(const char *title, int w, int h, int x, int y)
{
    win_clamp_spawn_geometry(&x, &y, &w, &h);

    int idx = win_find_free_slot();
    if (idx < 0) return -1;

    window_t *win = &windows[idx];
    memset(win, 0, sizeof(*win));
    win->used = true;
    win->panel_shm_id = -1;          /* nessun pannello SHM finche' richiesto */
    win->id = next_win_id++;
    strncpy(win->title, title, 63);
    win->title[63] = '\0';
    win->x = x;
    win->y = y;
    win->width = w;
    win->height = h;
    win->visible = true;
    win->ready = false;   /* niente blit fino alla prima Invalidate */
    win->maximized = false;
    win->focused = false;
    win->no_resize = false;
    win->no_maximize = false;
    win->cursor_override = CURSOR_DEFAULT;

    win->z_order = win_next_top_z();

    win->body_layer = DV_HANDLE_NONE;
    win->surface_dirty = true;
    win_alloc_video(win);

    z_sort_valid = false;
    di_mark_dirty(DIRTY_FULL);
    return idx;
}

/* win_destroy — smonta la finestra e, in cascata, le sue figlie
 * (sub-window, dialoghi, modali muoiono con la madre). Snapshot
 * dell'id PRIMA: lo slot si libera sotto. Ricorre per le nipoti; una
 * figlia cross-process (es. popup modale in processo separato) perde
 * qui il suo slot e viene mietuta quando il suo processo ormai senza
 * finestre esce. */
void win_destroy(int idx)
{
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return;

    uint32_t my_id = windows[idx].id;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (i != idx && windows[i].used && windows[i].parent_id == my_id)
            win_destroy(i);

    /* Prima giu' cmdlist/layer lato driver, cosi' il processo BGA non
     * tiene riferimenti stantii alla prossima compose. */
    win_free_video(&windows[idx]);
    win_tex_pool_free(&windows[idx]);   /* qui la finestra muore davvero */
    /* win_free_video tiene vivo last_cmdbuf (per il repaint di
     * resize/minimize); la finestra ora sparisce sul serio: rilascia. */
    if (windows[idx].last_cmdbuf)
    {
        free(windows[idx].last_cmdbuf);
        windows[idx].last_cmdbuf      = NULL;
        windows[idx].last_cmdbuf_size = 0;
        windows[idx].last_cmdbuf_cap  = 0;
    }
    cmdbuf_reasm_free(&windows[idx].reasm);
    if (windows[idx].panel_shm_id >= 0)
    {
        shm_unmap(windows[idx].panel_shm_id);
        windows[idx].panel_shm_id = -1;
        windows[idx].panel_ptr    = NULL;
    }
    windows[idx].used = false;
    if (focused_win == idx) focused_win = -1;
    /* Molla la cattura del puntatore se la teneva questa finestra —
     * altrimenti la prossima release proverebbe a smistare su uno
     * slot liberato. */
    if (pointer_capture_win == idx) pointer_capture_win = -1;
    z_sort_valid = false;
    di_mark_dirty(DIRTY_FULL);
}

/* win_restack_for — riassegna gli z cosi' che l'intero gruppo owner
 * di `target` salga in cima come unita', preservando gli invarianti
 * (figlia sopra madre, modale sopra fratelli, bersaglio sopra i
 * pari). Le finestre fuori dal gruppo tengono l'ordine relativo,
 * sotto il gruppo. Sostituisce il vecchio bump di z singolo del
 * focus. O(n^2), n <= MAX_WINDOWS. */
void win_restack_for(int target)
{
    int root = win_top_ancestor(target);

    int grp[MAX_WINDOWS], ng[MAX_WINDOWS];
    int grp_n = 0, ng_n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
    {
        if (!windows[i].used) continue;
        if (win_top_ancestor(i) == root) grp[grp_n++] = i;
        else                             ng[ng_n++]   = i;
    }

    /* Fuori gruppo: stabile per z corrente. */
    for (int i = 1; i < ng_n; i++)
    {
        int k = ng[i], j = i - 1;
        while (j >= 0 && windows[ng[j]].z_order > windows[k].z_order)
        { ng[j + 1] = ng[j]; j--; }
        ng[j + 1] = k;
    }
    /* Gruppo: crescente per chiave di stack (minima -> fondo gruppo). */
    for (int i = 1; i < grp_n; i++)
    {
        int k = grp[i], j = i - 1;
        while (j >= 0 && win_stack_below(k, grp[j], target))
        { grp[j + 1] = grp[j]; j--; }
        grp[j + 1] = k;
    }

    int z = 0;
    for (int i = 0; i < ng_n;  i++) windows[ng[i]].z_order  = z++;
    for (int i = 0; i < grp_n; i++) windows[grp[i]].z_order = z++;
    z_sort_valid = false;
}

/* win_focus — porta il focus (e il gruppo) su `idx`.
 * Cancello modale: una finestra con un discendente modale vivo non
 * puo' MAI prendere il focus — redirige al modale bloccante, suona
 * l'attention ding e avvia il lampeggio del bordo. Copre la Raise
 * programmatica e il fallback dell'unfocus; il percorso del clic
 * ingoia ancora prima (prima di drag/resize). */
void win_focus(int idx)
{
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return;

    int blk = win_modal_blocker(idx);
    if (blk >= 0 && blk != idx)
    {
        win_attention_ding();
        flash_start(blk);
        idx = blk;
    }

    maindob_menu_open = false;  /* ogni cambio di focus chiude il menu */

    /* Defocus della precedente. Bordo e sfondo header del chrome
     * dipendono dallo stato di focus: il bitmap della body_surf va
     * ri-renderizzato, o la ex-focalizzata terrebbe il bordo blu
     * anche dopo aver perso il focus. */
    if (focused_win >= 0 && focused_win < MAX_WINDOWS && windows[focused_win].used)
    {
        windows[focused_win].focused = false;
        windows[focused_win].surface_dirty = true;
    }

    windows[idx].focused = true;
    windows[idx].surface_dirty = true;     /* stesso motivo — lo prende */
    focused_win = idx;

    win_restack_for(idx);

    /* Pannello: il titolo della focalizzata prende lo slot header al
     * posto di "MainDOB"; sotto compaiono i comandi finestra. */
    panel_focus_window(&windows[idx]);
    panel_sync_context();
    di_mark_dirty(DIRTY_FULL);
}

/* win_unfocus_all — sfoca e ripiega sulla visibile piu' in alto: cosi'
 * quando un popup si chiude, la finestra dietro prende il focus e i
 * suoi comandi pannello compaiono da soli. */
void win_unfocus_all(void)
{
    maindob_menu_open = false;

    if (focused_win >= 0 && focused_win < MAX_WINDOWS && windows[focused_win].used)
    {
        windows[focused_win].focused = false;
        windows[focused_win].surface_dirty = true;   /* colore chrome */
    }
    focused_win = -1;

    panel_focus_desktop();

    int best = -1;
    int best_z = -1;
    for (int i = 0; i < MAX_WINDOWS; i++)
    {
        /* Non si puo' focalizzare cio' che l'utente non vede ancora. */
        if (windows[i].used && windows[i].visible && windows[i].ready &&
            windows[i].z_order > best_z)
        {
            best = i;
            best_z = windows[i].z_order;
        }
    }

    if (best >= 0)
    {
        win_focus(best);
    }
    else
    {
        panel_sync_context();
        di_mark_dirty(DIRTY_FULL);
    }
}

/* win_unfocus_to_desktop — sfoca tutto e mostra il pannello desktop,
 * SENZA rifocalizzare: e' il clic esplicito dell'utente sullo sfondo. */
void win_unfocus_to_desktop(void)
{
    maindob_menu_open = false;

    if (focused_win >= 0 && focused_win < MAX_WINDOWS && windows[focused_win].used)
    {
        windows[focused_win].focused = false;
        windows[focused_win].surface_dirty = true;   /* colore chrome */
    }
    focused_win = -1;

    panel_focus_desktop();
    panel_sync_context();
    di_mark_dirty(DIRTY_FULL);
}

/* hit_test_window — quale finestra a (mx, my) e dove?
 * Ritorna l'indice o -1. *zone: 0=corpo, 1=header, 2=bordo/angolo
 * di resize. La zona di presa del resize si estende RESIZE_GRAB px
 * VERSO L'INTERNO da tutti i bordi E anche RESIZE_GRAB/2 px VERSO
 * L'ESTERNO — si afferra anche da poco fuori. Molto piu' facile da
 * prendere. L'header ha priorita': dentro la finestra e nella
 * striscia header e' una maniglia di drag, non zona resize. */
int hit_test_window(int mx, int my, int *zone)
{
    int grab = RESIZE_GRAB;
    int outer = grab / 2;

    sort_windows_by_z();
    for (int i = sorted_count - 1; i >= 0; i--)
    {
        int idx = sorted_wins[i];
        window_t *w = &windows[idx];
        int total_h = w->height + WIN_HEADER_H;

        int wx0 = w->x;
        int wy0 = w->y;
        int wx1 = w->x + w->width;
        int wy1 = w->y + total_h;

        /* Bounding box estesa (zona di presa esterna inclusa). */
        if (mx < wx0 - outer || mx >= wx1 + outer ||
            my < wy0 - outer || my >= wy1 + outer)
            continue;

        bool inside = (mx >= wx0 && mx < wx1 && my >= wy0 && my < wy1);

        if (inside && my < wy0 + WIN_HEADER_H)
        {
            *zone = 1;
            return idx;
        }

        bool near_left   = (mx < wx0 + grab);
        bool near_right  = (mx >= wx1 - grab);
        bool near_top    = (my < wy0 + grab);
        bool near_bottom = (my >= wy1 - grab);

        if (near_left || near_right || near_top || near_bottom)
        {
            *zone = 2;
            return idx;
        }

        *zone = 0;
        return idx;
    }
    return -1;
}

/* win_reap_dead_owners — miete le finestre il cui owner e' stato
 * scoperto morto da una post verificata (owner_dead). Event-driven:
 * sostituisce il rilevamento a poll per le finestre a cui abbiamo
 * appena provato a instradare input. Chiamata a un punto sicuro del
 * ciclo (fuori da hit-test/dispatch), dove distruggere una finestra
 * non corrompe iterazioni in corso. Stessa semantica di teardown del
 * vecchio cleanup: toast, defocus, cleanup widget, waitpid. */
void win_reap_dead_owners(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
    {
        if (!windows[i].used || !windows[i].owner_dead) continue;

        char notify[128];
        snprintf(notify, sizeof(notify),
                 "Processo terminato: %s", windows[i].title);
        toast_show(notify);

        pid_t dead_pid    = windows[i].owner_pid;
        bool  was_focused = (focused_win == i);
        win_destroy(i);
        if (was_focused) win_unfocus_all();
        if (dead_pid > 0)
        {
            widget_cleanup_for_pid(dead_pid);
            waitpid(dead_pid);
        }
    }
}
