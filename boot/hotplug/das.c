/* MainDOB Device Automation Script (DAS) — engine for hotplug
 *
 * See das.h for the file format and the public API.
 *
 * High-level structure of this file:
 *
 *   1. Tiny string helpers (trim, parse uint, tokenize)
 *   2. Per-section parsers (kv, bitmap block, action block, errors block)
 *   3. File loader and directory scanner (das_load_all)
 *   4. Matcher (das_match)
 *   5. Attach payload builder (das_fill_attach_payload)
 *   6. Action interpreter — variable substitution + primitive dispatch
 *      + automatic error fallback
 *
 * Each primitive in the dispatcher maps 1:1 onto a pre-existing C
 * function (spawn_file, dob_registry_wait, dob_ipc_post,
 * dobpopup_Error, dobpopup_Info, ...). The interpreter is therefore
 * a thin keyword switch, not a runtime.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <dob/types.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/spawn.h>
#include <dob/device_icon.h>
#include <dob/hotplug_events.h>

#include <DobFileSystem.h>
#include <DobPopup.h>

#include "das.h"

/* Storage */

#define DAS_FILE_BUF_SIZE 8192

static das_entry_t db[DAS_MAX_ENTRIES];
static int         db_count = 0;

int das_count(void)
{
    return db_count;
}

const das_entry_t *das_get(int idx)
{
    if (idx < 0 || idx >= db_count) return NULL;
    return &db[idx];
}

/* 1. Tiny string helpers */

static void das_trim(char *s)
{
    if (!s) return;
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'
                  || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
}

static uint32_t das_parse_uint(const char *s)
{
    if (!s) return 0;
    uint32_t val = 0;
    int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    while (*s)
    {
        int d = -1;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f') d = 10 + (*s - 'a');
        else if (base == 16 && *s >= 'A' && *s <= 'F') d = 10 + (*s - 'A');
        else break;
        val = val * base + (uint32_t)d;
        s++;
    }
    return val;
}

static uint8_t das_parse_bus_type(const char *s)
{
    if (!s) return BUS_TYPE_PCI;
    if (strcmp(s, "legacy_fdc") == 0)       return BUS_TYPE_LEGACY_FDC;
    if (strcmp(s, "legacy_ide_atapi") == 0) return BUS_TYPE_LEGACY_IDE_ATAPI;
    if (strcmp(s, "subdevice") == 0)        return BUS_TYPE_SUBDEVICE;
    return BUS_TYPE_PCI;
}

static uint32_t das_parse_kind(const char *s)
{
    if (!s) return DEV_KIND_GUI;
    if (strcmp(s, "system") == 0) return DEV_KIND_SYSTEM;
    if (strcmp(s, "GUI")    == 0) return DEV_KIND_GUI;
    if (strcmp(s, "gui")    == 0) return DEV_KIND_GUI;
    /* Legacy hints: storage / usb_host / unknown all behaved as
     * "show an icon, defer spawn to user activation" — that is the
     * GUI policy under the new naming. Keep accepting them so older
     * .das files continue to work without edit. */
    if (strcmp(s, "storage")  == 0) return DEV_KIND_GUI;
    if (strcmp(s, "usb_host") == 0) return DEV_KIND_GUI;
    if (strcmp(s, "unknown")  == 0) return DEV_KIND_GUI;
    return DEV_KIND_GUI;
}

/* Tokenize a line into a primitive: words separated by whitespace,
 * with double-quoted spans treated as a single token (quotes stripped).
 * Recognises a trailing `on_fail <name>` and stores it on the side.
 * Returns the number of body tokens (not counting on_fail). */
static int das_tokenize_primitive(const char *line, das_primitive_t *out)
{
    memset(out, 0, sizeof(*out));

    const char *p = line;
    int n = 0;

    while (*p && n < DAS_MAX_TOKENS_PER_ACTION)
    {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        char *dst = out->tokens[n];
        int   left = DAS_MAX_TOKEN - 1;

        if (*p == '"')
        {
            p++;
            while (*p && *p != '"' && left > 0) { *dst++ = *p++; left--; }
            if (*p == '"') p++;
        }
        else
        {
            while (*p && *p != ' ' && *p != '\t' && left > 0)
            { *dst++ = *p++; left--; }
        }
        *dst = '\0';
        n++;
    }

    /* Detect a trailing `on_fail <name>` pair and pull it out of the
     * body token list. We look at the last two tokens. */
    if (n >= 2 && strcmp(out->tokens[n - 2], "on_fail") == 0)
    {
        strncpy(out->on_fail, out->tokens[n - 1], sizeof(out->on_fail) - 1);
        out->on_fail[sizeof(out->on_fail) - 1] = '\0';
        out->tokens[n - 2][0] = '\0';
        out->tokens[n - 1][0] = '\0';
        n -= 2;
    }

    out->token_count = (uint8_t)n;
    return n;
}

/* 2. Per-section parsers */

static void das_bitmap_set_pixel(uint8_t *data, int stride, int x, int y)
{
    int byte_idx = y * stride + (x >> 3);
    int bit      = 7 - (x & 7);
    data[byte_idx] |= (uint8_t)(1u << bit);
}

static void das_apply_kv(das_entry_t *e, const char *key, const char *value)
{
    if      (strcmp(key, "bus_type") == 0)  e->bus_type   = das_parse_bus_type(value);
    else if (strcmp(key, "io_base") == 0)   e->io_base    = (uint16_t)das_parse_uint(value);
    else if (strcmp(key, "vendor_id") == 0) e->vendor_id  = (uint16_t)das_parse_uint(value);
    else if (strcmp(key, "device_id") == 0) e->device_id  = (uint16_t)das_parse_uint(value);
    /* class / subclass match keys for BUS_TYPE_SUBDEVICE. PCI-style
     * values: 0x01/0x05 = storage/optical (CDROM, DVDROM, BD). */
    else if (strcmp(key, "class") == 0)     e->class_code = (uint8_t)das_parse_uint(value);
    else if (strcmp(key, "subclass") == 0)  e->subclass   = (uint8_t)das_parse_uint(value);
    else if (strcmp(key, "prog_if") == 0)   e->prog_if    = (uint8_t)das_parse_uint(value);
    /* PCI subsystem identification — narrows a vendor:device match to
     * one specific board variant. Both default to 0 ("any"). */
    else if (strcmp(key, "subsystem_vendor") == 0)
        e->subsystem_vendor = (uint16_t)das_parse_uint(value);
    else if (strcmp(key, "subsystem_device") == 0)
        e->subsystem_device = (uint16_t)das_parse_uint(value);
    /* PCI chip stepping. -1 in storage means "any". */
    else if (strcmp(key, "revision") == 0)
        e->revision = (int16_t)das_parse_uint(value);
    /* Specificity override; lower value = more specific. -1 means
     * "use auto-computed score". Range 0..65535. */
    else if (strcmp(key, "rank") == 0)
        e->rank = (int32_t)das_parse_uint(value);
    /* SUBDEVICE filesystem-volume tag. Symbolic ("fat32") accepted to
     * make DAS files readable; numeric also works for forward-compat. */
    else if (strcmp(key, "volume_fs") == 0)
    {
        if (strcmp(value, "fat32") == 0)
            e->volume_fs = VOLUME_FS_FAT32;
        else if (strcmp(value, "exfat") == 0)
            e->volume_fs = VOLUME_FS_EXFAT;
        else if (strcmp(value, "none") == 0)
            e->volume_fs = VOLUME_FS_NONE;
        else
            e->volume_fs = (uint8_t)das_parse_uint(value);
    }
    else if (strcmp(key, "kind") == 0)      e->kind       = das_parse_kind(value);
    else if (strcmp(key, "label") == 0)
    {
        strncpy(e->label, value, sizeof(e->label) - 1);
        e->label[sizeof(e->label) - 1] = '\0';
    }
    /* Authoritative driver path and registered service name.
     * A DAS file with no `driver` line means "match the device but
     * don't spawn anything" — useful for legacy bubbles whose driver
     * is launched lazily from the action block on user interaction. */
    else if (strcmp(key, "driver") == 0)
    {
        strncpy(e->driver, value, sizeof(e->driver) - 1);
        e->driver[sizeof(e->driver) - 1] = '\0';
    }
    else if (strcmp(key, "service") == 0)
    {
        strncpy(e->service, value, sizeof(e->service) - 1);
        e->service[sizeof(e->service) - 1] = '\0';
    }
    else if (strcmp(key, "bitmap_w") == 0)    e->bitmap.width  = (uint16_t)das_parse_uint(value);
    else if (strcmp(key, "bitmap_h") == 0)    e->bitmap.height = (uint16_t)das_parse_uint(value);
    else if (strcmp(key, "bitmap_fg_r") == 0) e->bitmap.fg_r   = (uint8_t)das_parse_uint(value);
    else if (strcmp(key, "bitmap_fg_g") == 0) e->bitmap.fg_g   = (uint8_t)das_parse_uint(value);
    else if (strcmp(key, "bitmap_fg_b") == 0) e->bitmap.fg_b   = (uint8_t)das_parse_uint(value);
}

/* Parser block-state. Block selection lets the line loop in
 * das_parse_file route each line to the right collector. */
typedef enum
{
    BLK_NONE,
    BLK_BITMAP,
    BLK_ACTION,
    BLK_ERRORS,
    BLK_MENU,
    BLK_MENU_ITEM
} das_block_t;

/* 3. File loader */

/* Parse one DAS file into db[db_count]. Returns true on success. */
static bool das_parse_file(const char *path)
{
    if (db_count >= DAS_MAX_ENTRIES)
    {
        debug_print("[hotplug/das] table full\n");
        return false;
    }

    int fd = dobfs_Open(path, FS_READ);
    if (fd < 0)
    {
        char diag[160];
        sprintf(diag, "[hotplug/das] open failed: %s\n", path);
        debug_print(diag);
        return false;
    }

    static char buf[DAS_FILE_BUF_SIZE];
    int total = 0;
    int n;
    while (total < (int)sizeof(buf) - 1
           && (n = dobfs_Read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
        total += n;
    dobfs_Close(fd);

    if (total <= 0)
    {
        char diag[160];
        sprintf(diag, "[hotplug/das] empty/read-fail: %s\n", path);
        debug_print(diag);
        return false;
    }
    buf[total] = '\0';

    das_entry_t *e = &db[db_count];
    memset(e, 0, sizeof(*e));
    e->bitmap.format = ICON_FMT_1BPP_MASK;
    e->prog_if       = 0xFF;   /* sentinel: "prog_if not specified" */
    e->revision      = -1;     /* sentinel: "revision not specified" */
    e->rank          = -1;     /* sentinel: "use auto-computed specificity" */

    /* Extract name from `path`: take everything after the last '/'
     * and strip the trailing ".das". A path like
     * "/SYSTEM/CONFIG/DAS/cdrom_ide.das" → e->name = "cdrom_ide". */
    {
        const char *slash = path;
        for (const char *s = path; *s; s++)
            if (*s == '/') slash = s + 1;
        size_t nlen = strlen(slash);
        if (nlen > 4 && strcmp(slash + nlen - 4, ".das") == 0)
            nlen -= 4;
        if (nlen >= sizeof(e->name)) nlen = sizeof(e->name) - 1;
        memcpy(e->name, slash, nlen);
        e->name[nlen] = '\0';
    }

    das_block_t block      = BLK_NONE;
    int         bmp_row    = 0;
    int         bmp_stride = 0;
    int         cur_menu   = -1;     /* Index of menu entry being filled */

    char *line = buf;
    while (line && *line)
    {
        char *eol = line;
        while (*eol && *eol != '\n') eol++;
        bool last = (*eol == '\0');
        *eol = '\0';

        char linebuf[256];
        strncpy(linebuf, line, sizeof(linebuf) - 1);
        linebuf[sizeof(linebuf) - 1] = '\0';
        das_trim(linebuf);

        /* --- Inside-block routing --- */
        if (block == BLK_BITMAP)
        {
            if (linebuf[0] == '}') { block = BLK_NONE; goto next_line; }
            if (linebuf[0] != '\0'
                && (int)strlen(linebuf) == (int)e->bitmap.width
                && bmp_row < (int)e->bitmap.height)
            {
                for (int x = 0; x < (int)e->bitmap.width; x++)
                    if (linebuf[x] == '#')
                        das_bitmap_set_pixel(e->bitmap.data,
                                             bmp_stride, x, bmp_row);
                bmp_row++;
            }
            goto next_line;
        }

        if (block == BLK_ACTION)
        {
            if (linebuf[0] == '}') { block = BLK_NONE; goto next_line; }
            if (linebuf[0] == '#' || linebuf[0] == '\0') goto next_line;
            if (e->action_count < DAS_MAX_ACTIONS)
            {
                das_tokenize_primitive(linebuf,
                                       &e->actions[e->action_count]);
                if (e->actions[e->action_count].token_count > 0)
                    e->action_count++;
            }
            goto next_line;
        }

        if (block == BLK_ERRORS)
        {
            if (linebuf[0] == '}') { block = BLK_NONE; goto next_line; }
            if (linebuf[0] == '#' || linebuf[0] == '\0') goto next_line;
            /* `name = primitive ...` */
            char *eq = strchr(linebuf, '=');
            if (eq && e->error_count < DAS_MAX_ERRORS)
            {
                *eq = '\0';
                char *name = linebuf;
                char *prim = eq + 1;
                das_trim(name);
                das_trim(prim);
                das_error_t *err = &e->errors[e->error_count];
                strncpy(err->name, name, sizeof(err->name) - 1);
                err->name[sizeof(err->name) - 1] = '\0';
                das_tokenize_primitive(prim, &err->prim);
                if (err->prim.token_count > 0) e->error_count++;
            }
            goto next_line;
        }

        /* Inside `menu { ... }` — expecting `item "Label"` openers
         * or the closing `}` for the whole section. */
        if (block == BLK_MENU)
        {
            if (linebuf[0] == '}') { block = BLK_NONE; goto next_line; }
            if (linebuf[0] == '#' || linebuf[0] == '\0') goto next_line;
            /* Allow the opening `{` of an item to live on its own line. */
            if (linebuf[0] == '{')
            {
                if (cur_menu >= 0) block = BLK_MENU_ITEM;
                goto next_line;
            }
            if (strncmp(linebuf, "item", 4) == 0
                && (linebuf[4] == ' ' || linebuf[4] == '\t'))
            {
                if (e->menu_count >= DAS_MAX_MENU_ITEMS) goto next_line;
                /* Pull the quoted label. */
                const char *p = linebuf + 4;
                while (*p == ' ' || *p == '\t') p++;
                if (*p != '"') goto next_line;
                p++;
                cur_menu = e->menu_count;
                das_menu_entry_t *m = &e->menu[cur_menu];
                memset(m, 0, sizeof(*m));
                int li = 0;
                while (*p && *p != '"' && li < (int)sizeof(m->label) - 1)
                    m->label[li++] = *p++;
                m->label[li] = '\0';
                /* Allow inline `{` after the label on the same line. */
                if (*p == '"') p++;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '{') block = BLK_MENU_ITEM;
                e->menu_count++;
            }
            goto next_line;
        }

        /* Inside one specific `item "X" { ... }` — collect primitives
         * until the closing `}`. */
        if (block == BLK_MENU_ITEM)
        {
            if (linebuf[0] == '}')
            {
                block = BLK_MENU;
                cur_menu = -1;
                goto next_line;
            }
            if (linebuf[0] == '#' || linebuf[0] == '\0') goto next_line;
            if (cur_menu >= 0)
            {
                das_menu_entry_t *m = &e->menu[cur_menu];
                if (m->prim_count < DAS_MAX_MENU_PRIMS)
                {
                    das_tokenize_primitive(linebuf, &m->prims[m->prim_count]);
                    if (m->prims[m->prim_count].token_count > 0)
                        m->prim_count++;
                }
            }
            goto next_line;
        }

        /* --- Top-level lines --- */
        if (linebuf[0] == '#' || linebuf[0] == '\0') goto next_line;

        /* Block openers: `bitmap {`, `action {`, `errors {` */
        if (strncmp(linebuf, "bitmap", 6) == 0 && strchr(linebuf, '{'))
        {
            if (e->bitmap.width == 0 || e->bitmap.height == 0
                || e->bitmap.width  > ICON_MAX_W
                || e->bitmap.height > ICON_MAX_H)
            {
                debug_print("[hotplug/das] bitmap without valid w/h\n");
            }
            else
            {
                block      = BLK_BITMAP;
                bmp_row    = 0;
                bmp_stride = (e->bitmap.width + 7) / 8;
                memset(e->bitmap.data, 0, ICON_DATA_MAX);
            }
            goto next_line;
        }
        if (strncmp(linebuf, "action", 6) == 0 && strchr(linebuf, '{'))
        {
            block = BLK_ACTION;
            goto next_line;
        }
        if (strncmp(linebuf, "errors", 6) == 0 && strchr(linebuf, '{'))
        {
            block = BLK_ERRORS;
            goto next_line;
        }
        if (strncmp(linebuf, "menu", 4) == 0 && strchr(linebuf, '{'))
        {
            block = BLK_MENU;
            goto next_line;
        }

        /* `key = value` */
        char *eq = strchr(linebuf, '=');
        if (eq)
        {
            *eq = '\0';
            char *key   = linebuf;
            char *value = eq + 1;
            das_trim(key);
            das_trim(value);
            das_apply_kv(e, key, value);
        }

next_line:
        if (last) break;
        line = eol + 1;
    }

    /* Validation. The label must always be set (used in logs even for
     * system devices). The bitmap must be set ONLY for kind=GUI: those
     * are the ones that produce a desktop icon. kind=system entries
     * have no UI presence, so demanding a bitmap from them would be
     * dead weight in every audio/video/NIC/HBA .das file. */
    if (e->label[0] == '\0')
    {
        debug_print("[hotplug/das] rejecting: missing label\n");
        return false;
    }
    if (e->kind == DEV_KIND_GUI &&
        (e->bitmap.width == 0 || e->bitmap.height == 0))
    {
        char diag[160];
        sprintf(diag, "[hotplug/das] rejecting GUI entry (label='%s' w=%u h=%u)\n",
                e->label, e->bitmap.width, e->bitmap.height);
        debug_print(diag);
        return false;
    }

    {
        char diag[200];
        sprintf(diag,
                "[hotplug/das] parsed: label='%s' bus=%u io=0x%x actions=%u errors=%u\n",
                e->label, e->bus_type, e->io_base,
                e->action_count, e->error_count);
        debug_print(diag);
    }

    e->used = true;
    db_count++;
    return true;
}

/* Walk /SYSTEM/CONFIG/DAS and load every *.das file. */
void das_load_all(void)
{
    static dobfs_dirent_t entries[64];
    uint32_t count = 0;

    debug_print("[hotplug/das] scanning /SYSTEM/CONFIG/DAS\n");

    int rc = dobfs_List("/SYSTEM/CONFIG/DAS", entries,
                        sizeof(entries) / sizeof(entries[0]), &count);
    if (rc < 0)
    {
        char diag[64];
        sprintf(diag, "[hotplug/das] dobfs_List rc=%d\n", rc);
        debug_print(diag);
        return;
    }

    for (uint32_t i = 0; i < count; i++)
    {
        const char *name = entries[i].name;
        int nlen = (int)strlen(name);
        if (nlen < 5) continue;
        const char *ext = name + nlen - 4;
        bool is_das =
            (ext[0] == '.'
             && (ext[1] == 'd' || ext[1] == 'D')
             && (ext[2] == 'a' || ext[2] == 'A')
             && (ext[3] == 's' || ext[3] == 'S'));
        if (!is_das) continue;

        char path[256];
        strncpy(path, "/SYSTEM/CONFIG/DAS/", sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        strncat(path, name, sizeof(path) - strlen(path) - 1);

        bool ok = das_parse_file(path);
        char line[256];
        sprintf(line, "[hotplug/das] %s: %s\n",
                ok ? "loaded" : "skipped", name);
        debug_print(line);
    }

    char diag[64];
    sprintf(diag, "[hotplug/das] %d entries active\n", db_count);
    debug_print(diag);
}

/* 4. Matcher
 *
 * Walks the database and returns the index of the most specific entry
 * whose signature matches the device. "Most specific" is implemented
 * via a per-entry score: a vendor:device match outranks a class match,
 * and a class match with prog_if outranks one without. Subsystem ID
 * and chip revision narrow a vendor:device match further when the
 * DAS opts in.
 *
 * An explicit `rank` field in the DAS overrides the auto-score. Rank
 * follows the Windows convention of "lower = more specific". This is
 * how a generic catch-all driver (vbe.das, rank = 65000) gets beaten
 * by every more specific GPU driver, automatically. */

#define DAS_RANK_MAX  65535
#define DAS_NO_MATCH  -1

/* Score one entry against one device. Higher = more specific.
 * Negative return = entry does not match the device at all. */
static int das_score(const das_entry_t *e, const hw_device_t *hw)
{
    if (!e->used) return DAS_NO_MATCH;
    if (e->bus_type != hw->bus_type) return DAS_NO_MATCH;

    if (hw->bus_type == BUS_TYPE_LEGACY_FDC)
    {
        return (e->io_base == hw->io_base) ? 100 : DAS_NO_MATCH;
    }

    if (hw->bus_type == BUS_TYPE_LEGACY_IDE_ATAPI)
    {
        /* IDE ATAPI: bus_type alone is the match. io_base in the DAS
         * (if set) must also match the channel base; if the DAS leaves
         * it zero we accept either channel. */
        if (e->io_base == 0 || e->io_base == hw->io_base) return 100;
        return DAS_NO_MATCH;
    }

    if (hw->bus_type == BUS_TYPE_SUBDEVICE)
    {
        /* Three orthogonal filters: class, subclass, volume_fs. A zero
         * value in the DAS entry means "any" for that filter. A
         * non-zero value must match exactly. volume_fs is the canonical
         * selector for filesystem-volume subdevices emitted by
         * libdob/dob/partition (FAT32 partitions today). class/subclass
         * remain the selector for AHCI optical / future class-based
         * subdevices. The two coexist; a DAS file uses whichever fits. */
        if ((e->class_code == 0 || e->class_code == hw->class_code) &&
            (e->subclass   == 0 || e->subclass   == hw->subclass)   &&
            (e->volume_fs  == 0 || e->volume_fs  == hw->volume_fs))
        {
            int s = 0;
            if (e->class_code) s += 10;
            if (e->subclass)   s += 10;
            if (e->volume_fs)  s += 30;   /* More specific than raw class+subclass */
            return s;
        }
        return DAS_NO_MATCH;
    }

    if (hw->bus_type == BUS_TYPE_PCI)
    {
        /* Two paths for PCI matching:
         *   - exact vendor:device  (highest specificity, ~1000)
         *   - class:subclass top-level fallback, optionally refined
         *     with prog_if (~10..100)
         * Both can coexist in the database; the highest-scoring entry
         * wins, which is exactly what we want when a quirky chip has
         * its own DAS while a generic class DAS covers the rest.
         *
         * subsystem_vendor / subsystem_device / revision narrow a
         * V:D match further when the DAS sets them. They all default
         * to "any" (0 / 0 / -1) so existing DAS files keep matching. */
        if (e->vendor_id != 0)
        {
            if (e->vendor_id != hw->vendor_id ||
                e->device_id != hw->device_id) return DAS_NO_MATCH;

            int s = 1000;

            if (e->prog_if != 0xFF)
            {
                if (e->prog_if != hw->prog_if) return DAS_NO_MATCH;
                s += 1;
            }

            if (e->subsystem_vendor != 0)
            {
                if (e->subsystem_vendor != hw->subsystem_vendor_id)
                    return DAS_NO_MATCH;
                s += 200;   /* SVID match — board-variant precision */
                if (e->subsystem_device != 0)
                {
                    if (e->subsystem_device != hw->subsystem_device_id)
                        return DAS_NO_MATCH;
                    s += 100;   /* Both SVID+SDID — most specific PCI key */
                }
            }

            if (e->revision >= 0)
            {
                if (e->revision != hw->revision) return DAS_NO_MATCH;
                s += 5;
            }

            return s;
        }

        /* No vendor:device — match by class topology. */
        if (e->class_code != 0 && e->class_code != hw->class_code) return DAS_NO_MATCH;
        if (e->subclass   != 0 && e->subclass   != hw->subclass)   return DAS_NO_MATCH;
        if (e->prog_if    != 0xFF && e->prog_if != hw->prog_if)    return DAS_NO_MATCH;

        int s = 0;
        if (e->class_code != 0) s += 10;
        if (e->subclass   != 0) s += 10;
        if (e->prog_if    != 0xFF) s += 50;   /* prog_if beats plain class */
        return s;
    }

    return DAS_NO_MATCH;
}

/* Effective score: explicit rank (when set) overrides auto-computed
 * specificity. Lower rank in the DAS file = more specific = higher
 * effective score, so the matcher's "max wins" logic stays uniform. */
static int das_effective_score(const das_entry_t *e, const hw_device_t *hw)
{
    int s = das_score(e, hw);
    if (s < 0) return DAS_NO_MATCH;
    if (e->rank >= 0)
        return DAS_RANK_MAX - e->rank;
    return s;
}

int das_match(const hw_device_t *hw)
{
    int best_idx   = DAS_NO_MATCH;
    int best_score = DAS_NO_MATCH;

    for (int i = 0; i < db_count; i++)
    {
        int s = das_effective_score(&db[i], hw);
        if (s > best_score)
        {
            best_score = s;
            best_idx   = i;
        }
    }
    return best_idx;
}

int das_match_all(const hw_device_t *hw, int *out_indices, int max)
{
    if (!out_indices || max <= 0) return 0;

    /* Collect all matching indices with their scores. */
    struct { int idx; int score; } cand[DAS_MAX_ENTRIES];
    int n = 0;

    for (int i = 0; i < db_count && n < DAS_MAX_ENTRIES; i++)
    {
        int s = das_effective_score(&db[i], hw);
        if (s < 0) continue;
        cand[n].idx   = i;
        cand[n].score = s;
        n++;
    }

    /* Sort descending by score. Insertion sort — n is at most
     * DAS_MAX_ENTRIES (32), and in practice the candidate list for
     * any one device is 1-3 entries. */
    for (int i = 1; i < n; i++)
    {
        int ki = cand[i].idx;
        int ks = cand[i].score;
        int j = i - 1;
        while (j >= 0 && cand[j].score < ks)
        {
            cand[j + 1] = cand[j];
            j--;
        }
        cand[j + 1].idx   = ki;
        cand[j + 1].score = ks;
    }

    /* Emit up to `max` indices to the caller. */
    if (n > max) n = max;
    for (int i = 0; i < n; i++)
        out_indices[i] = cand[i].idx;
    return n;
}

/* 5. Attach payload builder */

/* Forward declaration: das_subst is defined in section 6 below
 * (action interpreter). The label substitution path here reuses it
 * for consistency, so we need its prototype before the call. */
static void das_subst(const char *in, const das_run_ctx_t *ctx, char *out);

int das_fill_attach_payload(int idx, uint32_t device_id, uint8_t unit,
                            gui_device_attach_t *out)
{
    if (idx < 0 || idx >= db_count || !out) return -1;
    const das_entry_t *e = &db[idx];

    memset(out, 0, sizeof(*out));
    out->device_id = device_id;
    out->kind      = e->kind;

    /* Apply $unit / $unit1 substitution to the label so that DAS files
     * for multi-unit hardware (e.g. floppy) can produce per-icon names
     * like "Floppy 1" / "Floppy 2" from a single template line. We
     * reuse das_subst for full consistency with action-time expansion. */
    {
        das_run_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.device_id = device_id;
        ctx.unit      = unit;
        ctx.io_base   = e->io_base;

        char expanded[DAS_MAX_TOKEN];
        das_subst(e->label, &ctx, expanded);
        strncpy(out->label, expanded, sizeof(out->label) - 1);
    }

    /* All hotplug-managed icons route Eject/Activate back to hotplug. */
    strncpy(out->service_name, "hotplug", sizeof(out->service_name) - 1);
    out->bitmap = e->bitmap;

    /* Ship the menu labels declared in the DAS menu {} section, with
     * full $-substitution applied. dobinterface uses these directly to
     * populate the right-side context panel — no hardcoding. */
    {
        das_run_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.device_id = device_id;
        ctx.unit      = unit;
        ctx.io_base   = e->io_base;

        out->menu_count = e->menu_count;
        for (int i = 0; i < e->menu_count && i < DEV_MENU_MAX_ITEMS; i++)
        {
            char expanded[DAS_MAX_TOKEN];
            das_subst(e->menu[i].label, &ctx, expanded);
            strncpy(out->menu_items[i], expanded,
                    DEV_MENU_LABEL_LEN - 1);
            out->menu_items[i][DEV_MENU_LABEL_LEN - 1] = '\0';
        }
    }
    return 0;
}

/*  *  6. Action interpreter
 *
 *  Variable substitution is done into a fixed-size scratch buffer
 *  immediately before each primitive runs, so the parsed token table
 *  remains untouched and reusable across devices.
 */

/* Substitute $unit / $io_base / $device_id occurrences in `in` and
 * write the result into `out` (size DAS_MAX_TOKEN). */
static void das_subst(const char *in, const das_run_ctx_t *ctx, char *out)
{
    int oi = 0;
    int olen = DAS_MAX_TOKEN - 1;
    const char *p = in;

    while (*p && oi < olen)
    {
        if (*p == '$')
        {
            char rep[32]; rep[0] = '\0';
            /* $unit1 must be checked BEFORE $unit because $unit is a
             * prefix of $unit1 and strncmp would otherwise match the
             * shorter form and leave "1" as a literal character. */
            if (strncmp(p, "$unit1", 6) == 0)
            {
                sprintf(rep, "%u", (unsigned)ctx->unit + 1u);
                p += 6;
            }
            else if (strncmp(p, "$unit", 5) == 0)
            {
                sprintf(rep, "%u", (unsigned)ctx->unit);
                p += 5;
            }
            else if (strncmp(p, "$io_base", 8) == 0)
            {
                sprintf(rep, "0x%x", (unsigned)ctx->io_base);
                p += 8;
            }
            else if (strncmp(p, "$device_id", 10) == 0)
            {
                sprintf(rep, "%u", (unsigned)ctx->device_id);
                p += 10;
            }
            else if (strncmp(p, "$bus", 4) == 0)
            {
                /* Human-readable bus tag derived from provider_service.
                 * "ata" → "IDE", "ahci" → "SATA". Anything else falls
                 * back to the provider name verbatim (forward-compat
                 * for new bus drivers — better than displaying "?"). */
                const char *bus = "?";
                if (strcmp(ctx->provider_service, "ata") == 0)       bus = "IDE";
                else if (strcmp(ctx->provider_service, "ahci") == 0) bus = "SATA";
                else                                                  bus = ctx->provider_service;
                int k = 0;
                while (bus[k] && k < 31) rep[k] = bus[k], k++;
                rep[k] = '\0';
                p += 4;
            }
            else if (strncmp(p, "$provider", 9) == 0)
            {
                /* Name of the bus driver that owns this sub-device.
                 * Used by the DAS action to build the "provider:token"
                 * payload that the CDROM driver deserializes to know
                 * which service to call and what index to pass. */
                int k = 0;
                while (ctx->provider_service[k] && k < 31)
                    rep[k] = ctx->provider_service[k], k++;
                rep[k] = '\0';
                p += 9;
            }
            else if (strncmp(p, "$partition", 10) == 0)
            {
                /* High 8 bits of provider_token = partition index, 0-based
                 * (see libdob/dob/partition.c:make_token). The UI shows
                 * it 1-based so the first partition is "Partizione 1",
                 * matching disk-management conventions. */
                unsigned idx = (unsigned)((ctx->provider_token >> 24) & 0xFFu);
                sprintf(rep, "%u", idx + 1u);
                p += 10;
            }
            else if (strncmp(p, "$disk", 5) == 0)
            {
                /* Low 24 bits of provider_token = native disk selector,
                 * 0-based. Matches the index DobDisk's dropdown shows
                 * ("IDE 0", "IDE 1", "SATA 0"). */
                unsigned sel = (unsigned)(ctx->provider_token & 0x00FFFFFFu);
                sprintf(rep, "%u", sel);
                p += 5;
            }
            else if (strncmp(p, "$token", 6) == 0)
            {
                sprintf(rep, "%u", (unsigned)ctx->provider_token);
                p += 6;
            }
            else
            {
                out[oi++] = *p++;
                continue;
            }
            for (int k = 0; rep[k] && oi < olen; k++) out[oi++] = rep[k];
        }
        else
        {
            out[oi++] = *p++;
        }
    }
    out[oi] = '\0';
}

/* Look up a named error handler. */
static const das_primitive_t *das_find_error(const das_entry_t *e,
                                             const char *name)
{
    if (!name || !*name) return NULL;
    for (int i = 0; i < e->error_count; i++)
        if (strcmp(e->errors[i].name, name) == 0)
            return &e->errors[i].prim;
    return NULL;
}

/* Reply code of the last failing ipc_call primitive, for per-code error
 * refinement in das_run_seq (it first tries label "<on_fail>_<N>" with
 * N = -code, then plain "<on_fail>"). Reset to 0 at the start of every
 * primitive so a stale code can never leak into the next failure. */
static int32_t das_last_fail_code = 0;

/* Run one primitive. Returns 0 on success, -1 on failure. The expanded
 * argument tokens (after $-substitution) are passed in `argv`. */
static int das_exec_one(const das_entry_t *e,
                        const char (*argv)[DAS_MAX_TOKEN], int argc,
                        const das_run_ctx_t *ctx)
{
    das_last_fail_code = 0;
    if (argc < 1) return -1;
    const char *kw = argv[0];

    /* spawn <path> [argv1 argv2 ...]
     * The DAS spawn primitive is by design a hardware-handler launcher
     * (driver or device-control program), so the spawned process is
     * always elevated to driver privileges. Without this, the child
     * runs with is_driver=false and the kernel denies it port I/O,
     * IRQ registration and debug_print — silently.
     *
     * Any tokens beyond the path are forwarded as argv to the spawned
     * binary. crt0 makes them available as main(int argc, char **argv)
     * starting at argv[1]; argv[0] is set by the kernel to the binary's
     * basename. This is what lets the cdrom DAS pass "ata:$unit" so
     * each ATAPI slot gets a dedicated driver instance with its own
     * registry name. */
    if (strcmp(kw, "spawn") == 0)
    {
        if (argc < 2) return -1;

        /* Build a NULL-terminated argv array for spawn_file_driver.
         * The pointers are into our local argv[][] buffer which lives
         * for the duration of this call — spawn_file_driver copies the
         * argv contents synchronously into the kernel during _spawn_make_req,
         * so the lifetime is sufficient. */
        const char *child_argv[16];
        int cn = 0;
        for (int t = 1; t < argc && cn < 15; t++)
            child_argv[cn++] = argv[t];
        child_argv[cn] = NULL;

        /* spawn_file_driver does the ELF load + driver promotion in a
         * background worker, so bubble processing on the hotplug main
         * thread keeps running. */
        if (spawn_file_driver(argv[1], (cn > 1) ? child_argv : NULL) < 0)
            return -1;
        return 0;
    }

    /* run <path> [argv1 argv2 ...]
     * Come 'spawn' ma lancia un PROGRAMMA utente/GUI, NON promosso a
     * driver. 'spawn' fa make_driver() sul figlio (giusto per driver e
     * programmi di controllo hardware), ma per un'app a finestra come
     * DobDisk la promozione a driver e' sbagliata: il processo gira ma non
     * ottiene una finestra da dobinterface. 'run' usa spawn_file (nessuna
     * promozione), il percorso con cui parte ogni altro programma GUI. */
    if (strcmp(kw, "run") == 0)
    {
        if (argc < 2) return -1;

        const char *child_argv[16];
        int cn = 0;
        for (int t = 1; t < argc && cn < 15; t++)
            child_argv[cn++] = argv[t];
        child_argv[cn] = NULL;

        if (spawn_file(argv[1], (cn > 1) ? child_argv : NULL) < 0)
        {
            char l[128];
            snprintf(l, sizeof(l), "[das] run: spawn_file FAILED for %s\n",
                     argv[1]);
            debug_print(l);
            return -1;
        }
        return 0;
    }

    /* open_program <path> [argv...] — launch a GUI program THROUGH
     * dobinterface (GUI_LAUNCH_PROGRAM): the identical spawn path the
     * desktop "Tutti i programmi" launcher exercises daily.  Introduced
     * because `run` (a plain spawn_file from the hotplug action runner)
     * failed to surface a DobDisk window in the field (Formatta menu:
     * unit registered, no window, no popup) while the same binary opens
     * fine from the desktop — routing the spawn through dobinterface
     * removes every runner-environment variable from the equation.
     * Sync: the reply's arg0 says whether the spawn was accepted, so
     * on_fail labels fire exactly like ipc_call failures. */
    if (strcmp(kw, "open_program") == 0)
    {
        if (argc < 2) return -1;
        uint32_t gp = dob_registry_find("dobinterface");
        if (!gp)
        {
            debug_print("[das] open_program: dobinterface not found\n");
            return -1;
        }
        static char cmd[192];
        int off = 0;
        for (int t = 1; t < argc; t++)
        {
            int w = snprintf(cmd + off, sizeof(cmd) - (uint32_t)off,
                             (t == 1) ? "%s" : " %s", argv[t]);
            if (w < 0 || off + w >= (int)sizeof(cmd)) break;
            off += w;
        }
        dob_msg_t m = {0}, r = {0};
        m.code         = 133;   /* GUI_LAUNCH_PROGRAM */
        m.payload      = cmd;
        m.payload_size = (uint32_t)(off + 1);
        if (dob_ipc_call(gp, &m, &r) != DOB_OK) return -1;
        if (r.arg0 != 1)
        {
            debug_print("[das] open_program: dobinterface spawn failed\n");
            return -1;
        }
        return 0;
    }

    /* wait_service <name> [timeout=<ms>] */
    if (strcmp(kw, "wait_service") == 0)
    {
        if (argc < 2) return -1;
        uint32_t timeout = 3000;
        for (int i = 2; i < argc; i++)
            if (strncmp(argv[i], "timeout=", 8) == 0)
                timeout = das_parse_uint(argv[i] + 8);
        return dob_registry_wait(argv[1], timeout) ? 0 : -1;
    }

    /* popup_info <title> <message> */
    if (strcmp(kw, "popup_info") == 0)
    {
        const char *title = (argc >= 2) ? argv[1] : e->label;
        const char *msg   = (argc >= 3) ? argv[2] : "";
        dobpopup_Info(title, msg);
        return 0;
    }

    /* popup_error <title> <message> */
    if (strcmp(kw, "popup_error") == 0)
    {
        const char *title = (argc >= 2) ? argv[1] : e->label;
        const char *msg   = (argc >= 3) ? argv[2] : "";
        dobpopup_Error(title, msg);
        return 0;
    }

    /* ipc_post <service> <opcode> [payload_string]
     * If a fourth token is supplied it is shipped verbatim in
     * msg.payload (NUL terminator included). */
    if (strcmp(kw, "ipc_post") == 0)
    {
        if (argc < 3) return -1;
        uint32_t port = dob_registry_find(argv[1]);
        if (!port) return -1;
        dob_msg_t m = {0};
        m.code = das_parse_uint(argv[2]);
        if (argc >= 4 && argv[3][0] != '\0')
        {
            m.payload      = (void *)argv[3];
            m.payload_size = (uint32_t)(strlen(argv[3]) + 1);
        }
        return dob_ipc_post(port, &m) == DOB_OK ? 0 : -1;
    }

    /* ipc_call <service> <opcode> [payload_string] — sync.
     *
     * Forwards ctx->hijack_target_port as arg0 so probe-and-mount
     * handlers (floppy, cdrom) can route their OpenMount to a
     * specific DobFiles window. Drivers that don't need it ignore
     * arg0 — 0 is the safe default for the desktop-click path. */
    if (strcmp(kw, "ipc_call") == 0)
    {
        if (argc < 3) return -1;
        uint32_t port = dob_registry_find(argv[1]);
        if (!port) return -1;
        dob_msg_t m = {0}, r = {0};
        m.code = das_parse_uint(argv[2]);
        m.arg0 = ctx->hijack_target_port;
        if (argc >= 4 && argv[3][0] != '\0')
        {
            m.payload      = (void *)argv[3];
            m.payload_size = (uint32_t)(strlen(argv[3]) + 1);
        }
        if (dob_ipc_call(port, &m, &r) != DOB_OK) return -1;
        if ((int32_t)r.code < 0)
        {
            das_last_fail_code = (int32_t)r.code;   /* for label refinement */
            return -1;
        }
        return 0;
    }

    /* Unknown keyword. */
    {
        char diag[96];
        sprintf(diag, "[hotplug/das] unknown primitive '%s'\n", kw);
        debug_print(diag);
    }
    return -1;
}

/* Run a sequence of primitives with shared error/fallback semantics.
 * Used by both das_run_action (the double-click pipeline) and
 * das_run_menu (the right-side panel pipeline) so the two paths share
 * exactly the same execution rules. */
static void das_run_seq(const das_entry_t *e,
                        const das_primitive_t *prims, int count,
                        const das_run_ctx_t *ctx)
{
    for (int i = 0; i < count; i++)
    {
        const das_primitive_t *p = &prims[i];

        /* Expand variables into a local argv. */
        char argv[DAS_MAX_TOKENS_PER_ACTION][DAS_MAX_TOKEN];
        for (int t = 0; t < p->token_count; t++)
            das_subst(p->tokens[t], ctx, argv[t]);

        int rc = das_exec_one(e, (const char (*)[DAS_MAX_TOKEN])argv,
                              p->token_count, ctx);
        if (rc == 0) continue;

        /* Failure — refined per-code label first ("<on_fail>_<N>",
         * N = -reply code from a failed ipc_call: e.g. novolume_9 for
         * DOB_ERR_NO_MEDIA), then the plain on_fail, else generic popup.
         * .das files that define no refined labels behave exactly as
         * before. */
        const das_primitive_t *fb = NULL;
        if (das_last_fail_code < 0 && p->on_fail[0] != '\0')
        {
            char refined[64];
            snprintf(refined, sizeof(refined), "%s_%d",
                     p->on_fail, (int)(-das_last_fail_code));
            fb = das_find_error(e, refined);
        }
        if (!fb) fb = das_find_error(e, p->on_fail);
        if (fb)
        {
            char fbargv[DAS_MAX_TOKENS_PER_ACTION][DAS_MAX_TOKEN];
            for (int t = 0; t < fb->token_count; t++)
                das_subst(fb->tokens[t], ctx, fbargv[t]);
            das_exec_one(e, (const char (*)[DAS_MAX_TOKEN])fbargv,
                         fb->token_count, ctx);
        }
        else
        {
            char body[160];
            sprintf(body,
                    "Operazione fallita: '%s' in %s.",
                    argv[0], e->label);
            dobpopup_Error(e->label[0] ? e->label : "Dispositivo", body);
        }

        /* Stop on first failure. */
        return;
    }
}

void das_run_action(int idx, const das_run_ctx_t *ctx)
{
    if (idx < 0 || idx >= db_count || !ctx) return;
    const das_entry_t *e = &db[idx];
    das_run_seq(e, e->actions, e->action_count, ctx);
}

void das_run_menu(int idx, int menu_idx, const das_run_ctx_t *ctx)
{
    if (idx < 0 || idx >= db_count || !ctx) return;
    const das_entry_t *e = &db[idx];
    if (menu_idx < 0 || menu_idx >= e->menu_count) return;
    const das_menu_entry_t *m = &e->menu[menu_idx];
    das_run_seq(e, m->prims, m->prim_count, ctx);
}
