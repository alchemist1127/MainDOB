/* MainDOB USB-DAS matcher — implementation. See dob/usb_das.h.
 *
 * Pure file-reading + byte comparison; no hardware, no IPC. One device is
 * matched at a time (USB enumeration is serialized in the controller
 * drivers), so module-static scratch buffers are fine.
 */
#include "usb_das.h"
#include <DobFileSystem.h>
#include <dob/types.h>
#include <unistd.h>   /* debug_print */

/* ---- tiny local string helpers (drivers are freestanding) ---- */

static uint32_t s_len(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static bool s_eq_n(const char *a, const char *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i]) return false;
    return true;
}

/* case-insensitive ASCII compare of a NUL-terminated key against a token of
 * known length (token need not be NUL-terminated). */
static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool key_is(const char *tok, uint32_t toklen, const char *key)
{
    if (s_len(key) != toklen) return false;
    for (uint32_t i = 0; i < toklen; i++)
        if (lower(tok[i]) != lower(key[i])) return false;
    return true;
}

/* Parse a hex/dec byte value like "0x08" or "8" from [p, end). Returns the
 * value; *ok=false if nothing parseable. */
static uint8_t parse_byte(const char *p, const char *end, bool *ok)
{
    /* skip leading spaces */
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    *ok = false;
    uint32_t v = 0;
    if (p + 1 < end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        p += 2;
        while (p < end)
        {
            char c = *p;
            uint32_t d;
            if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
            else break;
            v = (v << 4) | d;
            *ok = true;
            p++;
        }
    }
    else
    {
        while (p < end && *p >= '0' && *p <= '9')
        {
            v = v * 10 + (uint32_t)(*p - '0');
            *ok = true;
            p++;
        }
    }
    return (uint8_t)(v & 0xFF);
}

/* 16-bit variant of parse_byte (same syntax: decimal or 0x hex).
 * For vendor_id / product_id keys. */
static uint16_t parse_u16(const char *p, const char *end, bool *ok)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    *ok = false;
    uint32_t v = 0;
    if (p + 1 < end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        p += 2;
        while (p < end)
        {
            char c = *p; uint32_t d;
            if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
            else break;
            v = (v << 4) | d; *ok = true; p++;
        }
    }
    else
    {
        while (p < end && *p >= '0' && *p <= '9')
        {
            v = v * 10 + (uint32_t)(*p - '0'); *ok = true; p++;
        }
    }
    return (uint16_t)(v & 0xFFFF);
}

/* ---- one parsed DAS ---- */

typedef struct
{
    uint8_t usb_class, usb_subclass, usb_protocol;  /* 0xFF = any */
    usb_das_match_on_t match_on;
    uint8_t subdev_class, subdev_subclass;
    char    label[32];
    bool    have_class;     /* at least usb_class was specified */
    uint16_t vendor_id;     /* 0 = any. Optional refinement: lets a DAS
                             * target ONE product (e.g. a laptop's soldered
                             * card reader) that shares its class triple
                             * with every other mass-storage device. */
    uint16_t product_id;    /* 0 = any; checked only when vendor_id != 0 */
} parsed_das_t;

/* Forensic latches for the LAST file the matcher attempted (declared up
 * here because read_file_all sets them). -100 = never attempted. */
static int      g_last_open_fd     = -100;
static int      g_last_read_len    = -100;

/* Read an entire small file into buf (NUL-terminated). Returns length, or
 * -1 on error. DAS files are a few hundred bytes; cap defensively. */
static int read_file_all(const char *path, char *buf, int cap)
{
    /* FS_READ is mandatory: handle_open accepts flags=0 (the fd is
     * created), but handle_read checks (flags & O_READ) and DENIES
     * every read on such an fd. Field signature on the Armada E500:
     * "Open fd=0, Read len=0" on a 3704-byte file. hotplug's das.c
     * always opened with FS_READ — this caller didn't. */
    int fd = dobfs_Open(path, FS_READ);
    g_last_open_fd = fd;             /* forensic latch: Open vs Read */
    if (fd < 0) { g_last_read_len = -1; return -1; }
    int total = 0;
    while (total < cap - 1)
    {
        int n = dobfs_Read(fd, buf + total, (uint32_t)(cap - 1 - total));
        if (n <= 0) break;
        total += n;
    }
    dobfs_Close(fd);
    buf[total] = '\0';
    g_last_read_len = total;
    return total;
}

/* Parse one DAS file's text into `out`. Lines are `key = value`; '#' starts
 * a comment; blank lines ignored. Unknown keys ignored. */
static void parse_das_text(const char *text, int len, parsed_das_t *out)
{
    /* defaults */
    out->usb_class = USB_DAS_ANY;
    out->usb_subclass = USB_DAS_ANY;
    out->usb_protocol = USB_DAS_ANY;
    out->match_on = USB_DAS_MATCH_INTERFACE;
    out->subdev_class = 0;
    out->subdev_subclass = 0;
    out->label[0] = '\0';
    out->have_class = false;
    out->vendor_id  = 0;
    out->product_id = 0;

    const char *p = text;
    const char *end = text + len;

    while (p < end)
    {
        /* start of a line */
        const char *ls = p;
        while (p < end && *p != '\n') p++;
        const char *le = p;          /* line is [ls, le) */
        if (p < end) p++;            /* consume newline */

        /* trim leading space */
        while (ls < le && (*ls == ' ' || *ls == '\t')) ls++;
        if (ls >= le || *ls == '#') continue;   /* blank / comment */

        /* split on '=' */
        const char *eq = ls;
        while (eq < le && *eq != '=') eq++;
        if (eq >= le) continue;      /* no '=' on this line */

        /* key = [ls, ke)  trimmed */
        const char *ke = eq;
        while (ke > ls && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        uint32_t klen = (uint32_t)(ke - ls);

        /* value = [vs, le), trim leading space; strip trailing comment */
        const char *vs = eq + 1;
        while (vs < le && (*vs == ' ' || *vs == '\t')) vs++;
        const char *ve = vs;
        while (ve < le && *ve != '#') ve++;            /* stop at comment */
        while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;

        bool ok = false;
        if (key_is(ls, klen, "usb_class"))
        {
            uint8_t v = parse_byte(vs, ve, &ok);
            if (ok) { out->usb_class = v; out->have_class = true; }
        }
        else if (key_is(ls, klen, "usb_subclass"))
        {
            uint8_t v = parse_byte(vs, ve, &ok);
            if (ok) out->usb_subclass = v;
        }
        else if (key_is(ls, klen, "usb_protocol"))
        {
            uint8_t v = parse_byte(vs, ve, &ok);
            if (ok) out->usb_protocol = v;
        }
        else if (key_is(ls, klen, "subdev_class"))
        {
            uint8_t v = parse_byte(vs, ve, &ok);
            if (ok) out->subdev_class = v;
        }
        else if (key_is(ls, klen, "subdev_subclass"))
        {
            uint8_t v = parse_byte(vs, ve, &ok);
            if (ok) out->subdev_subclass = v;
        }
        else if (key_is(ls, klen, "match_on"))
        {
            uint32_t vlen = (uint32_t)(ve - vs);
            if (s_len("device") == vlen && s_eq_n(vs, "device", vlen))
                out->match_on = USB_DAS_MATCH_DEVICE;
            else
                out->match_on = USB_DAS_MATCH_INTERFACE;
        }
        else if (key_is(ls, klen, "vendor_id"))
        {
            uint16_t v = parse_u16(vs, ve, &ok);
            if (ok) out->vendor_id = v;
        }
        else if (key_is(ls, klen, "product_id"))
        {
            uint16_t v = parse_u16(vs, ve, &ok);
            if (ok) out->product_id = v;
        }
        else if (key_is(ls, klen, "label"))
        {
            uint32_t vlen = (uint32_t)(ve - vs);
            if (vlen > sizeof(out->label) - 1) vlen = sizeof(out->label) - 1;
            for (uint32_t i = 0; i < vlen; i++) out->label[i] = vs[i];
            out->label[vlen] = '\0';
        }
        /* other keys (bus, kind, category, driver, service) ignored here */
    }
}

/* Does `das` match `dev`? Fills *score with specificity (count of
 * non-wildcard fields that matched) when it does. */
static bool das_matches(const parsed_das_t *das, const usb_das_device_t *dev,
                        int *score)
{
    uint8_t c, sc, pr;
    if (das->match_on == USB_DAS_MATCH_DEVICE)
    {
        c = dev->dev_class; sc = dev->dev_subclass; pr = dev->dev_protocol;
    }
    else
    {
        c = dev->if_class; sc = dev->if_subclass; pr = dev->if_protocol;
    }

    int s = 0;

    if (das->usb_class != USB_DAS_ANY)
    {
        if (das->usb_class != c) return false;
        s++;
    }
    if (das->usb_subclass != USB_DAS_ANY)
    {
        if (das->usb_subclass != sc) return false;
        s++;
    }
    if (das->usb_protocol != USB_DAS_ANY)
    {
        if (das->usb_protocol != pr) return false;
        s++;
    }

    /* Optional VID/PID refinement. A vendor match weighs more than the
     * whole class triple (+4 each) so a product-specific DAS ALWAYS
     * beats the generic class DAS for that product — e.g. the CQ62's
     * soldered SD reader (0x0DDA) gets its own icon/label while every
     * other 08:06:50 device keeps matching the generic pendrive DAS.
     * dev->vid == 0 means the caller didn't provide identity: a
     * vid-specific DAS then never matches (fail closed). */
    if (das->vendor_id != 0)
    {
        if (dev->vid == 0 || das->vendor_id != dev->vid) return false;
        s += 4;
        if (das->product_id != 0)
        {
            if (das->product_id != dev->pid) return false;
            s += 4;
        }
    }

    *score = s;
    return true;
}

/* ---- public entry point ---- */

#define DAS_PATH_MAX  320
/* 8 KB: USB-DAS files are documentation-heavy by project style —
 * mass_storage.das is 3704 bytes with its first directive at byte 2699.
 * The old 2048 truncated the file to pure comments: parser saw zero
 * rules, match impossible ("Read len=2047" + "Match: Nessuno" on the
 * Armada E500 with everything upstream finally green). */
#define DAS_TEXT_MAX  8192

static uint32_t g_last_das_files   = 0;
static int      g_last_list_rc     = -99;  /* -99 = match never ran */
static uint32_t g_last_dir_entries = 0;

/* How many .das files the LAST usb_das_match call actually parsed.
 * 0 with a missing/empty /SYSTEM/CONFIG/DAS/USB/ is the fingerprint of
 * the image-staging failure class (DAS dir not copied to the disk). */
uint32_t usb_das_last_file_count(void)
{
    return g_last_das_files;
}

/* Raw outcome of the dobfs_List in the LAST usb_das_match call: the
 * file counter alone cannot distinguish "List failed from the driver
 * process" (rc < 0) from "directory empty / files filtered out"
 * (rc == 0, entries == 0/N). -99 = match never ran since driver start. */
int usb_das_last_list_rc(void)      { return g_last_list_rc; }
int usb_das_last_open_fd(void)      { return g_last_open_fd; }
int usb_das_last_read_len(void)     { return g_last_read_len; }
uint32_t usb_das_last_dir_entries(void) { return g_last_dir_entries; }

bool usb_das_match(const usb_das_device_t *dev, usb_das_result_t *out)
{
    out->matched = false;
    out->subdev_class = 0;
    out->subdev_subclass = 0;
    out->label[0] = '\0';
    g_last_das_files   = 0;
    g_last_dir_entries = 0;

    dobfs_dirent_t entries[32];
    uint32_t count = 0;
    int rc = dobfs_List(USB_DAS_DIR, entries, 32, &count);
    g_last_list_rc = rc;
    if (rc < 0) return false;
    g_last_dir_entries = count;

    int best_score = -1;
    static char path[DAS_PATH_MAX];
    static char text[DAS_TEXT_MAX];

    for (uint32_t i = 0; i < count; i++)
    {
        if (entries[i].type != FS_TYPE_FILE) continue;

        /* require a ".das" suffix */
        const char *nm = entries[i].name;
        uint32_t nl = s_len(nm);
        if (nl < 4 || !s_eq_n(nm + nl - 4, ".das", 4)) continue;

        /* build full path: USB_DAS_DIR "/" name */
        uint32_t dirlen = s_len(USB_DAS_DIR);
        if (dirlen + 1 + nl + 1 > DAS_PATH_MAX) continue;
        uint32_t k = 0;
        for (uint32_t j = 0; j < dirlen; j++) path[k++] = USB_DAS_DIR[j];
        path[k++] = '/';
        for (uint32_t j = 0; j < nl; j++) path[k++] = nm[j];
        path[k] = '\0';

        int len = read_file_all(path, text, DAS_TEXT_MAX);
        if (len <= 0) continue;
        if (len == DAS_TEXT_MAX - 1)
        {
            /* Buffer filled to the brim: the file is almost certainly
             * larger and got TRUNCATED — directives may be missing.
             * Parse what we have, but leave a trace. */
            debug_print("[usb_das] WARNING: .das truncated at buffer "
                        "size, directives may be lost\n");
        }
        g_last_das_files++;

        parsed_das_t das;
        parse_das_text(text, len, &das);
        if (!das.have_class) continue;   /* a USB-DAS must specify a class */

        int score = 0;
        if (!das_matches(&das, dev, &score)) continue;

        if (score > best_score)
        {
            best_score = score;
            out->subdev_class = das.subdev_class;
            out->subdev_subclass = das.subdev_subclass;
            uint32_t j = 0;
            for (; das.label[j] && j < sizeof(out->label) - 1; j++)
                out->label[j] = das.label[j];
            out->label[j] = '\0';
            out->matched = true;
        }
    }

    return out->matched;
}
