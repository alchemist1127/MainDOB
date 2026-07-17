/* MainDOB DobInterface 2.0 — foglio: style.c
 *
 * I feedback visivi "di carattere" del window manager:
 *
 *  - TYPED-TEXT: alla prima GUI_WIN_INVALIDATE di una finestra, ogni
 *    run di testo del CORPO si "batte" da sola, come un terminale
 *    anni '80 che riempie lo schermo. Le run sono indipendenti e
 *    rivelate in PARALLELO: data la frazione di progresso p =
 *    trascorso / g_typed_ms in [0,1], una run di N glifi mostra i
 *    primi (p*N) glifi. Stessa p, N diversi => velocita' per
 *    carattere diverse ("marce diverse"), ma ogni run finisce entro
 *    g_typed_ms. Solo il testo del corpo: chrome (titolo), sfondi,
 *    rect e icone rendono pieni dal primo frame.
 *
 *  - LAMPEGGIO MODALE: su clic della madre bloccata, il bordo del
 *    modale lampeggia rosso 3 volte; ogni fase (acceso/spento) dura
 *    FLASH_HALF_MS. La fase corrente e' DERIVATA dal tempo trascorso,
 *    cosi' il colore e' sempre coerente a ogni repaint, comunque ci
 *    si arrivi.
 *
 * Entrambi gli effetti girano su UN timer periodico ciascuno,
 * condiviso da tutte le finestre che animano, e cancellato quando
 * nessuna anima piu'. La cadenza del typed-text (33 ms) sta sopra il
 * gate di pacing (16 ms), quindi questi repaint flippano subito
 * invece di venire coalizzati. */

#include "di_internal.h"

/* ================= stato condiviso (proprieta' di questo foglio) ====== */

/* Tuning live, letto dal setting ui.window_typing al boot e all'Apply
 * dell'editor (typed_settings_load). Se settingsd e' irraggiungibile
 * valgono i default. g_typed_ms e' sempre serrato in
 * [TYPE_ANIM_MS_MIN, TYPE_ANIM_MS_MAX]: la matematica del reveal non
 * divide mai per zero. */
bool     g_typed_enabled = true;
uint32_t g_typed_ms      = TYPE_ANIM_MS;

/* ================= stato privato ====================================== */

static int type_anim_timer_id = -1;   /* pompa periodica condivisa, o -1 */
static int flash_timer_id     = -1;   /* specchio di type_anim           */

/* ================= verbi esecutivi: typed-text ======================== */

/* Arma la pompa periodica condivisa se non gira gia'. */
static void type_anim_arm(void)
{
    if (type_anim_timer_id >= 0) return;
    int tid = timer_set(gui_port, TYPE_ANIM_FRAME_MS, /*repeat=*/1);
    if (tid >= 0) type_anim_timer_id = tid;
}

/* Avvia il reveal per una finestra appena aperta. No-op (e niente
 * timer) se il corpo non ha testo da battere. */
void type_anim_start(window_t *w)
{
    if (!g_typed_enabled)
        return;                      /* effetto spento nei setting */
    if (!w->last_cmdbuf || w->last_cmdbuf_size <= DOBUI_CMDBUF_HDR_SIZE)
        return;
    if (!cmdbuf_has_text(w->last_cmdbuf, w->last_cmdbuf_size))
        return;
    w->type_anim_active   = true;
    w->type_anim_start_ms = (uint32_t)clock_ms();
    type_anim_arm();
}

/* Scatto della pompa: avanza ogni finestra che anima ri-marcandone la
 * surface sporca, cosi' il prossimo repaint la ri-rivela al progresso
 * corrente, e chiede quel repaint. A budget esaurito la finestra
 * riceve un ultimo render pieno ed esce; quando nessuna anima piu',
 * il timer si cancella. */
void type_anim_pump(void)
{
    uint32_t now      = (uint32_t)clock_ms();
    bool     any_left = false;
    bool     repaint  = false;

    for (int i = 0; i < MAX_WINDOWS; i++)
    {
        window_t *w = &windows[i];
        if (!w->used || !w->type_anim_active) continue;

        uint32_t elapsed = now - w->type_anim_start_ms;
        if (elapsed >= g_typed_ms)
            w->type_anim_active = false;   /* l'ultimo rebuild rende pieno */
        else
            any_left = true;

        w->surface_dirty = true;           /* ri-rivela / finalizza        */
        repaint = true;
    }

    if (repaint) di_mark_dirty(DIRTY_FULL);

    if (!any_left && type_anim_timer_id >= 0)
    {
        timer_cancel_async(type_anim_timer_id);
        type_anim_timer_id = -1;
    }
}

/* ================= verbi esecutivi: lampeggio modale ================== */

/* Fase corrente del lampeggio di windows[idx], o -1 se non lampeggia
 * (mai partito, o le FLASH_PHASES sono esaurite). */
static int flash_phase(int idx)
{
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used
        || !windows[idx].flash_active)
        return -1;
    uint32_t el = (uint32_t)clock_ms() - windows[idx].flash_start_ms;
    int ph = (int)(el / FLASH_HALF_MS);
    return (ph >= FLASH_PHASES) ? -1 : ph;
}

/* Bordo da disegnare rosso adesso? (fasi pari = acceso) */
bool flash_is_red(int idx)
{
    int ph = flash_phase(idx);
    return ph >= 0 && (ph & 1) == 0;
}

static void flash_arm_timer(void)
{
    if (flash_timer_id >= 0) return;
    int tid = timer_set(gui_port, FLASH_HALF_MS, /*repeat=*/1);
    if (tid >= 0) flash_timer_id = tid;
}

/* Avvia/riavvia il lampeggio del bordo di windows[idx]. */
void flash_start(int idx)
{
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return;
    windows[idx].flash_active   = true;
    windows[idx].flash_start_ms = (uint32_t)clock_ms();
    windows[idx].surface_dirty  = true;   /* forza win_bake (bordo) */
    flash_arm_timer();
    di_mark_dirty(DIRTY_FULL);
}

/* Scatto del timer: chiude le finestre che hanno finito e ridisegna;
 * quando nessuna lampeggia piu', cancella il timer. */
void flash_pump(void)
{
    bool any_left = false;
    for (int i = 0; i < MAX_WINDOWS; i++)
    {
        if (!windows[i].used || !windows[i].flash_active) continue;
        if (flash_phase(i) < 0) windows[i].flash_active = false;
        else                    any_left = true;
        windows[i].surface_dirty = true;  /* bordo alla fase corrente */
    }
    di_mark_dirty(DIRTY_FULL);
    if (!any_left && flash_timer_id >= 0)
    {
        timer_cancel_async(flash_timer_id);
        flash_timer_id = -1;
    }
}

/* ================= logica ad alto livello ============================= */

/* Dichiara (idempotente) e legge il setting del typed-text da
 * settingsd in g_typed_enabled / g_typed_ms. Chiamata una volta
 * all'avvio e a ogni SETTINGS_RELOAD_CALL dell'editor. Best-effort:
 * se settingsd non risponde (boot precoce) restano i valori
 * correnti, e l'effetto gira coi default. Il compositor non e' nella
 * catena di dipendenze di DobFileSystem, quindi questa declare non
 * puo' bloccare il boot come farebbe quella di un driver disco. */
void typed_settings_load(void)
{
    static const setting_field_t fields[] = {
        { SETTING_BOOL,   "Attivo",      "true", 0 },
        { SETTING_STRING, "Durata (ms)", "1000", 0 },
    };
    declareSettingMulti("ui.window_typing",
                        "Effetto digitazione finestre", fields, 2);

    char on[16];
    if (settingField("ui.window_typing", 0, on, sizeof(on)) == 0)
        g_typed_enabled = (strcmp(on, "true") == 0);

    char ms[16];
    if (settingField("ui.window_typing", 1, ms, sizeof(ms)) == 0)
    {
        /* Parse decimale manuale (32 bit, niente helper libc/64 bit),
         * poi clamp. Un valore non numerico o fuori range lascia
         * g_typed_ms invariato: un refuso non puo' mai produrre un
         * reveal da 0 ms (divisione per zero). */
        uint32_t v = 0;
        bool any = false;
        const char *p = ms;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9')
        {
            v = v * 10u + (uint32_t)(*p - '0');
            any = true;
            p++;
            if (v > 1000000u) break;   /* guardia overflow */
        }
        if (any && v >= TYPE_ANIM_MS_MIN && v <= TYPE_ANIM_MS_MAX)
            g_typed_ms = v;
    }
}

/* Routing degli scatti timer del codice 70: i timer id sono privati
 * di questo foglio, quindi il foglio input chiede "e' uno dei tuoi?".
 * Ritorna true se lo scatto era di una pompa di stile (e la pompa e'
 * girata); false = non nostro, il chiamante lo passa al pacing. */
bool style_timer_fired(int timer_id)
{
    if (type_anim_timer_id >= 0 && timer_id == type_anim_timer_id)
    {
        type_anim_pump();
        return true;
    }
    if (flash_timer_id >= 0 && timer_id == flash_timer_id)
    {
        flash_pump();
        return true;
    }
    return false;
}
