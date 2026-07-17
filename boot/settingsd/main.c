/* MainDOB settingsd -- the settings daemon.
 *
 * The sole process that touches `.setting` files on disk.  A `critical`
 * boot service: it must always be reachable, because programs read
 * their settings at any time.  Listed in Startup_modules with
 *
 *     /SYSTEM/OperatingSystem/settingsd/settingsd.mdl<TAB>primary driver needs:DobFileSystem
 *
 *   - primary  : process_destroy schedules a respawn if it ever dies.
 *   - driver   : grants DobFileSystem sandbox bypass.  The daemon must
 *                write `.setting` files inside *other* programs' folders;
 *                without the bypass the per-folder sandbox would refuse
 *                every cross-folder write.  This flag is what makes the
 *                daemon, and only the daemon, able to own those files.
 *   - needs:DobFileSystem : parked until the filesystem is up, so file
 *                I/O below is always safe.
 *
 * See <dob/dobsettings_protocol.h> for the wire contract, the identity
 * model, the two entry classes and the entry/field structure.  This
 * file implements the daemon side of that contract.
 *
 *
 * STORAGE MODEL.
 *
 * All `.setting` files live in one central directory under the
 * DobSettings program's folder: /SYSTEM/PROGRAMS/DobSettings/.
 * Filenames follow "<name>.setting", where <name> is the basename
 * of the caller's home directory (from SYS_GET_HOME_DIR). So
 * "/SYSTEM/DRIVERS/ata/" yields "ata.setting" in the central
 * folder, "/SYSTEM/PROGRAMS/demosettings/" yields
 * "demosettings.setting", etc.
 *
 * Centralisation keeps user-tunable state in one easy-to-back-up
 * place and removes the cross-sandbox write the previous model
 * required (the daemon had to drop files into other programs'
 * homes). Identity — i.e. which file a caller may touch — is
 * still derived from the caller's home basename, so a program
 * can only declare and read its OWN setting file.
 *
 * The daemon does NOT keep every file materialised in RAM.  It holds a
 * light registry -- one (name, path) pair per known file -- and loads
 * exactly one file at a time into a single working model (g_file) to
 * serve a request.  The server loop is single-threaded, so one shared
 * working model and one shared text buffer are reentrancy-safe.  That
 * text buffer (g_text) is reused for three jobs that never overlap
 * within a request: loading file text, serialising for a write, and
 * building a READ_SCHEMA reply.
 *
 * The on-disk format is this daemon's private business (nothing else
 * reads or writes a `.setting` file).  It is line-based text:
 *
 *     # MainDOB .setting -- managed by settingsd. Do not edit by hand.
 *     @ <key>
 *     label=<text>
 *     class=<n>
 *     service=<text>     -- EPS class only
 *     readop=<n>         -- EPS class only
 *     writeop=<n>        -- EPS class only
 *     .field
 *     type=<n>
 *     sublabel=<text>
 *     default=<text>
 *     value=<text>
 *     opt=<text>         -- repeated, enum fields only
 *     .field
 *     ...
 *     @ <key2>
 *     ...
 *
 * An entry runs from its `@` line to the next `@` or EOF.  Entry-level
 * fields precede the first `.field`; everything after a `.field` marker
 * belongs to that field.  Lines starting with `#` are ignored.
 *
 * Writes are atomic: serialise to "<path>.tmp", then rename over the
 * real file.  A reader therefore never sees a half-written file.
 *
 *
 * MEMORY.  Worst-case static footprint is dominated by the per-field
 * option storage materialised at the protocol caps.  If a leaner build
 * is wanted, lower SETTINGS_MAX_* in <dob/dobsettings_protocol.h> (the
 * caps are explicitly tunable) and SETTINGS_TEXT_MAX below in step.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <dob/server.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/types.h>
#include <dob/dobsettings_protocol.h>

#include <DobFileSystem.h>

/* ====================================================================
 *  Daemon-internal limits (not part of the wire contract).
 * ==================================================================== */

#define SETTINGS_MAX_FILES      128      /* registered .setting files       */
#define SETTINGS_PATH_MAX       192      /* full path to a .setting file     */
#define SETTINGS_HOME_MAX       160      /* a home_dir string                */
#define SETTINGS_TEXT_MAX     196608     /* file text / serialise / schema   */
#define SETTINGS_SMALL_REPLY    8192     /* GET_VALUE / LIST_FILES reply      */
#define SETTINGS_DIRENTS          64     /* dir-scan batch size              */

/* Centralised settings directory.
 *
 * All `.setting` files live in one place, owned by the DobSettings
 * program, regardless of where the program that declared them is
 * installed. Previously each program stored its file in its own home
 * (e.g. /SYSTEM/DRIVERS/ata/ata.setting); that required the settings
 * daemon to write across sandbox boundaries — a real complication —
 * and left settings sprinkled across the filesystem instead of one
 * easy-to-back-up folder.
 *
 * The daemon still uses each caller's home-dir BASENAME to derive
 * the filename (ata → ata.setting), so identity is unchanged: a
 * program can only declare/read its OWN setting file, just like
 * before. Only the storage location changed. */
#define SETTINGS_DIR  "/SYSTEM/PROGRAMS/DobSettings/"

/* ====================================================================
 *  In-memory model.
 * ==================================================================== */

/* One field == one control inside an entry's rectangle. */
typedef struct
{
    char     sublabel[SETTINGS_MAX_SUBLABEL_LEN];
    char     value[SETTINGS_MAX_VALUE_LEN];
    char     defval[SETTINGS_MAX_VALUE_LEN];
    char     options[SETTINGS_MAX_OPTIONS][SETTINGS_MAX_OPTION_LEN];
    uint8_t  type;            /* SETTING_BOOL / STRING / ENUM */
    uint8_t  option_count;
} field_t;

/* One invalidation rule: while the owning entry's first field holds
 * `value`, every entry named in `targets` is greyed out. */
typedef struct
{
    char     value[SETTINGS_MAX_VALUE_LEN];
    char     targets[SETTINGS_MAX_INVAL_TARGETS][SETTINGS_MAX_KEY_LEN];
    uint8_t  target_count;
} inval_rule_t;

/* One entry == one setting == one rectangle. */
typedef struct
{
    char     key[SETTINGS_MAX_KEY_LEN];
    char     label[SETTINGS_MAX_LABEL_LEN];
    char     service[SETTINGS_MAX_SERVICE_LEN];   /* EPS class only */
    uint8_t  setting_class;
    uint8_t  field_count;
    uint8_t  rule_count;                          /* invalidation rules */
    uint32_t eps_read_op;
    uint32_t eps_write_op;
    field_t  fields[SETTINGS_MAX_FIELDS];
    inval_rule_t rules[SETTINGS_MAX_INVAL_RULES];
} entry_t;

/* The single working model: exactly one .setting file at a time. */
static struct
{
    char     name[SETTINGS_MAX_NAME_LEN];
    char     path[SETTINGS_PATH_MAX];
    uint32_t count;
    entry_t  entries[SETTINGS_MAX_ENTRIES];
} g_file;

/* Registry: every .setting file the daemon knows of. Rebuilt every boot
 * by the directory scan; programs also add themselves on first declare.
 * The files are the persistent state -- this is only a runtime index. */
typedef struct
{
    char name[SETTINGS_MAX_NAME_LEN];
    char path[SETTINGS_PATH_MAX];
} reg_entry_t;

static reg_entry_t g_registry[SETTINGS_MAX_FILES];
static uint32_t    g_reg_count;

/* Shared buffers (single-threaded server loop -> reentrancy-safe). */
static char g_text[SETTINGS_TEXT_MAX];           /* load / serialise / schema */
static char g_small_reply[SETTINGS_SMALL_REPLY]; /* GET_VALUE / LIST_FILES    */

/* ====================================================================
 *  Small utilities.
 * ==================================================================== */

static void copy_bounded(char *dst, const char *src, uint32_t cap)
{
    /* cap includes the NUL. */
    uint32_t i = 0;
    if (cap == 0) return;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static uint32_t str_to_u32(const char *s)
{
    uint32_t v = 0;
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10u + (uint32_t)(*s - '0'); s++; }
    return v;
}

/* Walk one NUL-terminated string out of a bounded payload buffer.
 * Returns the string and advances *off past its NUL, or NULL if the
 * payload is malformed (no NUL before the end). */
static const char *payload_str(const char *base, uint32_t size, uint32_t *off)
{
    uint32_t i;
    const char *s;
    if (!base || *off >= size) return NULL;
    s = base + *off;
    for (i = *off; i < size && base[i] != '\0'; i++) { }
    if (i >= size) return NULL;
    *off = i + 1;
    return s;
}

/* Read n raw bytes out of a bounded payload buffer, advancing *off.
 * Returns a pointer to them, or NULL if they would run past the end. */
static const void *payload_bytes(const char *base, uint32_t size,
                                 uint32_t *off, uint32_t n)
{
    const void *p;
    if (!base || *off + n > size) return NULL;
    p = base + *off;
    *off += n;
    return p;
}

/* ====================================================================
 *  Caller identity.
 *
 *  SYS_GET_HOME_DIR yields "/SYSTEM/PROGRAMS/<name>/" (or a /DRIVERS/
 *  home).  The final path component is the program name -- matched
 *  exactly, never as a substring, exactly as DobFileSystem's sandbox
 *  does, so "my_settings_tool" can never pass for "DobSettings".
 * ==================================================================== */

static bool home_basename(const char *home, char *out, uint32_t out_cap)
{
    int hlen, ls, i, blen;
    if (!home || !home[0]) return false;
    hlen = (int)strlen(home);
    if (hlen > 0 && home[hlen - 1] == '/') hlen--;     /* drop trailing '/' */
    ls = -1;
    for (i = hlen - 1; i >= 0; i--)
        if (home[i] == '/') { ls = i; break; }
    if (ls < 0) return false;
    blen = hlen - ls - 1;
    if (blen <= 0 || (uint32_t)blen >= out_cap) return false;
    memcpy(out, home + ls + 1, (uint32_t)blen);
    out[blen] = '\0';
    return true;
}

/* Resolve the caller into its program name and the path of its own
 * `.setting` file.  Returns false if identity cannot be established.
 *
 * Name comes from the caller's home-dir basename (so "ata" stays
 * "ata", "demosettings" stays "demosettings"), but the path is in
 * the central settings directory, NOT in the caller's home. This
 * removes the need for the daemon to write outside its own sandbox. */
static bool caller_file(pid_t pid, char *name_out, uint32_t name_cap,
                        char *path_out, uint32_t path_cap)
{
    char home[SETTINGS_HOME_MAX];
    char name[SETTINGS_MAX_NAME_LEN];

    if (get_home_dir(pid, home, sizeof(home)) != 0 || home[0] == '\0')
        return false;
    if (!home_basename(home, name, sizeof(name)))
        return false;
    /* SETTINGS_DIR + name + ".setting" + NUL */
    if (strlen(SETTINGS_DIR) + strlen(name) + 9u >= path_cap)
        return false;

    copy_bounded(name_out, name, name_cap);
    sprintf(path_out, "%s%s.setting", SETTINGS_DIR, name);
    return true;
}

/* True iff the caller is the editor -- the single privileged identity
 * for the editor-family opcodes. */
static bool caller_is_editor(pid_t pid)
{
    char home[SETTINGS_HOME_MAX];
    char name[SETTINGS_MAX_NAME_LEN];
    if (get_home_dir(pid, home, sizeof(home)) != 0) return false;
    if (!home_basename(home, name, sizeof(name)))   return false;
    return strcmp(name, SETTINGS_EDITOR_NAME) == 0;
}

/* ====================================================================
 *  File I/O via the DobFileSystem stub.
 * ==================================================================== */

/* Load a file's text into g_text. Returns true if the file existed and
 * was read (false if absent -- the caller then treats it as empty). */
static bool read_file_text(const char *path, uint32_t *len_out)
{
    int fd, got;
    uint32_t total = 0;

    fd = dobfs_Open(path, FS_READ);
    if (fd < 0) { *len_out = 0; return false; }

    while (total < SETTINGS_TEXT_MAX - 1)
    {
        got = dobfs_Read(fd, g_text + total, SETTINGS_TEXT_MAX - 1 - total);
        if (got <= 0) break;
        total += (uint32_t)got;
    }
    g_text[total] = '\0';
    dobfs_Close(fd);
    *len_out = total;
    return true;
}

/* Write data to path atomically: fill a temp file, then rename over the
 * target.  A concurrent reader sees either the old file or the new one,
 * never a partial write. */
static bool write_file_atomic(const char *path, const char *data, uint32_t len)
{
    char tmp[SETTINGS_PATH_MAX];
    int fd, written;
    uint32_t done = 0;

    if (strlen(path) + 5u >= sizeof(tmp)) return false;
    sprintf(tmp, "%s.tmp", path);

    fd = dobfs_Open(tmp, FS_WRITE | FS_CREATE | FS_TRUNC);
    if (fd < 0) return false;
    while (done < len)
    {
        written = dobfs_Write(fd, data + done, len - done);
        if (written <= 0) { dobfs_Close(fd); return false; }
        done += (uint32_t)written;
    }
    dobfs_Close(fd);

    /* Rename onto the target. If the backend will not clobber an
     * existing file, fall back to unlink + rename. */
    if (dobfs_Rename(tmp, path) == 0)
        return true;
    dobfs_Unlink(path);
    return dobfs_Rename(tmp, path) == 0;
}

/* ====================================================================
 *  Parsing: file text -> g_file.
 * ==================================================================== */

static bool field_is(const char *p, const char *eq, const char *name)
{
    uint32_t n = (uint32_t)strlen(name);
    return (uint32_t)(eq - p) == n && memcmp(p, name, n) == 0;
}

/* Parse g_text (length len) into g_file.entries. g_file.name/path must
 * already be set; g_file.count is reset here. */
static void parse_text(uint32_t len)
{
    const char *p   = g_text;
    const char *end = g_text + len;
    entry_t *cur_e = NULL;
    field_t *cur_f = NULL;
    inval_rule_t *cur_rule = NULL;

    g_file.count = 0;

    while (p < end)
    {
        const char *le = p;
        while (le < end && *le != '\n' && *le != '\r') le++;

        if (p < le && *p == '#')
        {
            /* comment */
        }
        else if (p < le && *p == '@')
        {
            /* New entry. Key is the text after '@' and any spaces. */
            const char *k = p + 1;
            while (k < le && (*k == ' ' || *k == '\t')) k++;

            if (g_file.count < SETTINGS_MAX_ENTRIES && k < le)
            {
                uint32_t kl = (uint32_t)(le - k);
                cur_e = &g_file.entries[g_file.count++];
                memset(cur_e, 0, sizeof(*cur_e));
                cur_e->setting_class = SETTING_CLASS_FILE;
                if (kl >= SETTINGS_MAX_KEY_LEN) kl = SETTINGS_MAX_KEY_LEN - 1;
                memcpy(cur_e->key, k, kl);
                cur_e->key[kl] = '\0';
            }
            else
            {
                cur_e = NULL;     /* cap reached -- ignore the rest */
            }
            cur_f = NULL;
            cur_rule = NULL;
        }
        else if (cur_e && (uint32_t)(le - p) == 6 && memcmp(p, ".field", 6) == 0)
        {
            /* New field within the current entry. */
            if (cur_e->field_count < SETTINGS_MAX_FIELDS)
            {
                cur_f = &cur_e->fields[cur_e->field_count++];
                cur_f->type = SETTING_STRING;     /* sane default */
            }
            else
            {
                cur_f = NULL;
            }
            cur_rule = NULL;
        }
        else if (cur_e && (uint32_t)(le - p) == 11
                       && memcmp(p, ".invalidate", 11) == 0)
        {
            /* New invalidation rule within the current entry. */
            if (cur_e->rule_count < SETTINGS_MAX_INVAL_RULES)
                cur_rule = &cur_e->rules[cur_e->rule_count++];
            else
                cur_rule = NULL;
            cur_f = NULL;
        }
        else if (cur_e && p < le)
        {
            /* field=value line. Split on the first '='. */
            const char *eq = p;
            while (eq < le && *eq != '=') eq++;
            if (eq < le && eq > p)
            {
                char val[SETTINGS_MAX_VALUE_LEN];
                uint32_t vlen = (uint32_t)(le - (eq + 1));
                if (vlen >= sizeof(val)) vlen = sizeof(val) - 1;
                memcpy(val, eq + 1, vlen);
                val[vlen] = '\0';

                if (cur_rule)
                {
                    /* Property of the current invalidation rule. */
                    if      (field_is(p, eq, "when"))
                        copy_bounded(cur_rule->value, val, sizeof(cur_rule->value));
                    else if (field_is(p, eq, "target"))
                    {
                        if (cur_rule->target_count < SETTINGS_MAX_INVAL_TARGETS)
                            copy_bounded(cur_rule->targets[cur_rule->target_count++],
                                         val, SETTINGS_MAX_KEY_LEN);
                    }
                }
                else if (cur_f)
                {
                    /* Property of the current field. */
                    if      (field_is(p, eq, "type"))
                        cur_f->type = (uint8_t)str_to_u32(val);
                    else if (field_is(p, eq, "sublabel"))
                        copy_bounded(cur_f->sublabel, val, sizeof(cur_f->sublabel));
                    else if (field_is(p, eq, "default"))
                        copy_bounded(cur_f->defval, val, sizeof(cur_f->defval));
                    else if (field_is(p, eq, "value"))
                        copy_bounded(cur_f->value, val, sizeof(cur_f->value));
                    else if (field_is(p, eq, "opt"))
                    {
                        if (cur_f->option_count < SETTINGS_MAX_OPTIONS)
                            copy_bounded(cur_f->options[cur_f->option_count++],
                                         val, SETTINGS_MAX_OPTION_LEN);
                    }
                }
                else
                {
                    /* Entry-level property (precedes the first .field). */
                    if      (field_is(p, eq, "label"))
                        copy_bounded(cur_e->label, val, sizeof(cur_e->label));
                    else if (field_is(p, eq, "class"))
                        cur_e->setting_class = (uint8_t)str_to_u32(val);
                    else if (field_is(p, eq, "service"))
                        copy_bounded(cur_e->service, val, sizeof(cur_e->service));
                    else if (field_is(p, eq, "readop"))
                        cur_e->eps_read_op = str_to_u32(val);
                    else if (field_is(p, eq, "writeop"))
                        cur_e->eps_write_op = str_to_u32(val);
                }
            }
        }

        p = le;
        while (p < end && (*p == '\n' || *p == '\r')) p++;
    }
}

/* ====================================================================
 *  Serialising: g_file -> file text.
 * ==================================================================== */

static uint32_t append(uint32_t pos, const char *s)
{
    while (*s && pos < SETTINGS_TEXT_MAX - 1) g_text[pos++] = *s++;
    return pos;
}

static uint32_t serialize_file(void)
{
    char line[48];
    uint32_t pos = 0, i, j, f;

    pos = append(pos, "# MainDOB .setting -- managed by settingsd. "
                      "Do not edit by hand.\n");

    for (i = 0; i < g_file.count; i++)
    {
        entry_t *e = &g_file.entries[i];

        pos = append(pos, "@ ");      pos = append(pos, e->key);   pos = append(pos, "\n");
        pos = append(pos, "label=");  pos = append(pos, e->label); pos = append(pos, "\n");
        sprintf(line, "class=%u\n", (unsigned)e->setting_class);    pos = append(pos, line);

        if (e->setting_class == SETTING_CLASS_EPS)
        {
            pos = append(pos, "service="); pos = append(pos, e->service); pos = append(pos, "\n");
            sprintf(line, "readop=%u\n",  (unsigned)e->eps_read_op);  pos = append(pos, line);
            sprintf(line, "writeop=%u\n", (unsigned)e->eps_write_op); pos = append(pos, line);
        }

        for (f = 0; f < e->field_count; f++)
        {
            field_t *fl = &e->fields[f];
            pos = append(pos, ".field\n");
            sprintf(line, "type=%u\n", (unsigned)fl->type);            pos = append(pos, line);
            pos = append(pos, "sublabel="); pos = append(pos, fl->sublabel); pos = append(pos, "\n");
            pos = append(pos, "default=");  pos = append(pos, fl->defval);   pos = append(pos, "\n");
            pos = append(pos, "value=");    pos = append(pos, fl->value);    pos = append(pos, "\n");
            for (j = 0; j < fl->option_count; j++)
            {
                pos = append(pos, "opt="); pos = append(pos, fl->options[j]); pos = append(pos, "\n");
            }
        }

        /* Invalidation rules: one .invalidate block per rule. */
        for (f = 0; f < e->rule_count; f++)
        {
            inval_rule_t *r = &e->rules[f];
            pos = append(pos, ".invalidate\n");
            pos = append(pos, "when="); pos = append(pos, r->value); pos = append(pos, "\n");
            for (j = 0; j < r->target_count; j++)
            {
                pos = append(pos, "target=");
                pos = append(pos, r->targets[j]);
                pos = append(pos, "\n");
            }
        }
    }

    g_text[pos] = '\0';
    return pos;
}

/* ====================================================================
 *  Registry.
 * ==================================================================== */

static const char *registry_path(const char *name)
{
    uint32_t i;
    for (i = 0; i < g_reg_count; i++)
        if (strcmp(g_registry[i].name, name) == 0)
            return g_registry[i].path;
    return NULL;
}

static void registry_add(const char *name, const char *path)
{
    if (registry_path(name) != NULL) return;            /* already known */
    if (g_reg_count >= SETTINGS_MAX_FILES)  return;     /* cap reached   */
    copy_bounded(g_registry[g_reg_count].name, name, SETTINGS_MAX_NAME_LEN);
    copy_bounded(g_registry[g_reg_count].path, path, SETTINGS_PATH_MAX);
    g_reg_count++;
}

/* Boot-time scan: discover .setting files of programs not (yet) running
 * this session, so the editor's listbox shows every settable program. */
/* Boot scan: register every .setting file in the central directory.
 *
 * One flat folder makes this trivial — directory entries that end in
 * ".setting" map to a program name by stripping the suffix. Pre-
 * existing files survive across reboots, so a user's customised
 * values persist without any program having to re-declare on every
 * startup. (Programs do re-declare for schema refresh, but the
 * stored values are preserved by declare's existing merge logic.) */
static void scan_for_setting_files(void)
{
    static dobfs_dirent_t dirents[SETTINGS_DIRENTS];
    uint32_t count = 0;

    int n = dobfs_List(SETTINGS_DIR, dirents, SETTINGS_DIRENTS, &count);
    if (n < 0)
    {
        /* Directory absent on a fresh disk is fine — first declare
         * will create files on demand via dobfs_Open(FS_WRITE). */
        printf("[settingsd] boot scan: no settings directory yet "
               "(0 files registered)\n");
        return;
    }

    for (uint32_t i = 0; i < count; i++)
    {
        if (dirents[i].type != FS_TYPE_FILE) continue;
        if (dirents[i].name[0] == '.')       continue;

        /* Match suffix ".setting" — derive program name by stripping it. */
        const char *fname = dirents[i].name;
        uint32_t flen = (uint32_t)strlen(fname);
        const char ext[] = ".setting";
        uint32_t elen = (uint32_t)strlen(ext);
        if (flen <= elen) continue;
        if (strcmp(fname + flen - elen, ext) != 0) continue;

        /* Extract the bare program name (stripped of the suffix). */
        char name[SETTINGS_MAX_NAME_LEN];
        uint32_t nlen = flen - elen;
        if (nlen >= sizeof(name)) continue;
        memcpy(name, fname, nlen);
        name[nlen] = '\0';

        /* Build the full path for the registry entry. */
        char path[SETTINGS_PATH_MAX];
        if (strlen(SETTINGS_DIR) + flen + 1u >= sizeof(path)) continue;
        sprintf(path, "%s%s", SETTINGS_DIR, fname);

        registry_add(name, path);
    }
    printf("[settingsd] boot scan: %u .setting file(s) registered.\n",
           (unsigned)g_reg_count);
}

/* ====================================================================
 *  Working model load / save.
 * ==================================================================== */

/* Populate g_file from (name, path). A missing file yields an empty
 * model (count 0) -- a valid starting point for the first declare. */
static void load_file(const char *name, const char *path)
{
    uint32_t len = 0;
    copy_bounded(g_file.name, name, sizeof(g_file.name));
    copy_bounded(g_file.path, path, sizeof(g_file.path));
    g_file.count = 0;
    if (read_file_text(path, &len) && len > 0)
        parse_text(len);
}

static dob_status_t save_file(void)
{
    uint32_t len = serialize_file();
    if (!write_file_atomic(g_file.path, g_text, len))
        return DOB_ERR_INTERNAL;
    return DOB_OK;
}

static entry_t *find_entry(const char *key)
{
    uint32_t i;
    for (i = 0; i < g_file.count; i++)
        if (strcmp(g_file.entries[i].key, key) == 0)
            return &g_file.entries[i];
    return NULL;
}

/* ====================================================================
 *  Field value validation and composite value handling.
 * ==================================================================== */

static bool field_value_valid(const field_t *f, const char *v)
{
    uint32_t i;
    if (!v) return false;

    /* The separator must never occur inside a field value -- it would
     * corrupt the composite split. */
    for (i = 0; v[i]; i++)
        if (v[i] == SETTINGS_FIELD_SEP)
            return false;

    switch (f->type)
    {
        case SETTING_BOOL:
            return strcmp(v, "true") == 0 || strcmp(v, "false") == 0;

        case SETTING_ENUM:
            for (i = 0; i < f->option_count; i++)
                if (strcmp(v, f->options[i]) == 0)
                    return true;
            return false;

        case SETTING_STRING:
        default:
            return strlen(v) < SETTINGS_MAX_VALUE_LEN;
    }
}

/* Join an entry's field values into a composite string. */
static uint32_t compose_value(const entry_t *e, char *out, uint32_t cap)
{
    uint32_t pos = 0, f, l;
    for (f = 0; f < e->field_count; f++)
    {
        if (f > 0)
        {
            if (pos + 1u >= cap) break;
            out[pos++] = SETTINGS_FIELD_SEP;
        }
        l = (uint32_t)strlen(e->fields[f].value);
        if (pos + l >= cap) l = cap - 1u - pos;
        memcpy(out + pos, e->fields[f].value, l);
        pos += l;
    }
    out[pos] = '\0';
    return pos;
}

/* Split a composite string into parts. Returns the part count. */
static uint32_t split_composite(const char *s,
                                char parts[][SETTINGS_MAX_VALUE_LEN])
{
    uint32_t n = 0, len;
    while (n < SETTINGS_MAX_FIELDS)
    {
        len = 0;
        while (*s && *s != SETTINGS_FIELD_SEP)
        {
            if (len < SETTINGS_MAX_VALUE_LEN - 1) parts[n][len++] = *s;
            s++;
        }
        parts[n][len] = '\0';
        n++;
        if (*s == SETTINGS_FIELD_SEP) { s++; continue; }
        break;
    }
    return n;
}

/* ====================================================================
 *  Opcode handlers.
 * ==================================================================== */

/* DECLARE_ENTRY -- declare or update one entry of the caller's own
 * file.  The wire carries each field's `default`, never its value. */
static dob_status_t handle_declare(dob_msg_t *msg)
{
    char name[SETTINGS_MAX_NAME_LEN];
    char path[SETTINGS_PATH_MAX];
    const char *base = (const char *)msg->payload;
    uint32_t size = msg->payload_size, off = 0;
    uint32_t field_count = msg->arg0;
    uint8_t  cls = (uint8_t)msg->arg1;
    const char *key, *label, *service = "";
    field_t  newf[SETTINGS_MAX_FIELDS];
    field_t  oldf[SETTINGS_MAX_FIELDS];
    uint32_t old_count = 0, i;
    entry_t *e;
    bool existed;

    if (!base || size == 0)
        return DOB_ERR_INVALID;
    if (cls > SETTING_CLASS_EPS)
        return DOB_ERR_INVALID;
    if (field_count == 0 || field_count > SETTINGS_MAX_FIELDS)
        return DOB_ERR_INVALID;
    /* An EPS-class entry is a scalar -- exactly one field. */
    if (cls == SETTING_CLASS_EPS && field_count != 1)
        return DOB_ERR_INVALID;

    if (!caller_file(msg->sender_pid, name, sizeof(name), path, sizeof(path)))
        return DOB_ERR_DENIED;
    /* An EPS-class entry directs the editor's IPC -- only a driver may
     * declare one. */
    if (cls == SETTING_CLASS_EPS && !is_driver(msg->sender_pid))
        return DOB_ERR_DENIED;

    key   = payload_str(base, size, &off);
    label = payload_str(base, size, &off);
    if (!key || !label || key[0] == '\0')
        return DOB_ERR_INVALID;
    if (cls == SETTING_CLASS_EPS)
    {
        service = payload_str(base, size, &off);
        if (!service) return DOB_ERR_INVALID;
    }

    /* Parse the field-declaration blocks into a temp array. */
    memset(newf, 0, sizeof(newf));
    for (i = 0; i < field_count; i++)
    {
        settings_field_hdr_t fh;
        const void *raw;
        const char *sub, *def;
        uint32_t j;

        raw = payload_bytes(base, size, &off, sizeof(fh));
        if (!raw) return DOB_ERR_INVALID;
        memcpy(&fh, raw, sizeof(fh));
        if (fh.type > SETTING_SECRET) return DOB_ERR_INVALID;
        /* Un campo SECRET e' ammesso SOLO in una entry EPS: il valore
         * vive (transita) nel servizio proprietario e il daemon non lo
         * vede mai.  In una entry FILE finirebbe persistito in chiaro
         * nel .setting -- esattamente cio' che il tipo esiste per
         * impedire. */
        if (fh.type == SETTING_SECRET && cls != SETTING_CLASS_EPS)
            return DOB_ERR_INVALID;

        sub = payload_str(base, size, &off);
        def = payload_str(base, size, &off);
        if (!sub || !def) return DOB_ERR_INVALID;

        newf[i].type = fh.type;
        copy_bounded(newf[i].sublabel, sub, sizeof(newf[i].sublabel));
        copy_bounded(newf[i].defval,   def, sizeof(newf[i].defval));
        copy_bounded(newf[i].value,    def, sizeof(newf[i].value)); /* value := default */

        for (j = 0; j < fh.option_count; j++)
        {
            const char *opt = payload_str(base, size, &off);
            if (!opt) return DOB_ERR_INVALID;
            if (newf[i].option_count < SETTINGS_MAX_OPTIONS)
                copy_bounded(newf[i].options[newf[i].option_count++],
                             opt, SETTINGS_MAX_OPTION_LEN);
        }
    }

    load_file(name, path);
    e = find_entry(key);
    existed = (e != NULL);

    if (existed)
    {
        /* Keep the old field values to carry compatible ones over. */
        memcpy(oldf, e->fields, sizeof(oldf));
        old_count = e->field_count;
    }
    else
    {
        if (g_file.count >= SETTINGS_MAX_ENTRIES)
            return DOB_ERR_NO_SPACE;
        e = &g_file.entries[g_file.count++];
        memset(e, 0, sizeof(*e));
        copy_bounded(e->key, key, sizeof(e->key));
    }

    /* Apply the new schema. */
    copy_bounded(e->label,   label,   sizeof(e->label));
    copy_bounded(e->service, service, sizeof(e->service));
    e->setting_class = cls;
    e->eps_read_op   = (cls == SETTING_CLASS_EPS) ? msg->arg2 : 0;
    e->eps_write_op  = (cls == SETTING_CLASS_EPS) ? msg->arg3 : 0;
    e->field_count   = (uint8_t)field_count;
    memcpy(e->fields, newf, sizeof(newf));   /* each value currently = default */

    /* Carry over old field values by position where still valid; an
     * incompatible (or missing) old value leaves the field at default. */
    if (existed)
    {
        for (i = 0; i < field_count && i < old_count; i++)
            if (field_value_valid(&e->fields[i], oldf[i].value))
                copy_bounded(e->fields[i].value, oldf[i].value,
                             sizeof(e->fields[i].value));
    }

    registry_add(name, path);
    return save_file();
}

/* GET_VALUE -- read the composite value of one entry of the caller's
 * own file. */
static dob_status_t handle_get(dob_msg_t *msg, dob_msg_t *reply)
{
    char name[SETTINGS_MAX_NAME_LEN];
    char path[SETTINGS_PATH_MAX];
    const char *key;
    uint32_t off = 0, len;
    entry_t *e;

    if (!msg->payload || msg->payload_size == 0)
        return DOB_ERR_INVALID;
    if (!caller_file(msg->sender_pid, name, sizeof(name), path, sizeof(path)))
        return DOB_ERR_DENIED;

    key = payload_str((const char *)msg->payload, msg->payload_size, &off);
    if (!key) return DOB_ERR_INVALID;

    load_file(name, path);
    e = find_entry(key);
    if (e == NULL)
        return DOB_ERR_NOT_FOUND;

    len = compose_value(e, g_small_reply, SETTINGS_SMALL_REPLY);
    reply->payload      = g_small_reply;
    reply->payload_size = len + 1;
    return DOB_OK;
}

/* SET_INVALIDATION -- set one invalidation rule on an entry of the
 * caller's own file.  payload = key '\0' option_value '\0'
 * target_key '\0' ... (zero or more targets to the end). */
static dob_status_t handle_set_invalidation(dob_msg_t *msg)
{
    char name[SETTINGS_MAX_NAME_LEN];
    char path[SETTINGS_PATH_MAX];
    const char *base = (const char *)msg->payload;
    uint32_t size = msg->payload_size, off = 0;
    const char *key, *value, *t;
    entry_t *e;
    inval_rule_t *rule = (inval_rule_t *)0;
    uint32_t i;

    if (!base || size == 0)
        return DOB_ERR_INVALID;
    if (!caller_file(msg->sender_pid, name, sizeof(name), path, sizeof(path)))
        return DOB_ERR_DENIED;

    key   = payload_str(base, size, &off);
    value = payload_str(base, size, &off);
    if (!key || !value)
        return DOB_ERR_INVALID;

    load_file(name, path);
    e = find_entry(key);
    if (e == NULL)
        return DOB_ERR_NOT_FOUND;

    /* Find an existing rule for this option value, to replace it. */
    for (i = 0; i < e->rule_count; i++)
        if (strcmp(e->rules[i].value, value) == 0)
            { rule = &e->rules[i]; break; }

    /* Collect the target keys that follow. */
    {
        char tbuf[SETTINGS_MAX_INVAL_TARGETS][SETTINGS_MAX_KEY_LEN];
        uint32_t tcount = 0;
        while ((t = payload_str(base, size, &off)) != (const char *)0)
        {
            if (tcount >= SETTINGS_MAX_INVAL_TARGETS)
                return DOB_ERR_NO_SPACE;
            copy_bounded(tbuf[tcount++], t, SETTINGS_MAX_KEY_LEN);
        }

        if (tcount == 0)
        {
            /* Empty target list: remove the rule for this value, if any. */
            if (rule)
            {
                uint32_t idx = (uint32_t)(rule - e->rules);
                for (i = idx; i + 1 < e->rule_count; i++)
                    e->rules[i] = e->rules[i + 1];
                e->rule_count--;
            }
            return save_file();
        }

        /* Add or replace the rule. */
        if (!rule)
        {
            if (e->rule_count >= SETTINGS_MAX_INVAL_RULES)
                return DOB_ERR_NO_SPACE;
            rule = &e->rules[e->rule_count++];
        }
        memset(rule, 0, sizeof(*rule));
        copy_bounded(rule->value, value, sizeof(rule->value));
        for (i = 0; i < tcount; i++)
            copy_bounded(rule->targets[i], tbuf[i], SETTINGS_MAX_KEY_LEN);
        rule->target_count = (uint8_t)tcount;
    }

    return save_file();
}

/* LIST_FILES -- enumerate every registered .setting file. */
static dob_status_t handle_list(dob_msg_t *reply)
{
    uint32_t pos = 0, i;

    for (i = 0; i < g_reg_count; i++)
    {
        uint32_t nlen = (uint32_t)strlen(g_registry[i].name);
        if (pos + nlen + 1u > SETTINGS_SMALL_REPLY) break;
        memcpy(g_small_reply + pos, g_registry[i].name, nlen + 1);
        pos += nlen + 1;
    }

    reply->arg0         = i;          /* files actually packed */
    reply->payload      = g_small_reply;
    reply->payload_size = pos;
    return DOB_OK;
}

/* READ_SCHEMA -- the full schema of one file, as packed wire records:
 * settings_entry_hdr_t, key/label[/service], then per field a
 * settings_field_hdr_t followed by sublabel/value/default/options. */
static dob_status_t handle_read_schema(dob_msg_t *msg, dob_msg_t *reply)
{
    const char *name, *path;
    uint32_t off = 0, pos = 0, i, f, j;

    /* Conservative worst-case size of one record. */
    const uint32_t REC_MAX =
        sizeof(settings_entry_hdr_t)
        + SETTINGS_MAX_KEY_LEN + SETTINGS_MAX_LABEL_LEN + SETTINGS_MAX_SERVICE_LEN
        + SETTINGS_MAX_FIELDS *
            (sizeof(settings_field_hdr_t)
             + SETTINGS_MAX_SUBLABEL_LEN + 2u * SETTINGS_MAX_VALUE_LEN
             + SETTINGS_MAX_OPTIONS * SETTINGS_MAX_OPTION_LEN)
        + SETTINGS_MAX_INVAL_RULES *
            (SETTINGS_MAX_VALUE_LEN + 1u
             + SETTINGS_MAX_INVAL_TARGETS * SETTINGS_MAX_KEY_LEN);

    if (!msg->payload || msg->payload_size == 0)
        return DOB_ERR_INVALID;
    name = payload_str((const char *)msg->payload, msg->payload_size, &off);
    if (!name) return DOB_ERR_INVALID;

    path = registry_path(name);
    if (path == NULL)
        return DOB_ERR_NOT_FOUND;

    load_file(name, path);   /* g_text consumed into g_file; now reusable */

    #define PUT(s) do {                                  \
        uint32_t _l = (uint32_t)strlen(s);               \
        memcpy(g_text + pos, (s), _l + 1);               \
        pos += _l + 1;                                   \
    } while (0)

    for (i = 0; i < g_file.count; i++)
    {
        entry_t *e = &g_file.entries[i];
        settings_entry_hdr_t eh;

        if (pos + REC_MAX > SETTINGS_TEXT_MAX)
            break;

        eh.setting_class    = e->setting_class;
        eh.field_count      = e->field_count;
        eh.inval_rule_count = e->rule_count;
        eh.reserved         = 0;
        eh.eps_read_op      = e->eps_read_op;
        eh.eps_write_op     = e->eps_write_op;
        memcpy(g_text + pos, &eh, sizeof(eh));
        pos += sizeof(eh);

        PUT(e->key);
        PUT(e->label);
        if (e->setting_class == SETTING_CLASS_EPS)
            PUT(e->service);

        for (f = 0; f < e->field_count; f++)
        {
            field_t *fl = &e->fields[f];
            settings_field_hdr_t fh;

            fh.type         = fl->type;
            fh.option_count = fl->option_count;
            memcpy(g_text + pos, &fh, sizeof(fh));
            pos += sizeof(fh);

            PUT(fl->sublabel);
            PUT(fl->value);
            PUT(fl->defval);
            for (j = 0; j < fl->option_count; j++)
                PUT(fl->options[j]);
        }

        /* Invalidation rules: option_value, target_count, target keys. */
        for (f = 0; f < e->rule_count; f++)
        {
            inval_rule_t *r = &e->rules[f];
            PUT(r->value);
            g_text[pos++] = (char)r->target_count;
            for (j = 0; j < r->target_count; j++)
                PUT(r->targets[j]);
        }
    }

    #undef PUT

    reply->arg0         = i;          /* entries actually packed */
    reply->payload      = g_text;
    reply->payload_size = pos;
    return DOB_OK;
}

/* SET_VALUE -- write the composite value of one entry. FILE-class only;
 * an EPS-class value is written by the editor straight to the device. */
static dob_status_t handle_set(dob_msg_t *msg)
{
    const char *base = (const char *)msg->payload;
    uint32_t size = msg->payload_size, off = 0;
    const char *name, *key, *value, *path;
    static char parts[SETTINGS_MAX_FIELDS][SETTINGS_MAX_VALUE_LEN];
    uint32_t nparts, i;
    entry_t *e;

    if (!base || size == 0)
        return DOB_ERR_INVALID;

    name  = payload_str(base, size, &off);
    key   = payload_str(base, size, &off);
    value = payload_str(base, size, &off);
    if (!name || !key || !value)
        return DOB_ERR_INVALID;

    path = registry_path(name);
    if (path == NULL)
        return DOB_ERR_NOT_FOUND;

    load_file(name, path);
    e = find_entry(key);
    if (e == NULL)
        return DOB_ERR_NOT_FOUND;

    /* An EPS-class value does not live in the file. */
    if (e->setting_class == SETTING_CLASS_EPS)
        return DOB_ERR_INVALID;

    /* Split the composite; it must yield exactly one part per field. */
    nparts = split_composite(value, parts);
    if (nparts != e->field_count)
        return DOB_ERR_INVALID;

    /* Validate every part before writing any -- all or nothing. */
    for (i = 0; i < nparts; i++)
        if (!field_value_valid(&e->fields[i], parts[i]))
            return DOB_ERR_INVALID;

    for (i = 0; i < nparts; i++)
        copy_bounded(e->fields[i].value, parts[i], sizeof(e->fields[i].value));

    return save_file();
}

/* SET_OWN_VALUE -- a program writes the composite value of one entry of
 * its OWN file (deduced from PID, like GET_VALUE).  The narrow,
 * deliberate exception to "only the editor writes": a program may
 * persist a USER-INITIATED change made through its own quick-toggle UI
 * without round-tripping the editor.  Scoped to the caller's own file --
 * a program cannot name or reach another's.  Validation and persistence
 * are identical to handle_set; only the file resolution differs (PID,
 * not a named file).  FILE-class only. */
static dob_status_t handle_set_own(dob_msg_t *msg)
{
    char name[SETTINGS_MAX_NAME_LEN];
    char path[SETTINGS_PATH_MAX];
    const char *base = (const char *)msg->payload;
    uint32_t size = msg->payload_size, off = 0;
    const char *key, *value;
    static char parts[SETTINGS_MAX_FIELDS][SETTINGS_MAX_VALUE_LEN];
    uint32_t nparts, i;
    entry_t *e;

    if (!base || size == 0)
        return DOB_ERR_INVALID;
    if (!caller_file(msg->sender_pid, name, sizeof(name), path, sizeof(path)))
        return DOB_ERR_DENIED;

    key   = payload_str(base, size, &off);
    value = payload_str(base, size, &off);
    if (!key || !value)
        return DOB_ERR_INVALID;

    load_file(name, path);
    e = find_entry(key);
    if (e == NULL)
        return DOB_ERR_NOT_FOUND;

    /* An EPS-class value does not live in the file. */
    if (e->setting_class == SETTING_CLASS_EPS)
        return DOB_ERR_INVALID;

    /* Split the composite; it must yield exactly one part per field. */
    nparts = split_composite(value, parts);
    if (nparts != e->field_count)
        return DOB_ERR_INVALID;

    /* Validate every part before writing any -- all or nothing. */
    for (i = 0; i < nparts; i++)
        if (!field_value_valid(&e->fields[i], parts[i]))
            return DOB_ERR_INVALID;

    for (i = 0; i < nparts; i++)
        copy_bounded(e->fields[i].value, parts[i], sizeof(e->fields[i].value));

    return save_file();
}

/* ====================================================================
 * ==================================================================== */

static dob_status_t handle_message(dob_msg_t *msg, dob_msg_t *reply)
{
    switch (msg->code)
    {
        case SETTINGS_PING:
            return DOB_OK;

        /* --- Family A: owning program. --- */
        case SETTINGS_DECLARE_ENTRY:
            return handle_declare(msg);
        case SETTINGS_GET_VALUE:
            return handle_get(msg, reply);
        case SETTINGS_SET_INVALIDATION:
            return handle_set_invalidation(msg);
        /* User-initiated write into the caller's OWN file.  No editor
         * gate: the identity check is inside (caller_file deduces the
         * file from PID, so the call can only ever touch the caller's
         * own settings). */
        case SETTINGS_SET_OWN_VALUE:
            return handle_set_own(msg);

        /* --- Family B: editor only. The gate is applied here, before
         *     any work, so a non-editor caller learns nothing. --- */
        case SETTINGS_LIST_FILES:
            if (!caller_is_editor(msg->sender_pid)) return DOB_ERR_DENIED;
            return handle_list(reply);
        case SETTINGS_READ_SCHEMA:
            if (!caller_is_editor(msg->sender_pid)) return DOB_ERR_DENIED;
            return handle_read_schema(msg, reply);
        case SETTINGS_SET_VALUE:
            if (!caller_is_editor(msg->sender_pid)) return DOB_ERR_DENIED;
            return handle_set(msg);

        default:
            return DOB_ERR_INVALID;
    }
}

/* ====================================================================
 *  Entry point.
 * ==================================================================== */

int main(void)
{
    set_priority(1);
    debug_print("[settingsd] starting settings daemon...\n");

    g_reg_count = 0;

    /* DobFileSystem is guaranteed up (needs:DobFileSystem in
     * Startup_modules), so the boot scan can read the disk now. */
    scan_for_setting_files();

    dob_server_init(SETTINGS_SERVICE_NAME);
    dob_server_register(handle_message);

    debug_print("[settingsd] ready.\n");
    dob_server_loop();
    return 0;
}
