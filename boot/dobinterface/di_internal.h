/* MainDOB DobInterface 2.0 — di_internal.h: il vocabolario comune.
 *
 * Riscrittura ex novo del window manager. Questo header e' l'unico
 * punto d'incontro fra i fogli (sheet): dichiara l'ABI di filo, la
 * specifica visiva, i tipi condivisi, lo stato condiviso e i "verbi
 * standard" che ogni foglio espone. Tutto cio' che non sta qui e'
 * privato del suo foglio (static): l'1.x aveva ~105 globali piatte,
 * il 2.0 ne condivide solo il sottoinsieme davvero trasversale.
 *
 * DUE CONTRATTI NON NEGOZIABILI, ereditati come specifica dall'1.x:
 *
 *   [ABI]  I valori del protocollo GUI_* e CURSOR_* sono ABI di filo:
 *          i client compilati contro DobInterface.h 1.x devono
 *          funzionare invariati. MAI rinumerare.
 *
 *   [PX]   Le costanti visive (colori, metriche, spessori, tempi)
 *          sono specifica al pixel: l'utente NON deve accorgersi
 *          della riscrittura. MAI ritoccare "per estetica".
 */
#ifndef DI_INTERNAL_H
#define DI_INTERNAL_H

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/types.h>
#include <dob/window_servo.h>
#include <dob/spawn.h>
#include <dob/device_icon.h>
#include <dob/hotplug_events.h>
#include <dob/font.h>
#include <dob/video.h>
#include <dob/dobui_cmdbuf.h>
#include <DobVideoControl.h>
#include <DobFileSystem.h>
#include <DobSettings.h>        /* effetto typed-text: legge il setting */

/* ======================================================================
 * 1. [ABI] PROTOCOLLO GUI SU IPC — congelato, byte per byte come 1.x
 * ====================================================================== */

/* Programmi -> DobInterface */
#define GUI_WIN_CREATE      100
#define GUI_WIN_DESTROY     101
#define GUI_WIN_SET_TITLE   102
#define GUI_WIN_RAISE       103
#define GUI_WIN_HIDE        104
#define GUI_WIN_SET_FLAGS   105   /* arg0=win_id, arg1=bitmask flag       */
/* Relazione owner per sub-window/dialogo/popup: arg0=figlia, arg1=madre
 * (0 = sgancia, torna top-level), arg2=flag GUI_WIN_PARENT_*. Da
 * chiamare subito dopo CREATE, prima della prima INVALIDATE (stesso
 * idioma di SET_FLAGS: la relazione e' in vigore prima che la finestra
 * sia mai disegnata). Sincrona. */
#define GUI_WIN_SET_PARENT  106
#define GUI_WIN_INVALIDATE  115
#define GUI_PANEL_SET_CMDS  120
#define GUI_PANEL_CLEAR_CMDS 121
#define GUI_SET_CURSOR      122
/* Toast da componenti senza finestra (runner DAS di hotplug, tool
 * diagnostici): arg0=severita' (0=info, 2=errore, riservata allo
 * styling), payload=testo NUL-terminato. Post-only, nessuna reply. */
#define GUI_SHOW_TOAST      123
#define GUI_SPAWN_DRIVER    130
#define GUI_BEGIN_DRAG      131
#define GUI_CANCEL_DRAG     132
/* payload = "path arg1 arg2...": spawn di un programma GUI con lo
 * STESSO spawn_file del launcher desktop. Sincrona; reply.code =
 * DOB_OK / DOB_ERR_*. Nata per il DAS `open_program` (Formatta ->
 * DobDisk): lo spawn passa da dobinterface, non dal runner di
 * hotplug, eliminando in blocco ogni dipendenza dall'ambiente del
 * runner. */
#define GUI_LAUNCH_PROGRAM  133

/* Tray widget */
#define GUI_WIDGET_CREATE       140
#define GUI_WIDGET_DESTROY      141
#define GUI_WIDGET_INVALIDATE   142

/* Texture pool lato server (BlitBuffer oltre la soglia inline) e
 * pannello SHM del contenuto. GUI_WIN_SHM_ENSURE: il client chiede un
 * framebuffer w x h in memoria condivisa legato alla finestra (arg1=w,
 * arg2=h); risposta arg0=rc, arg1=shm_id. Il client vi disegna
 * direttamente e DOBUI_OP_BLIT_SHMPANEL lo fa blittare nel corpo al
 * bake: i pixel non attraversano MAI l'IPC. Rialloca se richiamato
 * con misure diverse. */
#define GUI_WIN_TEX_ALLOC      145
#define GUI_WIN_TEX_UPDATE     146
#define GUI_WIN_TEX_FREE       147
#define GUI_WIN_SHM_ENSURE     148

/* Password di accesso (foglio logon) — i due opcode della entry EPS
 * SECRET `sicurezza.logon_password` dichiarata a settingsd: l'editor
 * Impostazioni li usa DIRETTAMENTE contro la nostra porta (il daemon
 * non vede mai il valore). READ risponde sempre valore vuoto; WRITE
 * porta la password ATTUALE battuta dall'utente — verificata, apre il
 * doppio prompt del cambio sotto tenda. Sincroni (msg.type == 1). */
#define GUI_LOGON_PW_READ   160
#define GUI_LOGON_PW_WRITE  161

/* Kernel -> noi: la porta (arg0,arg1) di un owner e' morta
 * (death-watch, anti-ABA). Sostituisce il poll dei processi. */
#define GUI_PEER_DIED       250

/* DobInterface -> programmi */
#define GUI_EVT_PANEL_CMD   200
#define GUI_EVT_MOUSE       201
#define GUI_EVT_KEY         202
#define GUI_EVT_SCROLL      203
#define GUI_EVT_CLOSE_REQ   210
#define GUI_EVT_RESIZE      212
#define GUI_EVT_MODCHANGE   213
#define GUI_EVT_DROP        214
#define GUI_EVT_DRAG_END    215
/* Hover continuo durante una sessione DRAG_FILES: postato alla
 * finestra sotto il cursore a ogni mossa (inclusa la sorgente).
 * arg0=id finestra bersaglio, arg1=locali impacchettate (x|y<<16) in
 * coordinate contenuto. Post-only. Quando il cursore esce del tutto
 * da una finestra il WM smette di postare per lei; il bersaglio
 * spegne l'evidenziazione dopo un breve idle o su drop/drag-end. */
#define GUI_EVT_DRAG_OVER   216
#define GUI_EVT_WIDGET_CLICK 220

/* Flag di policy trasportate in GUI_WIN_SET_FLAGS arg1.
 * ABI di filo — DEVONO combaciare con DOBUI_WIN_* in DobInterface.h. */
#define GUI_WIN_FLAG_NORESIZE    0x1
#define GUI_WIN_FLAG_NOMAXIMIZE  0x2
/* Flag di relazione per GUI_WIN_SET_PARENT arg2. */
#define GUI_WIN_PARENT_MODAL     0x1   /* la figlia blocca l'input alla madre */

/* Tasto speciale da inputd: Stamp R Sist. INTERCETTATO qui (foglio
 * screenshot), mai instradato alle finestre. */
#define SKEY_PRTSC          137

/* Identificatori dei puntatori — DEVONO combaciare con i CURSOR_*
 * pubblici di <DobInterface.h> (i client li passano via
 * GUI_SET_CURSOR). Duplicati e non #include'si per non trascinare
 * main nel grafo di dipendenze client; le _Static_assert sotto
 * saldano i due lati a compile time. */
#define CURSOR_ARROW     0
#define CURSOR_RESIZE    1
#define CURSOR_FILE_DRAG 2
#define CURSOR_HSPLIT    3
#define CURSOR_VSPLIT    4
#define CURSOR_COUNT     5
_Static_assert(CURSOR_ARROW     == 0,  "CURSOR_ARROW ABI mismatch with DobInterface.h");
_Static_assert(CURSOR_RESIZE    == 1,  "CURSOR_RESIZE ABI mismatch with DobInterface.h");
_Static_assert(CURSOR_FILE_DRAG == 2,  "CURSOR_FILE_DRAG ABI mismatch with DobInterface.h");
_Static_assert(CURSOR_HSPLIT    == 3,  "CURSOR_HSPLIT ABI mismatch with DobInterface.h");
_Static_assert(CURSOR_VSPLIT    == 4,  "CURSOR_VSPLIT ABI mismatch with DobInterface.h");
/* Sentinella per window_t.cursor_override: "nessun override". */
#define CURSOR_DEFAULT  ((int)-1)

/* ======================================================================
 * 2. [PX] SPECIFICA VISIVA — identica all'1.x al pixel
 * ====================================================================== */

/* Geometria schermo. Default 1024x768 — identico a ogni build
 * precedente, cosi' il percorso di boot e' invariato byte per byte e
 * i driver a modo fisso delicati (es. mach64) non vengono mai
 * interrogati. Aggiornata a runtime SOLO quando un provider
 * DobVideoControl spinge DOBVC_EVENT_MODE_CHANGED (vedi
 * video_on_mode_changed / video_relayout_to in video.c).
 * SCREEN_W/SCREEN_H alias delle globali: i siti aritmetici esistenti
 * non cambiano. */
extern int g_screen_w;
extern int g_screen_h;
#define SCREEN_W            (g_screen_w)
#define SCREEN_H            (g_screen_h)
#define SCREEN_W_DEFAULT    1024
#define SCREEN_H_DEFAULT    768
#define SCREEN_MIN_W        640
#define SCREEN_MIN_H        480
#define SCREEN_MAX_DIM      8192   /* strip dell'anteprima servo (buffer [8192]) */

/* Colori — layout BGRX su x86: byte0=B, byte1=G, byte2=R, byte3=X */
#define COLOR_BG        0x000000AA  /* blu MainDOB: B=0xAA, G=0, R=0 */
#define COLOR_BLACK     0x00000000
#define COLOR_WHITE     0x00FFFFFF
#define COLOR_CYAN      0x0000FFFF  /* B=0xFF, G=0xFF, R=0x00 = ciano */
#define COLOR_YELLOW    0x00FFFF00  /* R=0xFF, G=0xFF, B=0x00 = giallo */
#define COLOR_STRUCT_LINE 0x00112244 /* linee/bordi struttura (blu scuro) */
#define COLOR_WIN_BORDER  0x00224466 /* bordo finestra non-focus (blu-grigio) */
#define COLOR_WIN_FLASH   0x00FF3333 /* rosso allarme: lampeggio bordo modale su clic bloccato */
#define COLOR_TEXT_MUTE 0x00556699  /* testo muto/disabilitato (= DOBUI_DISABLED) */
#define COLOR_LABEL_BLUE 0x0000AAFF /* come benchmark COL_LABEL */
#define COLOR_WIN_HEAD  0x000000AA  /* header dim / non-focus = blu 170 */
#define COLOR_WIN_HEAD_ACTIVE 0x000000FF /* title bar in focus = blu 255 (RELIEF) */
#define COLOR_PANEL_BG  COLOR_BLACK
#define COLOR_PANEL_HOVER 0x00001133 /* hover su pannello nero */

/* Tempi del lampeggio modale (foglio style) */
#define FLASH_HALF_MS     500       /* durata di ogni semiperiodo (acceso o spento) */
#define FLASH_PHASES      6         /* 3 lampeggi = 3 accesi + 3 spenti */

/* Font a cella fissa (la tabella glifi vive in libdob, <dob/font.h>) */
#define FONT_W          8
#define FONT_H          16

/* Layout del pannello comandi */
#define PANEL_PAD_L     12
#define PANEL_PAD_R     12
#define PANEL_MARGIN_TOP  8
#define ITEM_PAD_TOP    6
#define ITEM_PAD_BOT    6
#define ITEM_HEIGHT     (FONT_H + ITEM_PAD_TOP + ITEM_PAD_BOT)
#define SEPARATOR_H     12

/* Finestre */
#define MAX_WINDOWS     64  /* Alzato da 32 dopo una sessione al tetto con
                             * un corredo pieno di programmi. Costo ~76 KiB
                             * statici fra windows[] + mc_thumbs[] +
                             * sorted_wins[]. Il pool cmdlist BGA (8 MiB)
                             * regge 64 finestre al cap iniziale di 64 KiB
                             * con margine per grow moderati; carichi densi
                             * (64 finestre classe DobTable a 512 KiB)
                             * resterebbero un tetto teorico da 32+ MiB da
                             * inseguire a parte. */
#define WIN_HEADER_H    24
#define WIN_MIN_SIZE    120
#define RESIZE_GRAB     10

/* Puntatore */
#define CURSOR_W        12
#define CURSOR_H        17

/* Mission Control */
#define MC_THUMB_GAP    20  /* spazio fra miniature */
#define MC_EDGE_SCROLL  30  /* px dal bordo che innescano lo scroll */
#define MC_SCROLL_SPEED 8   /* px per frame in edge-scroll */

/* Toast */
#define TOAST_LAYER_H   (FONT_H + 10)
#define TOAST_LAYER_Y   4

#define WPANEL_LAYER_Z  120  /* tray: sopra finestre (10..73) e servo (100),
                              * sotto toast (500) e cursore (999) */

#define LOGON_LAYER_Z   900  /* tenda (logon/screensaver): il backbuf sale
                              * qui quando la tenda e' su — sopra finestre,
                              * servo, tray e toast; sotto il SOLO cursore.
                              * Lo z=50 di MC/About non basta da sipario:
                              * copre le finestre normali ma per contratto
                              * la tenda deve coprire TUTTO. */

/* Icone DAS sul desktop */
#define ICON_W                  72
#define ICON_H                  80
#define ICON_MARGIN_RIGHT       16
#define ICON_MARGIN_BOTTOM      16
#define ICON_GAP                8
#define ICON_LABEL_H            20  /* FONT_H + piccolo padding */
#define MAX_DESKTOP_ICONS       64
#define ICON_DBLCLICK_MS        500

/* Tray widget */
#define MAX_WIDGETS         16
#define WIDGET_GAP          4   /* px fra widget impilati */
#define WIDGET_PAD          6   /* padding interno all'overlay del pannello */
#define WIDGET_SCROLL_STEP  20  /* px per tacca di scroll */

/* Effetto typed-text (apertura finestra) — vedi style.c */
#define TYPE_ANIM_MS         1000u  /* durata reveal per-run (default) */
#define TYPE_ANIM_FRAME_MS     33u  /* cadenza ~30 fps */
#define TYPE_ANIM_MS_MIN      100u
#define TYPE_ANIM_MS_MAX     5000u

/* ======================================================================
 * 3. QUOTE DEL PROGETTO (non visive, non ABI)
 * ====================================================================== */

#define MAX_WIN_CTX_CMDS    16
#define MAX_PANEL_ITEMS     64

/* Sezioni del pannello comandi */
#define SECTION_HEADER  0
#define SECTION_WINDOW  1
#define SECTION_CONTEXT 2
#define SECTION_FOOTER  3

/* Command ID interni del pannello */
#define CMD_WIN_CLOSE       1000
#define CMD_WIN_MAXIMIZE    1001
#define CMD_WIN_HIDE        1002
#define CMD_SHOW_WINDOWS    2000
#define CMD_OPEN_WIDGETS    2001
#define CMD_CTX_BASE        3000
/* Default del desktop (nessuna finestra in focus) */
#define CMD_OPEN_FILES      4000
#define CMD_OPEN_MODULES    4001
#define CMD_OPEN_SETTINGS   4002
/* Menu MainDOB */
#define CMD_MAINDOB_MENU    4100
#define CMD_SHUTDOWN        5000
#define CMD_ABOUT           5001
#define CMD_REBOOT          5002
#define CMD_LOCK            5003   /* Blocca: prompt se c'e' password, altrimenti oscura */
#define CMD_DARKEN          5004   /* Oscura schermo: screensaver immediato, sempre      */
/* Voci del menu {} DAS delle icone device: CMD_ICON_MENU_BASE + idx.
 * dobinterface non ha ALCUNA conoscenza cablata delle azioni device:
 * etichette e comportamento sono 100% data-driven dal file DAS. */
#define CMD_ICON_MENU_BASE  6100

/* Scratch di rasterizzazione CPU (atlante glifi, icone, screenshot):
 * dimensionato al caso peggiore, cosi' ogni upload entra senza
 * allocazioni per-chiamata. */
#define WIN_SCRATCH_W   (SCREEN_W + 2)
#define WIN_SCRATCH_H   (SCREEN_H + WIN_HEADER_H + 2)
#define WIN_SCRATCH_PX  (WIN_SCRATCH_W * WIN_SCRATCH_H)

/* ======================================================================
 * 4. TIPI CONDIVISI
 * ====================================================================== */

/* --- Riassemblaggio del cmdbuf segmentato (vedi dobui_cmdbuf.h) ---
 * Il buffer IPC del ricevente strippa in silenzio i payload oltre il
 * suo tetto (IPC_BUF_SIZE), quindi lo stub client spedisce i cmdbuf
 * grandi a segmenti: arg1=seq, arg2=segmenti totali (0 = legacy
 * monoblocco), arg3=byte totali. Si riassembla per finestra o per
 * widget; un buco di sequenza scarta il parziale e il prossimo
 * segmento 0 riparte pulito — il frame successivo risana da solo. */
typedef struct
{
    uint8_t  *buf;          /* accumulo (malloc, cresce al bisogno)   */
    uint32_t  cap;
    uint32_t  bytes;        /* byte accumulati finora                 */
    uint32_t  expect_seq;   /* prossimo segmento atteso               */
    uint32_t  total_segs;   /* dal segmento 0 corrente                */
    uint32_t  total_bytes;  /* idem                                   */
} cmdbuf_reasm_t;

/* --- Voce del pannello comandi --- */
typedef struct
{
    uint8_t     section;
    char        label[64];
    uint32_t    command_id;
    bool        visible;
    bool        enabled;
    int         hit_y;
    int         hit_h;
} panel_item_t;

/* --- Finestra --- */
typedef struct
{
    bool        used;
    uint32_t    id;
    char        title[64];
    int         x, y;
    int         width, height;
    pid_t       owner_pid;
    uint32_t    owner_port;
    uint32_t    owner_gen;   /* generazione della porta al binding: la
                              * consegna verificata rifiuta un id
                              * riciclato (anti-ABA, win_owner_post) */
    bool        owner_dead;  /* scoperto morto da una post verificata:
                              * distrutto al prossimo punto sicuro    */
    bool        visible;
    bool        maximized;
    int         z_order;
    bool        focused;
    /* Policy resize/maximize, impostate dall'owner via
     * GUI_WIN_SET_FLAGS. Indipendenti; default false (memset). */
    bool        no_resize;
    bool        no_maximize;
    /* === Relazione owned-window (sub-window, dialoghi, popup) ===
     * parent_id: id della finestra madre, 0 = top-level (via
     *   GUI_WIN_SET_PARENT subito dopo CREATE; puo' riferire una
     *   finestra di un ALTRO processo — e' un window id, non un pid).
     * modal: finche' true E visibile, ogni finestra su per la catena
     *   delle madri e' input-blocked; un clic su una bloccata alza
     *   QUESTA e suona l'attention ding. Default 0/false (memset). */
    uint32_t    parent_id;
    bool        modal;
    /* Comandi di contesto registrati dal programma (PANEL_SET_CMDS) */
    char        ctx_labels[MAX_WIN_CTX_CMDS][64];
    int         ctx_count;
    bool        content_dirty;  /* buffer cambiato: reblit, non full repaint */
    /* Il programma ha mai invalidato? False da CREATE alla prima
     * GUI_WIN_INVALIDATE. Il compositor salta le finestre non-ready
     * per non mostrare il frame a zero durante init/event_start. */
    bool        ready;
    /* Override del puntatore mentre il mouse e' nel corpo di questa
     * finestra. CURSOR_DEFAULT = ricadi su cursor_for_position. */
    int         cursor_override;

    /* Stato reveal typed-text (effetto apertura). Alzato la prima
     * volta che la finestra diventa ready, azzerato a reveal finito
     * (timestamp clock_ms). Default 0/false via memset: una finestra
     * che non anima non costa nulla qui. */
    bool        type_anim_active;
    uint32_t    type_anim_start_ms;

    /* Lampeggio del bordo (clic su madre bloccata da questo modale). */
    bool        flash_active;
    uint32_t    flash_start_ms;

    /* === Stato di rendering lato driver === */

    /* Layer del compositor per il corpo. DV_HANDLE_NONE se
     * l'allocazione e' fallita (la finestra resta invisibile finche'
     * la pressione sul cap non cala; last_cmdbuf e' conservato per
     * il retry). */
    dv_layer_t   body_layer;

    /* La surface del corpo va ri-bakata prima della prossima compose.
     * Alzato da win_create (primo render), GUI_WIN_INVALIDATE, cambi
     * di focus (il colore del chrome dipende dal focus) e resize.
     * Abbassato dal compositor dopo win_bake. */
    bool         surface_dirty;

    /* Corpo della finestra: surface in RAM di sistema (flag SYSRAM,
     * zero VRAM e zero quota), surf_w x surf_h. Disegnata UNA volta
     * da win_bake (chrome + replay del cmdbuf); la compose la blitta
     * come pixel finali — mai piu' replay nel buffer visibile. */
    dv_surface_t body_surf;

    /* Ultimo cmdbuf del client, copiato qui cosi' il corpo si
     * ricostruisce sui cambi di solo chrome (focus, titolo) senza
     * round-trip col client. NULL su finestra appena creata (il
     * chrome rende comunque; corpo vuoto fino alla prima
     * INVALIDATE). */
    uint8_t     *last_cmdbuf;
    uint32_t     last_cmdbuf_size;
    uint32_t     last_cmdbuf_cap;

    /* Riassemblaggio del cmdbuf segmentato: stato per-finestra. */
    cmdbuf_reasm_t reasm;

    /* Texture pool lato server per i record OP_BLIT_TEX — allocato
     * pigramente via GUI_WIN_TEX_ALLOC. Ogni BlitBuffer passa di qui
     * (il fast path inline e' stato ritirato per una race sullo
     * scratch cross-finestra).
     *
     * Profondita' WIN_TEX_POOL_SIZE: DobFiles, l'utente piu' pesante,
     * blitta ~10 raster distinti per finestra; 16 lascia spazio a
     * nuovi widget. Le entry sono pigre — solo gli slot usati
     * consumano VRAM. Eviction first-fit-replace. Su un adapter da
     * 4 MiB (Armada E500) anche 16 finestre x 16 entry taglia-icona
     * = 1 MiB, ci sta comodo. DEVE restare uguale a TEX_POOL_SIZE in
     * DobInterface_stub.c. */
    #define WIN_TEX_POOL_SIZE   16
    struct {
        dv_texture_t handle;
        uint16_t     w, h;
        /* Specchio CPU dei pixel (BGRA). I pixel della texture vivono
         * nella RAM del PROCESSO DRIVER (SYSRAM): senza specchio, la
         * ricomposizione software (screenshot, e in futuro thumbnail
         * fedeli) non avrebbe accesso ai raster. Costo: una copia in
         * piu' di cio' che il client gia' invia (icone: KB; la pagina
         * di DobWrite: MB — accettato e documentato). NULL se la
         * malloc fallisce: lo screenshot degrada a segnaposto. */
        uint32_t    *cpu;
    } tex_pool[WIN_TEX_POOL_SIZE];
    uint32_t     tex_pool_count;

    /* Pannello SHM del contenuto (GUI_WIN_SHM_ENSURE): buffer
     * condiviso che l'app scrive e win_bake legge — percorso a copia
     * singola per i contenuti grandi. shm_id < 0 = nessun pannello. */
    int32_t      panel_shm_id;
    uint32_t    *panel_ptr;
    uint16_t     panel_w, panel_h;
    /* Il corpo riflette gia' il pannello? Finche' e' false (pannello
     * nuovo, corpo ricreato da un resize, rettangolo di blit
     * cambiato) ogni banda dichiarata dal client viene promossa a
     * copia integrale: fuori banda il corpo non avrebbe pixel
     * validi. */
    bool         panel_synced;
    int16_t      panel_last_x, panel_last_y;
    uint16_t     panel_last_w, panel_last_h;
} window_t;

/* --- Slot widget del tray (mini-finestre SHM, aperte dal '<') --- */
typedef struct
{
    bool        used;
    uint32_t    id;
    pid_t       owner_pid;
    uint32_t    owner_port;
    int         width, height;
    int32_t     shm_id;
    uint32_t   *buffer;        /* puntatore SHM mappato */
    bool        content_dirty;
    int         hit_y, hit_h;  /* coordinate schermo assolute, fissate al draw */
    /* Texture lato server che rispecchia l'SHM del widget: allocata
     * pigramente al primo replay, aggiornata a ogni repaint SHM;
     * wpanel_draw la blitta cosi' il widget appare a schermo. */
    dv_texture_t cache_tex;
    /* Riassemblaggio del cmdbuf segmentato. */
    cmdbuf_reasm_t reasm;
} widget_slot_t;

/* --- Icona device sul desktop (DAS) --- */
typedef struct
{
    bool          in_use;
    uint32_t      device_id;
    uint32_t      kind;
    char          label[32];
    char          service_name[32];
    int           slot_col;    /* 0 = colonna piu' a destra */
    int           slot_row;    /* 0 = riga piu' in basso    */
    bool          selected;
    icon_bitmap_t bitmap;
    uint8_t       menu_count;
    char          menu_items[DEV_MENU_MAX_ITEMS][DEV_MENU_LABEL_LEN];
    /* Surface BGRA pre-rasterizzata dell'icona (alpha=0 fuori, 0xFF
     * sui pixel "accesi", colorata dal fg del bitmap). Allocata
     * pigramente al primo draw, liberata alla rimozione. */
    dv_surface_t  icon_surf;
    uint16_t      icon_surf_w;
    uint16_t      icon_surf_h;
} desktop_icon_t;

/* --- Miniatura di Mission Control --- */
typedef struct
{
    int win_idx;    /* indice in windows[] */
    int x, y;       /* posizione nel layout MC (prima dello scroll) */
    int thumb_size; /* lato della miniatura */
    /* Texture pre-renderizzata: allocata a mc_enter rasterizzando in
     * software l'ultimo cmdbuf della finestra, liberata a mc_exit. */
    dv_texture_t tex;
    int          tex_w, tex_h;
} mc_thumb_t;

/* --- Visitor del decoder cmdbuf (implementato da piu' fogli) ---
 * cmdbuf_decode e' l'UNICA fonte di verita' del wire-format: legge i
 * record, fa i bounds check e chiama i callback del visitor; i
 * callback NULL sono tollerati (il decoder avanza comunque oltre il
 * record, cosi' un consumatore ignora in silenzio gli opcode che non
 * gli interessano). Conservativo: record troncato, payload malformato
 * o opcode ignoto terminano il decode e scartano il resto.
 * Consumatori: bake finestra e SHM widget (cmdbuf.c), miniature
 * fedeli (missionctl.c, via specchio CPU del tex_pool), raster
 * software (screenshot.c). */
typedef struct cmdbuf_visitor {
    void *ctx;
    void (*fill_rect) (void *ctx, int x, int y, int w, int h, uint32_t color);
    void (*draw_rect) (void *ctx, int x, int y, int w, int h, uint32_t color);
    void (*draw_pixel)(void *ctx, int x, int y, uint32_t color);
    void (*draw_text) (void *ctx, int x, int y, uint32_t fg, uint32_t bg,
                       const uint8_t *text, uint32_t len, int fixed);
    void (*blit_inline)(void *ctx, int x, int y, int w, int h,
                        const uint32_t *pixels);
    void (*blit_tex)  (void *ctx, int x, int y, uint32_t handle, int w, int h);
    void (*blit_shmpanel)(void *ctx, int x, int y, int w, int h,
                          unsigned band_y0, unsigned band_rows);
} cmdbuf_visitor_t;

/* Esito del prescan del primo record SHMPANEL (vedi cmdbuf.c). */
typedef struct { bool found; int x, y, w, h; } shm_rect_scan_t;

/* ======================================================================
 * 5. STATO CONDIVISO (definito nel foglio proprietario indicato)
 * ====================================================================== */

/* video.c — pipeline dv_* */
extern dv_vproc_t   g_vproc;
extern dv_surface_t g_backbuf_surf;   /* surface fullscreen SYSRAM: e' anche lo sfondo */
extern bool         frame_flipped;    /* alzato da fb_flip, evita doppio flip per tick */

/* font.c */
extern dv_texture_t g_glyph_atlas;

/* main.c — porta IPC del server GUI */
extern uint32_t     gui_port;

/* draw.c — scratch CPU condiviso (glifi, icone, screenshot) */
extern uint32_t    *g_win_scratch;

/* window.c — parco finestre e z-order */
extern window_t     windows[MAX_WINDOWS];
extern int          focused_win;          /* indice in windows[], -1 = nessuna */
extern int          sorted_wins[MAX_WINDOWS];
extern int          sorted_count;
extern bool         z_sort_valid;
extern int          pointer_capture_win;  /* release instradata qui anche se il
                                           * cursore e' altrove (drag client) */

/* dirty.c — il "dirty state" del desktop e' stato PRIVATO del foglio
 * dirty (non piu' 4 booleani globali): si tocca solo via i tre verbi
 * di_mark_dirty / di_dirty / di_dirty_clear, dichiarati piu' sotto. */

/* panel.c — geometria pannello/desktop */
extern int          panel_w;
extern int          desktop_w;
extern int          panel_x;
extern bool         panel_width_changed;  /* alzato da panel_recalculate,
                                           * consumato dal compositor */
extern bool         maindob_menu_open;
extern bool         about_overlay_active;
extern int          panel_hover_idx;   /* voce sotto il mouse (dal foglio input) */

/* cursor.c */
extern int          cursor_x;
extern int          cursor_y;
extern int          current_cursor;

/* winactions.c — motore geometrico servo */
extern servo_t      win_servo;

/* missionctl.c */
extern bool         mc_active;

/* toast.c */
extern bool         toast_active;

/* tray.c */
extern widget_slot_t widgets[MAX_WIDGETS];
extern bool          widget_panel_open;
extern int           widget_hover_idx; /* slot sotto il mouse (-1 = nessuno) */
extern int           widget_grab_idx;  /* slot in drag  (-1 = nessuno)       */

/* icons.c */
extern desktop_icon_t desktop_icons[MAX_DESKTOP_ICONS];
extern int            desktop_icon_count;
extern int            selected_icon;

/* style.c — tuning live dell'effetto typed-text */
extern bool         g_typed_enabled;
extern uint32_t     g_typed_ms;

/* ======================================================================
 * 6. VERBI STANDARD (prototipi, per foglio)
 *    Ogni foglio: verbi esecutivi + raccoglitori; la logica ad alto
 *    livello sta in fondo al foglio. Qui solo cio' che altri fogli
 *    chiamano. [In crescita: le sezioni dei fogli non ancora scritti
 *    vengono aggiunte al loro arrivo, cosi' l'header non promette
 *    verbi che non esistono ancora.]
 * ====================================================================== */

/* --- video.c --- */
void video_init(void);                 /* porta su la pipeline; fatale se fallisce il core */
void fb_flip(void);                    /* compose+present con pacing e vsync */
void fb_flip_no_vsync(void);           /* idem senza vsync (mosse del puntatore) */
void fb_flip_rect(int x, int y, int w, int h);  /* present parziale scissored */
void pacing_run_deferred_flip(void);   /* dal loop eventi: timer di pacing scattato */
void backbuf_set_z(int new_z);         /* z del backbuf (overlay MC/About sopra le finestre) */
void video_on_mode_changed(void);      /* DOBVC_EVENT_MODE_CHANGED: valida e rilayouta */

/* --- dirty.c: ragioni di present del desktop (bitmask, un proprietario) ---
 * Ex 4 booleani globali; ora stato privato del foglio dirty. Un
 * present pieno (compositor_repaint) consuma FULL|PANEL|CONTENT; il
 * drag (compositor_drag_blit) consuma FULL|CONTENT; il cursore lo
 * consuma il ramo cursore del loop. */
#define DIRTY_FULL     0x1u   /* repaint pieno            (ex needs_repaint)        */
#define DIRTY_PANEL    0x2u   /* solo pannello            (ex needs_panel_repaint)  */
#define DIRTY_CONTENT  0x4u   /* contenuto finestra       (ex needs_content_blit)   */
#define DIRTY_CURSOR   0x8u   /* forma cursore a fermo    (ex needs_cursor_redraw)  */
void di_mark_dirty(uint32_t reasons);   /* accende (OR-in): unico modo di sporcare */
bool di_dirty(uint32_t mask);           /* interroga (loop-spec in main.c)          */
void di_dirty_clear(uint32_t mask);     /* spegne: solo chi ha appena presentato    */

/* --- draw.c ---
 * surf_*: primitive esecutive su una surface qualunque (nucleo
 * riusabile, usato dal compositor sul corpo finestra). fb_*: gli
 * stessi verbi legati al backbuf desktop, per il chrome di desktop. */
int  clamp(int val, int lo, int hi);
void surf_hline(dv_surface_t surf, int x, int y, int w, uint32_t color);
void surf_vline(dv_surface_t surf, int x, int y, int h, uint32_t color);
void surf_draw_rect(dv_surface_t surf, int x, int y, int w, int h,
                    uint32_t color, int thick);
void surf_fill_rect(dv_surface_t surf, int x, int y, int w, int h, uint32_t color);
void fb_draw_hline(int x, int y, int w, uint32_t color);
void fb_draw_vline(int x, int y, int h, uint32_t color);
void fb_draw_rect(int x, int y, int w, int h, uint32_t color, int thick);
void fb_fill_rect(int x, int y, int w, int h, uint32_t color);

/* --- video.c: verbo per cmdbuf.c (OP_BLIT_INLINE) ---
 * Prossima scratch del round-robin; DV_HANDLE_NONE se il pool manca.
 * dim_out (opzionale) riceve il lato della scratch. */
dv_texture_t video_blit_scratch_next(uint32_t *dim_out);

/* --- font.c (foglio in arrivo) --- */
void font_draw_string(int x, int y, const char *str, uint32_t fg, uint32_t bg);
void glyph_atlas_init(void);
uint32_t string_to_glyphs(const char *str, int x, int y,
                          dv_glyph_t *out, uint32_t out_cap);

/* --- cursor.c (foglio in arrivo) --- */
void cursor_init_video(void);
void cursor_upload_if_needed(void);
void cursor_layer_update_pos(void);
int  cursor_for_position(int mx, int my);
bool cursor_is_hw(void);               /* percorso present: registri vs flip  */
uint8_t cursor_sprite_pixel(int type, int row, int col); /* 0/1/2 per lo shot */

/* --- cmdbuf.c --- */
void cmdbuf_reasm_free(cmdbuf_reasm_t *r);
bool cmdbuf_reasm_ingest(cmdbuf_reasm_t *r, const dob_msg_t *msg,
                         const uint8_t **out_buf, uint32_t *out_size);
void cmdbuf_decode(const uint8_t *buf, uint32_t size,
                   const cmdbuf_visitor_t *v);
int  tex_pool_find(window_t *w, dv_texture_t handle);
/* Specchio CPU di un handle del pool (NULL se assente o saltato per
 * OOM): il verbo delle ricomposizioni software — screenshot e
 * miniature FEDELI del Mission Control leggono da qui i raster veri
 * invece di lasciare buchi sui blit_tex. */
const uint32_t *win_tex_pool_cpu_find(window_t *w, uint32_t handle,
                                      uint16_t *w_out, uint16_t *h_out);
void win_replay_cmdbuf(window_t *w, const uint8_t *buf, uint32_t size,
                       uint32_t reveal_num, uint32_t reveal_den);
bool cmdbuf_shmpanel_rect(const uint8_t *buf, uint32_t size,
                          shm_rect_scan_t *out);
bool cmdbuf_has_text(const uint8_t *buf, uint32_t size);
void widget_replay_cmdbuf_to_shm(widget_slot_t *ws,
                                 const uint8_t *buf, uint32_t size);

/* --- window.c --- */
int  win_find_by_id(uint32_t id);
int  win_create(const char *title, int w, int h, int x, int y);
void win_destroy(int idx);
void win_attention_ding(void);
int  win_top_ancestor(int idx);
int  win_depth(int idx);
int  win_modal_blocker(int idx);
void win_restack_for(int target);
void win_focus(int idx);
void win_unfocus_all(void);
void win_unfocus_to_desktop(void);
void sort_windows_by_z(void);
int  hit_test_window(int mx, int my, int *zone);
bool win_owner_post(int win_idx, dob_msg_t *msg);
void send_win_event(int win_idx, uint32_t code, uint32_t a0, uint32_t a1,
                    uint32_t a2);
void win_reap_dead_owners(void);

/* --- winactions.c --- */
void win_apply_geometry(int idx, int new_x, int new_y, int new_w, int new_h);
void win_toggle_maximize(int idx);
void servo_preview_init_video(void);
void servo_preview_hide(void);
void servo_preview_teardown(void);
void servo_draw_preview(void);

/* --- style.c --- */
void flash_start(int idx);
bool flash_is_red(int idx);
void flash_pump(void);
void type_anim_start(window_t *w);
void type_anim_pump(void);
void typed_settings_load(void);
bool style_timer_fired(int timer_id);  /* routing codice 70: true = pompa di stile */

/* --- toast.c --- */
void toast_show(const char *text);
void toast_init_video(void);
void toast_layer_hide(void);
void toast_layer_relayout(void);    /* dst_rect a misura nuova (relayout) */

/* --- panel.c ---
 * I verbi di focus incapsulano cio' che nell'1.x era manipolazione
 * diretta di panel_items[] da parte del motore finestre: header col
 * titolo (o "MainDOB"), visibilita' dei comandi finestra secondo
 * policy, etichetta Ingrandisci/Finestra dallo stato maximized. */
void     panel_init_items(void);
void     panel_focus_window(const window_t *w);
void     panel_focus_desktop(void);
void     panel_set_maximize_label(bool maximized);  /* "Finestra" / "Ingrandisci" */
void     panel_set_maximize_visible(bool visible);
void     panel_set_context_items(const char *labels[], const uint32_t cmds[],
                                 int count);
void     panel_recalculate(void);
void     panel_sync_context(void);
void     panel_draw(void);
int      hit_test_panel(int my);
uint32_t panel_item_command(int idx);
void     handle_panel_click(int item_idx);

/* --- tray.c --- */
int  widget_find_by_id(uint32_t id);
int  widget_create(int w, int h, pid_t owner_pid, uint32_t owner_port);
void widget_destroy(int idx);
void widget_cleanup_for_pid(pid_t pid);
void widget_send_mouse(int idx, int mx, int my, uint32_t etype);
void wpanel_init_video(void);
void wpanel_calc_geometry(void);
void wpanel_open(void);
bool wpanel_scroll_by(int delta_px);
void wpanel_layer_relayout(void);   /* dst_rect a misura nuova (relayout) */
bool wpanel_contains(int mx, int my);  /* (mx,my) dentro il rettangolo del tray? */
int  wpanel_hit_test(int mx, int my);
void wpanel_draw(void);

/* --- icons.c --- */
int  icon_find_by_device_id(uint32_t id);
int  icon_hit_test(int mx, int my);
void icon_draw_all(void);
void icon_draw_band(int y0, int y1);
int  icon_add(const gui_device_attach_t *p);
bool icon_remove_by_id(uint32_t device_id);
void icon_repack_slots(void);
void send_icon_activated(uint32_t device_id);
void icon_menu_activate(uint32_t menu_idx);
void handle_icon_click(int idx);

/* --- missionctl.c --- */
void mc_enter(void);
void mc_exit(int focus_idx);
void mc_draw(void);
int  mc_hit_test(int mx, int my);
bool mc_handle_scroll(int scroll, int cur_y);

/* --- screenshot.c --- */
void screenshot_take(void);

/* --- input.c --- */
void input_subscribe(void);
bool input_pump(void);              /* un giro: receive+drain+routing; true = cursore mosso */
void input_abort_sessions(void);    /* chiude drag/resize/grab vivi (ingresso tenda) */
bool dragfiles_begin(uint32_t src_win_id, uint32_t src_port,
                     const void *payload, uint32_t payload_size);
void dragfiles_cancel(void);

/* --- logon.c ---
 * La tenda: schermata di accesso, screensaver, timeout, cambio
 * password. Il compositor interroga logon_visible e disegna via
 * logon_draw; il foglio input consegna tasti (logon_key) e mouse
 * (logon_gate_mouse) e nota l'attivita' umana (logon_note_activity);
 * il pannello comanda logon_lock / logon_darken; l'IPC instrada i due
 * opcode EPS (logon_eps_read / logon_eps_write); il codice 70 passa
 * per logon_timer_fired nella catena stile -> logon -> pacing. */
void logon_settings_load(void);
void logon_boot_check(void);
bool logon_visible(void);
bool logon_has_password(void);
void logon_draw(void);
void logon_lock(void);
void logon_darken(void);
void logon_key(uint8_t key);
bool logon_gate_mouse(bool any_press, bool moved);
void logon_note_activity(void);
bool logon_timer_fired(int timer_id);
void logon_eps_read(dob_msg_t *reply);
void logon_eps_write(const dob_msg_t *msg, dob_msg_t *reply);

/* --- ipc.c --- */
void handle_gui_ipc(dob_msg_t *msg, dob_msg_t *reply);

/* --- compositor.c --- */
void compositor_repaint(void);
void compositor_repaint_rect(int x, int y, int w, int h);
void compositor_blit_window(window_t *w);
void compositor_content_blit(void);   /* solo-contenuto: rebake finestre + flip, no rebuild backbuf */
void compositor_drag_blit(int drag_idx);
void win_alloc_video(window_t *w);
void win_free_video(window_t *w);
void win_tex_pool_free(window_t *w);
void win_bake(window_t *w);
void win_update_layer_pos(window_t *w);

/* ======================================================================
 * 7. AIUTANTI INLINE CONDIVISI
 * ====================================================================== */

/* u32 BGRX -> dv_color_t (alpha pieno). Unico punto di conversione:
 * l'1.x ripeteva il literal in quattro primitive. */
static inline dv_color_t dv_color_from_u32(uint32_t c)
{
    dv_color_t k = {
        .b = (uint8_t)( c        & 0xFF),
        .g = (uint8_t)((c >>  8) & 0xFF),
        .r = (uint8_t)((c >> 16) & 0xFF),
        .a = 0xFF,
    };
    return k;
}

/* Larghezza proporzionale di una stringa (combacia con
 * font_draw_string e col testo proporzionale del compositor).
 * UTF-8 aware via dob_text_width: le etichette accentate misurano
 * per glifo, non per byte. */
static inline uint32_t font_string_width(const char *str)
{
    return (uint32_t)dob_text_width(str, (uint32_t)strlen(str));
}

/* Misure della surface del corpo per una finestra (w, h): bordo da
 * 1 px per lato e WIN_HEADER_H righe di header. */
static inline int win_surf_w(const window_t *w) { return w->width  + 2; }
static inline int win_surf_h(const window_t *w) { return w->height + WIN_HEADER_H + 2; }

/* Origine del dst_rect del layer: 1 px sopra e a sinistra di (x, y)
 * cosi' righe/colonne esterne della surface combaciano col bordo. */
static inline int win_layer_x(const window_t *w) { return w->x - 1; }
static inline int win_layer_y(const window_t *w) { return w->y - 1; }

/* z nel compositor BGA: 0..9 riservati ai layer fullscreen stile
 * backbuf (g_backbuf_layer e' z=0). I layer finestra occupano
 * 10..MAX_WINDOWS+9 sommando 10 allo z_order. Cursore a 999. */
static inline int win_layer_z(const window_t *w) { return 10 + w->z_order; }

#endif /* DI_INTERNAL_H */
