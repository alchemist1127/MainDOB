/* DobSettings client stub -- the program side of the settings wire.
 *
 * Compiled into any program that includes <DobSettings.h>.  Marshals
 * the calls in that header onto the settingsd IPC protocol (see
 * <dob/dobsettings_protocol.h>) and hides the wire entirely.
 *
 * The cached port is healed transparently through _dob_call_reconnect:
 * if the daemon is respawned, the next call re-discovers it.
 */

#include <DobSettings.h>
#include <dob/ipc.h>
#include <dob/reconnect.h>
#include <string.h>

static uint32_t settings_port = 0;

#define IPC_CALL(m, r) \
    _dob_call_reconnect(&settings_port, SETTINGS_SERVICE_NAME, 2000, (m), (r))

/* DECLARE_ENTRY payload packing buffer.  Worst case: key + label +
 * service + every field block at its protocol caps -- comfortably under
 * this size. */
#define DECLARE_BUF_SIZE  4096

/* ----- Payload packing helpers ------------------------------------------ */

/* Append a NUL-terminated string. Returns false on overflow. */
static bool put_str(char *buf, uint32_t cap, uint32_t *pos, const char *s)
{
    const char *p = s ? s : "";
    uint32_t l = (uint32_t)strlen(p) + 1u;
    if (*pos + l > cap) return false;
    memcpy(buf + *pos, p, l);
    *pos += l;
    return true;
}

/* Append raw bytes. Returns false on overflow. */
static bool put_raw(char *buf, uint32_t cap, uint32_t *pos,
                    const void *src, uint32_t n)
{
    if (*pos + n > cap) return false;
    memcpy(buf + *pos, src, n);
    *pos += n;
    return true;
}

/* Pack one field block: settings_field_hdr_t, sublabel, default,
 * options.  Returns false on overflow. */
static bool put_field(char *buf, uint32_t cap, uint32_t *pos,
                      const setting_field_t *f)
{
    settings_field_hdr_t fh;
    uint32_t opt_count = 0, i;

    if (f->type < SETTING_BOOL || f->type > SETTING_SECRET)
        return false;
    if (f->options)
        while (f->options[opt_count]) opt_count++;
    if (opt_count > SETTINGS_MAX_OPTIONS)
        return false;

    fh.type         = (uint8_t)f->type;
    fh.option_count = (uint8_t)opt_count;

    if (!put_raw(buf, cap, pos, &fh, sizeof(fh)))            return false;
    if (!put_str(buf, cap, pos, f->sublabel))                return false;
    if (!put_str(buf, cap, pos, f->default_value))           return false;
    for (i = 0; i < opt_count; i++)
        if (!put_str(buf, cap, pos, f->options[i]))          return false;
    return true;
}

/* ----- Shared declare back end ------------------------------------------ */

static int declare_common(const char *key, const char *label,
                           const setting_field_t *fields, int field_count,
                           int setting_class, const char *service,
                           unsigned read_op, unsigned write_op)
{
    char buf[DECLARE_BUF_SIZE];
    uint32_t pos = 0;
    int i;
    dob_msg_t msg = {0}, reply = {0};

    if (!key || !label || !fields)              return -1;
    if (field_count < 1 || field_count > SETTINGS_MAX_FIELDS) return -1;

    if (!put_str(buf, sizeof(buf), &pos, key))   return -1;
    if (!put_str(buf, sizeof(buf), &pos, label)) return -1;
    if (setting_class == SETTING_CLASS_EPS)
        if (!put_str(buf, sizeof(buf), &pos, service ? service : ""))
            return -1;
    for (i = 0; i < field_count; i++)
        if (!put_field(buf, sizeof(buf), &pos, &fields[i]))
            return -1;

    msg.code         = SETTINGS_DECLARE_ENTRY;
    msg.arg0         = (uint32_t)field_count;
    msg.arg1         = (uint32_t)setting_class;
    msg.arg2         = (setting_class == SETTING_CLASS_EPS) ? read_op  : 0u;
    msg.arg3         = (setting_class == SETTING_CLASS_EPS) ? write_op : 0u;
    msg.payload      = buf;
    msg.payload_size = pos;

    if (IPC_CALL(&msg, &reply) != DOB_OK)
        return -1;
    /* reply.code carries a dob_status_t: 0 on success, negative on a
     * daemon-side rejection (sign-extends correctly back to int). */
    return (int)reply.code;
}

/* ----- Declaration ------------------------------------------------------ */

int declareSetting(const char *key, int type, const char *label,
                   const char *default_value, const char *const *options)
{
    setting_field_t f;
    f.type          = type;
    f.sublabel      = "";                 /* a simple setting has no sub-label */
    f.default_value = default_value;
    f.options       = options;
    return declare_common(key, label, &f, 1,
                           SETTING_CLASS_FILE, (const char *)0, 0u, 0u);
}

int declareSettingMulti(const char *key, const char *label,
                        const setting_field_t *fields, int field_count)
{
    return declare_common(key, label, fields, field_count,
                           SETTING_CLASS_FILE, (const char *)0, 0u, 0u);
}

int declareEpsSetting(const char *key, int type, const char *label,
                      const char *default_value, const char *const *options,
                      const char *service, unsigned read_op, unsigned write_op)
{
    setting_field_t f;
    if (!service) return -1;
    f.type          = type;
    f.sublabel      = "";
    f.default_value = default_value;
    f.options       = options;
    return declare_common(key, label, &f, 1,
                           SETTING_CLASS_EPS, service, read_op, write_op);
}

int declareSettingInvalidation(const char *key, const char *option_value,
                               const char *const *target_keys)
{
    char buf[SETTINGS_MAX_KEY_LEN + SETTINGS_MAX_VALUE_LEN
             + SETTINGS_MAX_INVAL_TARGETS * SETTINGS_MAX_KEY_LEN];
    uint32_t pos = 0;
    int i;
    dob_msg_t msg = {0}, reply = {0};

    if (!key || !option_value)
        return -1;

    if (!put_str(buf, sizeof(buf), &pos, key))          return -1;
    if (!put_str(buf, sizeof(buf), &pos, option_value)) return -1;

    /* Append the NULL-terminated target list (may be empty -> removes). */
    if (target_keys)
        for (i = 0; target_keys[i]; i++)
        {
            if (i >= SETTINGS_MAX_INVAL_TARGETS)
                return -1;
            if (!put_str(buf, sizeof(buf), &pos, target_keys[i]))
                return -1;
        }

    msg.code         = SETTINGS_SET_INVALIDATION;
    msg.payload      = buf;
    msg.payload_size = pos;

    if (IPC_CALL(&msg, &reply) != DOB_OK)
        return -1;
    return (int)reply.code;
}

/* ----- Reading ---------------------------------------------------------- */

/* Fetch the composite value of `key` into the shared buffer.  Returns a
 * pointer to it, or NULL on error.  Valid until the next fetch. */
static const char *fetch_value(const char *key)
{
    static char value_buf[SETTINGS_MAX_COMPOSITE_LEN];

    dob_msg_t msg = {0}, reply = {0};
    uint32_t copy;

    if (!key) return (const char *)0;

    msg.code         = SETTINGS_GET_VALUE;
    msg.payload      = (void *)key;
    msg.payload_size = (uint32_t)strlen(key) + 1u;

    if (IPC_CALL(&msg, &reply) != DOB_OK)          return (const char *)0;
    if (reply.code != (uint32_t)DOB_OK)            return (const char *)0;
    if (!reply.payload || reply.payload_size == 0) return (const char *)0;

    copy = reply.payload_size;
    if (copy > sizeof(value_buf)) copy = sizeof(value_buf);
    memcpy(value_buf, reply.payload, copy);
    value_buf[copy - 1] = '\0';
    return value_buf;
}

const char *getSetting(const char *key)
{
    return fetch_value(key);
}

int settingFieldCount(const char *key)
{
    const char *v = fetch_value(key);
    int count = 1;
    if (!v) return -1;
    for (; *v; v++)
        if (*v == SETTINGS_FIELD_SEP) count++;
    return count;
}

int settingField(const char *key, int index, char *out, unsigned out_size)
{
    const char *v = fetch_value(key);
    int part = 0;
    unsigned n = 0;

    if (!v || !out || out_size == 0 || index < 0)
        return -1;

    /* Walk to the requested part. */
    while (*v && part < index)
    {
        if (*v == SETTINGS_FIELD_SEP) part++;
        v++;
    }
    if (part != index)
        return -1;                  /* index past the last field */

    while (*v && *v != SETTINGS_FIELD_SEP && n < out_size - 1u)
        out[n++] = *v++;
    out[n] = '\0';
    return 0;
}

/* ----- Writing the program's own value ---------------------------------- */

int writeSetting(const char *key, const char *value)
{
    /* key + composite value, NUL-terminated and packed back-to-back.
     * Sized for the worst-case composite at protocol caps. */
    char buf[SETTINGS_MAX_KEY_LEN + SETTINGS_MAX_COMPOSITE_LEN + 1];
    uint32_t pos = 0;
    dob_msg_t msg = {0}, reply = {0};

    if (!key || !value)
        return -1;

    if (!put_str(buf, sizeof(buf), &pos, key))   return -1;
    if (!put_str(buf, sizeof(buf), &pos, value)) return -1;

    msg.code         = SETTINGS_SET_OWN_VALUE;
    msg.payload      = buf;
    msg.payload_size = pos;

    if (IPC_CALL(&msg, &reply) != DOB_OK)
        return -1;
    /* reply.code carries a dob_status_t: 0 on success, negative on a
     * daemon-side rejection (sign-extends correctly back to int). */
    return (int)reply.code;
}
