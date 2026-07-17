/* MainDOB Config Server
 * Settings database. Defaults loaded at start.
 * Changes persisted to /SYSTEM/CONFIG/ via DobFileSystem when available.
 *
 * Protocol:
 *   code=1 GET           payload=key_name           -> reply.payload=value
 *   code=2 SET           payload=key\0value          -> (updates DB + disk)
 *   code=3 LIST          (no args)                   -> reply.payload=all keys
 *   code=6 GET_ASSOC     payload=".ext"              -> reply.payload=program path
 *   code=7 SET_ASSOC     payload=".ext\0/path"       -> (updates DB + disk)
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <strhash.h>
#include <sys/syscall.h>
#include <dob/server.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/types.h>

/* Constants */

#define CONFIG_CMD_GET          1
#define CONFIG_CMD_SET          2
#define CONFIG_CMD_LIST         3
#define CONFIG_CMD_GET_ASSOC    6
#define CONFIG_CMD_SET_ASSOC    7
#define CONFIG_CMD_REMOVE_ASSOC 8

#include <DobFileSystem.h>

#define CONFIG_CAPACITY         512
#define ASSOC_CAPACITY          128
#define FILE_BUF_SIZE           8192

/* State */

static strhash_entry_t config_storage[CONFIG_CAPACITY];
static strhash_t config_db;

static strhash_entry_t assoc_storage[ASSOC_CAPACITY];
static strhash_t assoc_db;

/* Default settings (fallback when disk has no settings.cfg) */

static void load_defaults(void)
{
    /* Query kernel version */
    uint32_t ver[5] = {0};
    syscall1(SYS_GETVERSION, (int)ver);
    char verbuf[32];
    sprintf(verbuf, "%u.%u.%u.%u.%u", ver[0], ver[1], ver[2], ver[3], ver[4]);

    struct { const char *key; const char *val; } defaults[] =
    {
        { "system.name",              "MainDOB" },
        { "system.version",           verbuf },
        { "desktop.background_color", "#000000AA" },
        { "desktop.accent_color",     "#2980b9" },
        { "desktop.panel_side",       "right" },
        { "desktop.panel_width",      "280" },
        { "desktop.theme",            "blue" },
        { "display.resolution",       "1024x768" },
        { "display.color_depth",      "32" },
        { "input.keyboard_layout",    "us" },
        { "input.mouse_speed",        "5" },
        { "filesystem.data_path",     "/DATA" },
        { "filesystem.system_path",   "/SYSTEM" },
        { NULL, NULL }
    };

    for (int i = 0; defaults[i].key != NULL; i++)
    {
        strhash_set(&config_db, defaults[i].key, defaults[i].val);
    }
}

/* File helpers — use DobFileSystem stub */

static uint32_t read_file(const char *path, char *buf, uint32_t buf_size)
{
    int fd = dobfs_Open(path, FS_READ);
    if (fd < 0) return 0;

    uint32_t total = 0;
    while (total < buf_size - 1)
    {
        int got = dobfs_Read(fd, buf + total, buf_size - 1 - total);
        if (got <= 0) break;
        total += (uint32_t)got;
    }
    buf[total] = '\0';
    dobfs_Close(fd);
    return total;
}

static bool write_file(const char *path, const char *data, uint32_t size)
{
    int fd = dobfs_Open(path, FS_WRITE | FS_CREATE | FS_TRUNC);
    if (fd < 0) return false;
    int written = dobfs_Write(fd, data, size);
    dobfs_Close(fd);
    return (uint32_t)written == size;
}

/*  *  Parse key=value text into a hashtable — one pair per line.
 *  Lines starting with # are comments. Whitespace is trimmed.
 *  Used for both settings.cfg and Associations.
 */

static void parse_keyvalue(const char *text, uint32_t len, strhash_t *db)
{
    const char *p = text;
    const char *end = text + len;

    while (p < end)
    {
        /* Skip leading whitespace */
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;

        /* Skip comments and empty lines */
        if (p >= end || *p == '#' || *p == '\n' || *p == '\r')
        {
            while (p < end && *p != '\n') p++;
            if (p < end) p++;
            continue;
        }

        /* Find end of line */
        const char *line_end = p;
        while (line_end < end && *line_end != '\n' && *line_end != '\r')
            line_end++;

        /* Find '=' separator */
        const char *eq = p;
        while (eq < line_end && *eq != '=')
            eq++;

        if (eq < line_end && eq > p)
        {
            char key[STRHASH_MAX_KEY];
            char val[STRHASH_MAX_VAL];

            uint32_t klen = (uint32_t)(eq - p);
            uint32_t vlen = (uint32_t)(line_end - (eq + 1));

            if (klen >= sizeof(key)) klen = sizeof(key) - 1;
            if (vlen >= sizeof(val)) vlen = sizeof(val) - 1;

            memcpy(key, p, klen);
            key[klen] = '\0';
            memcpy(val, eq + 1, vlen);
            val[vlen] = '\0';

            /* Trim trailing spaces from key */
            while (klen > 0 && key[klen - 1] == ' ')
                key[--klen] = '\0';

            /* Trim leading spaces from value */
            char *vp = val;
            while (*vp == ' ' || *vp == '\t') vp++;

            strhash_set(db, key, vp);
        }

        p = line_end;
        if (p < end) p++;
    }
}

/* Serialize settings DB to key=value text for writing to disk */

static uint32_t serialize_settings(char *buf, uint32_t buf_size)
{
    uint32_t pos = 0;

    for (uint32_t i = 0; i < config_db.capacity; i++)
    {
        if (config_db.entries[i].used != STRHASH_OCCUPIED)
            continue;

        uint32_t klen = strlen(config_db.entries[i].key);
        uint32_t vlen = strlen(config_db.entries[i].value);

        if (pos + klen + 1 + vlen + 1 >= buf_size)
            break;

        memcpy(buf + pos, config_db.entries[i].key, klen);
        pos += klen;
        buf[pos++] = '=';
        memcpy(buf + pos, config_db.entries[i].value, vlen);
        pos += vlen;
        buf[pos++] = '\n';
    }

    buf[pos] = '\0';
    return pos;
}

/* Persist changes to disk */

static void persist_settings(void)
{

    char out[FILE_BUF_SIZE];
    uint32_t len = serialize_settings(out, sizeof(out));
    write_file("/SYSTEM/CONFIG/settings.cfg", out, len);
}

static void persist_associations(void)
{

    char out[FILE_BUF_SIZE];
    uint32_t pos = 0;
    for (uint32_t i = 0; i < assoc_db.capacity; i++)
    {
        if (assoc_db.entries[i].used != STRHASH_OCCUPIED) continue;
        uint32_t kl = strlen(assoc_db.entries[i].key);
        uint32_t vl = strlen(assoc_db.entries[i].value);
        if (pos + kl + 1 + vl + 1 >= sizeof(out)) break;
        memcpy(out + pos, assoc_db.entries[i].key, kl); pos += kl;
        out[pos++] = '=';
        memcpy(out + pos, assoc_db.entries[i].value, vl); pos += vl;
        out[pos++] = '\n';
    }
    out[pos] = '\0';
    write_file("/SYSTEM/CONFIG/Associations", out, pos);
}

/* IPC message handler — the main dispatch */

/* Deferred persist: instead of writing to disk on every SET,
 * mark dirty and flush only when 2+ seconds have elapsed since
 * the last SET. This turns 100 rapid-fire clipboard updates into
 * a single disk write. */
#define PERSIST_DEBOUNCE_MS 2000
static bool settings_dirty = false;
static bool assoc_dirty = false;
static uint32_t last_settings_set_ms = 0;
static uint32_t last_assoc_set_ms = 0;

static void maybe_flush(void)
{
    uint32_t now = (uint32_t)clock_ms();
    if (settings_dirty && (now - last_settings_set_ms) >= PERSIST_DEBOUNCE_MS)
    {
        persist_settings();
        settings_dirty = false;
    }
    if (assoc_dirty && (now - last_assoc_set_ms) >= PERSIST_DEBOUNCE_MS)
    {
        persist_associations();
        assoc_dirty = false;
    }
}

static dob_status_t handle_message(dob_msg_t *msg, dob_msg_t *reply)
{
    /* Flush deferred writes if debounce period elapsed */
    maybe_flush();

    switch (msg->code)
    {
        case CONFIG_CMD_GET:
        {
            if (!msg->payload)
                return DOB_ERR_INVALID;
            strhash_entry_t *e = strhash_find(&config_db,
                                              (const char *)msg->payload);
            if (!e)
                return DOB_ERR_NOT_FOUND;
            reply->payload = (void *)e->value;
            reply->payload_size = strlen(e->value) + 1;
            return DOB_OK;
        }

        case CONFIG_CMD_SET:
        {
            if (!msg->payload)
                return DOB_ERR_INVALID;
            const char *key = (const char *)msg->payload;
            size_t key_len = strlen(key);
            const char *value = key + key_len + 1;
            if (!strhash_set(&config_db, key, value))
                return DOB_ERR_NO_MEMORY;
            settings_dirty = true;
            last_settings_set_ms = (uint32_t)clock_ms();
            return DOB_OK;
        }

        case CONFIG_CMD_LIST:
        {
            static char list_buf[8192];
            uint32_t pos = 0;
            for (uint32_t i = 0; i < config_db.capacity; i++)
            {
                if (config_db.entries[i].used != STRHASH_OCCUPIED)
                    continue;
                uint32_t klen = strlen(config_db.entries[i].key);
                if (pos + klen + 1 >= sizeof(list_buf))
                    break;
                memcpy(list_buf + pos, config_db.entries[i].key, klen);
                pos += klen;
                list_buf[pos++] = '\n';
            }
            if (pos > 0) pos--;
            list_buf[pos] = '\0';
            reply->payload = list_buf;
            reply->payload_size = pos + 1;
            return DOB_OK;
        }

        case CONFIG_CMD_GET_ASSOC:
        {
            if (!msg->payload) return DOB_ERR_INVALID;
            strhash_entry_t *e = strhash_find(&assoc_db,
                                              (const char *)msg->payload);
            if (!e) return DOB_ERR_NOT_FOUND;
            reply->payload = (void *)e->value;
            reply->payload_size = strlen(e->value) + 1;
            return DOB_OK;
        }

        case CONFIG_CMD_SET_ASSOC:
        {
            if (!msg->payload) return DOB_ERR_INVALID;
            const char *ext = (const char *)msg->payload;
            size_t ext_len = strlen(ext);
            const char *prog = ext + ext_len + 1;
            if (!strhash_set(&assoc_db, ext, prog))
                return DOB_ERR_NO_MEMORY;
            assoc_dirty = true;
            last_assoc_set_ms = (uint32_t)clock_ms();
            return DOB_OK;
        }

        case CONFIG_CMD_REMOVE_ASSOC:
        {
            if (!msg->payload) return DOB_ERR_INVALID;
            const char *ext = (const char *)msg->payload;
            if (strhash_remove(&assoc_db, ext) < 0)
                return DOB_ERR_NOT_FOUND;
            assoc_dirty = true;
            last_assoc_set_ms = (uint32_t)clock_ms();
            return DOB_OK;
        }

        default:
            return DOB_ERR_INVALID;
    }
}

/* Main — high-level algorithm */

int main(void)
{
    set_priority(1);
    debug_print("[config] Starting config server...\n");

    /* Step 1: Init settings DB with hardcoded defaults */
    strhash_init(&config_db, config_storage, CONFIG_CAPACITY);
    load_defaults();

    /* Step 2: Init associations DB from /SYSTEM/CONFIG/Associations.
     * This is the single source of truth for file associations.
     * When a program is installed, its associations are written here.
     * DobFileSystem is guaranteed to be running before config starts
     * (enforced by Startup_modules ordering). */
    strhash_init(&assoc_db, assoc_storage, ASSOC_CAPACITY);
    {
        char buf[FILE_BUF_SIZE];
        uint32_t len = read_file("/SYSTEM/CONFIG/Associations", buf, sizeof(buf));
        if (len > 0)
        {
            parse_keyvalue(buf, len, &assoc_db);
            debug_print("[config] Loaded associations from disk.\n");
        }
        else
        {
            debug_print("[config] No associations file on disk.\n");
        }
    }

    /* Step 3: Load settings from disk (overrides defaults) */
    {
        char buf[FILE_BUF_SIZE];
        uint32_t len = read_file("/SYSTEM/CONFIG/settings.cfg", buf, sizeof(buf));
        if (len > 0)
        {
            parse_keyvalue(buf, len, &config_db);
            debug_print("[config] Loaded settings from disk.\n");
        }
    }

    /* Step 4: Register as "config" in the registry */
    dob_server_init("config");
    dob_server_register(handle_message);

    debug_print("[config] Config server ready.\n");

    /* Step 5: Enter server loop */
    dob_server_loop();

    return 0;
}
