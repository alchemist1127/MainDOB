/* MainDOB DobInstaller — installs .dbp packages.
 *
 * Spawned by DobFiles association on .dbp files with argv = { dbp_path, NULL }.
 * Alternative invocation: argv = { "--uninstall", bubble_dir, NULL }.
 * Runs as driver (manifest.dob driver=true) so it can write into
 * /SYSTEM/PROGRAMS/, /SYSTEM/DRIVERS/ and (via the whitelist entry in
 * DobFileSystem's config_area_allowed) /SYSTEM/CONFIG/.
 *
 * Flow:
 *  1. Read target path from argv[0].
 *  2. Spawn DobArchive as unprivileged child with argv = { "--silent",
 *     dbp_path, NULL } so it skips its own DobFiles mount; wait for
 *     registration.
 *  3. Walk DobArchive's entry table; classify each entry (MODULI /
 *     DRIVERS / DAS / ASSOCIATIONS / STARTUP / RESOURCES / MANIFEST /
 *     DESCRIPTION).
 *  4. Read package manifest (priority: PROGRAMS > GAMES > DRIVERS) for
 *     pkg_name and pkg_version.
 *  5. Render summary: classified changes + description + Annulla/Installa.
 *  6. On Installa: warn if the package carries privileged items, then run
 *     phase A (extract files), B (append non-dup Startup_modules lines),
 *     C (merge associations into DobConfig), D (write ModuleFiles receipt).
 *  7. Upgrade path: if a matching bubble exists, confirm and reverse-apply
 *     its ModuleFiles (uninstall) before the new install.
 *
 * Layers (each only calls below):
 *   L0 arc_*       DobArchive child lifecycle
 *   L1 read_*      entry info / full-entry helpers
 *   L2 classify_*  categorise entries + parse manifest
 *   L3 fs_*        thin helpers on dobfs_* (mkdir_p, write_all, ...)
 *   L4 version_*   dotted-octet version parse + compare
 *   L5 bubble_*    find_existing_bubble, pick_main_bubble
 *   L6 uninstall_* reverse-apply a ModuleFiles receipt
 *   L7 phase_*     install phases A..D
 *   L8 ui_*        summary + warning + progress screens
 *   L9 main        bootstrap */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <dob/types.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/spawn.h>

#include <app.h>
#include <DobInterface.h>
#include <DobFileSystem.h>
#include <DobConfig.h>
#include <DobPopup.h>
#include <DobArchive.h>

#include <textbox.h>
#include <button.h>
#include <label.h>
#include <progressbar.h>

/* Tunables */

#define WIN_W                 620
#define WIN_H                 480

#define MAX_ENTRIES           4096     /* mirror of DobArchive cap */
#define MAX_ARCHIVE_SLOTS     16
#define DOBARCHIVE_PATH       "/SYSTEM/PROGRAMS/DobArchive/DobArchive.mdl"
#define REGISTRY_WAIT_MS      3000

#define MAX_DESC_LEN          2048
#define MAX_MANIFEST_LEN       512
#define MAX_SUMMARY_LEN      16384     /* well under DOBMT_MAX_TEXT (32K) */

/* Colors */
#include <dobui_theme.h>
#define COL_BG                DOBUI_SURFACE
#define COL_TEXT              DOBUI_TEXT_ALT
#define COL_HEADER            DOBUI_RELIEF

/* Window layout */
#define PAD                   12
#define HEADER_H              28
#define BTN_W                 110
#define BTN_H                 28
#define BTN_BOTTOM_PAD        14

/* State */

/* Path of the .dbp we were asked to install */
static char   dbp_path[256];

/* Name of the archiveN slot DobArchive registered under */
static char   archive_service[16];

/* Entry indices we care about, filled during classification */
typedef enum
{
    CAT_NONE = 0,
    CAT_MODULE,        /* module binary in a bubble          */
    CAT_DRIVER,        /* driver binary under DRIVERS        */
    CAT_DAS,           /* Device Automation Script           */
    CAT_STARTUP,       /* Startup_modules (root)             */
    CAT_ASSOC,         /* Associations (root)                */
    CAT_DESCRIPTION,   /* description.txt (root)             */
    CAT_MANIFEST,      /* manifest.dob or .manifest in bubble */
    CAT_RESOURCE,      /* anything else under /SYSTEM/        */
    CAT_SETTING,       /* .setting file -> central DobSettings dir   */
    CAT_MEM            /* .mem companion -> installed beside its binary */
} category_t;

typedef struct
{
    uint32_t    index;
    uint32_t    size;                    /* uncompressed size in bytes */
    category_t  cat;
    char        path[ARCHIVE_MAX_PATH];  /* absolute path in archive */
} ent_info_t;

static ent_info_t entries[MAX_ENTRIES];
static uint32_t   entry_count;

/* Package metadata extracted from the (first) .manifest found */
static char   pkg_name[64];
static char   pkg_version[32];
static char   pkg_type[16];          /* "program" | "game" | "driver" */
static bool   pkg_has_manifest;

/* Flags that trigger the privilege warning */
static bool   has_drivers;
static bool   has_das;
static bool   has_privileged_startup;  /* startup line with driver/primary */

/* UI widgets */
static dob_label_t        lbl_header;
static dob_multitextbox_t txt_summary;
static dob_button_t       btn_cancel;
static dob_button_t       btn_install;

/* Progress screen widgets (shown during install) */
static dob_label_t        lbl_progress_phase;
static dob_label_t        lbl_progress_detail;
static dob_progressbar_t  pb_install;

/* Which screen we're showing. Drives both drawing and event routing. */
typedef enum
{
    SCREEN_SUMMARY = 0,
    SCREEN_PROGRESS
} screen_t;
static screen_t cur_screen = SCREEN_SUMMARY;

/* Pre-rendered summary text (persists for the textbox's internal buffer) */
static char   summary_buf[MAX_SUMMARY_LEN];
static int    summary_len;

/* L0 — DobArchive child lifecycle */

/* Snapshot which archive slots are in use before we spawn our child,
 * so that after the spawn we can pick the *new* slot it registered. */
static uint16_t existing_slots_mask;

static void
arc_snapshot_slots(void)
{
    existing_slots_mask = 0;
    for (uint32_t n = 0; n < MAX_ARCHIVE_SLOTS; n++)
    {
        char name[16];
        snprintf(name, sizeof(name), "archive%u", n);
        if (dob_registry_find(name) != 0)
            existing_slots_mask |= (1u << n);
    }
}

/* Poll the registry for a slot that wasn't taken before we spawned.
 * Returns true (and fills out service_name) when found. */
static bool
arc_wait_new_slot(char *out, uint32_t out_cap, uint32_t timeout_ms)
{
    /* Rough busy-wait: registry_wait() per slot would block the whole
     * loop on the first missing one. We instead poll all 16 slots with
     * a short sleep between passes. */
    const uint32_t step_ms = 50;
    uint32_t waited = 0;
    while (waited < timeout_ms)
    {
        for (uint32_t n = 0; n < MAX_ARCHIVE_SLOTS; n++)
        {
            if (existing_slots_mask & (1u << n)) continue;
            char name[16];
            snprintf(name, sizeof(name), "archive%u", n);
            if (dob_registry_find(name) != 0)
            {
                strncpy(out, name, out_cap - 1);
                out[out_cap - 1] = '\0';
                return true;
            }
        }
        sleep_ms(step_ms);
        waited += step_ms;
    }
    return false;
}

static bool
arc_open_dbp(void)
{
    arc_snapshot_slots();

    /* DobArchive is spawned as an unprivileged child: it's just our
     * backend for reading the .dbp archive. All writes to /SYSTEM/ go
     * through us (we're the driver with the whitelist entry), using
     * dobarchive_ReadEntry + dobfs_Write rather than ExtractTo.
     * "--silent" keeps DobArchive headless: no file-explorer window
     * on top of the .dbp. */
    const char *argv[] = { "--silent", dbp_path, NULL };
    if (spawn_file(DOBARCHIVE_PATH, argv) < 0)
    {
        dobpopup_Error("DobInstaller",
                       "Impossibile avviare DobArchive.");
        return false;
    }

    bool ok = arc_wait_new_slot(archive_service, sizeof(archive_service),
                                REGISTRY_WAIT_MS);

    if (!ok)
    {
        dobpopup_Error("DobInstaller",
            "DobArchive non ha risposto entro il timeout.\n"
            "Verifica che il pacchetto .dbp sia un archivio ZIP valido.");
        return false;
    }
    return true;
}

static void
arc_close(void)
{
    if (archive_service[0])
        dobarchive_Close(archive_service);
}

/* L1 — Read helpers */

/* Read an entire entry by index into buf. Returns bytes read, or -1. */
static int
read_full_entry(uint32_t index, void *buf, uint32_t cap)
{
    uint32_t off = 0;
    uint8_t *p = (uint8_t *)buf;
    while (off < cap)
    {
        int n = dobarchive_ReadEntry(archive_service, index, off,
                                     p + off, cap - off);
        if (n < 0) return -1;
        if (n == 0) break;         /* EOF */
        off += (uint32_t)n;
    }
    return (int)off;
}

/* Find an entry by exact path. Linear search of our local table, which
 * is fine for the few dozen entries a .dbp typically has. Returns
 * index into `entries[]`, or -1 if not found. */
static int
find_entry_by_path(const char *path)
{
    for (uint32_t i = 0; i < entry_count; i++)
        if (strcmp(entries[i].path, path) == 0)
            return (int)i;
    return -1;
}

/* L2 — Classification + manifest parsing */

/* path_starts_with("/SYSTEM/PROGRAMS/", "/SYSTEM/PROGRAMS/foo/bar") -> true */
static bool
path_starts_with(const char *path, const char *prefix)
{
    uint32_t n = (uint32_t)strlen(prefix);
    return strncmp(path, prefix, n) == 0;
}

static bool
path_ends_with(const char *path, const char *suffix)
{
    uint32_t plen = (uint32_t)strlen(path);
    uint32_t slen = (uint32_t)strlen(suffix);
    if (slen > plen) return false;
    return strcmp(path + plen - slen, suffix) == 0;
}

/* A top-level entry is at the root of the archive (no '/' after any
 * leading '/'). DobArchive normalises entries to leading '/'. */
static bool
is_root_entry(const char *path)
{
    const char *p = path;
    if (*p == '/') p++;
    while (*p && *p != '/') p++;
    return (*p == '\0');
}

/* Return just the filename portion (after the last '/'). */
static const char *
basename_of(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') b = p + 1;
    return b;
}

/* The central directory settingsd scans at boot; every .setting lives
 * here regardless of where the package shipped it. Must match
 * SETTINGS_DIR in boot/settingsd/main.c. */
#define SETTINGS_CENTRAL_DIR  "/SYSTEM/PROGRAMS/DobSettings/"

static category_t
classify_path(const char *path, uint8_t type)
{
    /* Skip directory entries — we only need files for the summary. */
    if (type == ARCHIVE_TYPE_DIR) return CAT_NONE;

    /* Settings and .mem companions are recognized by extension: the
     * formats post-date the original path-based scheme, so we match them
     * up front wherever they appear. A .setting is relocated into the
     * central DobSettings directory at extract time (so settingsd
     * registers it on the next boot); a .mem stays beside its binary and
     * is only accepted under /SYSTEM/. */
    if (path_ends_with(path, ".setting")) return CAT_SETTING;
    if (path_ends_with(path, ".mem") && path_starts_with(path, "/SYSTEM/"))
        return CAT_MEM;

    /* Root-level metadata */
    if (is_root_entry(path))
    {
        const char *name = basename_of(path);
        if (strcmp(name, "description.txt") == 0) return CAT_DESCRIPTION;
        if (strcmp(name, "Startup_modules") == 0) return CAT_STARTUP;
        if (strcmp(name, "Associations")    == 0) return CAT_ASSOC;
        return CAT_NONE;   /* unknown root entry, ignore silently */
    }

    /* Under /SYSTEM/ */
    if (path_starts_with(path, "/SYSTEM/DRIVERS/"))
    {
        if (path_ends_with(path, ".mdl"))            return CAT_DRIVER;
        if (path_ends_with(path, ".manifest") ||
            path_ends_with(path, "/manifest.dob"))   return CAT_MANIFEST;
        return CAT_RESOURCE;
    }
    if (path_starts_with(path, "/SYSTEM/PROGRAMS/") ||
        path_starts_with(path, "/SYSTEM/GAMES/"))
    {
        if (path_ends_with(path, ".mdl"))            return CAT_MODULE;
        if (path_ends_with(path, ".manifest") ||
            path_ends_with(path, "/manifest.dob"))   return CAT_MANIFEST;
        return CAT_RESOURCE;
    }
    if (path_starts_with(path, "/SYSTEM/CONFIG/DAS/"))
    {
        if (path_ends_with(path, ".das")) return CAT_DAS;
        return CAT_RESOURCE;
    }

    /* Anything else under /SYSTEM/ is a generic resource. */
    if (path_starts_with(path, "/SYSTEM/")) return CAT_RESOURCE;

    /* Outside /SYSTEM/ and not root metadata — silently ignored.
     * The installer is deliberately strict about where things go. */
    return CAT_NONE;
}

/* Resolve the on-disk install destination for an entry. Everything
 * installs verbatim at its archive path except a .setting, which is
 * relocated into the central DobSettings directory (keyed by basename)
 * so settingsd discovers and registers it. The relocated path is what
 * gets recorded in the ModuleFiles receipt, so uninstall removes the
 * file from the central directory too. */
static void
resolve_dest(const ent_info_t *e, char *out, uint32_t cap)
{
    if (e->cat == CAT_SETTING)
        snprintf(out, cap, "%s%s", SETTINGS_CENTRAL_DIR, basename_of(e->path));
    else
        snprintf(out, cap, "%s", e->path);
}

/* Parse a .manifest file buffer for: name=, version=, type=.
 * Mutates `buf` (null-terminates lines in place). */
static void
parse_manifest_buf(char *buf, int len)
{
    buf[len] = '\0';
    char *line = buf;
    while (*line)
    {
        char *eol = line;
        while (*eol && *eol != '\n') eol++;
        char saved = *eol;
        *eol = '\0';

        if (line[0] != '#' && line[0] != '\0')
        {
            char *eq = strchr(line, '=');
            if (eq)
            {
                *eq = '\0';
                const char *key = line;
                const char *val = eq + 1;
                if (strcmp(key, "name") == 0)
                    strncpy(pkg_name, val, sizeof(pkg_name) - 1);
                else if (strcmp(key, "version") == 0)
                    strncpy(pkg_version, val, sizeof(pkg_version) - 1);
                else if (strcmp(key, "type") == 0)
                    strncpy(pkg_type, val, sizeof(pkg_type) - 1);
            }
        }

        if (saved == '\0') break;
        line = eol + 1;
    }
}

/* Walk Startup_modules text, return true if any line has flag
 * "driver" or "primary" (bare-token match, not substring).        */
static bool
startup_has_privileged_flag(const char *text, int len)
{
    /* Parse line by line. Format: path\tflags (tab-separated). */
    const char *p = text;
    const char *end = text + len;
    while (p < end)
    {
        /* skip leading whitespace */
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end) break;

        /* comment or blank */
        if (*p == '#' || *p == '\n')
        {
            while (p < end && *p != '\n') p++;
            if (p < end) p++;
            continue;
        }

        /* skip path (until tab or newline) */
        while (p < end && *p != '\t' && *p != '\n') p++;
        if (p >= end || *p == '\n') { if (p < end) p++; continue; }
        /* *p == '\t': we're now in the flags portion */
        p++;

        const char *flags_start = p;
        const char *line_end = flags_start;
        while (line_end < end && *line_end != '\n') line_end++;

        /* Split flags on whitespace, compare each token. */
        const char *tok = flags_start;
        while (tok < line_end)
        {
            while (tok < line_end && (*tok == ' ' || *tok == '\t')) tok++;
            const char *tok_end = tok;
            while (tok_end < line_end && *tok_end != ' ' && *tok_end != '\t')
                tok_end++;
            uint32_t tl = (uint32_t)(tok_end - tok);
            if ((tl == 6 && strncmp(tok, "driver", 6) == 0) ||
                (tl == 7 && strncmp(tok, "primary", 7) == 0))
                return true;
            tok = tok_end;
        }

        p = (line_end < end) ? line_end + 1 : line_end;
    }
    return false;
}

/* Populate entries[], package metadata, and warning flags. */
static bool
classify_all(void)
{
    entry_count = 0;

    uint32_t count;
    uint8_t  fmt;
    if (dobarchive_Count(archive_service, &count, &fmt) != 0)
    {
        dobpopup_Error("DobInstaller",
                       "Errore nella lettura del contenuto dell'archivio.");
        return false;
    }
    if (fmt != ARCHIVE_FMT_ZIP)
    {
        /* .dbp is defined as a ZIP. TAR dobp files are not supported. */
        dobpopup_Error("DobInstaller",
            "Questo non e' un pacchetto .dbp valido:\n"
            "i pacchetti DobProject devono essere archivi ZIP.");
        return false;
    }

    for (uint32_t i = 0; i < count && entry_count < MAX_ENTRIES; i++)
    {
        archive_entry_info_t info;
        if (dobarchive_EntryInfo(archive_service, i, &info) != 0) continue;

        category_t cat = classify_path(info.path, info.type);
        if (cat == CAT_NONE) continue;

        ent_info_t *e = &entries[entry_count++];
        e->index = i;
        e->size  = info.size;
        e->cat   = cat;
        strncpy(e->path, info.path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';

        if (cat == CAT_DRIVER) has_drivers = true;
        if (cat == CAT_DAS)    has_das     = true;
    }

    /* Find the "principal" manifest: prefer one under PROGRAMS, then
     * GAMES, then DRIVERS. Without this priority, archive iteration
     * order can pick the manifest of an accessory driver instead of
     * the package's real identity, breaking pkg_name/version checks. */
    {
        static const char *prio[] = {
            "/SYSTEM/PROGRAMS/", "/SYSTEM/GAMES/", "/SYSTEM/DRIVERS/", NULL
        };
        int chosen = -1;
        for (uint32_t p = 0; prio[p] && chosen < 0; p++)
        {
            uint32_t plen = (uint32_t)strlen(prio[p]);
            for (uint32_t i = 0; i < entry_count; i++)
            {
                if (entries[i].cat != CAT_MANIFEST) continue;
                if (strncmp(entries[i].path, prio[p], plen) == 0)
                {
                    chosen = (int)i;
                    break;
                }
            }
        }
        if (chosen >= 0)
        {
            char mbuf[MAX_MANIFEST_LEN];
            int n = read_full_entry(entries[chosen].index, mbuf, sizeof(mbuf) - 1);
            if (n > 0)
            {
                parse_manifest_buf(mbuf, n);
                pkg_has_manifest = true;
            }
        }
    }

    /* Scan Startup_modules for privileged flags. */
    int si = find_entry_by_path("/Startup_modules");
    if (si < 0) si = find_entry_by_path("Startup_modules");
    if (si >= 0)
    {
        static char sbuf[4096];
        int n = read_full_entry(entries[si].index, sbuf, sizeof(sbuf));
        if (n > 0) has_privileged_startup = startup_has_privileged_flag(sbuf, n);
    }

    /* Fallback package name: basename of the .dbp without extension. */
    if (pkg_name[0] == '\0')
    {
        const char *b = basename_of(dbp_path);
        strncpy(pkg_name, b, sizeof(pkg_name) - 1);
        char *dot = strrchr(pkg_name, '.');
        if (dot) *dot = '\0';
    }

    return true;
}

/* L3 — UI: summary dialog */

/* Simple string append — returns new length. Clamps at cap-1. */
static int
append_str(char *buf, int cap, int len, const char *s)
{
    while (len < cap - 1 && *s) buf[len++] = *s++;
    buf[len] = '\0';
    return len;
}

/* Append one category's entries to the summary text. */
static int
append_category(char *buf, int cap, int len,
                const char *heading, category_t cat)
{
    /* First pass: count, to decide whether to emit the heading at all. */
    uint32_t n_in_cat = 0;
    for (uint32_t i = 0; i < entry_count; i++)
        if (entries[i].cat == cat) n_in_cat++;
    if (n_in_cat == 0) return len;

    len = append_str(buf, cap, len, heading);
    len = append_str(buf, cap, len, ":\n");
    for (uint32_t i = 0; i < entry_count; i++)
    {
        if (entries[i].cat != cat) continue;
        len = append_str(buf, cap, len, "  * ");
        len = append_str(buf, cap, len, basename_of(entries[i].path));
        len = append_str(buf, cap, len, "\n");
    }
    len = append_str(buf, cap, len, "\n");
    return len;
}

/* Append raw file content (Startup_modules, Associations) verbatim. */
static int
append_raw_file(char *buf, int cap, int len,
                const char *heading, const char *archive_path)
{
    int ei = find_entry_by_path(archive_path);
    if (ei < 0)
    {
        /* DobArchive may or may not prefix with '/' — try both. */
        char alt[ARCHIVE_MAX_PATH];
        if (archive_path[0] == '/')
            snprintf(alt, sizeof(alt), "%s", archive_path + 1);
        else
            snprintf(alt, sizeof(alt), "/%s", archive_path);
        ei = find_entry_by_path(alt);
    }
    if (ei < 0) return len;

    len = append_str(buf, cap, len, heading);
    len = append_str(buf, cap, len, " (contenuto del file):\n");
    char fbuf[2048];
    int n = read_full_entry(entries[ei].index, fbuf, sizeof(fbuf) - 1);
    if (n > 0)
    {
        fbuf[n] = '\0';
        len = append_str(buf, cap, len, fbuf);
        if (n > 0 && fbuf[n-1] != '\n')
            len = append_str(buf, cap, len, "\n");
    }
    len = append_str(buf, cap, len, "\n");
    return len;
}

/* Build the summary text shown in the multiline textbox. */
static void
build_summary_text(void)
{
    int len = 0;
    int cap = (int)sizeof(summary_buf);

    len = append_category(summary_buf, cap, len, "MODULI",       CAT_MODULE);
    len = append_category(summary_buf, cap, len, "DRIVERS",      CAT_DRIVER);
    len = append_category(summary_buf, cap, len, "DAS",          CAT_DAS);
    len = append_category(summary_buf, cap, len, "SETTINGS",     CAT_SETTING);
    len = append_category(summary_buf, cap, len, "MEM",          CAT_MEM);
    len = append_raw_file(summary_buf, cap, len, "STARTUP",
                          "/Startup_modules");
    len = append_raw_file(summary_buf, cap, len, "ASSOCIATIONS",
                          "/Associations");

    /* Description at the bottom, if present. */
    int di = find_entry_by_path("/description.txt");
    if (di < 0) di = find_entry_by_path("description.txt");
    if (di >= 0)
    {
        len = append_str(summary_buf, cap, len, "---\nDESCRIZIONE:\n");
        char dbuf[MAX_DESC_LEN];
        int n = read_full_entry(entries[di].index, dbuf, sizeof(dbuf) - 1);
        if (n > 0)
        {
            dbuf[n] = '\0';
            len = append_str(summary_buf, cap, len, dbuf);
            if (n > 0 && dbuf[n-1] != '\n')
                len = append_str(summary_buf, cap, len, "\n");
        }
    }

    summary_len = len;
}

/* Build the window title / header line. */
static void
build_header_text(char *out, uint32_t cap)
{
    if (pkg_version[0])
        snprintf(out, cap, "Installare '%s' v%s?",
                 pkg_name, pkg_version);
    else
        snprintf(out, cap, "Installare '%s'?", pkg_name);
}

/* L5 — Filesystem helpers for install pipeline */

/* mkdir -p: create every missing component of `path`. `path` is expected
 * to be absolute (starts with '/'). We operate on dir components only;
 * the caller strips the filename first. */
static bool
fs_mkdir_p(const char *path)
{
    char buf[ARCHIVE_MAX_PATH];
    uint32_t plen = (uint32_t)strlen(path);
    if (plen >= sizeof(buf)) return false;
    strncpy(buf, path, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';

    /* Walk the path, terminating at each '/' and mkdir'ing up to there.
     * Skip the leading '/'. */
    for (uint32_t i = 1; i < plen; i++)
    {
        if (buf[i] == '/')
        {
            buf[i] = '\0';
            /* Ignore errors — the directory may already exist.
             * A real failure will surface on the write that follows. */
            dobfs_Mkdir(buf);
            buf[i] = '/';
        }
    }
    /* mkdir the final component too (no trailing slash). */
    dobfs_Mkdir(buf);
    return true;
}

/* Strip filename from an absolute path, leaving only the directory. */
static void
path_dirname(const char *path, char *out, uint32_t cap)
{
    strncpy(out, path, cap - 1);
    out[cap - 1] = '\0';
    char *last = NULL;
    for (char *p = out; *p; p++)
        if (*p == '/') last = p;
    if (last) *last = '\0';
    else out[0] = '\0';
}

/* Write a buffer to a file, overwriting any existing content. */
static bool
fs_write_all(const char *path, const void *data, uint32_t size)
{
    int fd = dobfs_Open(path, FS_WRITE | FS_CREATE | FS_TRUNC);
    if (fd < 0) return false;
    uint32_t off = 0;
    const uint8_t *p = (const uint8_t *)data;
    while (off < size)
    {
        int n = dobfs_Write(fd, p + off, size - off);
        if (n <= 0) { dobfs_Close(fd); return false; }
        off += (uint32_t)n;
    }
    dobfs_Close(fd);
    return true;
}

/* Append a buffer to an existing file (creating it if missing). */
static bool
fs_append_all(const char *path, const void *data, uint32_t size)
{
    int fd = dobfs_Open(path, FS_WRITE | FS_CREATE | FS_APPEND);
    if (fd < 0) return false;
    uint32_t off = 0;
    const uint8_t *p = (const uint8_t *)data;
    while (off < size)
    {
        int n = dobfs_Write(fd, p + off, size - off);
        if (n <= 0) { dobfs_Close(fd); return false; }
        off += (uint32_t)n;
    }
    dobfs_Close(fd);
    return true;
}

/* Read a file into a fixed buffer. Returns bytes read, < 0 on error
 * (including file-does-not-exist). */
static int
fs_read_all(const char *path, char *buf, uint32_t cap)
{
    int fd = dobfs_Open(path, FS_READ);
    if (fd < 0) return -1;
    uint32_t off = 0;
    while (off < cap - 1)
    {
        int n = dobfs_Read(fd, buf + off, cap - 1 - off);
        if (n < 0) { dobfs_Close(fd); return -1; }
        if (n == 0) break;
        off += (uint32_t)n;
    }
    dobfs_Close(fd);
    buf[off] = '\0';
    return (int)off;
}

/* Extract one archive entry to `dst_path`, streaming chunks from
 * DobArchive (ReadEntry) into DobFileSystem (Write). This replaces
 * dobarchive_ExtractTo, which would route the write through the
 * DobArchive process — and DobArchive isn't privileged enough to
 * write into /SYSTEM/. We (DobInstaller) are. Returns true iff every
 * byte was transferred. */
static bool
extract_entry_manual(uint32_t entry_idx, const char *dst_path, uint32_t size)
{
    int fd = dobfs_Open(dst_path, FS_WRITE | FS_CREATE | FS_TRUNC);
    if (fd < 0) return false;

    /* 4 KiB stays under typical IPC payload limits while still making
     * the loop fast for small files. Larger would just get fragmented
     * by the dobfs_protocol transport anyway. */
    static uint8_t xfer[4096];

    uint32_t off = 0;
    while (off < size)
    {
        uint32_t chunk = size - off;
        if (chunk > sizeof(xfer)) chunk = sizeof(xfer);

        int got = dobarchive_ReadEntry(archive_service, entry_idx,
                                       off, xfer, chunk);
        if (got <= 0) { dobfs_Close(fd); return false; }

        uint32_t written = 0;
        while (written < (uint32_t)got)
        {
            int wn = dobfs_Write(fd, xfer + written, (uint32_t)got - written);
            if (wn <= 0) { dobfs_Close(fd); return false; }
            written += (uint32_t)wn;
        }
        off += (uint32_t)got;
    }

    dobfs_Close(fd);
    return true;
}

/* L6 — Version comparison + bubble discovery */

/* Compare two dotted version strings (e.g. "1.0.0.0" vs "1.2"). Missing
 * components are treated as 0. Returns <0, 0, or >0. */
static int
version_cmp(const char *a, const char *b)
{
    while (*a || *b)
    {
        unsigned na = 0, nb = 0;
        while (*a >= '0' && *a <= '9') { na = na * 10 + (unsigned)(*a - '0'); a++; }
        while (*b >= '0' && *b <= '9') { nb = nb * 10 + (unsigned)(*b - '0'); b++; }
        if (na != nb) return (na < nb) ? -1 : +1;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if (*a == '\0' && *b == '\0') return 0;
        if (*a == '\0') { /* a exhausted: b may still have digits */ }
        if (*b == '\0') { /* b exhausted */ }
    }
    return 0;
}

/* Parse a manifest buffer (in place) into three output fields. */
static void
parse_manifest_into(char *buf, int len,
                    char *name_out, uint32_t name_cap,
                    char *ver_out,  uint32_t ver_cap,
                    char *type_out, uint32_t type_cap)
{
    if (name_out && name_cap) name_out[0] = '\0';
    if (ver_out  && ver_cap)  ver_out[0]  = '\0';
    if (type_out && type_cap) type_out[0] = '\0';
    buf[len] = '\0';

    char *line = buf;
    while (*line)
    {
        char *eol = line;
        while (*eol && *eol != '\n') eol++;
        char saved = *eol;
        *eol = '\0';

        if (line[0] != '#' && line[0] != '\0')
        {
            char *eq = strchr(line, '=');
            if (eq)
            {
                *eq = '\0';
                const char *key = line;
                const char *val = eq + 1;
                if (name_out && strcmp(key, "name") == 0)
                    strncpy(name_out, val, name_cap - 1);
                else if (ver_out && strcmp(key, "version") == 0)
                    strncpy(ver_out, val, ver_cap - 1);
                else if (type_out && strcmp(key, "type") == 0)
                    strncpy(type_out, val, type_cap - 1);
            }
        }

        if (saved == '\0') break;
        line = eol + 1;
    }
}

/* Given a bubble directory, locate its manifest (manifest.dob or
 * *.manifest) and read version= from it. Returns true if a version
 * was successfully read. */
static bool
read_bubble_version(const char *bubble_dir, char *ver_out, uint32_t cap)
{
    char mpath[ARCHIVE_MAX_PATH];
    char mbuf[MAX_MANIFEST_LEN];

    /* Try manifest.dob first. */
    snprintf(mpath, sizeof(mpath), "%s/manifest.dob", bubble_dir);
    int n = fs_read_all(mpath, mbuf, sizeof(mbuf));
    if (n > 0)
    {
        char tmp_name[64], tmp_type[16];
        parse_manifest_into(mbuf, n, tmp_name, sizeof(tmp_name),
                            ver_out, cap, tmp_type, sizeof(tmp_type));
        return (ver_out[0] != '\0');
    }

    /* Fall back to scanning directory for *.manifest. */
    dobfs_dirent_t ents[32];
    uint32_t ecount = 0;
    if (dobfs_List(bubble_dir, ents, 32, &ecount) < 0) return false;
    for (uint32_t i = 0; i < ecount; i++)
    {
        uint32_t nlen = (uint32_t)strlen(ents[i].name);
        if (nlen > 9 && strcmp(ents[i].name + nlen - 9, ".manifest") == 0)
        {
            snprintf(mpath, sizeof(mpath), "%s/%s", bubble_dir, ents[i].name);
            n = fs_read_all(mpath, mbuf, sizeof(mbuf));
            if (n > 0)
            {
                char tmp_name[64], tmp_type[16];
                parse_manifest_into(mbuf, n, tmp_name, sizeof(tmp_name),
                                    ver_out, cap, tmp_type, sizeof(tmp_type));
                return (ver_out[0] != '\0');
            }
        }
    }
    return false;
}

/* Search /SYSTEM/PROGRAMS, /SYSTEM/GAMES, /SYSTEM/DRIVERS for a bubble
 * whose folder name matches pkg_name. On success, fill bubble_out with
 * the full path (e.g. "/SYSTEM/PROGRAMS/DobExcelDemo") and old_ver_out
 * with the version read from its manifest (empty string if no version
 * could be determined). Returns true iff a bubble was found. */
static bool
find_existing_bubble(char *bubble_out, uint32_t bcap,
                     char *old_ver_out, uint32_t vcap)
{
    static const char *roots[] = {
        "/SYSTEM/PROGRAMS",
        "/SYSTEM/GAMES",
        "/SYSTEM/DRIVERS",
        NULL
    };
    old_ver_out[0] = '\0';

    for (uint32_t r = 0; roots[r]; r++)
    {
        snprintf(bubble_out, bcap, "%s/%s", roots[r], pkg_name);
        dobfs_stat_t st;
        if (dobfs_Stat(bubble_out, &st) == 0 && st.type == FS_TYPE_DIR)
        {
            read_bubble_version(bubble_out, old_ver_out, vcap);
            return true;
        }
    }
    bubble_out[0] = '\0';
    return false;
}

/*  *  L7 — Uninstall (used for upgrade path, and by Module Manager later)
 *
 *  ModuleFiles format (one file per installed package, stored in the
 *  main bubble, written at install time, read at uninstall time):
 *
 *    # DobInstaller ModuleFiles — <pkg>
 *    # (free-form comment block)
 *    MODULI:
 *      /SYSTEM/PROGRAMS/<b>/foo.mdl
 *    ...
 *    STARTUP:
 *      /SYSTEM/DRIVERS/<b>/x.mdl\tdriver needs:hotplug
 *    ASSOCIATIONS:
 *      .docx=/SYSTEM/PROGRAMS/<b>/word.mdl
 *    RESOURCES:
 *      /SYSTEM/PROGRAMS/<b>/icon.bmp
 *
 *  The uninstaller doesn't actually care about the section headings:
 *  it just needs, per line, to know whether it's a filesystem path
 *  (unlink), a startup line (remove from Startup_modules by path
 *  match), or an association (remove by extension).
 */

/* Remove a line from /SYSTEM/CONFIG/Startup_modules whose path field
 * (before any tab/flags) equals `mdl_path`. Rewrites the file in place
 * with the matching line stripped. Returns true on success, false if
 * Startup_modules couldn't be read or written. */
static bool
startup_remove_line(const char *mdl_path)
{
    static char startup[8192];
    int len = fs_read_all("/SYSTEM/CONFIG/Startup_modules",
                          startup, sizeof(startup));
    if (len < 0) return false;

    static char out[8192];
    int olen = 0;

    uint32_t match_len = (uint32_t)strlen(mdl_path);
    int i = 0;
    while (i < len)
    {
        /* Find end of current line */
        int line_start = i;
        while (i < len && startup[i] != '\n') i++;
        int line_end = i;
        if (i < len) i++;   /* consume \n */

        /* Extract path portion of this line. Startup_modules format
         * uses TAB as the path/flags separator, so splitting on space
         * would break paths like "/SYSTEM/PROGRAM FILES/...". */
        int path_end = line_start;
        while (path_end < line_end && startup[path_end] != '\t') path_end++;

        uint32_t pl = (uint32_t)(path_end - line_start);
        bool is_comment = (pl > 0 && startup[line_start] == '#');

        bool is_match = !is_comment &&
                        pl == match_len &&
                        strncmp(startup + line_start, mdl_path, pl) == 0;
        if (is_match) continue;   /* drop this line */

        /* Copy line (including its trailing \n) verbatim */
        int copy_len = line_end - line_start + (i > line_end ? 1 : 0);
        if (olen + copy_len > (int)sizeof(out)) return false;
        memcpy(out + olen, startup + line_start, (uint32_t)copy_len);
        olen += copy_len;
    }

    return fs_write_all("/SYSTEM/CONFIG/Startup_modules", out, (uint32_t)olen);
}

/* Walk a ModuleFiles text and perform reverse operations. `text` is
 * mutated in place (null terminators inserted). */
static void
uninstall_apply_modulefiles(char *text, int len)
{
    text[len] = '\0';

    /* Section tracking: the heading ends with ':' on its own line. We
     * only need distinct behaviour for three sections (STARTUP,
     * ASSOCIATIONS, everything else = filesystem paths), so we keep
     * a single byte state. */
    enum { SEC_FILES, SEC_STARTUP, SEC_ASSOC } section = SEC_FILES;

    char *line = text;
    while (*line)
    {
        char *eol = line;
        while (*eol && *eol != '\n') eol++;
        char saved = *eol;
        *eol = '\0';

        /* Trim leading whitespace and bullet */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '*') { p++; while (*p == ' ') p++; }

        if (*p != '\0' && *p != '#')
        {
            /* Is this a section heading (ends with ':') ? */
            uint32_t plen = (uint32_t)strlen(p);
            if (plen > 1 && p[plen - 1] == ':')
            {
                if (strncmp(p, "STARTUP",      7) == 0) section = SEC_STARTUP;
                else if (strncmp(p, "ASSOCIATIONS", 12) == 0) section = SEC_ASSOC;
                else section = SEC_FILES;
            }
            else
            {
                if (section == SEC_STARTUP)
                {
                    /* Line is "path\tflags" in Startup_modules format;
                     * split on TAB only (paths may contain spaces). */
                    char mpath[ARCHIVE_MAX_PATH];
                    char *tab = p;
                    while (*tab && *tab != '\t') tab++;
                    uint32_t mlen = (uint32_t)(tab - p);
                    if (mlen < sizeof(mpath))
                    {
                        memcpy(mpath, p, mlen);
                        mpath[mlen] = '\0';
                        startup_remove_line(mpath);
                    }
                }
                else if (section == SEC_ASSOC)
                {
                    /* Line is ".ext=path"; strip at '=' and Remove. */
                    char *eq = strchr(p, '=');
                    if (eq)
                    {
                        *eq = '\0';
                        dobconfig_RemoveAssoc(p);
                    }
                }
                else
                {
                    /* A regular file path — unlink. */
                    dobfs_Unlink(p);
                }
            }
        }

        if (saved == '\0') break;
        line = eol + 1;
    }
}

/* Read the ModuleFiles from a bubble and apply its reverse operations.
 * Returns true if a ModuleFiles was found and processed. */
static bool
uninstall_bubble(const char *bubble_dir)
{
    char mfpath[ARCHIVE_MAX_PATH];
    snprintf(mfpath, sizeof(mfpath), "%s/ModuleFiles", bubble_dir);

    static char mf_text[16384];
    int n = fs_read_all(mfpath, mf_text, sizeof(mf_text));
    if (n <= 0) return false;

    uninstall_apply_modulefiles(mf_text, n);
    /* Finally, attempt to remove the (now empty) bubble folder itself.
     * Ignore errors — it may still contain files we didn't track. */
    dobfs_Unlink(bubble_dir);
    return true;
}

/*  *  L8 — Install pipeline
 *
 *  Contract:
 *    - caller has confirmed (summary seen, privilege warning dismissed)
 *    - caller has done any upgrade-uninstall already
 *    - DobArchive child is still up; `archive_service` is still valid
 *    - pkg_name, pkg_version, entries[] are populated
 *
 *  Pipeline stages (each bumps the progress bar):
 *    A. Extract each file entry under SYSTEM (files + directories created on the fly)
 *    B. Append Startup_modules lines (if any) to /SYSTEM/CONFIG/Startup_modules
 *    C. Merge Associations entries into DobConfig
 *    D. Write ModuleFiles into the main bubble
 */

/* Full path to the ModuleFiles receipt, chosen per install. */
static char install_main_bubble[ARCHIVE_MAX_PATH];

/* Build accumulator for the ModuleFiles body as we install. */
static char modulefiles_buf[MAX_SUMMARY_LEN];
static int  modulefiles_len;

static void
mf_append(const char *s)
{
    modulefiles_len = append_str(modulefiles_buf, (int)sizeof(modulefiles_buf),
                                 modulefiles_len, s);
}

/* Determine the primary bubble where the ModuleFiles should live.
 * Priority:
 *   1. A bubble whose folder name equals pkg_name, under PROGRAMS >
 *      GAMES > DRIVERS (so a multi-bubble package can declare which
 *      bubble is its "face" via the name= in its principal manifest).
 *   2. Fallback: first bubble found under PROGRAMS > GAMES > DRIVERS. */
static bool
pick_main_bubble(char *out, uint32_t cap)
{
    static const char *roots[] = {
        "/SYSTEM/PROGRAMS/", "/SYSTEM/GAMES/", "/SYSTEM/DRIVERS/", NULL
    };

    /* Pass 1: match on bubble name == pkg_name */
    if (pkg_name[0])
    {
        for (uint32_t r = 0; roots[r]; r++)
        {
            uint32_t plen = (uint32_t)strlen(roots[r]);
            for (uint32_t i = 0; i < entry_count; i++)
            {
                if (strncmp(entries[i].path, roots[r], plen) != 0) continue;
                const char *p = entries[i].path + plen;
                const char *slash = p;
                while (*slash && *slash != '/') slash++;
                if (*slash != '/') continue;
                uint32_t blen = (uint32_t)(slash - p);
                if (blen == strlen(pkg_name) &&
                    strncmp(p, pkg_name, blen) == 0)
                {
                    uint32_t abs_len = (uint32_t)(slash - entries[i].path);
                    if (abs_len >= cap) continue;
                    memcpy(out, entries[i].path, abs_len);
                    out[abs_len] = '\0';
                    return true;
                }
            }
        }
    }

    /* Pass 2: first bubble in any root (pkg_name unknown or mismatch) */
    for (uint32_t r = 0; roots[r]; r++)
    {
        uint32_t plen = (uint32_t)strlen(roots[r]);
        for (uint32_t i = 0; i < entry_count; i++)
        {
            if (strncmp(entries[i].path, roots[r], plen) != 0) continue;
            const char *p = entries[i].path + plen;
            const char *slash = p;
            while (*slash && *slash != '/') slash++;
            if (*slash != '/') continue;
            uint32_t abs_len = (uint32_t)(slash - entries[i].path);
            if (abs_len >= cap) continue;
            memcpy(out, entries[i].path, abs_len);
            out[abs_len] = '\0';
            return true;
        }
    }
    return false;
}

/* Count entries that will be extracted as regular files. Drives the
 * progress bar maximum. */
static uint32_t
count_extractable(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < entry_count; i++)
    {
        category_t c = entries[i].cat;
        if (c == CAT_MODULE || c == CAT_DRIVER ||
            c == CAT_DAS    || c == CAT_RESOURCE ||
            c == CAT_MANIFEST || c == CAT_SETTING || c == CAT_MEM)
            n++;
    }
    /* +1 for description.txt if it will be copied into the bubble */
    int di = find_entry_by_path("/description.txt");
    if (di < 0) di = find_entry_by_path("description.txt");
    if (di >= 0) n++;
    return n;
}

/* Update the progress UI. Safe to call during install loop. */
static void
progress_update(int done, int total, const char *phase, const char *detail)
{
    if (total < 1) total = 1;
    int pct = (done * 100) / total;
    if (pct > 100) pct = 100;
    pb_install.max = 100;
    dobpb_SetValue(&pb_install, pct);

    if (phase) doblbl_SetText(&lbl_progress_phase, phase);
    if (detail)
    {
        /* Trim detail to avoid overflowing the label's 256 chars. */
        char tmp[240];
        strncpy(tmp, detail, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        doblbl_SetText(&lbl_progress_detail, tmp);
    }

    uint32_t wid = dobui_window();
    dobui_FillRect(wid, 0, 0, dobui_width(), dobui_height(), COL_BG);
    doblbl_Draw(&lbl_header);
    doblbl_Draw(&lbl_progress_phase);
    doblbl_Draw(&lbl_progress_detail);
    dobpb_Draw(&pb_install);
    dobui_Invalidate(wid);
}

/* Emit one section of the ModuleFiles: header + indented "  * path"
 * lines for every entry whose category matches, in archive order.
 * Also, as a side effect, extract each matching entry to disk and
 * advance the progress bar. */
static bool
phase_extract_section(uint32_t *done_ref, uint32_t total,
                      const char *section_name, category_t want)
{
    /* First pass: does this section have any entries at all? */
    bool any = false;
    for (uint32_t i = 0; i < entry_count; i++)
        if (entries[i].cat == want) { any = true; break; }
    if (!any) return true;

    /* Emit section header once. */
    mf_append(section_name);
    mf_append(":\n");

    for (uint32_t i = 0; i < entry_count; i++)
    {
        if (entries[i].cat != want) continue;

        char dstbuf[ARCHIVE_MAX_PATH];
        resolve_dest(&entries[i], dstbuf, sizeof(dstbuf));
        const char *dst = dstbuf;

        /* Make sure the destination directory exists. */
        char dirbuf[ARCHIVE_MAX_PATH];
        path_dirname(dst, dirbuf, sizeof(dirbuf));
        if (dirbuf[0]) fs_mkdir_p(dirbuf);

        progress_update((int)(*done_ref), (int)total,
                        "Estrazione file...", basename_of(dst));

        if (!extract_entry_manual(entries[i].index, dst, entries[i].size))
        {
            char msg[320];
            snprintf(msg, sizeof(msg),
                     "Estrazione fallita: %s", dst);
            dobpopup_Error("DobInstaller", msg);
            return false;
        }

        /* Record in ModuleFiles */
        mf_append("  * ");
        mf_append(dst);
        mf_append("\n");

        (*done_ref)++;
    }
    return true;
}

/* Extract description.txt into the main bubble as a side deliverable,
 * so users (and a future ModulesManager "About" panel) can read it
 * after install. The dbp has it at the archive root; we copy it to
 * <main_bubble>/description.txt. Recorded in the RESOURCES section of
 * ModuleFiles so uninstall removes it. */
static bool
copy_description_to_bubble(uint32_t *done_ref, uint32_t total,
                           bool *resources_header_emitted)
{
    if (!install_main_bubble[0]) return true;  /* no bubble */

    int di = find_entry_by_path("/description.txt");
    if (di < 0) di = find_entry_by_path("description.txt");
    if (di < 0) return true;        /* no description to copy */

    char dst[ARCHIVE_MAX_PATH];
    snprintf(dst, sizeof(dst), "%s/description.txt", install_main_bubble);

    progress_update((int)(*done_ref), (int)total,
                    "Copia description.txt...", dst);

    if (!extract_entry_manual(entries[di].index, dst, entries[di].size))
    {
        /* Non-fatal: the package still installs, description is just
         * not available post-install. */
        return true;
    }

    if (!*resources_header_emitted)
    {
        mf_append("RESOURCES:\n");
        *resources_header_emitted = true;
    }
    mf_append("  * ");
    mf_append(dst);
    mf_append("\n");
    (*done_ref)++;
    return true;
}

/* Phase A: extract every file entry, grouping ModuleFiles output by
 * category. Section order in the receipt mirrors the order seen in the
 * install summary: MODULI / DRIVERS / DAS / RESOURCES. */
static bool
phase_extract(uint32_t *done_ref, uint32_t total)
{
    if (!phase_extract_section(done_ref, total, "MODULI",  CAT_MODULE))  return false;
    if (!phase_extract_section(done_ref, total, "DRIVERS", CAT_DRIVER))  return false;
    if (!phase_extract_section(done_ref, total, "DAS",     CAT_DAS))     return false;
    if (!phase_extract_section(done_ref, total, "SETTINGS",CAT_SETTING)) return false;
    if (!phase_extract_section(done_ref, total, "MEM",     CAT_MEM))     return false;

    /* RESOURCES: manifests + generic resources + description.txt (into
     * bubble). The section header is emitted lazily so we don't leave
     * an empty "RESOURCES:" line behind when a package has none. */
    bool res_emitted = false;
    for (uint32_t i = 0; i < entry_count; i++)
    {
        category_t c = entries[i].cat;
        if (c != CAT_RESOURCE && c != CAT_MANIFEST) continue;

        const char *dst = entries[i].path;

        char dirbuf[ARCHIVE_MAX_PATH];
        path_dirname(dst, dirbuf, sizeof(dirbuf));
        if (dirbuf[0]) fs_mkdir_p(dirbuf);

        progress_update((int)(*done_ref), (int)total,
                        "Estrazione file...", basename_of(dst));

        if (!extract_entry_manual(entries[i].index, dst, entries[i].size))
        {
            char msg[320];
            snprintf(msg, sizeof(msg),
                     "Estrazione fallita: %s", dst);
            dobpopup_Error("DobInstaller", msg);
            return false;
        }

        if (!res_emitted) { mf_append("RESOURCES:\n"); res_emitted = true; }
        mf_append("  * ");
        mf_append(dst);
        mf_append("\n");
        (*done_ref)++;
    }

    /* description.txt -> bubble (if present). Counts toward RESOURCES. */
    if (!copy_description_to_bubble(done_ref, total, &res_emitted))
        return false;

    return true;
}

/* Phase B: append entries from the archive's Startup_modules to the
 * system-wide /SYSTEM/CONFIG/Startup_modules. Each new line is also
 * recorded in ModuleFiles under STARTUP: so uninstall can reverse it. */
static bool
phase_startup(uint32_t *done_ref, uint32_t total)
{
    int si = find_entry_by_path("/Startup_modules");
    if (si < 0) si = find_entry_by_path("Startup_modules");
    if (si < 0) return true;        /* no startup entries — nothing to do */

    progress_update((int)(*done_ref), (int)total,
                    "Registrazione moduli di avvio...",
                    "/SYSTEM/CONFIG/Startup_modules");

    static char sbuf[4096];
    int n = read_full_entry(entries[si].index, sbuf, sizeof(sbuf) - 1);
    if (n <= 0) return true;
    sbuf[n] = '\0';

    /* Make sure the system file has a trailing newline before we append. */
    static char existing[8192];
    int elen = fs_read_all("/SYSTEM/CONFIG/Startup_modules",
                           existing, sizeof(existing));
    if (elen > 0 && existing[elen - 1] != '\n')
    {
        if (!fs_append_all("/SYSTEM/CONFIG/Startup_modules", "\n", 1))
            return false;
    }

    /* Append our block. */
    if (!fs_append_all("/SYSTEM/CONFIG/Startup_modules", sbuf, (uint32_t)n))
        return false;

    /* Record in ModuleFiles. */
    mf_append("STARTUP:\n");
    char *line = sbuf;
    while (*line)
    {
        char *eol = line;
        while (*eol && *eol != '\n') eol++;
        char saved = *eol;
        *eol = '\0';
        if (*line && *line != '#')
        {
            mf_append("  * ");
            mf_append(line);
            mf_append("\n");
        }
        if (saved == '\0') break;
        line = eol + 1;
    }
    return true;
}

/* Phase C: merge Associations from the archive into DobConfig. */
static bool
phase_associations(uint32_t *done_ref, uint32_t total)
{
    int ai = find_entry_by_path("/Associations");
    if (ai < 0) ai = find_entry_by_path("Associations");
    if (ai < 0) return true;        /* nothing to do */

    progress_update((int)(*done_ref), (int)total,
                    "Registrazione associazioni...", "");

    static char abuf[4096];
    int n = read_full_entry(entries[ai].index, abuf, sizeof(abuf) - 1);
    if (n <= 0) return true;
    abuf[n] = '\0';

    mf_append("ASSOCIATIONS:\n");

    char *line = abuf;
    while (*line)
    {
        char *eol = line;
        while (*eol && *eol != '\n') eol++;
        char saved = *eol;
        *eol = '\0';

        if (*line && *line != '#')
        {
            char *eq = strchr(line, '=');
            if (eq)
            {
                *eq = '\0';
                const char *ext  = line;
                const char *prog = eq + 1;
                dobconfig_SetAssoc(ext, prog);

                /* Record verbatim (ext=prog) in ModuleFiles */
                mf_append("  * ");
                mf_append(ext);
                mf_append("=");
                mf_append(prog);
                mf_append("\n");
            }
        }

        if (saved == '\0') break;
        line = eol + 1;
    }
    return true;
}

/* Phase D: write ModuleFiles in the main bubble. */
static bool
phase_write_modulefiles(void)
{
    if (!install_main_bubble[0]) return true;  /* no bubble — skip */

    char mfpath[ARCHIVE_MAX_PATH];
    snprintf(mfpath, sizeof(mfpath), "%s/ModuleFiles", install_main_bubble);

    /* Prepend a short header so humans can read the file. */
    static char out[MAX_SUMMARY_LEN + 256];
    int olen = 0;
    olen = append_str(out, (int)sizeof(out), olen,
        "# DobInstaller ModuleFiles\n"
        "# Questo file e' usato dall'uninstaller: NON MODIFICARE A MANO.\n"
        "# Pacchetto: ");
    olen = append_str(out, (int)sizeof(out), olen, pkg_name);
    if (pkg_version[0])
    {
        olen = append_str(out, (int)sizeof(out), olen, " v");
        olen = append_str(out, (int)sizeof(out), olen, pkg_version);
    }
    olen = append_str(out, (int)sizeof(out), olen, "\n\n");
    if (modulefiles_len > 0)
    {
        int cap_left = (int)sizeof(out) - olen - 1;
        int cp = (modulefiles_len < cap_left) ? modulefiles_len : cap_left;
        memcpy(out + olen, modulefiles_buf, (uint32_t)cp);
        olen += cp;
        out[olen] = '\0';
    }

    return fs_write_all(mfpath, out, (uint32_t)olen);
}

/* Screen transition: tear down summary widgets and lay out the progress
 * screen inside the same window. */
static void
screen_switch_to_progress(void)
{
    cur_screen = SCREEN_PROGRESS;
    uint32_t wid = dobui_window();
    int w = dobui_width();
    int h = dobui_height();

    /* Clear the whole canvas. */
    dobui_FillRect(wid, 0, 0, w, h, COL_BG);

    /* Update header */
    char header[128];
    snprintf(header, sizeof(header), "Installazione di '%s'...", pkg_name);
    doblbl_SetText(&lbl_header, header);

    /* Labels: phase (above bar) and detail (below bar) */
    doblbl_InitWithBg(&lbl_progress_phase,  wid, PAD, 64,
                      "Preparazione...", COL_TEXT, COL_BG);
    doblbl_InitWithBg(&lbl_progress_detail, wid, PAD, 120,
                      "", COL_HEADER, COL_BG);

    /* Progress bar: centered, ~90% wide */
    int pb_w = w - 2 * PAD;
    int pb_y = 90;
    dobpb_Init(&pb_install, wid, PAD, pb_y, pb_w, 0);
    pb_install.show_text = true;
    pb_install.max = 100;

    /* Initial draw */
    doblbl_Draw(&lbl_header);
    doblbl_Draw(&lbl_progress_phase);
    dobpb_Draw(&pb_install);
    doblbl_Draw(&lbl_progress_detail);
    dobui_Invalidate(wid);
}

/* The real install. Returns true on success. */
static bool
do_install_real(void)
{
    /* A0: privilege warning (if triggered) */
    if (has_drivers || has_das || has_privileged_startup)
    {
        char warn[512];
        snprintf(warn, sizeof(warn),
            "Il pacchetto '%s' contiene componenti con privilegi elevati:\n"
            "%s%s%s"
            "\nQuesti componenti vengono eseguiti con privilegi di sistema.\n"
            "Installa solo da fonti fidate.\n\n"
            "Continuare?",
            pkg_name,
            has_drivers            ? "  - driver\n" : "",
            has_das                ? "  - Device Automation Scripts\n" : "",
            has_privileged_startup ? "  - moduli caricati all'avvio\n" : "");
        int ans = dobpopup_YesNo("ATTENZIONE — Privilegi elevati", warn);
        if (ans != 0) return false;
    }

    /* A1: check for an existing bubble with the same name. */
    char existing_bubble[ARCHIVE_MAX_PATH] = {0};
    char existing_ver[32] = {0};
    bool upgrade_needed = false;
    if (find_existing_bubble(existing_bubble, sizeof(existing_bubble),
                             existing_ver, sizeof(existing_ver)))
    {
        /* Compare versions. */
        int cmp = 0;
        if (pkg_version[0] && existing_ver[0])
            cmp = version_cmp(pkg_version, existing_ver);

        char buf[512];
        int ans;
        if (!pkg_version[0] || !existing_ver[0])
        {
            snprintf(buf, sizeof(buf),
                "Un pacchetto chiamato '%s' e' gia' installato.\n"
                "La versione non e' disponibile per il confronto.\n\n"
                "Procedere con la reinstallazione?",
                pkg_name);
            ans = dobpopup_YesNo("Pacchetto gia' presente", buf);
        }
        else if (cmp > 0)
        {
            snprintf(buf, sizeof(buf),
                "Aggiornare '%s' dalla versione %s alla versione %s?",
                pkg_name, existing_ver, pkg_version);
            ans = dobpopup_YesNo("Aggiornamento", buf);
        }
        else if (cmp == 0)
        {
            snprintf(buf, sizeof(buf),
                "'%s' v%s e' gia' installato.\n\nReinstallare?",
                pkg_name, pkg_version);
            ans = dobpopup_YesNo("Gia' installato", buf);
        }
        else
        {
            snprintf(buf, sizeof(buf),
                "ATTENZIONE: stai installando una versione PIU' VECCHIA.\n"
                "Installata: %s\nPacchetto:  %s\n\n"
                "Continuare comunque?",
                existing_ver, pkg_version);
            ans = dobpopup_YesNo("Downgrade", buf);
        }
        if (ans != 0) return false;
        upgrade_needed = true;
    }

    /* A2: switch to progress screen BEFORE any destructive action. */
    screen_switch_to_progress();

    uint32_t total = count_extractable() + 3;  /* +3 for phases B/C/D */
    uint32_t done  = 0;

    /* B1: silent uninstall of the previous version (if any). */
    if (upgrade_needed && existing_bubble[0])
    {
        progress_update((int)done, (int)total,
                        "Rimozione versione precedente...", existing_bubble);
        uninstall_bubble(existing_bubble);
    }

    /* B2: decide where ModuleFiles will go (done here so we have the
     * answer for phase D even if the main bubble's directory only comes
     * into existence during phase A). */
    if (!pick_main_bubble(install_main_bubble, sizeof(install_main_bubble)))
    {
        /* No /SYSTEM entries at all? degenerate package, nothing to write. */
        install_main_bubble[0] = '\0';
    }

    /* Reset ModuleFiles accumulator */
    modulefiles_len = 0;
    modulefiles_buf[0] = '\0';

    /* Phase A: extract all file entries */
    if (!phase_extract(&done, total)) return false;

    /* Phase B: append Startup_modules */
    if (!phase_startup(&done, total)) return false;
    done++;
    progress_update((int)done, (int)total,
                    "Moduli di avvio registrati.", "");

    /* Phase C: merge Associations */
    if (!phase_associations(&done, total)) return false;
    done++;
    progress_update((int)done, (int)total,
                    "Associazioni registrate.", "");

    /* Phase D: write ModuleFiles for uninstall */
    if (!phase_write_modulefiles())
    {
        dobpopup_Error("DobInstaller",
            "Installazione riuscita ma scrittura di ModuleFiles fallita.\n"
            "La disinstallazione da ModulesManager potrebbe non funzionare.");
    }
    done++;
    progress_update((int)done, (int)total,
                    "Completamento...", "");

    /* Final: show success popup. */
    char done_msg[320];
    if (has_privileged_startup)
        snprintf(done_msg, sizeof(done_msg),
            "'%s' installato con successo.\n\n"
            "Il pacchetto contiene moduli di avvio: riavvia per attivarli.",
            pkg_name);
    else
        snprintf(done_msg, sizeof(done_msg),
            "'%s' installato con successo.", pkg_name);
    dobpopup_Info("DobInstaller", done_msg);
    return true;
}

/* UI handlers */

static void
do_install(void)
{
    /* Run the pipeline. On success or failure we tear down DobArchive
     * and close the window. The user already saw the outcome as a
     * popup inside do_install_real. */
    (void)do_install_real();
    arc_close();
    dobui_quit();
}

static void
do_cancel(void)
{
    arc_close();
    dobui_quit();
}

/* dobui event handlers */

static void
redraw_all(void)
{
    uint32_t wid = dobui_window();
    dobui_FillRect(wid, 0, 0, dobui_width(), dobui_height(), COL_BG);
    doblbl_Draw(&lbl_header);
    dobmt_Draw(&txt_summary);
    dobbtn_Draw(&btn_cancel);
    dobbtn_Draw(&btn_install);
    dobui_Invalidate(wid);
}

void
event_start(void)
{
    uint32_t wid = dobui_window();

    /* Header label */
    char header[128];
    build_header_text(header, sizeof(header));
    doblbl_InitWithBg(&lbl_header, wid, PAD, PAD, header,
                      COL_HEADER, COL_BG);

    /* Summary textbox: fills middle of the window, readonly by
     * convention (dobmt has no read-only flag — we just ignore key
     * events for it in event_key, below). */
    int tb_y = PAD + HEADER_H;
    int tb_h = dobui_height() - tb_y - PAD - BTN_H - BTN_BOTTOM_PAD;
    dobmt_Init(&txt_summary, wid, PAD, tb_y,
               dobui_width() - 2 * PAD, tb_h);
    txt_summary.show_line_numbers = false;
    dobmt_SetText(&txt_summary, summary_buf, summary_len);

    /* Buttons at bottom, right-aligned */
    int by = dobui_height() - BTN_H - BTN_BOTTOM_PAD;
    int bx_install = dobui_width() - PAD - BTN_W;
    int bx_cancel  = bx_install - PAD - BTN_W;
    dobbtn_Init(&btn_cancel,  wid, bx_cancel,  by, BTN_W, BTN_H, "Annulla");
    dobbtn_Init(&btn_install, wid, bx_install, by, BTN_W, BTN_H, "Installa");

    redraw_all();
}

void
event_mouseclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (cur_screen != SCREEN_SUMMARY) return;   /* install in progress */
    if (dobbtn_OnClick(&btn_cancel, x, y))
    {
        redraw_all();
        do_cancel();
        return;
    }
    if (dobbtn_OnClick(&btn_install, x, y))
    {
        redraw_all();
        do_install();
        return;
    }
    if (dobmt_OnClick(&txt_summary, x, y))
    {
        redraw_all();
    }
}

void
event_mouserelease(int x, int y, uint8_t buttons)
{
    (void)x; (void)y; (void)buttons;
    if (cur_screen != SCREEN_SUMMARY) return;
    dobbtn_OnRelease(&btn_cancel);
    dobbtn_OnRelease(&btn_install);
    redraw_all();
}

void
event_scroll(int delta)
{
    if (cur_screen != SCREEN_SUMMARY) return;
    if (dobmt_OnScroll(&txt_summary, delta))
    {
        redraw_all();
    }
}

void
event_key(uint8_t key)
{
    if (cur_screen != SCREEN_SUMMARY) return;

    /* The summary textbox is meant to be read-only, so we deliberately
     * don't forward typing keys to it. We only forward navigation keys
     * (arrows, page up/down, home/end) so the user can still scroll. */
    if (key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT ||
        key == KEY_RIGHT || key == KEY_HOME || key == KEY_END ||
        key == KEY_PGUP || key == KEY_PGDN)
    {
        if (dobmt_OnKey(&txt_summary, key))
        {
            redraw_all();
        }
        return;
    }

    /* Escape cancels */
    if (key == 27) { do_cancel(); return; }

    /* Enter confirms */
    if (key == '\n' || key == '\r') { do_install(); return; }
}

void
event_close(void)
{
    if (cur_screen == SCREEN_PROGRESS) return;  /* refuse close during install */
    do_cancel();
}

/*  *  L3.5 — Uninstall entry point
 *
 *  Invoked when the caller passes "--uninstall <bubble_dir>" on argv
 *  (typically the Module Manager's Rimuovi command). We don't open the
 *  main install window in this mode: the whole flow is three popups —
 *  confirm, do the work, report. Nice and contained, and uninstall is
 *  fast enough that a dedicated progress screen would just flicker.
 */

/* Same shape as read_bubble_version, but pulls out the `name=` key.
 * Used to title the confirmation popup ("Disinstallare 'X'?") when the
 * bubble folder name differs from the human package name. */
static bool
read_bubble_name(const char *bubble_dir, char *name_out, uint32_t cap)
{
    char mpath[ARCHIVE_MAX_PATH];
    char mbuf[MAX_MANIFEST_LEN];

    if (name_out && cap) name_out[0] = '\0';

    snprintf(mpath, sizeof(mpath), "%s/manifest.dob", bubble_dir);
    int n = fs_read_all(mpath, mbuf, sizeof(mbuf));
    if (n > 0)
    {
        char tmp_ver[32], tmp_type[16];
        parse_manifest_into(mbuf, n, name_out, cap,
                            tmp_ver, sizeof(tmp_ver),
                            tmp_type, sizeof(tmp_type));
        return (name_out[0] != '\0');
    }

    dobfs_dirent_t ents[32];
    uint32_t ecount = 0;
    if (dobfs_List(bubble_dir, ents, 32, &ecount) < 0) return false;
    for (uint32_t i = 0; i < ecount; i++)
    {
        uint32_t nlen = (uint32_t)strlen(ents[i].name);
        if (nlen > 9 && strcmp(ents[i].name + nlen - 9, ".manifest") == 0)
        {
            snprintf(mpath, sizeof(mpath), "%s/%s", bubble_dir, ents[i].name);
            n = fs_read_all(mpath, mbuf, sizeof(mbuf));
            if (n > 0)
            {
                char tmp_ver[32], tmp_type[16];
                parse_manifest_into(mbuf, n, name_out, cap,
                                    tmp_ver, sizeof(tmp_ver),
                                    tmp_type, sizeof(tmp_type));
                return (name_out[0] != '\0');
            }
        }
    }
    return false;
}

/* Full uninstall flow. `bubble_dir` is the on-disk folder of the
 * installed package (e.g. "/SYSTEM/PROGRAMS/MyApp"). Returns an exit
 * code for main() — the caller has no way to observe it beyond the
 * popups we surface to the user. */
static int
run_uninstall(const char *bubble_dir)
{
    /* Validate: the path must resolve to a directory we can touch. */
    dobfs_stat_t st;
    if (!bubble_dir || bubble_dir[0] != '/'
        || dobfs_Stat(bubble_dir, &st) != 0
        || st.type != FS_TYPE_DIR)
    {
        char msg[320];
        snprintf(msg, sizeof(msg),
            "Impossibile disinstallare '%s'.\nIl modulo non esiste.",
            bubble_dir ? bubble_dir : "(null)");
        dobpopup_Error("DobInstaller", msg);
        return 1;
    }

    /* Human-friendly name for the prompt: manifest's `name=` if we
     * can read it, otherwise the folder basename. */
    char display_name[64];
    display_name[0] = '\0';
    if (!read_bubble_name(bubble_dir, display_name, sizeof(display_name)))
    {
        const char *bn = bubble_dir;
        for (const char *p = bubble_dir; *p; p++)
            if (*p == '/') bn = p + 1;
        strncpy(display_name, bn, sizeof(display_name) - 1);
        display_name[sizeof(display_name) - 1] = '\0';
    }

    /* Ask the user. 0 = Yes per dobpopup convention (see callers of
     * dobpopup_YesNo elsewhere in the codebase). */
    char prompt[192];
    snprintf(prompt, sizeof(prompt),
             "Sei sicuro di voler disinstallare '%s'?", display_name);
    int choice = dobpopup_YesNo("Disinstallare modulo", prompt);
    if (choice != 0) return 0;

    /* Do the work. uninstall_bubble reverse-applies ModuleFiles and
     * unlinks the bubble folder itself at the end. It returns false if
     * no ModuleFiles was found — in that case the package was installed
     * outside of DobInstaller (or predates it), and we can't safely
     * walk back its changes without a receipt. */
    if (!uninstall_bubble(bubble_dir))
    {
        dobpopup_Error("DobInstaller",
            "Disinstallazione fallita.\n"
            "Il file ModuleFiles non esiste o non è leggibile.\n"
            "Questo modulo potrebbe essere stato installato manualmente.");
        return 1;
    }

    char done_msg[192];
    snprintf(done_msg, sizeof(done_msg),
             "Modulo '%s' disinstallato.", display_name);
    dobpopup_Info("Disinstallazione completata", done_msg);
    return 0;
}

/* L4 — Bootstrap */

int
main(int argc, char **argv)
{
    /* Argv contract:
     *   --uninstall <bubble_dir>   → three-popup uninstall flow
     *   <dbp_path>                 → normal install
     */
    if (argc >= 2 && strcmp(argv[0], "--uninstall") == 0)
    {
        return run_uninstall(argv[1]);
    }

    if (argc < 1 || !argv[0] || !argv[0][0])
    {
        dobpopup_Error("DobInstaller",
            "Nessun file specificato.\n"
            "DobInstaller va avviato aprendo un file .dbp.");
        return 1;
    }
    strncpy(dbp_path, argv[0], sizeof(dbp_path) - 1);
    dbp_path[sizeof(dbp_path) - 1] = '\0';

    /* Sanity: is the file there at all? */
    dobfs_stat_t st;
    if (dobfs_Stat(dbp_path, &st) != 0 || st.type != FS_TYPE_FILE)
    {
        char msg[320];
        snprintf(msg, sizeof(msg),
            "Impossibile aprire '%s'.\nIl file non esiste o non e' leggibile.",
            dbp_path);
        dobpopup_Error("DobInstaller", msg);
        return 1;
    }

    /* Spawn DobArchive as our backend for reading the .dbp. */
    if (!arc_open_dbp()) return 1;

    /* Classify every entry, load package metadata. */
    if (!classify_all()) { arc_close(); return 1; }

    /* Build the summary text once; the textbox will take a snapshot. */
    build_summary_text();

    /* Run the UI. Returns when dobui_quit() is called from a handler. */
    char title[96];
    snprintf(title, sizeof(title), "DobInstaller - %s", pkg_name);
    dobui_run(title, WIN_W, WIN_H);
    return 0;
}
