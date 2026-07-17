/* MainDOB_Setup — Live-CD installer wizard
 *
 * Auto-launched at the end of Startup_modules in live mode. Walks
 * the user through nine steps and lays down a permanent MainDOB
 * install on a target FAT32 partition.
 *
 *   1. Welcome                  6. Programs
 *   2. Disk selection           7. Summary
 *   3. License (placeholder)    8. Installation
 *   4. Computer parameters      9. Done / reboot
 *   5. Driver (DAS) selection
 *
 * Architecture: every step is a row in STEPS[]. The wizard frame in
 * main() routes events to the current row's handlers; steps own
 * their interactive widgets and stash user choices into the shared
 * `wizard_state_t st`.
 *
 * Widget lifecycle: every focus-aware control is created exactly
 * once at start-up (ui_init_once). Steps toggle each control's
 * `visible` flag in their render() so a control only fires events
 * while its step is active — libdobui's controls all gate OnClick
 * / OnKey on `visible && enabled`, so this is the cheap correct
 * way to give each step a private interaction surface without
 * fighting the singleton focus manager.
 *
 * Skeleton state (build 64): welcome, disk-selection, license, and
 * computer-parameters are real; DAS / programs / summary / install
 * / done share render_placeholder. Each unlocks as the wizard
 * fills in.
 *
 * The installer requires a live boot. On an installed system the
 * program exits immediately — wiring a friendly popup is on the
 * TODO list. */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <app.h>
#include <DobInterface.h>
#include <DobPopup.h>
#include <dob/types.h>
#include <dob/block.h>
#include <dob/partition.h>
#include <dob/spawn.h>
#include <dob/registry.h>
#include <dob/hotplug.h>
#include <dob/ipc.h>
#include <dob/logon.h>          /* record Logon_password.dat (condiviso con dobinterface) */

#include <label.h>
#include <listview.h>
#include <textbox.h>
#include <dropdown.h>
#include <checkbox.h>
#include <button.h>
#include <checked_listview.h>
#include <progressbar.h>
#include <focus.h>

#include "DobFileSystem.h"
#define WIN_W           620
#define WIN_H           460

/* MainDOB blue theme — matches the system desktop. */
#include <dobui_theme.h>
#define COL_BG          DOBUI_INSET
#define COL_TITLE       DOBUI_TEXT
#define COL_BODY        DOBUI_TEXT_ALT
#define COL_HINT        DOBUI_DISABLED
#define COL_WARN        DOBUI_DANGER

#define TITLE_X         24
#define TITLE_Y         24
#define BODY_X          24
#define BODY_Y          76
#define LINE_H          22

/* ===== Wizard steps ===== */

typedef enum
{
    STEP_WELCOME = 0,
    STEP_DISK,
    STEP_LICENSE,
    STEP_PARAMS,
    STEP_DAS,
    STEP_PROGRAMS,
    STEP_SUMMARY,
    STEP_INSTALL,
    STEP_DONE,

    STEP_COUNT
} step_t;

/* ===== Wizard state =====
 *
 * Everything the user has decided so far. The struct accretes one
 * field per UI step and is consumed wholesale by the install phase. */

#define MAX_TARGETS         16

typedef struct
{
    int      disk_index;        /* Valid only within one block_enumerate() cycle */
    int      part_index;        /* 0..3 (MBR primary) */
    uint32_t start_lba;
    uint32_t sectors;
    char     label[80];         /* Pre-formatted listview row */
} setup_target_t;

/* Hardcoded for now: the set of keyboard layouts ships with the live
 * blob (see programs/keymap/ for the .kbl files). Adding a new layout
 * means rebuilding the live anyway, so a static list is honest.
 *
 * KEYMAP_CODES feeds INST_PHASE_CONFIG which writes "US" or "IT" into
 * /SYSTEM/CONFIG/keymap on the target. */
static const char *KEYMAP_NAMES[] = { "Stati Uniti (US)", "Italia (IT)" };
static const char *KEYMAP_CODES[] = { "US",               "IT"          };
#define KEYMAP_COUNT (int)(sizeof(KEYMAP_NAMES) / sizeof(KEYMAP_NAMES[0]))

typedef struct
{
    /* STEP_DISK output. -1 = no selection. */
    int  selected_target;

    /* STEP_PARAMS output. */
    char computer_name[64];     /* Free-form, may be empty */
    char disk_label[12];        /* FAT32 volume label, 11 chars + NUL */
    int  keymap_index;          /* Index into KEYMAP_NAMES / KEYMAP_CODES */
    bool use_exfat_definitive;  /* STEP_PARAMS: register the detected exFAT volume */
    /* Password di accesso, opzionale. Vuota = nessun Logon_password.dat
     * sul target = il sistema installato va dritto al desktop. Piena =
     * il file viene generato in INST_PHASE_CONFIG (hash salato via
     * <dob/logon.h>, MAI la password in chiaro) e dobinterface mostra
     * il logon dal primo avvio. Cap DOB_LOGON_PW_MAX (63) + NUL. */
    char logon_password[DOB_LOGON_PW_MAX + 1];
} wizard_state_t;

static wizard_state_t st =
{
    .selected_target     = -1,
    .keymap_index        = 0,
    .use_exfat_definitive = true,
};

/* ===== DAS (driver) catalogue =====
 *
 * Parsed once on first entry into STEP_DAS from /SYSTEM/CONFIG/DAS/.
 * Each entry is one .das file; the `selected` flag is what the
 * install backend will copy to the target system. `auto_matched`
 * is a v1 stand-in for actual hardware detection: today the QEMU
 * defaults are hardcoded as the auto-match set, and future work
 * (3.6b) will replace this with a real hotplug query. */

#define MAX_DAS              32
#define DAS_NAME_MAX         32
#define DAS_LABEL_MAX        64
#define DAS_CATEGORY_MAX     16

typedef struct
{
    char name[DAS_NAME_MAX];        /* Filename minus .das */
    char label[DAS_LABEL_MAX];      /* From "label = ..." */
    char category[DAS_CATEGORY_MAX];/* From "category = ..." */
    bool selected;                  /* User's current choice */
    bool auto_matched;              /* Pre-selected at parse time */
} das_entry_t;

static das_entry_t das_entries[MAX_DAS];
static int         das_count;
static bool        das_parsed;

/* Display order. .das files outside this list are silently dropped
 * (they have no category or one we don't render). */
#define DAS_CAT_COUNT       5
static const char *DAS_CATEGORY_KEYS[DAS_CAT_COUNT] =
    { "video", "audio", "storage", "removable", "usb" };
static const char *DAS_CATEGORY_LABELS[DAS_CAT_COUNT] =
    { "Video", "Audio", "Storage", "Rimuovibili", "USB" };

/* Two-column layout for DAS step. Left column carries the first three
 * categories, right column the last two. Comfortable inside 460px. */
#define DAS_LEFT_CATS       3

/* ===== Programs catalogue =====
 *
 * Parsed once on first entry into STEP_PROGRAMS by walking
 * /SYSTEM/PROGRAMS/ and /SYSTEM/GAMES/ via the DFS client. Each
 * subdirectory is one selectable entry. `is_game` decides the
 * destination path during install (3.8). Default selection is
 * hardcoded in programs_default_selected(): end-user-facing
 * programs/games go on, dev/test programs go off, the user can
 * toggle freely. */

#define MAX_PROGRAMS         64
#define PROG_NAME_MAX        32

typedef struct
{
    char name[PROG_NAME_MAX];   /* "DobFiles", "snake", ... */
    bool is_game;
    bool selected;
} program_entry_t;

static program_entry_t program_entries[MAX_PROGRAMS];
static int             program_count;
static bool            programs_parsed;

/* ===== Install state machine =====
 *
 * STEP_INSTALL is tick-driven: when the user lands on it from the
 * summary, install_seeded is set and dobui_set_tick fires
 * INSTALL_TICK_MS later. Each tick runs one phase and advances.
 * When phase reaches INST_PHASE_COUNT, the wizard auto-transitions
 * to STEP_DONE.
 *
 * v1 stage: every phase is a debug-print stub. The real I/O
 * (GRUB blobs, file copy via spawned secondary DFS, config writes)
 * lands phase-by-phase in a follow-up turn — replacing each stub
 * one at a time keeps the visible progress UI working throughout.
 *
 * Target service: the secondary DobFileSystem instance spawned in
 * INST_PHASE_VERIFY registers under TARGET_SERVICE; all later
 * phases route their writes through dobfs_*On(TARGET_SERVICE, ...).
 * The numeric id 9999 is high enough to never collide with the
 * partition_fat32.das token format (token = part_index<<24 |
 * native_selector — top byte is partition_index 0..3). */
#define TARGET_ID            9999u
#define TARGET_SERVICE       "dobfs_9999"
/* Root-on-exFAT split: a second secondary mount on the exFAT root volume.
 * The FAT32 partition (dobfs_9999) becomes the boot stub; the bulk of the
 * system is written to the exFAT root (dobfs_9998). */
#define TARGET_ROOT_ID       9998u
#define TARGET_ROOT_SERVICE  "dobfs_9998"
#define DOBFS_MDL_PATH       "/SYSTEM/OperatingSystem/DobFileSystem/DobFileSystem.mdl"
#define TARGET_MOUNT_TIMEOUT_MS  5000

typedef enum
{
    INST_PHASE_VERIFY = 0,
    INST_PHASE_GRUB,
    INST_PHASE_KERNEL,
    INST_PHASE_OS,
    INST_PHASE_DRIVERS,
    INST_PHASE_PROGRAMS,
    INST_PHASE_CONFIG,
    INST_PHASE_BOOTSTUB,
    INST_PHASE_FINALIZE,

    INST_PHASE_COUNT
} install_phase_t;

static const char *INSTALL_PHASE_NAMES[INST_PHASE_COUNT] =
{
    "Verifica del disco di destinazione",
    "Installazione bootloader (GRUB)",
    "Copia del kernel",
    "Copia dei servizi di sistema",
    "Copia dei driver selezionati",
    "Copia di programmi e giochi",
    "Scrittura della configurazione",
    "Stub di avvio (FAT32)",
    "Finalizzazione",
};

#define INSTALL_TICK_MS    800
#define INSTALL_LOG_LINES   6

static bool             install_seeded;
static install_phase_t  install_phase;
static char             install_log[INSTALL_LOG_LINES][96];
static int              install_log_count;
/* Set to true when a phase fails; further ticks become no-ops and
 * the user has to Annulla. (A "retry" affordance is a polish item
 * for after the install backend stabilises.) */
static bool             install_halted;

/* ===== Panel ===== */

#define PANEL_BACK      0
#define PANEL_NEXT      1
#define PANEL_CANCEL    2

/* ===== Step table ===== */

typedef struct
{
    const char *title;
    void      (*render)    (void);
    /* Validation hook for Avanti. Returns false to block the
     * advance. NULL = no validation. */
    bool      (*on_next)   (void);
    bool        has_back;
    bool        has_next;
} step_def_t;

static void render_welcome   (void);
static void render_license   (void);
static void disk_render      (void);
static bool disk_on_next     (void);
static void params_render    (void);
static bool params_on_next   (void);
static void das_render       (void);
static bool das_on_next      (void);
static void programs_render  (void);
static bool programs_on_next (void);
static void summary_render   (void);
static void install_render   (void);
static void install_tick     (void);
static void done_render      (void);
/* redraw() is defined further down (near the event handlers) but
 * install_tick has to call it from earlier in the file. Forward-
 * declare it static so the implicit-extern fallback at the call site
 * doesn't clash with the real static definition. */
static void redraw           (void);

static const step_def_t STEPS[STEP_COUNT] =
{
    [STEP_WELCOME]  = { "Benvenuto",                render_welcome,     NULL,             false, true  },
    [STEP_DISK]     = { "Selezione disco",          disk_render,        disk_on_next,     true,  true  },
    [STEP_LICENSE]  = { "Licenza",                  render_license,     NULL,             true,  true  },
    [STEP_PARAMS]   = { "Parametri",                params_render,      params_on_next,   true,  true  },
    [STEP_DAS]      = { "Driver",                   das_render,         das_on_next,      true,  true  },
    [STEP_PROGRAMS] = { "Programmi",                programs_render,    programs_on_next, true,  true  },
    [STEP_SUMMARY]  = { "Riepilogo",                summary_render,     NULL,             true,  true  },
    [STEP_INSTALL]  = { "Installazione in corso",   install_render,     NULL,           false, false },
    [STEP_DONE]     = { "Completato",               done_render,        NULL,           false, false },
};

static step_t   current_step = STEP_WELCOME;
static uint32_t win_id;
static int      win_w = WIN_W;
static int      win_h = WIN_H;

/* Shared label storage — see render_welcome / render_placeholder for
 * usage. Steps that need additional widgets declare their own
 * statics next to their render fn. */
static dob_label_t lbl_title;
static dob_label_t lbl_body[12];

/* ===== Interactive widgets (lifetime = process) =====
 *
 * Every focus-aware widget is created once and reused for the whole
 * wizard's lifetime. Steps gate them via `visible` so the libdobui
 * controls' click-time check (`if (!visible || !enabled) return false`)
 * keeps clicks from leaking between steps. */

#define DLV_X           24
#define DLV_Y           110
#define DLV_W           572
#define DLV_H           220
#define DLV_HINT_Y      (DLV_Y + DLV_H + 14)
#define DLV_DEFVOL_Y    (DLV_HINT_Y + 24)

#define PARAMS_LABEL_X      BODY_X
#define PARAMS_FIELD_X      BODY_X
/* Pitch uniforme da 66 px (etichetta a -22 dal controllo): sei righe
 * — nome, etichetta disco, layout, password, conferma password, hint
 * — entrano nei 460 px del corpo con margine. */
#define PARAMS_TBNAME_Y     100
#define PARAMS_TBLABEL_Y    166
#define PARAMS_DDKEY_Y      232
#define PARAMS_TBPW1_Y      298
#define PARAMS_TBPW2_Y      364
#define PARAMS_HINT_Y       418
#define PARAMS_TB_W         360
#define PARAMS_TB_DISKLBL_W 200
#define PARAMS_DD_W         240
#define PARAMS_CTRL_H       28

static dob_listview_t lv_targets;
static dob_textbox_t  tb_computer;
static dob_textbox_t  tb_disk_label;
static dob_textbox_t  tb_pw1;              /* password di accesso (mascherata) */
static dob_textbox_t  tb_pw2;              /* conferma (mascherata)            */
static dob_dropdown_t dd_keymap;
static dob_checkbox_t cb_definitive;       /* "use the detected exFAT volume" */
static char           g_exfat_label[96];   /* human description of the detected exFAT */
static bool           g_has_exfat  = false;
static uint32_t       g_exfat_lba  = 0;
static int            g_exfat_part = -1;
static void detect_exfat_definitive(void); /* defined near install_phase_config */
/* Programs/games selection widget: single scrollable checked list
 * (libdobui's dob_checked_listview_t) replacing the previous
 * MAX_PROGRAMS-sized checkbox array. One widget regardless of how
 * many entries get scanned — scales without consuming focus-singleton
 * slots and without the 2-column layout overflow we were heading
 * toward as the program count grows. */
static dob_checked_listview_t lv_programs;
static char        prog_label_buf  [MAX_PROGRAMS][PROG_NAME_MAX + 8];
static const char *prog_items_pool [MAX_PROGRAMS];
static bool        prog_checked_pool[MAX_PROGRAMS];

/* STEP_INSTALL: progress bar tracks completed phases / total.
 * STEP_DONE: a single big "Riavvia ora" button hooks SYS_REBOOT. */
static dob_progressbar_t pb_install;
static dob_button_t      btn_reboot;

/* Per-category DAS controls. Initialised lazily on first das_render
 * via das_ctrls_init_once(). One cell per category: a ListView of
 * currently-selected drivers + a Dropdown of unselected + Aggiungi
 * / Rimuovi buttons. See the big comment block in the DAS step
 * implementation for the layout sketch. */
static dob_listview_t lv_sel_cat   [DAS_CAT_COUNT];
static dob_dropdown_t dd_avail_cat [DAS_CAT_COUNT];
static dob_button_t   btn_add_cat  [DAS_CAT_COUNT];
static dob_button_t   btn_rem_cat  [DAS_CAT_COUNT];

/* Disk-step transient state — rebuilt every render. */
static setup_target_t   targets[MAX_TARGETS];
static int              target_count;
static const char      *target_labels[MAX_TARGETS];

static void
hide_all_controls(void)
{
    lv_targets.visible    = false;
    tb_computer.visible   = false;
    tb_disk_label.visible = false;
    tb_pw1.visible        = false;
    tb_pw2.visible        = false;
    dd_keymap.visible     = false;
    cb_definitive.visible = false;
    /* Per-category DAS controls. The das_ctrls_inited check is
     * unnecessary — uninitialised BSS sets visible=false already. */
    for (int c = 0; c < DAS_CAT_COUNT; c++)
    {
        lv_sel_cat[c].visible   = false;
        dd_avail_cat[c].visible = false;
        btn_add_cat[c].visible  = false;
        btn_rem_cat[c].visible  = false;
    }
    lv_programs.visible = false;
    pb_install.visible  = false;
    btn_reboot.visible  = false;
}

static void
ui_init_once(void)
{
    /* Disk-selection list */
    doblv_Init(&lv_targets, win_id, DLV_X, DLV_Y, DLV_W, DLV_H);

    /* Computer-parameters controls */
    dobtb_Init(&tb_computer,   win_id,
               PARAMS_FIELD_X, PARAMS_TBNAME_Y,
               PARAMS_TB_W,    PARAMS_CTRL_H);
    dobtb_Init(&tb_disk_label, win_id,
               PARAMS_FIELD_X, PARAMS_TBLABEL_Y,
               PARAMS_TB_DISKLBL_W, PARAMS_CTRL_H);
    /* Password di accesso: doppia casella MASCHERATA — l'eco a pallini
     * evita sia l'occhio indiscreto sia il typo invisibile (la doppia
     * battitura e' proprio la difesa dal typo). */
    dobtb_Init(&tb_pw1, win_id,
               PARAMS_FIELD_X, PARAMS_TBPW1_Y,
               PARAMS_TB_W,    PARAMS_CTRL_H);
    tb_pw1.masked = true;
    dobtb_Init(&tb_pw2, win_id,
               PARAMS_FIELD_X, PARAMS_TBPW2_Y,
               PARAMS_TB_W,    PARAMS_CTRL_H);
    tb_pw2.masked = true;
    dobdd_Init(&dd_keymap,     win_id,
               PARAMS_FIELD_X, PARAMS_DDKEY_Y,
               PARAMS_DD_W,    PARAMS_CTRL_H,
               KEYMAP_NAMES,   KEYMAP_COUNT);
    /* Match window bg so the dropdown's popup-close ghost wipe paints
     * the correct background instead of leaving a white rectangle. */
    dd_keymap.col_clear = COL_BG;

    dobcb_Init(&cb_definitive, win_id,
               DLV_X, DLV_DEFVOL_Y, 0,
               "Usa la partizione exFAT come volume definitivo");
    dobcb_SetChecked(&cb_definitive, true);

    /* Programs/games checked listview. Position/size rewritten every
     * render but Init has to fire here so the focus singleton picks
     * it up and arrow keys / space work as soon as STEP_PROGRAMS is
     * focused. */
    dobclv_Init(&lv_programs, win_id, BODY_X, BODY_Y, WIN_W - BODY_X * 2, 280);

    /* STEP_INSTALL: progress bar fills most of the body, position
     * fixed up at render. show_text gives us the "N%" readout on
     * the right of the fill — useful even though we also draw the
     * "Fase N/8" line above it. */
    dobpb_Init(&pb_install, win_id,
               BODY_X, BODY_Y + 120, WIN_W - BODY_X * 2, 28);
    pb_install.show_text   = true;
    pb_install.show_border = true;
    pb_install.col_text_bg = COL_BG;
    pb_install.max         = INST_PHASE_COUNT;

    /* STEP_DONE: reboot button — single big call to action. Position
     * fixed up at done_render so the centering stays correct if
     * window dimensions ever shift. */
    dobbtn_Init(&btn_reboot, win_id,
                (WIN_W - 220) / 2, BODY_Y + 160, 220, 40, "Riavvia ora");

    hide_all_controls();
}

/* ===== Helpers ===== */

static bool
in_live_mode(void)
{
    return syscall0(SYS_LIVE_QUERY) > 0;
}

static void
put_line(int idx, int y, const char *text, uint32_t col)
{
    doblbl_InitWithBg(&lbl_body[idx], win_id, BODY_X, y, text, col, COL_BG);
    doblbl_Draw(&lbl_body[idx]);
}

static void
put_title(const char *text)
{
    doblbl_InitWithBg(&lbl_title, win_id, TITLE_X, TITLE_Y, text, COL_TITLE, COL_BG);
    doblbl_Draw(&lbl_title);
}

/* ===== Welcome step ===== */

static void
render_welcome(void)
{
    hide_all_controls();
    put_title("Benvenuto in MainDOB");

    int y = BODY_Y;
    put_line(0,  y, "MainDOB è un sistema operativo a microkernel sviluppato",   COL_BODY); y += LINE_H;
    put_line(1,  y, "da Dob Systems & Technologies. Questo programma di setup",  COL_BODY); y += LINE_H;
    put_line(2,  y, "ti guiderà attraverso l'installazione su un disco rigido.", COL_BODY); y += LINE_H * 2;

    put_line(3,  y, "Prima di procedere, assicurati di avere una partizione",    COL_BODY); y += LINE_H;
    put_line(4,  y, "FAT32 già preparata sul disco di destinazione. Se serve,",  COL_BODY); y += LINE_H;
    put_line(5,  y, "chiudi questa finestra e usa DobDisk per crearla, poi",     COL_BODY); y += LINE_H;
    put_line(6,  y, "riavvia il setup da /SYSTEM/PROGRAMS/MainDOB_Setup.",       COL_BODY); y += LINE_H * 2;

    put_line(7,  y, "L'installazione sovrascriverà il contenuto della",          COL_HINT); y += LINE_H;
    put_line(8,  y, "partizione scelta. Avrai un riepilogo finale di",           COL_HINT); y += LINE_H;
    put_line(9,  y, "conferma prima che venga toccato un solo settore.",         COL_HINT); y += LINE_H * 2;

    put_line(10, y, "Premi Avanti per iniziare.",                                COL_TITLE);
}

/* ===== Disk-selection step ===== */

static void
enumerate_targets(void)
{
    target_count = 0;

    int n = block_enumerate();
    for (int i = 0; i < n && target_count < MAX_TARGETS; i++)
    {
        const block_disk_t *d = block_get(i);
        if (!d) continue;

        uint8_t mbr[BLOCK_SECTOR_SIZE];
        if (!block_read(i, 0, 1, mbr)) continue;

        mbr_table_t tbl;
        partition_mbr_parse(mbr, &tbl);
        if (!tbl.valid_signature) continue;

        for (int p = 0; p < MBR_MAX_PRIMARY && target_count < MAX_TARGETS; p++)
        {
            const mbr_partition_t *e = &tbl.entries[p];
            if (e->sectors == 0) continue;
            if (!partition_type_is_fat32(e->type)) continue;

            setup_target_t *t = &targets[target_count];
            t->disk_index = i;
            t->part_index = p;
            t->start_lba  = e->start_lba;
            t->sectors    = e->sectors;

            uint64_t mb = ((uint64_t)e->sectors * BLOCK_SECTOR_SIZE) / (1024u * 1024u);
            snprintf(t->label, sizeof(t->label),
                     "Disco %d, partizione %d  -  FAT32  -  %u MB",
                     i + 1, p + 1, (unsigned)mb);

            target_labels[target_count] = t->label;
            target_count++;
        }
    }
}

static void
disk_render(void)
{
    hide_all_controls();
    put_title("Selezione disco");

    /* doblv_SetItems() clears `selected` and `scroll` — both have to
     * survive across redraws or the listview visually deselects every
     * click (event_mouseclick → redraw → SetItems → -1). Save before,
     * restore after if still valid. */
    int saved_sel    = lv_targets.selected;
    int saved_scroll = lv_targets.scroll;

    enumerate_targets();

    if (target_count == 0)
    {
        st.selected_target = -1;
        put_line(0, BODY_Y,                 "Nessuna partizione FAT32 trovata sui dischi disponibili.", COL_WARN);
        put_line(1, BODY_Y + LINE_H * 2,    "Apri DobDisk dal desktop, crea una partizione FAT32 sul",   COL_BODY);
        put_line(2, BODY_Y + LINE_H * 3,    "disco di destinazione, poi torna qui premendo Indietro",    COL_BODY);
        put_line(3, BODY_Y + LINE_H * 4,    "e poi Avanti per riprovare la ricerca.",                    COL_BODY);
        return;
    }

    put_line(0, BODY_Y, "Scegli la partizione FAT32 dove installare MainDOB:", COL_BODY);

    lv_targets.visible = true;
    doblv_SetItems(&lv_targets, target_labels, target_count);
    if (saved_sel >= 0 && saved_sel < target_count)
        lv_targets.selected = saved_sel;
    lv_targets.scroll = saved_scroll;

    /* Mirror to wizard state so disk_on_next sees the same row. */
    st.selected_target = lv_targets.selected;

    doblv_Draw(&lv_targets);

    put_line(1, DLV_HINT_Y,
             "L'installazione cancellerà tutti i dati sulla partizione scelta.",
             COL_HINT);

    /* exFAT definitive-volume offer for the SELECTED disk. A separate large
     * exFAT partition (the only option for disks > 120 GB) is attached at
     * boot as the definitive data volume. The box names the partition found;
     * leaving it unchecked installs FAT32-only. Detection keys off the row
     * just mirrored into st.selected_target. The checkbox keeps its own
     * checked state across redraws (set once at init), so a manual toggle
     * survives reselecting rows. */
    detect_exfat_definitive();
    if (g_has_exfat)
    {
        static char cbtxt[128];
        snprintf(cbtxt, sizeof(cbtxt),
                 "Usa come volume definitivo: %s", g_exfat_label);
        dobcb_SetText(&cb_definitive, cbtxt);
        cb_definitive.visible = true;
        dobcb_Draw(&cb_definitive);
    }
    else
    {
        put_line(2, DLV_DEFVOL_Y,
                 "Nessuna partizione exFAT trovata su questo disco.", COL_HINT);
    }
}

static bool
disk_on_next(void)
{
    if (target_count == 0) return false;
    if (st.selected_target < 0 || st.selected_target >= target_count) return false;
    /* g_has_exfat / cb_definitive reflect the row shown by disk_render. */
    st.use_exfat_definitive = g_has_exfat && cb_definitive.checked;
    return true;
}

/* ===== License step =====
 *
 * Deliberately empty body. Per the wizard spec the license window
 * is a ceremonial gate: title at the top, panel below, no body
 * content. If real text ever lands here it slots straight in. */

static void
render_license(void)
{
    hide_all_controls();
    put_title("Licenza");
}

/* ===== Computer-parameters step =====
 *
 * Three controls: computer name (free text), FAT32 volume label
 * (11 chars max — the textbox doesn't enforce this; params_on_next
 * truncates at save time), default keymap (dropdown over the
 * hardcoded KEYMAP_NAMES list).
 *
 * Widget content is synced from `st` exactly once, the first time
 * the step renders, then the widgets are the source of truth until
 * params_on_next() mirrors them back. Re-applying state every
 * render would clobber the user's typing as soon as the next event
 * triggers a redraw — exactly the bug pattern the listview's
 * SetItems call exposes.
 *
 * Note for users coming from the disk step: a focused textbox owns
 * the right-side panel (clipboard commands replace Indietro/Avanti/
 * Annulla). Click on the window background to defocus and get the
 * wizard navigation back. */

static bool params_seeded;

static void
params_render(void)
{
    hide_all_controls();
    put_title("Parametri sistema");

    if (!params_seeded)
    {
        params_seeded = true;
        dobtb_SetText  (&tb_computer,   st.computer_name);
        dobtb_SetText  (&tb_disk_label, st.disk_label);
        dobtb_SetText  (&tb_pw1,        st.logon_password);
        dobtb_SetText  (&tb_pw2,        st.logon_password);
        dobdd_SetSelected(&dd_keymap,   st.keymap_index);
    }

    /* Computer name */
    put_line(0, PARAMS_TBNAME_Y - 22, "Nome computer:", COL_BODY);
    tb_computer.visible = true;
    dobtb_Draw(&tb_computer);

    /* Disk label */
    put_line(1, PARAMS_TBLABEL_Y - 22, "Etichetta disco (max 11 caratteri, FAT32):", COL_BODY);
    tb_disk_label.visible = true;
    dobtb_Draw(&tb_disk_label);

    /* Keymap */
    put_line(2, PARAMS_DDKEY_Y - 22, "Layout tastiera:", COL_BODY);
    dd_keymap.visible = true;
    dobdd_Draw(&dd_keymap);

    /* Password di accesso (opzionale) — doppia battitura anti-typo.
     * Vuota = nessuna schermata di accesso sul sistema installato. */
    put_line(4, PARAMS_TBPW1_Y - 22,
             "Password di accesso (opzionale, vuota = nessuna):", COL_BODY);
    tb_pw1.visible = true;
    dobtb_Draw(&tb_pw1);

    put_line(5, PARAMS_TBPW2_Y - 22, "Conferma password:", COL_BODY);
    tb_pw2.visible = true;
    dobtb_Draw(&tb_pw2);

    put_line(3, PARAMS_HINT_Y, "Puoi cambiare questi valori dopo l'installazione.", COL_HINT);

    /* Popup overlays MUST flush last so they sit on top of the
     * labels and other controls. */
    dobdd_FlushPopup(&dd_keymap);
}

static bool
params_on_next(void)
{
    const char *cn = dobtb_GetText(&tb_computer);
    const char *dl = dobtb_GetText(&tb_disk_label);

    if (cn)
    {
        strncpy(st.computer_name, cn, sizeof(st.computer_name) - 1);
        st.computer_name[sizeof(st.computer_name) - 1] = '\0';
    }
    if (dl)
    {
        /* FAT32 volume labels are capped at 11 bytes by the spec;
         * truncate silently rather than reject. */
        strncpy(st.disk_label, dl, sizeof(st.disk_label) - 1);
        st.disk_label[sizeof(st.disk_label) - 1] = '\0';
    }
    st.keymap_index = dd_keymap.selected;
    if (st.keymap_index < 0 || st.keymap_index >= KEYMAP_COUNT)
        st.keymap_index = 0;

    /* Password di accesso: le due caselle DEVONO coincidere — e' la
     * ragione stessa della doppia battitura. Vuote entrambe = nessuna
     * password (legittimo). Oltre DOB_LOGON_PW_MAX si tronca in
     * silenzio, coerente con la tenda di dobinterface. */
    {
        const char *p1 = dobtb_GetText(&tb_pw1);
        const char *p2 = dobtb_GetText(&tb_pw2);
        if (!p1) p1 = "";
        if (!p2) p2 = "";
        if (strcmp(p1, p2) != 0)
        {
            dobpopup_Error("Parametri sistema",
                           "Le due password non coincidono.\n"
                           "Ribattile (o lasciale entrambe vuote).");
            return false;
        }
        strncpy(st.logon_password, p1, sizeof(st.logon_password) - 1);
        st.logon_password[sizeof(st.logon_password) - 1] = '\0';
    }

    /* Empty name is allowed — the installer will fall back to
     * "MainDOB" in step 8 when persisting. No hard validation here. */
    return true;
}

/* ===== Driver (DAS) selection step — listbox + dropdown + add/remove =====
 *
 * Two-column layout, one category cell per row:
 *
 *   ┌──────────────────────────┐  ┌──────────────────────────┐
 *   │ Video                    │  │ Rimuovibili              │
 *   │ ┌──────────────────────┐ │  │ ┌──────────────────────┐ │
 *   │ │ [A] bga              │ │  │ │ [A] cdrom_ahci       │ │
 *   │ │     mach64_ltpro     │ │  │ │                      │ │
 *   │ └──────────────────────┘ │  │ └──────────────────────┘ │
 *   │ [mach64_mobility ▼] [+]──┤  │ [cdrom_ide       ▼] [+]──┤
 *   │                      [-] │  │                      [-] │
 *   ├──────────────────────────┤  ├──────────────────────────┤
 *   │ Audio                    │  │ USB                      │
 *     ...                          ...
 *
 * Left column = video / audio / storage. Right = removable / usb.
 *
 * Per cell:
 *   - Header label
 *   - ListView showing currently-selected drivers ([A] prefix if
 *     auto-matched, blank prefix otherwise)
 *   - Dropdown of unselected drivers in the same category
 *   - Aggiungi (+) button : moves dropdown's choice into the listview
 *   - Rimuovi  (-) button : drops the listview's selection back into
 *                           the dropdown — disabled if the row is
 *                           auto-matched (locked)
 *
 * das_entries[].selected is the single source of truth. Per-render
 * views (cat_sel_*, cat_avail_*) are rebuilt every frame from it. */

#define DAS_MAX_PER_CAT      8
#define DAS_CELL_W         280
#define DAS_LV_H            60
#define DAS_DD_H            24
#define DAS_DD_W           150
#define DAS_BTN_W           54
#define DAS_BTN_GAP          8
#define DAS_HEADER_H        20
#define DAS_CELL_GAP         6
#define DAS_LEFT_X          24
#define DAS_RIGHT_X        316
#define DAS_TOP_Y           62

/* Per-render views — rebuilt each draw from das_entries[]. The
 * label backing store has to outlive the call so the ListView /
 * Dropdown have stable pointers to dereference. */
static char        cat_sel_buf   [DAS_CAT_COUNT][DAS_MAX_PER_CAT][80];
static const char *cat_sel_items [DAS_CAT_COUNT][DAS_MAX_PER_CAT];
static int         cat_sel_idx   [DAS_CAT_COUNT][DAS_MAX_PER_CAT];
static int         cat_sel_count [DAS_CAT_COUNT];

static char        cat_avail_buf   [DAS_CAT_COUNT][DAS_MAX_PER_CAT][80];
static const char *cat_avail_items [DAS_CAT_COUNT][DAS_MAX_PER_CAT];
static int         cat_avail_idx   [DAS_CAT_COUNT][DAS_MAX_PER_CAT];
static int         cat_avail_count [DAS_CAT_COUNT];

static bool das_ctrls_inited;

/* Auto-match: query hotplug for the DAS entries it already matched
 * against live hardware (PCI vendor/device, class fallbacks, legacy
 * bubbles for floppy/CDROM). Cached for the session. Failing softly
 * leaves zero auto-matches — the user can still tick by hand. */

#define DAS_AUTO_MATCH_MAX  32
static char        das_auto_match_names[DAS_AUTO_MATCH_MAX][32];
static int         das_auto_match_count = -1;     /* -1 = uninitialised */

static void
das_auto_match_load(void)
{
    if (das_auto_match_count >= 0) return;
    das_auto_match_count = 0;

    uint32_t port = dob_registry_find("hotplug");
    if (!port) return;   /* hotplug not up yet — fall through, zero auto-matches */

    dob_msg_t msg = {0}, reply = {0};
    msg.code = HOTPLUG_LIST;
    if (dob_ipc_call(port, &msg, &reply) != DOB_OK) return;
    if (!reply.payload || reply.payload_size == 0)  return;

    uint32_t n = reply.arg0;
    const hotplug_bubble_info_t *infos =
        (const hotplug_bubble_info_t *)reply.payload;
    uint32_t bytes_per = sizeof(hotplug_bubble_info_t);
    uint32_t safe_n = reply.payload_size / bytes_per;
    if (n > safe_n) n = safe_n;

    for (uint32_t i = 0; i < n && das_auto_match_count < DAS_AUTO_MATCH_MAX; i++)
    {
        if (!infos[i].das_name[0]) continue;
        /* dedup: cdrom_ide AND cdrom_ahci will both appear if both
         * bus types are present; we want each DAS to show up once. */
        bool already = false;
        for (int k = 0; k < das_auto_match_count; k++)
            if (strcmp(das_auto_match_names[k], infos[i].das_name) == 0)
            {
                already = true; break;
            }
        if (already) continue;
        strncpy(das_auto_match_names[das_auto_match_count],
                infos[i].das_name, 31);
        das_auto_match_names[das_auto_match_count][31] = '\0';
        das_auto_match_count++;
    }
}

static bool
das_is_default_match(const char *name)
{
    das_auto_match_load();
    for (int i = 0; i < das_auto_match_count; i++)
        if (strcmp(name, das_auto_match_names[i]) == 0) return true;
    return false;
}

static void
das_kv_extract(const char *line, const char *key, char *out, int out_max)
{
    size_t kl = strlen(key);
    if (strncmp(line, key, kl) != 0) return;
    const char *p = line + kl;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') return;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    int n = 0;
    while (*p && *p != '\n' && *p != '\r' && n < out_max - 1)
        out[n++] = *p++;
    while (n > 0 && (out[n-1] == ' ' || out[n-1] == '\t')) n--;
    out[n] = '\0';
}

static void
das_parse(void)
{
    if (das_parsed) return;
    das_parsed = true;
    das_count  = 0;

    dobfs_dirent_t entries[MAX_DAS];
    uint32_t       count = 0;
    if (dobfs_List("/SYSTEM/CONFIG/DAS", entries, MAX_DAS, &count) != 0)
        return;

    for (uint32_t i = 0; i < count && das_count < MAX_DAS; i++)
    {
        if (entries[i].type != FS_TYPE_FILE) continue;

        size_t nl = strlen(entries[i].name);
        if (nl < 5) continue;
        if (strcmp(entries[i].name + nl - 4, ".das") != 0) continue;

        das_entry_t *de = &das_entries[das_count];

        size_t bn = nl - 4;
        if (bn >= DAS_NAME_MAX) bn = DAS_NAME_MAX - 1;
        memcpy(de->name, entries[i].name, bn);
        de->name[bn] = '\0';
        de->label[0]    = '\0';
        de->category[0] = '\0';

        char path[160];
        snprintf(path, sizeof(path), "/SYSTEM/CONFIG/DAS/%s", entries[i].name);
        int fd = dobfs_Open(path, FS_READ);
        if (fd < 0) continue;

        /* Read full file (chunked, 16 KB cap) and stop early when
         * both label and category are found. Most .das files have a
         * 48×48 ASCII bitmap between the header fields and the
         * category line — a fixed 2 KB read would truncate inside
         * the bitmap and miss the category, silently dropping the
         * entry from the wizard. */
        static char buf[16384];
        int total = 0;
        bool got_what_we_need = false;
        while (total < (int)sizeof(buf) - 1)
        {
            int n = dobfs_Read(fd, buf + total, sizeof(buf) - 1 - total);
            if (n <= 0) break;
            total += n;
            buf[total] = '\0';

            de->label[0]    = '\0';
            de->category[0] = '\0';
            char *p = buf;
            while (p && *p)
            {
                char *eol = strchr(p, '\n');
                if (eol) *eol = '\0';
                char *line = p;
                while (*line == ' ' || *line == '\t') line++;
                if (*line && *line != '#')
                {
                    das_kv_extract(line, "label",    de->label,    sizeof(de->label));
                    das_kv_extract(line, "category", de->category, sizeof(de->category));
                }
                if (eol) *eol = '\n';
                p = eol ? eol + 1 : NULL;
            }
            if (de->label[0] && de->category[0]) { got_what_we_need = true; break; }
        }
        dobfs_Close(fd);
        if (!got_what_we_need && (total <= 0)) continue;

        if (!de->category[0]) continue;

        if (!de->label[0])
        {
            strncpy(de->label, de->name, sizeof(de->label) - 1);
            de->label[sizeof(de->label) - 1] = '\0';
        }

        de->auto_matched = das_is_default_match(de->name);
        /* Default everything to selected so the install ships full
         * hardware coverage out of the box. Auto-matched entries
         * stay locked (the dropdown's remove button is grayed); the
         * rest are pre-checked but the user can untick them. */
        de->selected     = true;

        das_count++;
    }
}

static int
das_find_cat_index(const char *cat)
{
    for (int c = 0; c < DAS_CAT_COUNT; c++)
        if (strcmp(DAS_CATEGORY_KEYS[c], cat) == 0) return c;
    return -1;
}

static void
das_rebuild_views(void)
{
    for (int c = 0; c < DAS_CAT_COUNT; c++)
    {
        cat_sel_count[c]   = 0;
        cat_avail_count[c] = 0;
    }
    for (int i = 0; i < das_count; i++)
    {
        int c = das_find_cat_index(das_entries[i].category);
        if (c < 0) continue;

        if (das_entries[i].selected)
        {
            if (cat_sel_count[c] >= DAS_MAX_PER_CAT) continue;
            int s = cat_sel_count[c];
            snprintf(cat_sel_buf[c][s], 80, "%s%s",
                     das_entries[i].auto_matched ? "[A] " : "    ",
                     das_entries[i].name);
            cat_sel_items[c][s] = cat_sel_buf[c][s];
            cat_sel_idx  [c][s] = i;
            cat_sel_count[c]++;
        }
        else
        {
            if (cat_avail_count[c] >= DAS_MAX_PER_CAT) continue;
            int a = cat_avail_count[c];
            snprintf(cat_avail_buf[c][a], 80, "%s", das_entries[i].name);
            cat_avail_items[c][a] = cat_avail_buf[c][a];
            cat_avail_idx  [c][a] = i;
            cat_avail_count[c]++;
        }
    }
}

static void
das_ctrls_init_once(void)
{
    if (das_ctrls_inited) return;
    das_ctrls_inited = true;
    for (int c = 0; c < DAS_CAT_COUNT; c++)
    {
        /* Position placeholders — rewritten every render. Items
         * pointers / counts also overwritten every render. */
        doblv_Init(&lv_sel_cat[c],   win_id, 0, 0, DAS_CELL_W, DAS_LV_H);
        dobdd_Init(&dd_avail_cat[c], win_id, 0, 0, DAS_DD_W,   DAS_DD_H, NULL, 0);
        dd_avail_cat[c].col_clear = COL_BG;
        dobbtn_Init(&btn_add_cat[c], win_id, 0, 0, DAS_BTN_W, DAS_DD_H, "Agg.");
        dobbtn_Init(&btn_rem_cat[c], win_id, 0, 0, DAS_BTN_W, DAS_DD_H, "Rim.");

        lv_sel_cat[c].visible   = false;
        dd_avail_cat[c].visible = false;
        btn_add_cat[c].visible  = false;
        btn_rem_cat[c].visible  = false;
    }
}

static void
das_render(void)
{
    hide_all_controls();
    put_title("Driver di sistema");

    das_parse();
    das_ctrls_init_once();

    if (das_count == 0)
    {
        put_line(0, BODY_Y,
                 "Nessun driver disponibile (lettura /SYSTEM/CONFIG/DAS fallita).",
                 COL_WARN);
        return;
    }

    das_rebuild_views();

    /* Header labels — one per category, drawn once per render. */
    static dob_label_t hdr[DAS_CAT_COUNT];

    int col_left_y  = DAS_TOP_Y;
    int col_right_y = DAS_TOP_Y;

    for (int c = 0; c < DAS_CAT_COUNT; c++)
    {
        bool is_left = (c < DAS_LEFT_CATS);
        int  cell_x  = is_left ? DAS_LEFT_X    : DAS_RIGHT_X;
        int *yp      = is_left ? &col_left_y   : &col_right_y;
        int  y       = *yp;

        /* Header */
        doblbl_InitWithBg(&hdr[c], win_id, cell_x, y,
                          DAS_CATEGORY_LABELS[c], COL_TITLE, COL_BG);
        doblbl_Draw(&hdr[c]);
        y += DAS_HEADER_H;

        /* ListView of selected drivers */
        lv_sel_cat[c].x       = cell_x;
        lv_sel_cat[c].y       = y;
        lv_sel_cat[c].w       = DAS_CELL_W;
        lv_sel_cat[c].h       = DAS_LV_H;
        lv_sel_cat[c].visible = true;
        /* SetItems wipes selected/scroll on every call; preserve both
         * so the listview doesn't visually deselect under the user's
         * click (event → redraw → SetItems → -1). */
        int sav_sel    = lv_sel_cat[c].selected;
        int sav_scroll = lv_sel_cat[c].scroll;
        doblv_SetItems(&lv_sel_cat[c], cat_sel_items[c], cat_sel_count[c]);
        if (sav_sel >= 0 && sav_sel < cat_sel_count[c])
            lv_sel_cat[c].selected = sav_sel;
        lv_sel_cat[c].scroll = sav_scroll;
        doblv_Draw(&lv_sel_cat[c]);
        y += DAS_LV_H + DAS_CELL_GAP;

        /* Dropdown of available + add/remove buttons on one row */
        dd_avail_cat[c].x       = cell_x;
        dd_avail_cat[c].y       = y;
        dd_avail_cat[c].w       = DAS_DD_W;
        dd_avail_cat[c].h       = DAS_DD_H;
        dd_avail_cat[c].visible = true;
        dd_avail_cat[c].items   = cat_avail_items[c];
        dd_avail_cat[c].count   = cat_avail_count[c];
        if (dd_avail_cat[c].selected < 0 ||
            dd_avail_cat[c].selected >= cat_avail_count[c])
            dd_avail_cat[c].selected = 0;
        dobdd_Draw(&dd_avail_cat[c]);

        btn_add_cat[c].x       = cell_x + DAS_DD_W + DAS_BTN_GAP;
        btn_add_cat[c].y       = y;
        btn_add_cat[c].w       = DAS_BTN_W;
        btn_add_cat[c].h       = DAS_DD_H;
        btn_add_cat[c].visible = true;
        dobbtn_SetEnabled(&btn_add_cat[c], cat_avail_count[c] > 0);
        dobbtn_Draw(&btn_add_cat[c]);

        btn_rem_cat[c].x       = cell_x + DAS_DD_W + DAS_BTN_GAP*2 + DAS_BTN_W;
        btn_rem_cat[c].y       = y;
        btn_rem_cat[c].w       = DAS_BTN_W;
        btn_rem_cat[c].h       = DAS_DD_H;
        btn_rem_cat[c].visible = true;
        /* Disabled when no listview row chosen, or chosen row is
         * auto-matched (locked). */
        int sel = lv_sel_cat[c].selected;
        bool can_rem = (sel >= 0 && sel < cat_sel_count[c] &&
                        !das_entries[cat_sel_idx[c][sel]].auto_matched);
        dobbtn_SetEnabled(&btn_rem_cat[c], can_rem);
        dobbtn_Draw(&btn_rem_cat[c]);

        y += DAS_DD_H + DAS_CELL_GAP;
        *yp = y;
    }

    /* All dropdown popups flushed on top after every other widget. */
    for (int c = 0; c < DAS_CAT_COUNT; c++)
        dobdd_FlushPopup(&dd_avail_cat[c]);
}

/* Called from event_mouseclick when STEP_DAS is active, with whatever
 * pointer the focus singleton returned for the consumed click. */
static void
das_handle_button_click(void *clicked)
{
    if (!clicked) return;
    for (int c = 0; c < DAS_CAT_COUNT; c++)
    {
        if (clicked == &btn_add_cat[c])
        {
            int s = dd_avail_cat[c].selected;
            if (s >= 0 && s < cat_avail_count[c])
                das_entries[cat_avail_idx[c][s]].selected = true;
            return;
        }
        if (clicked == &btn_rem_cat[c])
        {
            int s = lv_sel_cat[c].selected;
            if (s >= 0 && s < cat_sel_count[c])
            {
                int e = cat_sel_idx[c][s];
                if (!das_entries[e].auto_matched)
                    das_entries[e].selected = false;
            }
            return;
        }
    }
}

static bool
das_on_next(void)
{
    /* das_entries[].selected is the single source of truth and is
     * updated live by the add/remove handlers. Nothing to mirror. */
    return true;
}

/* ===== Programs / games selection step =====
 *
 * Walks /SYSTEM/PROGRAMS and /SYSTEM/GAMES via DFS at first entry,
 * one entry per subdirectory. Renders all of them as a single
 * scrollable dob_checked_listview_t — one widget regardless of
 * count, which solves both the slot-budget pressure of the prior
 * cb_programs[] design and the layout overflow that two columns of
 * fixed checkboxes would have hit at ~30+ entries.
 *
 * Label format: "    DobFiles" for programs, "[G] snake" for games,
 * stable across redraws via prog_label_buf[]. The widget reads
 * prog_items_pool[] (string pointers) and prog_checked_pool[]
 * (per-row state) — both caller-owned, lifetime = the process.
 *
 * Default selection: end-user-facing programs and all games go on;
 * dev/test programs (uidemo, widget_test, benchmark, modules,
 * demosettings, valueParser, videotest, boomtest) go off. The user
 * is free to override either way.
 *
 * The install backend in 3.8/3.9 reads program_entries[].selected
 * (synced from prog_checked_pool by programs_on_next). is_game
 * decides whether each chosen entry copies to /SYSTEM/PROGRAMS or
 * /SYSTEM/GAMES on the target. */

static void
programs_scan_dir(const char *path, bool is_game)
{
    dobfs_dirent_t entries[MAX_PROGRAMS];
    uint32_t       count = 0;
    if (dobfs_List(path, entries, MAX_PROGRAMS, &count) != 0) return;

    for (uint32_t i = 0; i < count && program_count < MAX_PROGRAMS; i++)
    {
        if (entries[i].type != FS_TYPE_DIR) continue;
        if (entries[i].name[0] == '.') continue;     /* skip . / .. */

        program_entry_t *pe = &program_entries[program_count];
        strncpy(pe->name, entries[i].name, sizeof(pe->name) - 1);
        pe->name[sizeof(pe->name) - 1] = '\0';
        pe->is_game  = is_game;
        pe->selected = true;   /* all programs ship by default; user can untick */

        /* Pre-format the visible label once. Games get a [G] tag so
         * the install destination is visible at a glance from the
         * UI; programs get 4 spaces of padding for visual alignment. */
        snprintf(prog_label_buf[program_count], sizeof(prog_label_buf[0]),
                 "%s%s",
                 is_game ? "[G] " : "    ",
                 pe->name);
        prog_items_pool[program_count]   = prog_label_buf[program_count];
        prog_checked_pool[program_count] = pe->selected;

        program_count++;
    }
}

static void
programs_parse(void)
{
    if (programs_parsed) return;
    programs_parsed = true;
    program_count   = 0;
    programs_scan_dir("/SYSTEM/PROGRAMS", false);
    programs_scan_dir("/SYSTEM/GAMES",    true);
}

static void
programs_render(void)
{
    hide_all_controls();
    put_title("Programmi e giochi");

    programs_parse();

    if (program_count == 0)
    {
        put_line(0, BODY_Y,
                 "Nessun programma trovato in /SYSTEM/PROGRAMS o /SYSTEM/GAMES.",
                 COL_WARN);
        return;
    }

    /* Widget fills most of the body, leaving a hint line at the
     * bottom. The window content area is ~380px below the title;
     * we reserve 30px for the hint + 8px gap. */
    lv_programs.x       = BODY_X;
    lv_programs.y       = BODY_Y - 8;
    lv_programs.w       = WIN_W - BODY_X * 2;
    lv_programs.h       = WIN_H - lv_programs.y - 50;
    lv_programs.visible = true;
    lv_programs.col_bg       = DOBUI_INSET;
    lv_programs.col_item_bg  = DOBUI_INSET;
    /* Bring focus on entry so Space/Arrows just work without a click. */
    if (!lv_programs.focused) dobfocus_set_focus(&lv_programs);

    dobclv_SetItems(&lv_programs, prog_items_pool, prog_checked_pool, program_count);
    dobclv_Draw(&lv_programs);

    put_line(0, lv_programs.y + lv_programs.h + 10,
             "Click o Spazio per spuntare. I [G] sono giochi e vanno in /SYSTEM/GAMES.",
             COL_HINT);
}

static bool
programs_on_next(void)
{
    /* Mirror per-row checked state back into program_entries[] so
     * the install step in 3.8 sees a single source of truth. */
    for (int i = 0; i < program_count; i++)
        program_entries[i].selected = prog_checked_pool[i];
    return true;
}

/* ===== Summary step =====
 *
 * Read-only recap of every choice. The user's Avanti from here lands
 * on STEP_INSTALL — which today is still a placeholder. Once 3.8 ships
 * the install backend, this becomes the point of no return: a confirm
 * popup will guard the click. For the skeleton, Avanti just advances
 * to the placeholder install screen. */

static int
count_selected_das(void)
{
    int n = 0;
    for (int i = 0; i < das_count; i++) if (das_entries[i].selected) n++;
    return n;
}

static int
count_selected_programs(void)
{
    int n = 0;
    for (int i = 0; i < program_count; i++) if (program_entries[i].selected) n++;
    return n;
}

static void
summary_render(void)
{
    hide_all_controls();
    put_title("Riepilogo installazione");

    char buf[160];
    int  y = BODY_Y;

    /* Target disk */
    if (st.selected_target >= 0 && st.selected_target < target_count)
    {
        setup_target_t *t = &targets[st.selected_target];
        snprintf(buf, sizeof(buf), "Destinazione: %s", t->label);
        put_line(0, y, buf, COL_BODY);
    }
    else
    {
        put_line(0, y, "Destinazione: (non selezionata)", COL_WARN);
    }
    y += LINE_H * 2;

    /* Computer parameters */
    snprintf(buf, sizeof(buf), "Nome computer:    %s",
             st.computer_name[0] ? st.computer_name : "(predefinito)");
    put_line(1, y, buf, COL_BODY); y += LINE_H;

    snprintf(buf, sizeof(buf), "Etichetta disco:  %s",
             st.disk_label[0] ? st.disk_label : "MAINDOB");
    put_line(2, y, buf, COL_BODY); y += LINE_H;

    int ki = st.keymap_index;
    if (ki < 0 || ki >= KEYMAP_COUNT) ki = 0;
    snprintf(buf, sizeof(buf), "Layout tastiera:  %s", KEYMAP_NAMES[ki]);
    put_line(3, y, buf, COL_BODY); y += LINE_H * 2;

    /* Counts */
    snprintf(buf, sizeof(buf), "Driver selezionati:     %d su %d",
             count_selected_das(), das_count);
    put_line(4, y, buf, COL_BODY); y += LINE_H;

    snprintf(buf, sizeof(buf), "Programmi selezionati:  %d su %d",
             count_selected_programs(), program_count);
    put_line(5, y, buf, COL_BODY); y += LINE_H;

    /* exFAT definitive volume — reflects the choice made in "Selezione disco". */
    if (st.use_exfat_definitive && g_exfat_label[0])
        snprintf(buf, sizeof(buf), "Volume definitivo: %s", g_exfat_label);
    else
        snprintf(buf, sizeof(buf), "Volume definitivo: nessuno (solo FAT32)");
    put_line(8, y, buf, COL_BODY); y += LINE_H;

    /* Password di accesso — MAI il valore, solo lo stato. */
    snprintf(buf, sizeof(buf), "Password di accesso: %s",
             st.logon_password[0] ? "impostata" : "nessuna");
    put_line(9, y, buf, COL_BODY); y += LINE_H * 2;

    put_line(6, y, "Premi Avanti per iniziare l'installazione.",                COL_TITLE); y += LINE_H;
    put_line(7, y, "L'operazione scriverà sul disco scelto e non è reversibile.", COL_WARN);
}

/* ===== Install step =====
 *
 * Tick-driven state machine. install_seeded latches on first entry,
 * dobui_set_tick(INSTALL_TICK_MS) starts the engine. Each tick runs
 * one phase via install_tick and appends a status line to
 * install_log[]; when the last phase clears, the wizard auto-
 * advances to STEP_DONE and the tick is disarmed.
 *
 * Every phase is currently a stub (logs the entry and returns).
 * Replacing them one at a time with real I/O — spawn secondary DFS,
 * file copy, GRUB blob writes, config emission — keeps the UI
 * functional throughout the migration. */

static void
install_log_add(const char *msg)
{
    if (install_log_count < INSTALL_LOG_LINES)
    {
        strncpy(install_log[install_log_count], msg,
                sizeof(install_log[0]) - 1);
        install_log[install_log_count][sizeof(install_log[0]) - 1] = '\0';
        install_log_count++;
        return;
    }
    /* Buffer full: shift up by one, drop the oldest. */
    for (int i = 1; i < INSTALL_LOG_LINES; i++)
        memcpy(install_log[i - 1], install_log[i], sizeof(install_log[0]));
    strncpy(install_log[INSTALL_LOG_LINES - 1], msg, sizeof(install_log[0]) - 1);
    install_log[INSTALL_LOG_LINES - 1][sizeof(install_log[0]) - 1] = '\0';
}

static void
install_render(void)
{
    hide_all_controls();
    put_title("Installazione in corso");

    if (!install_seeded)
    {
        install_seeded    = true;
        install_phase     = INST_PHASE_VERIFY;
        install_log_count = 0;
        debug_print("[MainDOB_Setup] install begin\n");
        dobui_set_tick(INSTALL_TICK_MS);
    }

    /* Current-phase status line */
    char status[160];
    if (install_halted)
        snprintf(status, sizeof(status),
                 "Errore alla fase %d/%d - vedi registro - premi Annulla",
                 (int)install_phase + 1, INST_PHASE_COUNT);
    else if (install_phase < INST_PHASE_COUNT)
        snprintf(status, sizeof(status), "Fase %d/%d: %s...",
                 (int)install_phase + 1, INST_PHASE_COUNT,
                 INSTALL_PHASE_NAMES[install_phase]);
    else
        snprintf(status, sizeof(status), "Completato.");
    put_line(0, BODY_Y, status, install_halted ? COL_WARN : COL_TITLE);

    /* Progress bar — value lags by one position vs. status line on
     * purpose: status shows what's running, bar shows what's done. */
    pb_install.x       = BODY_X;
    pb_install.y       = BODY_Y + 32;
    pb_install.w       = WIN_W - BODY_X * 2 - 50;  /* leave room for "N%" text */
    pb_install.value   = (int)install_phase;
    pb_install.max     = INST_PHASE_COUNT;
    pb_install.visible = true;
    dobpb_Draw(&pb_install);

    /* Log area */
    int log_y = BODY_Y + 80;
    put_line(0, log_y, "Registro:", COL_HINT);
    for (int i = 0; i < install_log_count; i++)
        put_line(i + 1, log_y + LINE_H * (i + 1), install_log[i], COL_BODY);
}

/* ---------- File-copy helpers ---------- */

/* Static scratch buffer for file-by-file copy. 16 KB is a reasonable
 * balance between IPC overhead (few enough round-trips for a typical
 * 50-200 KB .mdl) and footprint (we live in the same .mdl as the
 * wizard UI and shouldn't grow the binary's BSS without cause). */
#define INSTALL_COPY_BUF_SIZE  (16 * 1024)
static char install_copy_buf[INSTALL_COPY_BUF_SIZE];

/* OS_SVCLIST: canonical list of OS service directories under
 * /SYSTEM/OperatingSystem/ that must exist on every installed
 * system. Mirrors the same name list in tools/mklive.sh — keep them
 * in sync when adding or removing core services. MainDOB_Setup is
 * intentionally NOT here: the program ships into /SYSTEM/PROGRAMS
 * on the installed system but doesn't auto-launch (its
 * Startup_modules line lives only on the live blob). */
static const char *INSTALL_OS_SVCLIST[] =
{
    "console", "config", "hotplug", "DobFileSystem",
    "inputd", "dobinterface", "settingsd",
    NULL,
};

/* audioplayer and floppyprobe are drivers (audio service / CMOS floppy
 * probe). Their sources sit in boot/ but on an installed system they
 * belong under /SYSTEM/DRIVERS, not /SYSTEM/OperatingSystem. Installed
 * separately into DRIVERS by install_phase_os(). */
static const char *INSTALL_BOOT_DRIVERS[] =
{
    "audioplayer", "floppyprobe",
    NULL,
};

/* Copy one file: source via the default DFS (the live ramdisk),
 * destination via TARGET_SERVICE (the secondary DFS rooted on the
 * target partition). FS_WRITE | FS_CREATE | FS_TRUNC matches the
 * pattern used everywhere else in MainDOB userspace for a
 * fresh-write-may-overwrite open. */
/* Write target for the install_* helpers below. Defaults to the FAT32 boot
 * partition (dobfs_9999). In a root-on-exFAT split install it is switched to
 * the exFAT root (dobfs_9998) for the system phases and back to FAT32 for the
 * boot-stub phase. g_split latches st.use_exfat_definitive at install start. */
static const char *g_target_service = TARGET_SERVICE;
static bool        g_split          = false;
static char        g_boot_provider[20] = "ata";  /* "ata"/"ahci" of the boot disk */

static void
install_copy_diag(const char *step, const char *path, int rc)
{
#ifdef MAINDOB_DEBUG
    char d[192];
    snprintf(d, sizeof(d), "[MainDOB_Setup] copy %s FAILED rc=%d svc=%s %s\n",
             step, rc, g_target_service, path);
    debug_print(d);
#else
    (void)step; (void)path; (void)rc;
#endif
}

static bool
install_copy_file(const char *src, const char *dst)
{
    int sfd = dobfs_Open(src, FS_READ);
    if (sfd < 0) { install_copy_diag("open-src", src, sfd); return false; }
    int dfd = dobfs_OpenOn(g_target_service, dst,
                           FS_WRITE | FS_CREATE | FS_TRUNC);
    if (dfd < 0)
    {
        install_copy_diag("open-dst", dst, dfd);
        dobfs_Close(sfd);
        return false;
    }

    bool ok = true;
    int  n;
    while ((n = dobfs_Read(sfd, install_copy_buf, INSTALL_COPY_BUF_SIZE)) > 0)
    {
        int w = dobfs_Write(dfd, install_copy_buf, (uint32_t)n);
        if (w != n) { install_copy_diag("write", dst, w); ok = false; break; }
    }
    if (n < 0) { install_copy_diag("read", src, n); ok = false; }

    dobfs_Close(sfd);
    dobfs_Close(dfd);
    return ok;
}

/* Copy file if it exists in the source — otherwise silently succeed.
 * Used for manifest.dob, Visible, .mem companions, .kbl layouts and
 * other ancillary files that may not be present for every entry. */
static bool
install_copy_optional(const char *src, const char *dst)
{
    dobfs_stat_t sst;
    if (dobfs_Stat(src, &sst) != 0) return true;   /* source absent: skip */
    return install_copy_file(src, dst);
}

/* mkdir each component of an absolute path on the target. FAT32
 * doesn't auto-create parents on file create, and dobfs_Mkdir
 * returns an error rather than success when the dir already exists
 * — so each call gets its return value discarded, and the eventual
 * success or failure is judged by whether the file Open afterwards
 * works. */
static void
install_mkdir_p_target(const char *path)
{
    char buf[256];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    for (char *p = buf + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            (void)dobfs_MkdirOn(g_target_service, buf);
            *p = '/';
        }
    }
    (void)dobfs_MkdirOn(g_target_service, buf);
}

/* Write a fixed string as the entire contents of a file on the
 * target. Used for generated config (Startup_modules content,
 * computer_name, keymap, installed marker). */
static bool
install_write_string(const char *path, const char *content)
{
    int dfd = dobfs_OpenOn(g_target_service, path,
                           FS_WRITE | FS_CREATE | FS_TRUNC);
    if (dfd < 0) { install_copy_diag("open-dst", path, dfd); return false; }
    uint32_t n = (uint32_t)strlen(content);
    int w = (n > 0) ? dobfs_Write(dfd, content, n) : 0;
    dobfs_Close(dfd);
    if ((uint32_t)w != n) install_copy_diag("write", path, w);
    return (uint32_t)w == n;
}

/* Write a Visible marker file inside an installed program's
 * directory. The convention is one byte ('0' or '1') followed by a
 * newline — readers compare on the first character. */
static bool
install_write_visible(const char *target_dir, bool visible)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/Visible", target_dir);
    return install_write_string(path, visible ? "1\n" : "0\n");
}

/* ---------- Per-phase implementations ---------- */

/* INST_PHASE_GRUB — install GRUB's first stage to MBR + second stage
 * (core.img) to the post-MBR gap, then drop /boot/grub/grub.cfg on
 * the target so the just-installed core.img has a config to read.
 *
 * Layout decisions:
 *   - boot.img is exactly 512 bytes; we splice its first 440 over
 *     the existing target MBR, preserving the partition table
 *     (offsets 440-509) and BIOS signature (510-511).
 *   - core.img is variable size (modular: typically 30-60 KB with
 *     biosdisk + part_msdos + fat + multiboot + configfile + normal),
 *     padded to sector multiple, written from LBA 1. Must fit in
 *     the gap (1 .. start_lba-1) — for a partition at LBA 2048
 *     that's 2047 sectors / ~1 MB, plenty.
 *
 * Reads via the default (live) DFS, writes via libdob/block.c
 * directly to the target disk's raw sectors. The dobfs_target mount
 * stays out of the picture for sectors 0..N — we work below the FS
 * boundary. grub.cfg, by contrast, IS a filesystem write and goes
 * through the FAT32 mount.
 *
 * Running BEFORE INST_PHASE_KERNEL is fine: grub.cfg only references
 * the kernel's eventual path, the file doesn't have to exist yet —
 * GRUB reads grub.cfg at boot, by which time the kernel is in
 * place. */
static bool
install_phase_grub(void)
{
    setup_target_t *t = &targets[st.selected_target];

    /* ---- 1. boot.img → splice into target MBR ---- */
    uint8_t boot_img[512];
    {
        int fd = dobfs_Open("/SYSTEM/OperatingSystem/grub-blobs/boot.img", FS_READ);
        if (fd < 0)
        {
            install_log_add("Errore: boot.img mancante nella live");
            return false;
        }
        int n = dobfs_Read(fd, boot_img, sizeof(boot_img));
        dobfs_Close(fd);
        if (n != (int)sizeof(boot_img))
        {
            install_log_add("Errore: boot.img read incompleta");
            return false;
        }
    }

    uint8_t target_mbr[512];
    if (!block_read(t->disk_index, 0, 1, target_mbr))
    {
        install_log_add("Errore: lettura MBR target fallita");
        return false;
    }

    /* Splice: GRUB boot code (0..439) + existing pt/sig (440..511).
     * The trailing block of boot.img includes a partition-table slot
     * with all-zeros — overwriting that area would wipe the user's
     * partition. memcpy of only 440 bytes is the right move. */
    memcpy(target_mbr, boot_img, 440);

    /* Mark the target partition active. DobDisk creates partitions
     * with active=0, and while GRUB's own boot.img doesn't strictly
     * need an active partition (it knows where its core.img sits
     * because grub-mkimage embedded the layout), SeaBIOS and many
     * real-hardware BIOSes refuse to boot a disk where no partition
     * carries the 0x80 flag. Set exactly one — the partition we just
     * installed onto — and clear the rest so the boot order is
     * unambiguous. */
    for (int p = 0; p < 4; p++)
        target_mbr[446 + p * 16] = (p == t->part_index) ? 0x80 : 0x00;

    if (!block_write(t->disk_index, 0, 1, target_mbr))
    {
        install_log_add("Errore: scrittura MBR target fallita");
        return false;
    }

    /* Read-back check: re-read sector 0 and compare the first 16
     * bytes against the known boot.img prefix. Catches the silent
     * failure mode where block_write returns true but bytes never
     * reach the host file (QEMU block-0 protection on auto-detected
     * raw images, driver issues, etc.). */
    {
        uint8_t verify[512];
        if (!block_read(t->disk_index, 0, 1, verify))
        {
            install_log_add("Errore: rilettura MBR fallita");
            return false;
        }
        if (memcmp(verify, boot_img, 16) != 0)
        {
            install_log_add("Errore: MBR write non persistito");
            return false;
        }
    }
    install_log_add("MBR aggiornato");

    /* ---- 2. core.img → sectors 1..N ---- */
    dobfs_stat_t cst;
    if (dobfs_Stat("/SYSTEM/OperatingSystem/grub-blobs/core.img", &cst) != 0)
    {
        install_log_add("Errore: core.img mancante nella live");
        return false;
    }
    if (cst.size == 0 || cst.size > 512u * 1024u)
    {
        install_log_add("Errore: core.img dimensione invalida");
        return false;
    }

    uint32_t core_sectors = (cst.size + 511u) / 512u;
    /* Gap available: sectors 1 .. (start_lba - 1). */
    if (core_sectors >= t->start_lba)
    {
        install_log_add("Errore: core.img troppo grande per il gap MBR");
        return false;
    }

    uint32_t core_alloc = core_sectors * 512u;
    uint8_t *core_buf = (uint8_t *)malloc(core_alloc);
    if (!core_buf)
    {
        install_log_add("Errore: out of memory per buffer core.img");
        return false;
    }
    memset(core_buf, 0, core_alloc);
    {
        int fd = dobfs_Open("/SYSTEM/OperatingSystem/grub-blobs/core.img", FS_READ);
        if (fd < 0)
        {
            free(core_buf);
            install_log_add("Errore: apertura core.img fallita");
            return false;
        }
        uint32_t total = 0;
        while (total < cst.size)
        {
            int n = dobfs_Read(fd, core_buf + total, cst.size - total);
            if (n <= 0) break;
            total += (uint32_t)n;
        }
        dobfs_Close(fd);
        if (total != cst.size)
        {
            free(core_buf);
            install_log_add("Errore: lettura core.img incompleta");
            return false;
        }
    }

    /* ATA caps each IPC at MAX_SECTORS=128, and libdob's do_call
     * masks server-side errors as success. Chunk at 64 sectors so
     * an oversized write can never silently drop. */
    {
        uint32_t written = 0;
        const uint32_t chunk_max = 64;
        while (written < core_sectors)
        {
            uint32_t chunk = core_sectors - written;
            if (chunk > chunk_max) chunk = chunk_max;
            if (!block_write(t->disk_index, (uint64_t)(1 + written), chunk,
                             core_buf + written * 512u))
            {
                free(core_buf);
                char err[120];
                snprintf(err, sizeof(err),
                         "Errore: scrittura core.img fallita @LBA %u",
                         (unsigned)(1 + written));
                install_log_add(err);
                return false;
            }
            written += chunk;
        }
    }
    free(core_buf);
    {
        char line[96];
        snprintf(line, sizeof(line),
                 "core.img scritto (%u byte, %u settori)",
                 (unsigned)cst.size, (unsigned)core_sectors);
        install_log_add(line);
    }

    /* ---- 3. grub.cfg on target FAT32 partition ---- */
    install_mkdir_p_target("/boot");
    install_mkdir_p_target("/boot/grub");
    static const char *GRUB_CFG_CONTENT =
        "# Generated by MainDOB_Setup\n"
        "# timeout=3 makes errors visible during early development —\n"
        "# drop it to 0 once boot is reliable.\n"
        "set timeout=3\n"
        "set default=0\n"
        "set root=(hd0,msdos1)\n"
        "\n"
        "menuentry \"MainDOB\" {\n"
        "    multiboot /boot/kernel.bin\n"
        "    boot\n"
        "}\n";
    if (!install_write_string("/boot/grub/grub.cfg", GRUB_CFG_CONTENT))
    {
        install_log_add("Errore: scrittura grub.cfg fallita");
        return false;
    }
    install_log_add("grub.cfg scritto");

    return true;
}

/* INST_PHASE_KERNEL — copy kernel.bin to /boot/kernel.bin (where
 * GRUB's grub.cfg multiboot directive points) AND to
 * /SYSTEM/OperatingSystem/kernel.bin (canonical running-system path,
 * mirrors the live layout). */
static bool
install_phase_kernel(void)
{
    install_mkdir_p_target("/boot");
    if (!install_copy_file("/SYSTEM/OperatingSystem/kernel.bin",
                           "/boot/kernel.bin"))
    {
        install_log_add("Errore: copia /boot/kernel.bin fallita");
        return false;
    }

    install_mkdir_p_target("/SYSTEM");
    install_mkdir_p_target("/SYSTEM/OperatingSystem");
    if (!install_copy_file("/SYSTEM/OperatingSystem/kernel.bin",
                           "/SYSTEM/OperatingSystem/kernel.bin"))
    {
        install_log_add("Errore: copia /SYSTEM/OperatingSystem/kernel.bin fallita");
        return false;
    }
    install_log_add("Copiato kernel.bin in /boot e /SYSTEM/OperatingSystem");
    return true;
}

/* INST_PHASE_OS — copy each core OS service. .mdl is required (a
 * missing source aborts the install); manifest.dob is optional.
 * Visible defaults to "0" (no desktop icon for system services). */
static bool
install_phase_os(void)
{
    char src[256], dst[256], dir[256];
    int  count = 0;

    for (int i = 0; INSTALL_OS_SVCLIST[i]; i++)
    {
        const char *svc = INSTALL_OS_SVCLIST[i];

        snprintf(dir, sizeof(dir), "/SYSTEM/OperatingSystem/%s", svc);
        install_mkdir_p_target(dir);

        snprintf(src, sizeof(src), "/SYSTEM/OperatingSystem/%s/%s.mdl", svc, svc);
        snprintf(dst, sizeof(dst), "/SYSTEM/OperatingSystem/%s/%s.mdl", svc, svc);
        if (!install_copy_file(src, dst))
        {
            char err[160];
            snprintf(err, sizeof(err), "Errore: copia %s.mdl fallita", svc);
            install_log_add(err);
            return false;
        }

        snprintf(src, sizeof(src), "/SYSTEM/OperatingSystem/%s/manifest.dob", svc);
        snprintf(dst, sizeof(dst), "/SYSTEM/OperatingSystem/%s/manifest.dob", svc);
        install_copy_optional(src, dst);

        install_write_visible(dir, false);
        count++;
    }

    /* DobFileSystem's exFAT parser companion. Like iso9660.mem for cdrom,
     * it is a .mem loaded at runtime (dob_mem_load), not a service .mdl, so
     * the loop above doesn't catch it. Without it every exFAT mount — the
     * desktop partition icons AND the boot-time definitive-volume attach —
     * fails with "cannot open exfat.mem". Optional: source is absent only
     * when exFAT support wasn't built. The DobFileSystem dir was just made
     * by the loop above. */
    install_copy_optional("/SYSTEM/OperatingSystem/DobFileSystem/exfat.mem",
                          "/SYSTEM/OperatingSystem/DobFileSystem/exfat.mem");

    /* audioplayer + floppyprobe: drivers that live in /SYSTEM/DRIVERS
     * on the installed system. Source is the live medium's DRIVERS dir
     * (mklive/mkbootdisk already place them there). .mdl required,
     * manifest optional, hidden from the desktop. */
    for (int i = 0; INSTALL_BOOT_DRIVERS[i]; i++)
    {
        const char *drv = INSTALL_BOOT_DRIVERS[i];

        snprintf(dir, sizeof(dir), "/SYSTEM/DRIVERS/%s", drv);
        install_mkdir_p_target(dir);

        snprintf(src, sizeof(src), "/SYSTEM/DRIVERS/%s/%s.mdl", drv, drv);
        snprintf(dst, sizeof(dst), "/SYSTEM/DRIVERS/%s/%s.mdl", drv, drv);
        if (!install_copy_file(src, dst))
        {
            char err[160];
            snprintf(err, sizeof(err), "Errore: copia %s.mdl fallita", drv);
            install_log_add(err);
            return false;
        }

        snprintf(src, sizeof(src), "/SYSTEM/DRIVERS/%s/manifest.dob", drv);
        snprintf(dst, sizeof(dst), "/SYSTEM/DRIVERS/%s/manifest.dob", drv);
        install_copy_optional(src, dst);

        install_write_visible(dir, false);
        count++;
    }

    char line[96];
    snprintf(line, sizeof(line), "Copiati %d servizi di sistema", count);
    install_log_add(line);
    return true;
}

/* INST_PHASE_DRIVERS — for each selected DAS, parse it for
 * `driver = /SYSTEM/DRIVERS/X/X.mdl` and copy that driver. Driver
 * paths are deduplicated (cdrom_ide + cdrom_ahci share cdrom.mdl).
 * DAS files without a driver= line (e.g. partition_fat32) are
 * skipped — their .das still travels via the CONFIG phase. ata is
 * copied unconditionally because DobFileSystem loads it from
 * Startup_modules before hotplug exists. cdrom additionally gets
 * its iso9660.mem companion. */
static bool
install_drv_copy_one(const char *drv_dir_name)
{
    char src[256], dst[256], dir[256];
    snprintf(src, sizeof(src), "/SYSTEM/DRIVERS/%s/%s.mdl",
             drv_dir_name, drv_dir_name);
    dobfs_stat_t sst;
    if (dobfs_Stat(src, &sst) != 0) return false;

    snprintf(dir, sizeof(dir), "/SYSTEM/DRIVERS/%s", drv_dir_name);
    install_mkdir_p_target(dir);
    snprintf(dst, sizeof(dst), "/SYSTEM/DRIVERS/%s/%s.mdl",
             drv_dir_name, drv_dir_name);
    if (!install_copy_file(src, dst)) return false;

    snprintf(src, sizeof(src), "/SYSTEM/DRIVERS/%s/manifest.dob", drv_dir_name);
    snprintf(dst, sizeof(dst), "/SYSTEM/DRIVERS/%s/manifest.dob", drv_dir_name);
    install_copy_optional(src, dst);

    install_write_visible(dir, false);

    if (strcmp(drv_dir_name, "cdrom") == 0)
        install_copy_optional("/SYSTEM/DRIVERS/cdrom/iso9660.mem",
                              "/SYSTEM/DRIVERS/cdrom/iso9660.mem");
    return true;
}

/* Extract the leaf directory name from a `driver = /SYSTEM/DRIVERS/X/X.mdl`
 * line.  Writes up to dst_size-1 chars + NUL into dst.  Returns false
 * on a malformed path. */
static bool
install_drv_name_from_das_path(const char *das_driver_path,
                               char *dst, uint32_t dst_size)
{
    const char *prefix = "/SYSTEM/DRIVERS/";
    uint32_t plen = (uint32_t)strlen(prefix);
    if (strncmp(das_driver_path, prefix, plen) != 0) return false;
    const char *p = das_driver_path + plen;
    const char *slash = p;
    while (*slash && *slash != '/') slash++;
    uint32_t n = (uint32_t)(slash - p);
    if (n == 0 || n >= dst_size) return false;
    memcpy(dst, p, n);
    dst[n] = '\0';
    return true;
}

/* Read the .das at `das_path` looking for the first non-comment line
 * that starts with `driver` and equals-assigns a path. Writes the
 * leaf directory name into out_drv_dir (e.g. "floppy", "cdrom").
 * Returns false if the file has no driver= line (e.g. partition_*).*/
static bool
install_drv_dir_from_das(const char *das_path, char *out_drv_dir, uint32_t out_size)
{
    int fd = dobfs_Open(das_path, FS_READ);
    if (fd < 0) return false;

    static char buf[4096];
    int n = dobfs_Read(fd, buf, sizeof(buf) - 1);
    dobfs_Close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';

    char *p = buf;
    while (*p)
    {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
        {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }
        if (strncmp(p, "driver", 6) == 0)
        {
            const char *q = p + 6;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=')
            {
                q++;
                while (*q == ' ' || *q == '\t') q++;
                const char *path_start = q;
                while (*q && *q != '\n' && *q != '\r' && *q != ' '
                       && *q != '\t' && *q != '#') q++;
                uint32_t plen = (uint32_t)(q - path_start);
                if (plen > 0 && plen < 256)
                {
                    char drv_path[256];
                    memcpy(drv_path, path_start, plen);
                    drv_path[plen] = '\0';
                    return install_drv_name_from_das_path(drv_path,
                                                          out_drv_dir, out_size);
                }
            }
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return false;
}

static bool
install_phase_drivers(void)
{
    install_mkdir_p_target("/SYSTEM/DRIVERS");

    /* Track which driver dirs we've already copied to dedup the
     * cdrom_ide + cdrom_ahci → cdrom.mdl case. */
    static char copied_drv[32][32];
    int copied_n = 0;

    int count = 0;

    /* ---- ata: always (DobFileSystem startup requirement) ---- */
    if (!install_drv_copy_one("ata"))
    {
        install_log_add("Errore: copia ata.mdl fallita (driver disco obbligatorio)");
        return false;
    }
    strncpy(copied_drv[copied_n++], "ata", sizeof(copied_drv[0]) - 1);
    count++;

    /* ---- ahci: always too. It's now listed in Startup_modules next to
     * ata so the boot disk is readable whether it's IDE or SATA. On an
     * IDE-only machine the standalone scan finds no AHCI controller and
     * the driver simply exits; harmless. ---- */
    if (!install_drv_copy_one("ahci"))
    {
        install_log_add("Errore: copia ahci.mdl fallita (driver disco obbligatorio)");
        return false;
    }
    strncpy(copied_drv[copied_n++], "ahci", sizeof(copied_drv[0]) - 1);
    count++;

    /* ---- usb_mass_storage: class sub-driver, no driver= line names it
     * (it's spawned by the pendrive icon's DAS *action*, not matched as
     * a device driver). Copied unconditionally as the companion of
     * every USB host-controller DAS; soft-fail, a USB-less system just
     * carries ~20 KB it never runs. ---- */
    if (install_drv_copy_one("usb_mass_storage"))
    {
        strncpy(copied_drv[copied_n++], "usb_mass_storage",
                sizeof(copied_drv[0]) - 1);
        count++;
    }
    else
        install_log_add("Avviso: usb_mass_storage.mdl assente dal supporto");

    /* ---- One driver per selected DAS, deduped ---- */
    for (int i = 0; i < das_count; i++)
    {
        if (!das_entries[i].selected) continue;
        const char *das_name = das_entries[i].name;

        char das_path[200];
        snprintf(das_path, sizeof(das_path),
                 "/SYSTEM/CONFIG/DAS/%s.das", das_name);

        char drv_dir[32];
        if (!install_drv_dir_from_das(das_path, drv_dir, sizeof(drv_dir)))
            continue;   /* No driver= line (e.g. partition_fat32) */

        bool already = false;
        for (int k = 0; k < copied_n; k++)
            if (strcmp(copied_drv[k], drv_dir) == 0) { already = true; break; }
        if (already) continue;

        if (!install_drv_copy_one(drv_dir))
        {
            char err[160];
            snprintf(err, sizeof(err),
                     "Errore: copia driver %s (per DAS %s) fallita",
                     drv_dir, das_name);
            install_log_add(err);
            return false;
        }
        if (copied_n < (int)(sizeof(copied_drv) / sizeof(copied_drv[0])))
        {
            strncpy(copied_drv[copied_n], drv_dir,
                    sizeof(copied_drv[0]) - 1);
            copied_drv[copied_n][sizeof(copied_drv[0]) - 1] = '\0';
            copied_n++;
        }
        count++;
    }

    char line[96];
    snprintf(line, sizeof(line), "Copiati %d driver", count);
    install_log_add(line);
    return true;
}

/* INST_PHASE_PROGRAMS — copy selected entries from /SYSTEM/PROGRAMS
 * and /SYSTEM/GAMES, preserving the source's program/game split
 * (program_entries[i].is_game). Special case for keymap (.kbl
 * layouts). */
static bool
install_phase_programs(void)
{
    install_mkdir_p_target("/SYSTEM/PROGRAMS");
    install_mkdir_p_target("/SYSTEM/GAMES");
    char src[256], dst[256], dir[256];
    int  count_p = 0, count_g = 0;

    for (int i = 0; i < program_count; i++)
    {
        if (!program_entries[i].selected) continue;
        const char *name    = program_entries[i].name;
        bool        is_game = program_entries[i].is_game;
        const char *base    = is_game ? "/SYSTEM/GAMES" : "/SYSTEM/PROGRAMS";

        snprintf(dir, sizeof(dir), "%s/%s", base, name);
        install_mkdir_p_target(dir);

        snprintf(src, sizeof(src), "%s/%s/%s.mdl", base, name, name);
        snprintf(dst, sizeof(dst), "%s/%s/%s.mdl", base, name, name);
        if (!install_copy_file(src, dst))
        {
            char err[160];
            snprintf(err, sizeof(err), "Errore: copia %s/%s fallita", base, name);
            install_log_add(err);
            return false;
        }

        snprintf(src, sizeof(src), "%s/%s/manifest.dob", base, name);
        snprintf(dst, sizeof(dst), "%s/%s/manifest.dob", base, name);
        install_copy_optional(src, dst);

        install_write_visible(dir, true);

        /* keymap ships per-layout .kbl files alongside its .mdl. */
        if (!is_game && strcmp(name, "keymap") == 0)
        {
            install_copy_optional("/SYSTEM/PROGRAMS/keymap/US.kbl",
                                  "/SYSTEM/PROGRAMS/keymap/US.kbl");
            install_copy_optional("/SYSTEM/PROGRAMS/keymap/IT.kbl",
                                  "/SYSTEM/PROGRAMS/keymap/IT.kbl");
        }

        if (is_game) count_g++; else count_p++;
    }
    char line[96];
    snprintf(line, sizeof(line),
             "Copiati %d programmi, %d giochi", count_p, count_g);
    install_log_add(line);
    return true;
}

/* Read the `autostart` field from a program's (or game's) source
 * manifest.dob. Returns true and fills `out` with the field value
 * (e.g. "needs:dobinterface", or "" for a bare autostart with no deps)
 * when the manifest exists AND declares autostart. Returns false when
 * there's no manifest or no autostart line -- such a program is copied
 * but not added to Startup_modules.
 *
 * This is what makes auto-start DECLARATIVE: a program opts in via its
 * own manifest, so adding a new auto-starting program never requires
 * editing the installer. Core OS services (no manifest in this format,
 * delicate dependency ordering) stay hardcoded below. */
static bool
read_autostart(const char *name, bool is_game, char *out, uint32_t out_sz)
{
    out[0] = '\0';
    const char *base = is_game ? "/SYSTEM/GAMES" : "/SYSTEM/PROGRAMS";
    char path[256];
    snprintf(path, sizeof(path), "%s/%s/manifest.dob", base, name);

    int fd = dobfs_Open(path, FS_READ);
    if (fd < 0)
        return false;
    char buf[512];
    int len = dobfs_Read(fd, buf, sizeof(buf) - 1);
    dobfs_Close(fd);
    if (len <= 0)
        return false;
    buf[len] = '\0';

    /* Line-by-line key=value scan, mirroring modules/parse_manifest. */
    char *line = buf;
    bool found = false;
    while (*line)
    {
        char *eol = line; while (*eol && *eol != '\n') eol++;
        char saved = *eol; *eol = '\0';

        if (line[0] != '#' && line[0] != '\0')
        {
            char *eq = line; while (*eq && *eq != '=') eq++;
            if (*eq == '=')
            {
                *eq = '\0';
                char *key = line, *val = eq + 1;
                /* trim trailing CR */
                uint32_t vl = strlen(val);
                while (vl > 0 && (val[vl-1] == '\r' || val[vl-1] == ' '))
                    val[--vl] = '\0';
                if (strcmp(key, "autostart") == 0)
                {
                    strncpy(out, val, out_sz - 1);
                    out[out_sz - 1] = '\0';
                    found = true;
                }
            }
        }
        if (saved == '\0') break;
        line = eol + 1;
    }
    return found;
}

/* exFAT "definitive" volume detection.
 *
 * The chosen layout is: the kernel boots from the usual FAT32 partition,
 * then attaches the large exFAT volume (the only option for disks > ~120 GB).
 * Scan the install disk's MBR for an exFAT partition OTHER than the FAT32
 * boot target; if one is present, the config phase writes an explicit
 * /SYSTEM/CONFIG/Definitive_volume so exfat_attach mounts it at every boot.
 * First exFAT partition wins. Confirmed by the "EXFAT   " boot-sector
 * signature, not just the 0x07 type byte (0x07 is also NTFS/IFS). */
static void
detect_exfat_definitive(void)
{
    g_has_exfat  = false;
    g_exfat_lba  = 0;
    g_exfat_part = -1;
    g_exfat_label[0] = '\0';

    if (st.selected_target < 0 || st.selected_target >= target_count)
        return;
    int disk     = targets[st.selected_target].disk_index;
    int boot_part = targets[st.selected_target].part_index;

    uint8_t mbr[BLOCK_SECTOR_SIZE];
    if (!block_read(disk, 0, 1, mbr))
        return;
    mbr_table_t tbl;
    partition_mbr_parse(mbr, &tbl);
    if (!tbl.valid_signature)
        return;

    for (int p = 0; p < MBR_MAX_PRIMARY; p++)
    {
        const mbr_partition_t *e = &tbl.entries[p];
        if (e->sectors == 0)        continue;
        if (p == boot_part)         continue;   /* skip the FAT32 boot target */
        if (e->type != MBR_TYPE_EXFAT) continue;

        uint8_t bs[BLOCK_SECTOR_SIZE];
        if (!block_read(disk, e->start_lba, 1, bs))
            continue;
        static const char exsig[8] = { 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' ' };
        bool is_ex = true;
        for (int k = 0; k < 8; k++)
            if (bs[3 + k] != (uint8_t)exsig[k]) { is_ex = false; break; }
        if (!is_ex)
            continue;

        g_has_exfat  = true;
        g_exfat_lba  = e->start_lba;
        g_exfat_part = p;
        {
            uint64_t mb64 = ((uint64_t)e->sectors * BLOCK_SECTOR_SIZE) / (1024u * 1024u);
            unsigned mb   = (unsigned)mb64;
            if (mb >= 1024u)
                snprintf(g_exfat_label, sizeof(g_exfat_label),
                         "Disco %d, partizione %d, exFAT (%u GB)",
                         disk + 1, p + 1, mb / 1024u);
            else
                snprintf(g_exfat_label, sizeof(g_exfat_label),
                         "Disco %d, partizione %d, exFAT (%u MB)",
                         disk + 1, p + 1, mb);
        }
        return;
    }
}

/* INST_PHASE_CONFIG — generate the target's /SYSTEM/CONFIG. The big
 * piece here is Startup_modules, which is synthesised from the
 * selected DAS drivers, a fixed list of core OS services, and the
 * selected programs that DECLARE autostart in their manifest. Other
 * files (Associations, DAS files, computer_name, keymap, installed
 * marker) are simpler copies or fixed-string writes. */
static bool
install_phase_config(void)
{
    detect_exfat_definitive();   /* may set g_has_exfat for the writes below */

    install_mkdir_p_target("/SYSTEM/CONFIG");
    install_mkdir_p_target("/SYSTEM/CONFIG/DAS");

    /* Defensive: ensure program_entries[] is populated even if the
     * step order ever changes. Idempotent (guarded by programs_parsed),
     * so this is a no-op on the normal path where STEP_PROGRAMS already
     * ran. */
    programs_parse();

    /* ----- Startup_modules ----- */
    static char startup[8192];
    int sp = 0;
    sp += snprintf(startup + sp, sizeof(startup) - sp,
        "# MainDOB Startup Modules - generated by MainDOB_Setup\n"
        "# Hardware drivers selected via DAS are spawned by hotplug on its\n"
        "# PCI scan. The disk drivers are listed here too so the boot disk\n"
        "# is readable before DobFileSystem: ata for IDE/PATA, ahci for\n"
        "# SATA. ahci.das also exists, so hotplug may try to spawn a second\n"
        "# ahci -- each disk driver carries an anti-double-run watchdog\n"
        "# (registry check) so the duplicate exits cleanly. Both can run at\n"
        "# once on distinct controllers (a SATA boot disk plus IDE devices).\n"
        "/SYSTEM/DRIVERS/ata/ata.mdl\tdriver\n"
        "/SYSTEM/DRIVERS/ahci/ahci.mdl\tdriver\n"
        "\n# OS services (dependency-ordered)\n"
        "/SYSTEM/OperatingSystem/inputd/inputd.mdl\tdriver\n"
        "/SYSTEM/OperatingSystem/DobFileSystem/DobFileSystem.mdl\n"
        "/SYSTEM/OperatingSystem/settingsd/settingsd.mdl\tdriver primary needs:DobFileSystem\n"
        "/SYSTEM/OperatingSystem/config/config.mdl\n"
        "/SYSTEM/OperatingSystem/hotplug/hotplug.mdl\tdriver\n"
        "/SYSTEM/DRIVERS/floppyprobe/floppyprobe.mdl\tdriver needs:hotplug\n"
        "/SYSTEM/OperatingSystem/dobinterface/dobinterface.mdl\tdriver primary needs:video\n"
        "/SYSTEM/DRIVERS/audioplayer/audioplayer.mdl\tdriver\n");

    /* ----- Desktop programs: declarative autostart -----
     * Walk the selected programs/games; any whose manifest declares
     * `autostart=...` gets a Startup_modules line. This replaces the
     * old hardcoded DobFiles/keymap/clock block -- a new auto-starting
     * program now needs only its own manifest, never an installer edit.
     * programs_parse() has already populated program_entries[] (it runs
     * on entry to STEP_PROGRAMS, well before this phase). */
    sp += snprintf(startup + sp, sizeof(startup) - sp,
        "\n# Desktop programs (autostart declared per-manifest)\n");
    for (int i = 0; i < program_count; i++)
    {
        if (!program_entries[i].selected)
            continue;
        char as[96];
        if (!read_autostart(program_entries[i].name,
                            program_entries[i].is_game, as, sizeof(as)))
            continue;   /* copied, but not an autostart program */

        const char *base = program_entries[i].is_game
                           ? "/SYSTEM/GAMES" : "/SYSTEM/PROGRAMS";
        if (as[0] != '\0')
            sp += snprintf(startup + sp, sizeof(startup) - sp,
                           "%s/%s/%s.mdl\t%s\n", base,
                           program_entries[i].name,
                           program_entries[i].name, as);
        else
            sp += snprintf(startup + sp, sizeof(startup) - sp,
                           "%s/%s/%s.mdl\n", base,
                           program_entries[i].name,
                           program_entries[i].name);
    }

    if (!install_write_string("/SYSTEM/CONFIG/Startup_modules", startup))
    {
        install_log_add("Errore: scrittura Startup_modules fallita");
        return false;
    }

    /* ----- Shared common_files (fonts): the live ships /SYSTEM/CONFIG/
     * common_files/fonts/ *.ttf (mklive.sh), but this phase used to generate
     * CONFIG from scratch and never copied it — an installed system had NO
     * fonts dir, so DobWrite only ever offered the built-in system font.
     * Copy every file in the fonts dir; absent dir = nothing to do (a build
     * that ships no fonts is legal). Per-file failures are logged but not
     * fatal: fonts are ancillary, the install must not die on them. ----- */
    {
        install_mkdir_p_target("/SYSTEM/CONFIG/common_files");
        install_mkdir_p_target("/SYSTEM/CONFIG/common_files/fonts");
        dobfs_dirent_t fents[32];
        uint32_t fcnt = 0;
        if (dobfs_List("/SYSTEM/CONFIG/common_files/fonts",
                       fents, 32, &fcnt) == 0)
        {
            if (fcnt > 32) fcnt = 32;
            int fonts_copied = 0;
            for (uint32_t i = 0; i < fcnt; i++)
            {
                if (fents[i].type != FS_TYPE_FILE) continue;
                char fsrc[192], fdst[192];
                snprintf(fsrc, sizeof(fsrc),
                         "/SYSTEM/CONFIG/common_files/fonts/%s", fents[i].name);
                snprintf(fdst, sizeof(fdst),
                         "/SYSTEM/CONFIG/common_files/fonts/%s", fents[i].name);
                if (install_copy_file(fsrc, fdst))
                    fonts_copied++;
                else
                {
                    char msg[96];
                    snprintf(msg, sizeof(msg), "Avviso: copia font %s fallita",
                             fents[i].name);
                    install_log_add(msg);
                }
            }
            if (fonts_copied)
            {
                char msg[64];
                snprintf(msg, sizeof(msg), "%d font copiati", fonts_copied);
                install_log_add(msg);
            }
        }
    }

    /* ----- Associations: copy the live's verbatim ----- */
    if (!install_copy_optional("/SYSTEM/CONFIG/Associations",
                               "/SYSTEM/CONFIG/Associations"))
    {
        install_log_add("Errore: copia Associations fallita");
        return false;
    }

    /* ----- DAS files: copy each selected. Same selection drives
     * both this CONFIG copy and the DRIVERS phase's driver=
     * parsing, so a DAS that's not on disk can never have its
     * driver triggered at boot. ----- */
    int das_copied = 0;
    for (int i = 0; i < das_count; i++)
    {
        if (!das_entries[i].selected) continue;
        char src[160], dst[160];
        snprintf(src, sizeof(src), "/SYSTEM/CONFIG/DAS/%s.das",
                 das_entries[i].name);
        snprintf(dst, sizeof(dst), "/SYSTEM/CONFIG/DAS/%s.das",
                 das_entries[i].name);
        if (install_copy_file(src, dst)) das_copied++;
    }

    /* ----- DAS/USB subdirectory: USB *device-level* DAS (read by the
     * USB host-controller drivers at enumeration time, NOT by hotplug's
     * PCI scan). These are class definitions ("mass storage -> icon X,
     * action Y"), not selectable drivers, so they are copied
     * UNCONDITIONALLY. This subdirectory was previously never copied:
     * the DAS step lists /SYSTEM/CONFIG/DAS and skips non-FILE entries,
     * so the USB/ directory was invisible to it — live media had the
     * files (mklive/mkbootdisk copy them) but every INSTALLED system
     * lacked them. Field signature: enumeration completes, then
     * "File DAS USB letti: 0", no match, no announce, no icon. ----- */
    install_mkdir_p_target("/SYSTEM/CONFIG/DAS/USB");
    {
        static dobfs_dirent_t uentries[32];
        uint32_t ucount = 0;
        if (dobfs_List("/SYSTEM/CONFIG/DAS/USB", uentries, 32, &ucount) == 0)
        {
            for (uint32_t i = 0; i < ucount; i++)
            {
                if (uentries[i].type != FS_TYPE_FILE) continue;
                size_t nl = strlen(uentries[i].name);
                if (nl < 5 ||
                    strcmp(uentries[i].name + nl - 4, ".das") != 0)
                    continue;
                char src[160], dst[160];
                snprintf(src, sizeof(src), "/SYSTEM/CONFIG/DAS/USB/%s",
                         uentries[i].name);
                snprintf(dst, sizeof(dst), "/SYSTEM/CONFIG/DAS/USB/%s",
                         uentries[i].name);
                if (install_copy_file(src, dst)) das_copied++;
            }
        }
        else
        {
            install_log_add("Avviso: DAS/USB assente dal supporto sorgente");
        }
    }

    /* ----- Marker: presence means "installed system" ----- */
    if (!install_write_string("/SYSTEM/CONFIG/installed", "installed\n"))
    {
        install_log_add("Errore: scrittura marker installed fallita");
        return false;
    }

    /* ----- Computer name (empty allowed; written either way) ----- */
    {
        char buf[80];
        snprintf(buf, sizeof(buf), "%s\n", st.computer_name);
        install_write_string("/SYSTEM/CONFIG/computer_name", buf);
    }

    /* ----- Password di accesso (solo se impostata) -----
     * Presenza del file = logon attivo su dobinterface; assenza =
     * desktop diretto. Il record e' l'hash salato di <dob/logon.h>
     * (salt dal pool di entropia del kernel), MAI la password in
     * chiaro. Fallimento non fatale: un'installazione senza logon e'
     * un sistema funzionante — si logga e si prosegue. */
    if (st.logon_password[0])
    {
        uint32_t salt = random_u32();
        if (salt == 0) salt = 0xD0B10617u;   /* mai salt nullo */
        char rec[DOB_LOGON_RECORD_MAX];
        dob_logon_make_record(rec, salt, st.logon_password);
        if (install_write_string("/SYSTEM/CONFIG/Logon_password.dat", rec))
            install_log_add("Password di accesso impostata");
        else
            install_log_add("Avviso: scrittura Logon_password.dat fallita");
    }

    /* ----- Keymap code (US / IT) ----- */
    {
        int ki = st.keymap_index;
        const int kcount = (int)(sizeof(KEYMAP_CODES) / sizeof(KEYMAP_CODES[0]));
        if (ki < 0 || ki >= kcount) ki = 0;
        char buf[16];
        snprintf(buf, sizeof(buf), "%s\n", KEYMAP_CODES[ki]);
        install_write_string("/SYSTEM/CONFIG/keymap", buf);
    }

    char line[96];
    snprintf(line, sizeof(line), "Configurazione scritta (%d DAS)", das_copied);
    install_log_add(line);
    return true;
}

/* INST_PHASE_FINALIZE — quick sanity check: stat the kernel on the
 * target via TARGET_SERVICE. If something silently went wrong with
 * the FAT32 metadata (write reported success but the directory entry
 * didn't make it), we catch it here. */
static bool
install_phase_finalize(void)
{
    /* In a split install /DATA lives on the exFAT root (the data volume),
     * not the FAT32 stub. */
    g_target_service = g_split ? TARGET_ROOT_SERVICE : TARGET_SERVICE;

    /* /DATA + standard subdirs: matches the live layout. Desktop
     * reads /DATA/Desktop; file dialogs default to /DATA/Documents
     * etc. Not required for boot, but expected by userspace. */
    static const char *DATA_DIRS[] =
    {
        "/DATA",
        "/DATA/Desktop", "/DATA/Documents", "/DATA/Downloads",
        "/DATA/Music",   "/DATA/Video",     "/DATA/Pictures",
        "/DATA/Screenshots",
        NULL,
    };
    for (int i = 0; DATA_DIRS[i]; i++)
        install_mkdir_p_target(DATA_DIRS[i]);

    /* Copy the system error sound (the "quack") to the installed disk.
     * The live CD ships it at /DATA/Music/quack.mp2; without copying it
     * here the installed system boots with a working audio driver and a
     * volume control, but has no file to play on the error sound — so
     * errors are silent.  Optional-copy: a missing source must not abort
     * a finalize that has otherwise succeeded. */
    install_copy_optional("/DATA/Music/quack.mp2",
                          "/DATA/Music/quack.mp2");

    /* Split-install diagnostic: confirm the system structure is readable on
     * the exFAT root in this same session. If these stats succeed here but
     * the booted system can't open them, the loss is in cross-mount
     * persistence; if they fail here, the writes themselves didn't take. */
    if (g_split)
    {
        dobfs_stat_t s;
        char line[160];
        int r1 = dobfs_StatOn(TARGET_ROOT_SERVICE, "/SYSTEM", &s);
        int r2 = dobfs_StatOn(TARGET_ROOT_SERVICE, "/SYSTEM/CONFIG", &s);
        int r3 = dobfs_StatOn(TARGET_ROOT_SERVICE, "/SYSTEM/CONFIG/Startup_modules", &s);
        snprintf(line, sizeof(line),
                 "exFAT readback: /SYSTEM=%d CONFIG=%d Startup_modules=%d (0=OK)",
                 r1, r2, r3);
        install_log_add(line);
        if (r3 == 0)
        {
            snprintf(line, sizeof(line),
                     "exFAT Startup_modules leggibile (%u byte)", (unsigned)s.size);
            install_log_add(line);
        }
    }

    dobfs_stat_t kst;
    if (dobfs_StatOn(TARGET_SERVICE,
                     "/boot/kernel.bin", &kst) != 0)
    {
        install_log_add("Avviso: /boot/kernel.bin non rileggibile dopo l'installazione");
    }
    else
    {
        char line[96];
        snprintf(line, sizeof(line),
                 "Verificato: /boot/kernel.bin (%u byte)", (unsigned)kst.size);
        install_log_add(line);
    }
    return true;
}

/* INST_PHASE_VERIFY — spawn the secondary DobFileSystem instance
 * bound to the user's chosen partition, wait for it to register,
 * sanity-check by stat'ing the root. All later phases route their
 * writes through TARGET_SERVICE via dobfs_*On.
 *
 * The spawn is async (spawn_file_driver hands the worker thread the
 * .mdl path + argv blob and returns immediately) but registry_wait
 * blocks up to TARGET_MOUNT_TIMEOUT_MS — the UI freezes for up to
 * five seconds here. Tolerable for v1; splitting into sub-phases
 * (SUBMIT → POLL → VERIFY, one tick each) is a polish item once the
 * full backend lands. */
static bool
install_phase_verify(void)
{
    if (st.selected_target < 0)
    {
        install_log_add("Errore: nessun disco di destinazione selezionato");
        return false;
    }

    /* Re-enumerate: block indices are not stable across calls (per
     * the block.h contract), so the disk_index captured at STEP_DISK
     * time has to be revalidated against the live table here. We
     * find the matching disk by native_selector + bus pair, which
     * are stable across enumeration cycles. */
    int n = block_enumerate();
    setup_target_t *t = &targets[st.selected_target];
    if (t->disk_index < 0 || t->disk_index >= n)
    {
        install_log_add("Errore: il disco di destinazione e' scomparso");
        return false;
    }
    const block_disk_t *d = block_get(t->disk_index);
    if (!d)
    {
        install_log_add("Errore: block_get del disco fallita");
        return false;
    }

    /* Servizio e selector dal layer block (block_provider_binding):
     * l'encoding del routing e' OPACO e su un secondo controller SATA
     * il servizio giusto e' "ahci_N", non "ahci" — hardcodarlo e
     * passare il selector codificato mandava la lettura del boot
     * sector a un'istanza sbagliata con una porta inesistente: mount
     * mai completato, "timeout attesa mount target" alla fase 1/9. */
    static char provider_name[16];
    uint32_t    native_sel = 0;
    if (!block_provider_binding(t->disk_index, provider_name,
                                sizeof(provider_name), &native_sel))
    {
        install_log_add("Errore: binding provider del disco fallito");
        return false;
    }
    strncpy(g_boot_provider, provider_name, sizeof(g_boot_provider) - 1);
    g_boot_provider[sizeof(g_boot_provider) - 1] = '\0';

    static char arg_provider[24];
    static char arg_selector[24];
    static char arg_lba[24];
    static char arg_id   [24];
    snprintf(arg_provider, sizeof(arg_provider), "provider=%s", provider_name);
    snprintf(arg_selector, sizeof(arg_selector), "selector=%u", (unsigned)native_sel);
    snprintf(arg_lba,      sizeof(arg_lba),      "lba=%u",      (unsigned)t->start_lba);
    snprintf(arg_id,       sizeof(arg_id),       "id=%u",       TARGET_ID);

    const char *av[] = {
        "--mount", arg_provider, arg_selector, arg_lba, arg_id, "fs=fat32", NULL,
    };

    if (spawn_file_driver(DOBFS_MDL_PATH, av) < 0)
    {
        install_log_add("Errore: spawn di DobFileSystem fallita");
        return false;
    }
    install_log_add("DFS secondaria avviata, attesa registrazione...");

    uint32_t port = dob_registry_wait(TARGET_SERVICE, TARGET_MOUNT_TIMEOUT_MS);
    if (port == 0)
    {
        install_log_add("Errore: timeout attesa mount target");
        return false;
    }

    /* Round-trip sanity check: if the service registered but the
     * FS layer didn't actually mount (bad LBA, FAT magic missing),
     * stat on "/" comes back negative. */
    dobfs_stat_t st_root;
    int rc = dobfs_StatOn(TARGET_SERVICE, "/", &st_root);
    if (rc != 0)
    {
        char dbg[120];
        snprintf(dbg, sizeof(dbg),
                 "[MainDOB_Setup] StatOn(%s, /) returned %d\n", TARGET_SERVICE, rc);
        debug_print(dbg);
        install_log_add("Errore: stat / sul target fallita");
        return false;
    }

    install_log_add("Mount target FAT32 attivo");

    /* Root-on-exFAT split: if the user chose to use the exFAT volume as the
     * definitive root, mount it too (dobfs_9998) so the system phases can
     * write the bulk of /SYSTEM, the programs and /DATA there. The boot stub
     * stays on FAT32 (dobfs_9999). g_split drives the per-phase target. */
    detect_exfat_definitive();   /* refresh g_has_exfat / g_exfat_lba / g_exfat_part */
    g_split = (g_has_exfat && st.use_exfat_definitive);
    if (g_split)
    {
        static char r_provider[20], r_selector[24], r_lba[24], r_id[24];
        snprintf(r_provider, sizeof(r_provider), "provider=%s", provider_name);
        snprintf(r_selector, sizeof(r_selector), "selector=%u", (unsigned)native_sel);
        snprintf(r_lba,      sizeof(r_lba),      "lba=%u",      (unsigned)g_exfat_lba);
        snprintf(r_id,       sizeof(r_id),       "id=%u",       TARGET_ROOT_ID);
        const char *rav[] = {
            "--mount", r_provider, r_selector, r_lba, r_id, "fs=exfat", NULL,
        };
        if (spawn_file_driver(DOBFS_MDL_PATH, rav) < 0)
        {
            install_log_add("Errore: spawn DFS exFAT (root) fallita");
            return false;
        }
        uint32_t rport = dob_registry_wait(TARGET_ROOT_SERVICE, TARGET_MOUNT_TIMEOUT_MS);
        if (rport == 0)
        {
            install_log_add("Errore: timeout mount root exFAT");
            return false;
        }
        dobfs_stat_t rst;
        if (dobfs_StatOn(TARGET_ROOT_SERVICE, "/", &rst) != 0)
        {
            install_log_add("Errore: stat / sul root exFAT fallita");
            return false;
        }
        install_log_add("Mount root exFAT attivo (sistema su exFAT)");
    }

    install_log_add("Pronto per la scrittura");
    return true;
}

/* INST_PHASE_BOOTSTUB — root-on-exFAT only. Lays the minimal boot stub on the
 * FAT32 partition: the Phase-1 modules the kernel must load from FAT32 (the
 * boot disk driver, DobFileSystem, the modules service, phase2_init), plus
 * exfat.mem, a minimal Phase-1 Startup_modules, and the Root_volume marker
 * (the exFAT partition index). The rest of the system has already been written
 * to the exFAT root by the earlier phases. No-op for a classic FAT32 install.
 *
 * Writes here target FAT32 (install_tick set g_target_service back to the
 * FAT32 service for this phase). */
static bool
install_phase_boot_stub(void)
{
    if (!g_split)
    {
        install_log_add("[OK] Nessuno stub (installazione FAT32 classica)");
        return true;
    }

    install_mkdir_p_target("/SYSTEM");
    install_mkdir_p_target("/SYSTEM/CONFIG");
    install_mkdir_p_target("/SYSTEM/OperatingSystem/DobFileSystem");
    install_mkdir_p_target("/SYSTEM/PROGRAMS/modules");
    install_mkdir_p_target("/SYSTEM/PROGRAMS/phase2_init");

    /* DobFileSystem + its exFAT companion. */
    if (!install_copy_file("/SYSTEM/OperatingSystem/DobFileSystem/DobFileSystem.mdl",
                           "/SYSTEM/OperatingSystem/DobFileSystem/DobFileSystem.mdl"))
    {
        install_log_add("Errore: copia DobFileSystem.mdl sullo stub fallita");
        return false;
    }
    install_copy_optional("/SYSTEM/OperatingSystem/DobFileSystem/manifest.dob",
                          "/SYSTEM/OperatingSystem/DobFileSystem/manifest.dob");
    if (!install_copy_file("/SYSTEM/OperatingSystem/DobFileSystem/exfat.mem",
                           "/SYSTEM/OperatingSystem/DobFileSystem/exfat.mem"))
    {
        install_log_add("Errore: copia exfat.mem sullo stub fallita");
        return false;
    }

    /* modules service + phase2_init loader. */
    if (!install_copy_file("/SYSTEM/PROGRAMS/modules/modules.mdl",
                           "/SYSTEM/PROGRAMS/modules/modules.mdl"))
    {
        install_log_add("Errore: copia modules.mdl sullo stub fallita");
        return false;
    }
    install_copy_optional("/SYSTEM/PROGRAMS/modules/manifest.dob",
                          "/SYSTEM/PROGRAMS/modules/manifest.dob");
    if (!install_copy_file("/SYSTEM/PROGRAMS/phase2_init/phase2_init.mdl",
                           "/SYSTEM/PROGRAMS/phase2_init/phase2_init.mdl"))
    {
        install_log_add("Errore: copia phase2_init.mdl sullo stub fallita");
        return false;
    }

    /* Boot disk driver (ata or ahci) — the kernel needs it on FAT32. */
    {
        char ddir[96], dpath[160], dman[160];
        snprintf(ddir,  sizeof(ddir),  "/SYSTEM/DRIVERS/%s", g_boot_provider);
        snprintf(dpath, sizeof(dpath), "/SYSTEM/DRIVERS/%s/%s.mdl",
                 g_boot_provider, g_boot_provider);
        snprintf(dman,  sizeof(dman),  "/SYSTEM/DRIVERS/%s/manifest.dob",
                 g_boot_provider);
        install_mkdir_p_target(ddir);
        if (!install_copy_file(dpath, dpath))
        {
            install_log_add("Errore: copia driver disco sullo stub fallita");
            return false;
        }
        install_copy_optional(dman, dman);
    }

    /* Minimal Phase-1 Startup_modules on FAT32: the kernel loads only these
     * from FAT32. DobFileSystem then pivots the root to exFAT and phase2_init
     * launches everything else from the exFAT Startup_modules. */
    {
        char sm[512];
        snprintf(sm, sizeof(sm),
            "# MainDOB Phase-1 boot stub (root-on-exFAT).\n"
            "# Loads only what is needed to mount the exFAT root; phase2_init\n"
            "# then launches the rest from the exFAT Startup_modules.\n"
            "/SYSTEM/DRIVERS/%s/%s.mdl\tdriver primary\n"
            "/SYSTEM/OperatingSystem/DobFileSystem/DobFileSystem.mdl\tprimary\n"
            "/SYSTEM/PROGRAMS/modules/modules.mdl\tprimary\n"
            "/SYSTEM/PROGRAMS/phase2_init/phase2_init.mdl\tdriver needs:DobFileSystem\n",
            g_boot_provider, g_boot_provider);
        if (!install_write_string("/SYSTEM/CONFIG/Startup_modules", sm))
        {
            install_log_add("Errore: scrittura Startup_modules stub fallita");
            return false;
        }
    }

    /* Root_volume marker: the exFAT partition index. DobFileSystem re-derives
     * the absolute LBA from the MBR at boot and pivots the root onto it. */
    {
        char rv[16];
        snprintf(rv, sizeof(rv), "%d\n", g_exfat_part);
        if (!install_write_string("/SYSTEM/CONFIG/Root_volume", rv))
        {
            install_log_add("Errore: scrittura marcatore Root_volume fallita");
            return false;
        }
    }

    install_log_add("Stub FAT32 scritto (root su exFAT)");
    return true;
}

/* Stub for phases not yet implemented. Logs a single "[OK] ... (stub)"
 * line and returns success so the wizard keeps moving. Each stub gets
 * replaced by its real implementation one turn at a time. */
static bool
install_phase_stub(void)
{
    char line[96];
    snprintf(line, sizeof(line), "[OK] %s (stub)", INSTALL_PHASE_NAMES[install_phase]);
    install_log_add(line);
    return true;
}

static void
install_tick(void)
{
    if (!install_seeded || install_halted) return;
    if (install_phase >= INST_PHASE_COUNT)  return;

    char dbg[160];
    snprintf(dbg, sizeof(dbg),
             "[MainDOB_Setup] phase %d/%d: %s\n",
             (int)install_phase + 1, INST_PHASE_COUNT,
             INSTALL_PHASE_NAMES[install_phase]);
    debug_print(dbg);

    /* Route writes. In a split install the system phases land on the exFAT
     * root; GRUB, the kernel, the boot stub and everything in a classic
     * install go to the FAT32 partition. */
    switch (install_phase)
    {
        case INST_PHASE_OS:
        case INST_PHASE_DRIVERS:
        case INST_PHASE_PROGRAMS:
        case INST_PHASE_CONFIG:
            g_target_service = g_split ? TARGET_ROOT_SERVICE : TARGET_SERVICE;
            break;
        default:
            g_target_service = TARGET_SERVICE;
            break;
    }

    bool ok;
    switch (install_phase)
    {
        case INST_PHASE_VERIFY:    ok = install_phase_verify();    break;
        case INST_PHASE_GRUB:      ok = install_phase_grub();      break;
        case INST_PHASE_KERNEL:    ok = install_phase_kernel();    break;
        case INST_PHASE_OS:        ok = install_phase_os();        break;
        case INST_PHASE_DRIVERS:   ok = install_phase_drivers();   break;
        case INST_PHASE_PROGRAMS:  ok = install_phase_programs();  break;
        case INST_PHASE_CONFIG:    ok = install_phase_config();    break;
        case INST_PHASE_BOOTSTUB:  ok = install_phase_boot_stub(); break;
        case INST_PHASE_FINALIZE:  ok = install_phase_finalize();  break;
        default:                   ok = install_phase_stub();      break;
    }

    if (!ok)
    {
        install_halted = true;
        debug_print("[MainDOB_Setup] install halted on failure\n");
        dobui_set_tick(0);
        redraw();
        return;
    }

    install_phase = (install_phase_t)((int)install_phase + 1);

    if (install_phase >= INST_PHASE_COUNT)
    {
        debug_print("[MainDOB_Setup] install complete -> STEP_DONE\n");
        dobui_set_tick(0);
        current_step = STEP_DONE;
    }
    redraw();
}

/* ===== Done step =====
 *
 * Terminal screen. The single action available is "Riavvia ora",
 * which fires SYS_REBOOT (KBC 0x64 ← 0xFE). The user can also close
 * the window via Annulla in the chrome, which dobui_quit's and
 * returns control to the live shell. */

static void
done_render(void)
{
    hide_all_controls();
    put_title("Installazione completata");

    put_line(0, BODY_Y,              "MainDOB e' stato installato con successo.",                  COL_BODY);
    put_line(1, BODY_Y + LINE_H,     "Rimuovi il CD dal lettore e riavvia per avviare il sistema", COL_BODY);
    put_line(2, BODY_Y + LINE_H * 2, "installato dal disco.",                                       COL_BODY);

    btn_reboot.x       = (WIN_W - 220) / 2;
    btn_reboot.y       = BODY_Y + 100;
    btn_reboot.visible = true;
    dobbtn_Draw(&btn_reboot);
}

/* ===== Top-level redraw ===== */

static void
redraw(void)
{
    /* Wipe any open dropdown ghost (popups that closed since the
     * last frame), repaint background, then let the step paint
     * itself. Finally commit with Invalidate — libdobui controls
     * batch into a draw-list and need the explicit flush. */
    dobdd_ClearGhost(&dd_keymap);
    for (int c = 0; c < DAS_CAT_COUNT; c++)
        dobdd_ClearGhost(&dd_avail_cat[c]);

    dobui_FillRect(win_id, 0, 0, win_w, win_h, COL_BG);
    STEPS[current_step].render();

    dobui_Invalidate(win_id);
}

/* ===== Event handlers (libdobui app framework) ===== */

void event_start(void)
{
    win_id = dobui_window();
    ui_init_once();
    /* Attach the panel to the focus manager so focused textboxes can
     * swap in clipboard commands. Base commands return as soon as
     * focus moves off any text widget. */
    dobfocus_attach_panel(win_id, "Indietro\nAvanti\nAnnulla");
    redraw();
}

void event_resize(int w, int h)
{
    win_w = w;
    win_h = h;
    redraw();
}

void event_mouseclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    /* Focus manager hit-tests every registered control and dispatches
     * the click to the one under the cursor (respecting per-control
     * visibility). Anything not consumed falls on the window
     * background, which defocuses the active textbox and brings the
     * base panel back. */
    void *clicked = dobfocus_click(x, y);
    /* DAS step has per-category Aggiungi/Rimuovi buttons that need
     * an extra dispatch round — the focus manager routes the click
     * to the button instance, but only this app knows what category
     * + action that maps to. */
    if (current_step == STEP_DAS)
        das_handle_button_click(clicked);
    /* STEP_DONE: reboot button is the only interactive widget. */
    if (current_step == STEP_DONE && clicked == &btn_reboot)
    {
        debug_print("[MainDOB_Setup] reboot requested via SYS_REBOOT\n");
        syscall0(SYS_REBOOT);
        /* SYS_REBOOT shouldn't return; if it does the kernel rejected
         * us (probably we're not a driver — which we should be from
         * manifest driver=true). Fall through and let the user retry. */
    }
    redraw();
}

void event_mouserelease(int x, int y, uint8_t buttons)
{
    (void)x; (void)y; (void)buttons;
    dobfocus_release();
    redraw();
}

void event_mousemove(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (dobfocus_drag(x, y))
        redraw();
}

void event_dblclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (dobfocus_dblclick(x, y))
        redraw();
}

void event_key(uint8_t key)
{
    dobfocus_key(key);
    redraw();
}

void event_scroll(int delta)
{
    if (dobfocus_scroll(delta))
        redraw();
}

void event_panel(int idx)
{
    /* Contextual (textbox clipboard) panel commands take priority. */
    if (dobfocus_panel(idx))
    {
        redraw();
        return;
    }

    const step_def_t *s = &STEPS[current_step];

    switch (idx)
    {
        case PANEL_BACK:
            if (s->has_back && current_step > 0)
            {
                current_step--;
                redraw();
            }
            break;

        case PANEL_NEXT:
            if (!s->has_next) break;
            if (s->on_next && !s->on_next()) break;
            if (current_step + 1 < STEP_COUNT)
            {
                current_step++;
                redraw();
            }
            break;

        case PANEL_CANCEL:
            /* Friendly confirmation popup is on the TODO list for
             * the summary step; for now Annulla closes immediately. */
            dobui_quit();
            break;

        default:
            break;
    }
}

void event_close(void)
{
    dobui_quit();
}

/* Tick callback — fires every dobui_set_tick(N) ms. Used only by the
 * install state machine; install_tick is a no-op outside STEP_INSTALL,
 * but the guard here keeps an accidental future tick consumer from
 * waking the install engine. */
void event_tick(void)
{
    if (current_step == STEP_INSTALL)
        install_tick();
}

/* ===== Entry point ===== */

int main(void)
{
    if (!in_live_mode())
        return 0;

    dobui_set_panel("Indietro\nAvanti\nAnnulla");
    dobui_run("MainDOB_Setup", WIN_W, WIN_H);
    return 0;
}
