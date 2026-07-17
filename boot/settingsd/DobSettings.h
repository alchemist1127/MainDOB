#ifndef MAINDOB_STUBS_DOBSETTINGS_H
#define MAINDOB_STUBS_DOBSETTINGS_H

/* DobSettings -- program-facing API for the MainDOB settings system.
 *
 * A program does not open, read or write its `.setting` file: it talks
 * to the settings daemon (settingsd) through the calls below.  The file
 * is created and owned by the daemon; the program only describes its
 * settings and reads their values.
 *
 *
 * ENTRIES AND FIELDS.
 *
 * An entry is one setting.  The editor draws it as one rectangle.
 *
 * Most settings are simple -- one control -- and have a single field.
 * declareSetting() covers that case directly.  A composite setting
 * (screen resolution = width + height, an RGB colour = 3 channels) is
 * still ONE setting, ONE entry, ONE rectangle, but has several fields;
 * declare it with declareSettingMulti() and an array of setting_field_t.
 *
 * A read returns the entry's COMPOSITE value: the field values joined
 * by SETTINGS_FIELD_SEP.  Use settingFieldCount() / settingField() to
 * split it -- the program declared the fields, so it knows their order.
 * A single-field entry's composite is simply its one value, so for the
 * common case getSetting() is used directly with no splitting.
 *
 *
 * Usage -- simple settings:
 *   #include <DobSettings.h>
 *
 *   declareSetting("ui.confirm_exit", SETTING_BOOL,
 *                  "Confirm on exit", "true", 0);
 *
 *   static const char *const themes[] = { "Blue", "Dark", "Light", 0 };
 *   declareSetting("ui.theme", SETTING_ENUM, "Theme", "Blue", themes);
 *
 *   const char *theme = getSetting("ui.theme");
 *
 * Usage -- a composite setting (screen resolution, two fields):
 *   static const setting_field_t res_fields[] = {
 *       { SETTING_STRING, "Width",  "1024", 0 },
 *       { SETTING_STRING, "Height", "768",  0 },
 *   };
 *   declareSettingMulti("display.resolution", "Screen resolution",
 *                       res_fields, 2);
 *
 *   char w[32], h[32];
 *   settingField("display.resolution", 0, w, sizeof(w));   -- w = "1024"
 *   settingField("display.resolution", 1, h, sizeof(h));   -- h = "768"
 *
 * A program may only ever READ its values; it can never write one.
 * Values are changed exclusively by the user, through the DobSettings
 * editor.  This is a security property, not a limitation.
 */

#include <dob/dobsettings_protocol.h>   /* SETTING_*, caps, SETTINGS_FIELD_SEP */

/* One field == one control inside an entry's rectangle. */
typedef struct
{
    int          type;            /* SETTING_BOOL / STRING / ENUM / SECRET   */
    const char  *sublabel;        /* per-field caption (e.g. "Width")       */
    const char  *default_value;   /* initial value for this field           */
    const char *const *options;   /* NULL-terminated; for SETTING_ENUM only  */
} setting_field_t;


/* --- Declaration ------------------------------------------------------- */

/* Declare, or refresh the schema of, a simple single-field FILE-class
 * setting.  `options` is a NULL-terminated array for SETTING_ENUM, or
 * NULL for SETTING_BOOL / SETTING_STRING.  Idempotent: safe on every
 * boot.  Returns 0 on success, < 0 on error. */
int declareSetting(const char *key, int type, const char *label,
                   const char *default_value, const char *const *options);

/* Declare, or refresh the schema of, a composite FILE-class setting:
 * one entry / one rectangle holding `field_count` controls (1..
 * SETTINGS_MAX_FIELDS).  Idempotent.  Returns 0 on success, < 0 on
 * error. */
int declareSettingMulti(const char *key, const char *label,
                        const setting_field_t *fields, int field_count);

/* Driver-only: declare an EPS-class setting -- a single-field scalar
 * whose live value lives in this driver, reached by the editor through
 * `service` with `read_op` / `write_op`, and not persisted in a file.
 * The settings daemon rejects this call from a non-driver caller.
 * Returns 0 on success, < 0 on error. */
int declareEpsSetting(const char *key, int type, const char *label,
                      const char *default_value, const char *const *options,
                      const char *service, unsigned read_op, unsigned write_op);


/* --- Reading ----------------------------------------------------------- */

/* Read the composite value of one of the calling program's own
 * settings.  Returns a pointer to the value string, or NULL if `key`
 * was never declared by this program (a caller bug) or the daemon is
 * unreachable.
 *
 * For a simple (single-field) setting this is the value itself.  For a
 * composite setting it is the fields joined by SETTINGS_FIELD_SEP --
 * prefer settingField() in that case.
 *
 * The string lives in a buffer internal to the stub: it is valid only
 * until the next getSetting() / settingField() / settingFieldCount()
 * call.  Copy it if you must keep it. */
const char *getSetting(const char *key);

/* Number of fields in the composite value of `key`, or < 0 on error.
 * For a simple setting this is 1. */
int settingFieldCount(const char *key);

/* Copy field `index` of `key`'s composite value into `out` (a buffer of
 * `out_size` bytes, always NUL-terminated on success).  Returns 0 on
 * success, < 0 if the key is undeclared, the index is out of range, or
 * the daemon is unreachable. */
int settingField(const char *key, int index, char *out, unsigned out_size);


/* --- Writing the program's own value ----------------------------------- */

/* Persist a new composite value for one of the calling program's OWN
 * settings.  This is the deliberate, narrow exception to "values change
 * only through the editor": use it to save a USER-INITIATED choice made
 * through the program's own UI -- a tray quick-toggle, a view-style
 * switch -- so it survives a restart without forcing the user into the
 * editor.  It writes only the caller's own file (deduced from PID); a
 * program cannot name or write another's.
 *
 * `value` is the composite: for a simple (single-field) setting it is
 * just the value; for a composite one it is the field values joined by
 * SETTINGS_FIELD_SEP.  The daemon validates it against the declared
 * schema (type, enum membership, length, field count) and writes
 * nothing if it does not fit.  Returns 0 on success, < 0 on error
 * (undeclared key, schema mismatch, EPS-class entry, or unreachable
 * daemon).  Declare the entry first; only FILE-class entries are
 * writable here. */
int writeSetting(const char *key, const char *value);


/* --- Editor-style targeting (declaration-time) ------------------------- */

/* Declare one invalidation rule on `key`, an entry this program owns:
 * while `key`'s control holds `option_value`, every entry listed in
 * `target_keys` is greyed out and disabled in the settings editor.  The
 * clause belongs to the option -- call this once per option that
 * disables something (per enum option, or for a checkbox's "true" /
 * "false").  `target_keys` is a NULL-terminated array of entry keys in
 * this same file; a NULL or empty array removes the rule for that
 * option_value.  Call after the controlling entry and all targets are
 * declared.  Idempotent.  Returns 0 on success, < 0 on error.
 *
 * The editor greys the targets out; this program, which reads `key`,
 * must still honour the consequence itself at runtime. */
int declareSettingInvalidation(const char *key, const char *option_value,
                               const char *const *target_keys);

#endif /* MAINDOB_STUBS_DOBSETTINGS_H */
