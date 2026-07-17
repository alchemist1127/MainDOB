/* MainDOB DobInterface 2.0 — foglio: logon.c
 *
 * La tenda: schermata di accesso con password, screensaver e timeout
 * di inattivita'. Un solo stato-macchina, quattro modi visibili:
 *
 *   L_OFF     desktop normale (la tenda non esiste);
 *   L_SAVER   schermo oscurato. Qualunque tasto/clic/mossa risveglia:
 *             verso L_PROMPT se una password esiste, verso il desktop
 *             altrimenti. L'evento di risveglio viene INGOIATO, mai
 *             consegnato (ne' alle finestre ne' come primo carattere
 *             della password);
 *   L_PROMPT  richiesta password. Invio verifica; Esc svuota il campo.
 *             Un tentativo errato accende un lockout breve
 *             (LOGON_FAIL_MS) durante il quale l'input e' sordo — il
 *             ritmo di un attacco a tentativi crolla senza mai
 *             bloccare il thread (il loop eventi resta puro);
 *   L_NEW1/2  cambio password (doppio prompt). Entrata: la scrittura
 *             EPS dall'editor Impostazioni, DOPO la verifica della
 *             password attuale. Campo vuoto al primo prompt = la
 *             password si RIMUOVE (e il file sparisce). Esc annulla
 *             l'intero cambio.
 *
 * LA PRESENZA DEL FILE E' L'INTERRUTTORE. /SYSTEM/CONFIG/
 * Logon_password.dat esiste -> logon al boot e al risveglio; non
 * esiste -> dritti al desktop (la ISO live non lo contiene mai). Il
 * formato del record e la verifica vivono in <dob/logon.h>, condiviso
 * con MainDOB_Setup che crea il file all'installazione. Sicurezza
 * PASSIVA e dichiarata tale: nessuna cifratura, solo una tenda per
 * l'impiccione occasionale — ma nel file c'e' un hash salato, mai la
 * password in chiaro.
 *
 * RENDERING. La tenda e' il backbuf stesso, alzato a LOGON_LAYER_Z
 * (sopra finestre, servo, tray e toast; sotto il solo cursore): lo
 * stesso idioma di MC/About, spinto a uno z da sipario. Il ramo
 * curtain di compositor_rebuild pulisce a nero, chiama logon_draw e
 * NON disegna nient'altro: niente icone, niente pannello, niente
 * finestre — non un pixel del desktop trapela. I present restano i
 * soliti (flip pieno, flash-free su ferro).
 *
 * INPUT. Mentre la tenda e' su, il foglio input consegna ogni tasto a
 * logon_key e sopprime in blocco l'instradamento del mouse
 * (logon_gate_mouse): nessun evento raggiunge finestre, pannello,
 * icone o widget. Lo screenshot (Stamp) e' irraggiungibile per la
 * stessa via. All'ingresso della tenda le sessioni di drag vive
 * vengono chiuse (input_abort_sessions) e menu/tray/overlay chiusi:
 * al risveglio il desktop e' in uno stato quieto e coerente.
 *
 * INATTIVITA'. Zero polling, fedele al ciclo: un timestamp
 * (last_activity, aggiornato dal foglio input a ogni evento umano) e
 * UN timer one-shot a riarmo pigro. Il timer e' armato per l'intero
 * timeout; quando scatta si confronta il tempo davvero trascorso
 * dall'ultima attivita': se non basta, ci si riarma per la sola
 * differenza. Un solo scatto per periodo di timeout, MAI una syscall
 * per keystroke. Timeout dalla impostazione ui.idle_screensaver
 * (minuti; 0 = mai), live-reload con l'Applica dell'editor come il
 * typed-text.
 *
 * CAMBIO PASSWORD — LA VIA EPS. dobinterface dichiara una entry di
 * classe EPS e tipo SECRET (sicurezza.logon_password): l'editor la
 * rende come casella mascherata e all'Applica spedisce il testo
 * digitato DIRETTAMENTE alla nostra porta (GUI_LOGON_PW_WRITE), senza
 * che il daemon veda o persista mai nulla. Quel testo e' la password
 * ATTUALE: se combacia (o non ce n'e' una), reply DOB_OK e si apre il
 * doppio prompt sotto tenda; se no, DOB_ERR_DENIED e un toast avvisa.
 * GUI_LOGON_PW_READ risponde sempre valore vuoto: nulla da esporre.
 * Il doppio prompt e' UI di QUESTO foglio e non un DobPopup: i popup
 * sono stub client con loop annidato dentro una finestra servita da
 * noi — chiamarli da noi stessi sarebbe un deadlock per costruzione. */

#include "di_internal.h"
#include <dob/logon.h>

/* ================= stato privato ====================================== */

#define L_OFF     0
#define L_SAVER   1
#define L_PROMPT  2
#define L_NEW1    3
#define L_NEW2    4

/* Lockout dopo un tentativo errato (ms). */
#define LOGON_FAIL_MS   1500u

/* Tetto di sicurezza del timeout (minuti) contro i refusi. */
#define IDLE_MIN_MAX    600u

static int      g_state = L_OFF;

/* Record caricato (o assente). */
static bool     g_have_pw   = false;
static uint32_t g_pw_salt   = 0;
static uint64_t g_pw_hash   = 0;

/* Buffer di battitura del prompt corrente + prima meta' del cambio. */
static char     g_buf[DOB_LOGON_PW_MAX + 1];
static int      g_buf_len = 0;
static char     g_new1[DOB_LOGON_PW_MAX + 1];

/* Riga di esito/errore mostrata nel prompt ("" = nessuna). */
static const char *g_msg = "";

/* Lockout: input sordo finche' clock_ms() < g_fail_until. */
static uint32_t g_fail_until = 0;

/* Inattivita'. */
static uint32_t g_idle_timeout_ms = 10u * 60000u;   /* default 10 min  */
static uint32_t g_last_activity   = 0;
static int      g_idle_timer_id   = -1;
static int      g_msg_timer_id    = -1;             /* fine lockout    */

/* ================= verbi esecutivi: file password ===================== */

/* Legge e interpreta il record. Aggiorna g_have_pw/salt/hash; un file
 * assente o malformato vale "nessuna password" (fail-open dichiarato:
 * la tenda e' passiva, un FS zoppo non deve murare il desktop). */
static void pw_file_load(void)
{
    g_have_pw = false;

    int fd = dobfs_Open(DOB_LOGON_PATH, FS_READ);
    if (fd < 0)
        return;

    char text[DOB_LOGON_RECORD_MAX * 2];
    int  got = dobfs_Read(fd, text, sizeof(text) - 1);
    dobfs_Close(fd);
    if (got <= 0)
        return;
    text[got] = '\0';

    uint32_t salt; uint64_t hash;
    if (dob_logon_parse(text, &salt, &hash))
    {
        g_pw_salt = salt;
        g_pw_hash = hash;
        g_have_pw = true;
    }
    else
    {
        debug_print("[dobinterface] logon: record malformato, "
                    "trattato come assente.\n");
    }
}

/* Scrive il record per una nuova password. true su successo. */
static bool pw_file_save(const char *password)
{
    uint32_t salt = random_u32();
    if (salt == 0) salt = 0xD0B10617u;   /* mai salt nullo */

    char rec[DOB_LOGON_RECORD_MAX];
    int  n = dob_logon_make_record(rec, salt, password);

    int fd = dobfs_Open(DOB_LOGON_PATH, FS_WRITE | FS_CREATE | FS_TRUNC);
    if (fd < 0)
        return false;
    bool ok = (dobfs_Write(fd, rec, (uint32_t)n) == n);
    dobfs_Close(fd);
    if (!ok)
        return false;

    g_pw_salt = salt;
    g_pw_hash = dob_logon_hash(salt, password);
    g_have_pw = true;
    return true;
}

/* Rimuove il record: la tenda con password cessa di esistere. */
static void pw_file_remove(void)
{
    dobfs_Unlink(DOB_LOGON_PATH);
    g_have_pw = false;
    g_pw_salt = 0;
    g_pw_hash = 0;
}

/* ================= verbi esecutivi: buffer e tenda ==================== */

/* Azzera il buffer di battitura CANCELLANDO i byte (un segreto non
 * resta a dormire nella BSS piu' del necessario). */
static void buf_reset(void)
{
    memset(g_buf, 0, sizeof(g_buf));
    g_buf_len = 0;
}

static void new1_reset(void)
{
    memset(g_new1, 0, sizeof(g_new1));
}

/* Ingresso in tenda (da OFF o fra stati di tenda). Alla PRIMA salita
 * chiude cio' che era aperto sul desktop: menu MainDOB, tray, About,
 * Mission Control, sessioni di drag — al ritorno tutto e' quieto. */
static void curtain_enter(int state)
{
    if (g_state == L_OFF)
    {
        maindob_menu_open = false;
        if (widget_panel_open)
            widget_panel_open = false;
        about_overlay_active = false;
        if (mc_active)
            mc_exit(-1);
        input_abort_sessions();
        panel_sync_context();
    }
    g_state = state;
    buf_reset();
    g_msg = "";
    di_mark_dirty(DIRTY_FULL);
}

static void logon_idle_arm(void);   /* definito sotto (logica timer) */

/* Uscita dalla tenda verso il desktop. */
static void curtain_exit(void)
{
    g_state = L_OFF;
    buf_reset();
    new1_reset();
    g_msg = "";
    g_fail_until = 0;
    g_last_activity = (uint32_t)clock_ms();
    logon_idle_arm();
    di_mark_dirty(DIRTY_FULL);
}

/* ================= verbi esecutivi: disegno =========================== */

/* Il prompt (accesso o cambio password), centrato sulla tenda nera.
 * Chiamato dal compositor SOLO col curtain attivo; il backbuf e' gia'
 * stato pulito a nero. In L_SAVER non si disegna nulla: il nero E' lo
 * screensaver. */
void logon_draw(void)
{
    if (g_state == L_OFF || g_state == L_SAVER)
        return;

    const char *caption =
        (g_state == L_PROMPT) ? "Inserisci la password di accesso" :
        (g_state == L_NEW1)   ? "Nuova password (vuota per rimuoverla)"
                              : "Conferma la nuova password";
    const char *hint =
        (g_state == L_PROMPT) ? "Invio conferma  -  Esc svuota il campo"
                              : "Invio conferma  -  Esc annulla il cambio";

    int bw = 420, bh = 168;
    int bx = (SCREEN_W - bw) / 2;
    int by = (SCREEN_H - bh) / 2;

    fb_fill_rect(bx, by, bw, bh, COLOR_BG);
    fb_draw_rect(bx, by, bw, bh, COLOR_WHITE, 1);

    font_draw_string(bx + 20, by + 14, "MainDOB", COLOR_WHITE, COLOR_BG);
    font_draw_string(bx + 20, by + 38, caption, COLOR_CYAN, COLOR_BG);

    /* Campo: pallini quanti i caratteri battuti, piu' un cursore a
     * blocco. Mai i caratteri veri. */
    int fx = bx + 20, fy = by + 66, fw = bw - 40, fh = FONT_H + 8;
    fb_fill_rect(fx, fy, fw, fh, COLOR_BLACK);
    fb_draw_rect(fx, fy, fw, fh, COLOR_STRUCT_LINE, 1);

    int max_dots = (fw - 8 - FONT_W) / FONT_W;
    int dots     = (g_buf_len < max_dots) ? g_buf_len : max_dots;
    char stars[64];
    if (dots > (int)sizeof(stars) - 1) dots = (int)sizeof(stars) - 1;
    for (int i = 0; i < dots; i++) stars[i] = '*';
    stars[dots] = '\0';
    font_draw_string(fx + 4, fy + 4, stars, COLOR_WHITE, COLOR_BLACK);
    /* Cursore. */
    fb_fill_rect(fx + 4 + dots * FONT_W, fy + 3, FONT_W, FONT_H + 2,
                 COLOR_CYAN);

    /* Riga di esito (lockout / mismatch). */
    if (g_msg[0])
        font_draw_string(bx + 20, fy + fh + 12, g_msg,
                         COLOR_YELLOW, COLOR_BG);

    font_draw_string(bx + 20, by + bh - FONT_H - 12, hint,
                     COLOR_TEXT_MUTE, COLOR_BG);
}

/* ================= verbi esecutivi: timer inattivita' ================= */

/* Riarmo pigro del one-shot: per l'intero timeout, o per il residuo
 * calcolato allo scatto. Niente timer con tenda su o timeout 0. */
static void idle_arm_for(uint32_t ms)
{
    if (g_idle_timer_id >= 0)
    {
        timer_cancel_async(g_idle_timer_id);
        g_idle_timer_id = -1;
    }
    if (ms == 0)
        return;
    int tid = timer_set(gui_port, ms, /*repeat=*/0);
    if (tid >= 0)
        g_idle_timer_id = tid;
}

static void logon_idle_arm(void)
{
    if (g_idle_timeout_ms == 0 || g_state != L_OFF)
    {
        idle_arm_for(0);
        return;
    }
    idle_arm_for(g_idle_timeout_ms);
}

/* ================= terra di mezzo: sottomissioni dei prompt =========== */

/* Invio in L_PROMPT: verifica. */
static void submit_prompt(void)
{
    if (!g_have_pw || dob_logon_check(g_pw_salt, g_pw_hash, g_buf))
    {
        curtain_exit();
        return;
    }
    buf_reset();
    g_msg        = "Password errata";
    g_fail_until = (uint32_t)clock_ms() + LOGON_FAIL_MS;
    /* Un one-shot ripulisce la riga a lockout finito (senza, resterebbe
     * fino al prossimo evento). */
    if (g_msg_timer_id >= 0)
        timer_cancel_async(g_msg_timer_id);
    g_msg_timer_id = timer_set(gui_port, LOGON_FAIL_MS, /*repeat=*/0);
    di_mark_dirty(DIRTY_FULL);
}

/* Invio in L_NEW1: vuoto = rimozione; pieno = passa alla conferma. */
static void submit_new1(void)
{
    if (g_buf_len == 0)
    {
        pw_file_remove();
        curtain_exit();
        toast_show("Password di accesso rimossa");
        return;
    }
    memcpy(g_new1, g_buf, sizeof(g_new1));
    g_state = L_NEW2;
    buf_reset();
    g_msg = "";
    di_mark_dirty(DIRTY_FULL);
}

/* Invio in L_NEW2: conferma e salva, o riparti dal primo prompt. */
static void submit_new2(void)
{
    if (strcmp(g_buf, g_new1) == 0)
    {
        bool ok = pw_file_save(g_new1);
        new1_reset();
        curtain_exit();
        toast_show(ok ? "Password di accesso aggiornata"
                      : "Errore: salvataggio password fallito");
        return;
    }
    new1_reset();
    g_state = L_NEW1;
    buf_reset();
    g_msg = "Le password non coincidono";
    di_mark_dirty(DIRTY_FULL);
}

/* Risveglio dallo screensaver: al logon se serve, al desktop se no.
 * L'evento che risveglia e' gia' stato ingoiato dal chiamante. */
static void saver_wake(void)
{
    if (g_have_pw)
    {
        g_state = L_PROMPT;
        buf_reset();
        g_msg = "";
        di_mark_dirty(DIRTY_FULL);
    }
    else
    {
        curtain_exit();
    }
}

/* ================= logica ad alto livello ============================= */

bool logon_visible(void)
{
    return g_state != L_OFF;
}

bool logon_has_password(void)
{
    return g_have_pw;
}

/* "Blocca" del menu MainDOB: al prompt se una password esiste,
 * altrimenti degrada al solo oscuramento. */
void logon_lock(void)
{
    curtain_enter(g_have_pw ? L_PROMPT : L_SAVER);
}

/* "Oscura schermo": screensaver immediato, sempre e comunque. Al
 * risveglio vale la regola unica (password -> prompt). */
void logon_darken(void)
{
    curtain_enter(L_SAVER);
}

/* Timestamp di attivita' umana: chiamato dal foglio input su ogni
 * evento di tastiera/mouse. Solo un'assegnazione — mai una syscall. */
void logon_note_activity(void)
{
    g_last_activity = (uint32_t)clock_ms();
}

/* Ogni tasto mentre la tenda e' su. L'evento non prosegue mai oltre
 * questo verbo. */
void logon_key(uint8_t key)
{
    if (g_state == L_SAVER)
    {
        saver_wake();
        return;
    }

    /* Lockout: sordo a tutto finche' non scade. */
    if (g_fail_until != 0)
    {
        uint32_t now = (uint32_t)clock_ms();
        if ((int32_t)(g_fail_until - now) > 0)
            return;
        g_fail_until = 0;
        g_msg = "";
    }

    if (key == 27)   /* Esc */
    {
        if (g_state == L_PROMPT)
        {
            buf_reset();
            di_mark_dirty(DIRTY_FULL);
        }
        else
        {
            new1_reset();
            curtain_exit();
            toast_show("Cambio password annullato");
        }
        return;
    }

    if (key == '\b' || key == 127)
    {
        if (g_buf_len > 0)
        {
            g_buf[--g_buf_len] = '\0';
            di_mark_dirty(DIRTY_FULL);
        }
        return;
    }

    if (key == '\n' || key == 13)
    {
        if      (g_state == L_PROMPT) submit_prompt();
        else if (g_state == L_NEW1)   submit_new1();
        else                          submit_new2();
        return;
    }

    /* Carattere di testo: stesso criterio dei textbox (printable
     * ASCII + Latin-1 stampabile), cosi' una password battibile qui
     * e' battibile identica nel wizard e nell'editor. */
    if (((key >= 32 && key < 127) || key >= 0xA0)
        && g_buf_len < DOB_LOGON_PW_MAX)
    {
        g_buf[g_buf_len++] = (char)key;
        g_buf[g_buf_len]   = '\0';
        di_mark_dirty(DIRTY_FULL);
    }
}

/* Cancello del mouse mentre la tenda e' su: true = il chiamante
 * SOPPRIME tutto l'instradamento (press, release, drag, scroll,
 * pannello, icone). In L_SAVER qualunque press o mossa risveglia. */
bool logon_gate_mouse(bool any_press, bool moved)
{
    if (g_state == L_OFF)
        return false;

    if (g_state == L_SAVER && (any_press || moved))
        saver_wake();

    /* Nei prompt il mouse non fa nulla: la tenda e' solo tastiera. */
    return true;
}

/* Scatti del codice 70 di pertinenza di questo foglio. true = era
 * nostro (idle o fine-lockout); false = il chiamante prosegue la
 * catena (stile, pacing). */
bool logon_timer_fired(int timer_id)
{
    if (g_idle_timer_id >= 0 && timer_id == g_idle_timer_id)
    {
        g_idle_timer_id = -1;

        if (g_state != L_OFF || g_idle_timeout_ms == 0)
            return true;   /* stantio: tenda gia' su o timeout spento */

        uint32_t now     = (uint32_t)clock_ms();
        uint32_t elapsed = now - g_last_activity;
        if (elapsed + 20u >= g_idle_timeout_ms)
        {
            logon_darken();
        }
        else
        {
            /* Riarmo pigro: solo il residuo. */
            idle_arm_for(g_idle_timeout_ms - elapsed);
        }
        return true;
    }

    if (g_msg_timer_id >= 0 && timer_id == g_msg_timer_id)
    {
        g_msg_timer_id = -1;
        if (g_state != L_OFF && g_fail_until != 0)
        {
            uint32_t now = (uint32_t)clock_ms();
            if ((int32_t)(g_fail_until - now) <= 0)
            {
                g_fail_until = 0;
                g_msg = "";
                di_mark_dirty(DIRTY_FULL);
            }
        }
        return true;
    }

    return false;
}

/* Dichiara (idempotente) e rilegge le impostazioni del foglio:
 * il timeout di inattivita' (FILE-class, minuti, 0 = mai) e la entry
 * EPS SECRET del cambio password. Chiamata all'avvio e a ogni
 * SETTINGS_RELOAD_CALL, come typed_settings_load. Best-effort. */
void logon_settings_load(void)
{
    static const setting_field_t idle_fields[] = {
        { SETTING_STRING, "Minuti (0 = mai)", "10", 0 },
    };
    declareSettingMulti("ui.idle_screensaver",
                        "Screensaver e blocco per inattivita'",
                        idle_fields, 1);

    declareEpsSetting("sicurezza.logon_password", SETTING_SECRET,
                      "Password di accesso (digita quella attuale e "
                      "Applica per cambiarla o impostarla)",
                      "", 0,
                      "dobinterface",
                      GUI_LOGON_PW_READ, GUI_LOGON_PW_WRITE);

    char mins[16];
    if (settingField("ui.idle_screensaver", 0, mins, sizeof(mins)) == 0)
    {
        /* Parse decimale manuale + clamp, come style.c: un refuso non
         * produce mai un timeout assurdo. "0" spegne. */
        uint32_t v = 0;
        bool any = false;
        const char *p = mins;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9')
        {
            v = v * 10u + (uint32_t)(*p - '0');
            any = true;
            p++;
            if (v > IDLE_MIN_MAX) { v = IDLE_MIN_MAX; break; }
        }
        if (any)
            g_idle_timeout_ms = v * 60000u;
    }

    logon_idle_arm();
}

/* Controllo del boot, dopo i servizi e PRIMA del primo paint: se il
 * record esiste, il primissimo frame del desktop E' la tenda. */
void logon_boot_check(void)
{
    pw_file_load();
    g_last_activity = (uint32_t)clock_ms();
    if (g_have_pw)
    {
        curtain_enter(L_PROMPT);
        debug_print("[dobinterface] logon: password presente, "
                    "schermata di accesso attiva.\n");
    }
    else
    {
        debug_print("[dobinterface] logon: nessuna password, "
                    "desktop diretto.\n");
    }
}

/* ---- Lato EPS (editor Impostazioni), via handle_gui_ipc ---- */

/* read_op: mai nulla da esporre — valore vuoto, sempre. */
void logon_eps_read(dob_msg_t *reply)
{
    reply->code         = (uint32_t)DOB_OK;
    reply->payload      = 0;
    reply->payload_size = 0;
}

/* write_op: il payload e' la password ATTUALE battuta nell'editor.
 * Verifica (o assenza) -> DOB_OK e doppio prompt sotto tenda;
 * mismatch -> DOB_ERR_DENIED e un toast sul desktop. */
void logon_eps_write(const dob_msg_t *msg, dob_msg_t *reply)
{
    char attempt[DOB_LOGON_PW_MAX + 1];
    uint32_t n = 0;

    if (msg->payload && msg->payload_size > 0)
    {
        n = msg->payload_size;
        if (n > DOB_LOGON_PW_MAX) n = DOB_LOGON_PW_MAX;
        memcpy(attempt, msg->payload, n);
    }
    attempt[n] = '\0';
    /* Il payload dell'editor e' NUL-terminato: la NUL inclusa nel
     * size accorcia la stringa da se'. Un'eventuale coda oltre la
     * prima NUL e' irrilevante: strcmp/hash si fermano li'. */

    /* Rilettura difensiva del record: se un'installazione parallela o
     * un ripristino ha toccato il file, si giudica sullo stato VERO. */
    pw_file_load();

    if (g_have_pw && !dob_logon_check(g_pw_salt, g_pw_hash, attempt))
    {
        memset(attempt, 0, sizeof(attempt));
        reply->code = (uint32_t)DOB_ERR_DENIED;
        toast_show("Password attuale errata");
        di_mark_dirty(DIRTY_FULL);
        return;
    }

    memset(attempt, 0, sizeof(attempt));
    reply->code = (uint32_t)DOB_OK;
    curtain_enter(L_NEW1);
}
