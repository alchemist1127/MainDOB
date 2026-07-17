/* DobSettings IPC protocol -- opcodes and wire formats.
 *
 * MainDOB's settings system.  Two MDLs cooperate:
 *
 *   - settingsd.mdl   -- the daemon.  A `critical` service launched at
 *     boot from Startup_modules.  It is the ONLY process that touches
 *     `.setting` files on disk: it parses them, validates writes,
 *     persists atomically (temp file + rename), and keeps the registry
 *     of known `.setting` files.  Always resident, because programs
 *     read settings at any time.
 *
 *   - DobSettings.mdl -- the editor.  An on-demand GUI program.  It
 *     does NOT touch `.setting` files directly, not even to read: it
 *     is a client of the daemon.  It is the only client permitted to
 *     write a setting's value.
 *
 * This header is the wire contract shared by both MDLs and by the
 * libdob client stub.  Normal programs do not include it -- they use
 * the <DobSettings.h> stub, which hides the wire entirely.
 *
 * Transport: the standard dob_msg_t (arg0..arg3 + payload).  Payload
 * strings are NUL-terminated and packed back-to-back.  Status is
 * returned in reply.code as a dob_status_t.
 *
 *
 * IDENTITY -- the foundation of the security model.
 *
 * The daemon identifies every caller by PID.  SYS_GET_HOME_DIR(pid)
 * yields the caller's home ("/SYSTEM/PROGRAMS/<name>/"); the exact
 * final path component is the program name.  This is the same
 * spoof-resistant scheme DobFileSystem uses for its sandbox -- an
 * exact basename match, never a substring test.
 *
 * A program's `.setting` file is DEDUCED from its identity.  A program
 * never names a settings file: the daemon resolves "<name>" from the
 * caller's PID and operates on that file alone.  A program therefore
 * cannot address another program's settings -- it has no way to name
 * one.
 *
 * The editor is the single identity recognised as privileged.  The
 * daemon matches the caller's program name against SETTINGS_EDITOR_NAME;
 * only that caller may issue the editor-family opcodes below.
 *
 *
 * WHO MAY DO WHAT.
 *
 *   Owning program  -- DECLARE_ENTRY, GET_VALUE, SET_INVALIDATION,
 *                      SET_OWN_VALUE.  Operates only on its own file
 *                      (deduced from PID).  May read values and declare
 *                      schema.  It may write a value ONLY through
 *                      SET_OWN_VALUE, and only into its own deduced
 *                      file: the narrow, user-initiated exception (a
 *                      tray quick-toggle, a view-style switch) to "a
 *                      value changes through the editor".  It can never
 *                      name, address or write another program's file.
 *   Editor          -- LIST_FILES, READ_SCHEMA, SET_VALUE.  Names files
 *                      explicitly.  The writer of any file's values.
 *   Anyone          -- PING.
 *
 * The daemon applies an identity gate before dispatch: an opcode not
 * permitted for the caller's class is rejected with DOB_ERR_DENIED
 * before any work is done.
 *
 *
 * ENTRIES AND FIELDS.
 *
 * An entry is one setting.  The editor draws it as one rectangle.
 *
 * Most settings are simple -- a single control -- and have one FIELD.
 * A composite setting (screen resolution, an RGB colour, page margins)
 * is still ONE setting, ONE entry, ONE rectangle, but carries several
 * fields, each its own control with its own sub-label; the rectangle
 * grows to fit them.  A field has its own type, so one rectangle may
 * mix controls.
 *
 * The entry's stored value is the COMPOSITE of its field values, joined
 * by SETTINGS_FIELD_SEP.  A program reads that one composite string via
 * getSetting() and splits it -- it declared the fields, so it knows
 * their count and order.  GET_VALUE and SET_VALUE therefore stay
 * one-value-per-entry; only DECLARE_ENTRY and READ_SCHEMA carry the
 * field structure.  A single-field entry's composite is simply its one
 * value, with no separator -- simple settings are unaffected.
 *
 *
 * TWO CLASSES OF ENTRY.
 *
 *   SETTING_CLASS_FILE -- the daemon holds both the schema and the
 *     value.  The value is persisted in the `.setting` file.  This is
 *     the ordinary case (program preferences, system data settings).
 *
 *   SETTING_CLASS_EPS  -- the daemon holds only the schema, including a
 *     route: an EPS service name plus a read opcode and a write
 *     opcode.  The live value lives in a device/driver, not in a file.
 *     The editor renders the control from the schema, then performs
 *     GET/SET directly against the named EPS service.  The daemon never
 *     sees the value.  EPS class is limited to scalar settings (audio
 *     volume, output device, power profile) and is therefore always
 *     single-field.  A struct-valued or transactional device setting
 *     (display mode, multi-monitor arrangement) is not expressible here
 *     and belongs to a bespoke editor panel.
 *
 * An EPS-class entry is effectively code: it directs the editor's IPC.
 * Declaring one is therefore privileged -- the daemon accepts a
 * DECLARE_ENTRY with class SETTING_CLASS_EPS only from a system-trusted
 * caller (home under /SYSTEM/DRIVERS/).  An ordinary program attempting
 * it receives DOB_ERR_DENIED.
 */

#ifndef MAINDOB_DOBSETTINGS_PROTOCOL_H
#define MAINDOB_DOBSETTINGS_PROTOCOL_H

#include <dob/types.h>

/* Registry service name of the daemon. */
#define SETTINGS_SERVICE_NAME       "settingsd"

/* Program name of the editor -- the single privileged identity for the
 * editor-family opcodes.  The daemon matches the caller's home basename
 * against this string. */
#define SETTINGS_EDITOR_NAME        "DobSettings"


/* ----- Capacity caps -- enforced by the daemon. --------------------------
 * A DECLARE_ENTRY that would exceed a cap, or any string argument longer
 * than its cap, is rejected (DOB_ERR_NO_SPACE / DOB_ERR_INVALID).
 * Enforcing the per-file entry cap at declare time guarantees a
 * READ_SCHEMA reply can never overflow.  All values are tunable; bump
 * them in lockstep across daemon, editor and stub. */
#define SETTINGS_MAX_ENTRIES         32    /* entries (settings) per file      */
#define SETTINGS_MAX_FIELDS           4    /* controls per entry / rectangle   */
#define SETTINGS_MAX_KEY_LEN         64    /* incl. NUL                        */
#define SETTINGS_MAX_LABEL_LEN       96    /* incl. NUL -- rectangle caption    */
#define SETTINGS_MAX_SUBLABEL_LEN    40    /* incl. NUL -- per-field caption    */
#define SETTINGS_MAX_VALUE_LEN      256    /* incl. NUL -- one field value      */
#define SETTINGS_MAX_OPTIONS         12    /* enum options per field           */
#define SETTINGS_MAX_OPTION_LEN      48    /* incl. NUL, per option            */
#define SETTINGS_MAX_SERVICE_LEN     64    /* incl. NUL -- EPS service name     */
#define SETTINGS_MAX_NAME_LEN        64    /* incl. NUL -- .setting file name   */

/* Invalidation -- an entry's options can grey out other entries.  A
 * rule belongs to one option value of a control: while that value is
 * the selected one, the rule's target entries are disabled.  An entry
 * holds up to SETTINGS_MAX_INVAL_RULES rules (typically one per option
 * that has a clause); each rule lists up to SETTINGS_MAX_INVAL_TARGETS
 * target entry keys. */
#define SETTINGS_MAX_INVAL_RULES      8    /* invalidation rules per entry     */
#define SETTINGS_MAX_INVAL_TARGETS    8    /* target entries per rule          */

/* The composite of an entry's field values: the GET_VALUE / SET_VALUE
 * wire value and the getSetting() return.  Worst case is every field at
 * its cap plus the separators. */
#define SETTINGS_MAX_COMPOSITE_LEN  (SETTINGS_MAX_FIELDS * SETTINGS_MAX_VALUE_LEN)

/* Field separator inside a composite value.  ASCII Unit Separator: a
 * control character that does not occur in normal setting values.  The
 * daemon rejects a field value that contains it.  A single-field entry
 * has no separator in its composite. */
#define SETTINGS_FIELD_SEP          '\x1F'


/* ----- Control type -- how the editor renders one field. -----------------
 * Declared per field; carried in the schema; consumed only by the
 * editor.  The program's read path never sees the type -- it gets the
 * composite value as a string and interprets its own data. */
#define SETTING_BOOL                 0     /* checkbox                        */
#define SETTING_STRING               1     /* textbox                         */
#define SETTING_ENUM                 2     /* dropdown list                   */
/* A masked textbox: the editor echoes bullets, never the characters,
 * and refuses Copy/Cut on the control.  Reserved to EPS-class entries
 * BY THE DAEMON: a secret must never land in a `.setting` file in
 * clear, so a FILE-class declaration with a SECRET field is rejected.
 * The canonical consumer is a change-secret handshake: the editor
 * sends the typed text to the owning service's write_op on Apply and
 * the service takes it from there; read_op replies an empty value, so
 * nothing is ever echoed back.  The editor commits a SECRET entry
 * only when the user actually typed in it (see commit_entry), and
 * clears the box after a successful commit. */
#define SETTING_SECRET               3     /* masked textbox (EPS-class only) */


/* ----- Entry class -- where the value lives. -----------------------------
 * Declared in DECLARE_ENTRY (arg1). */
#define SETTING_CLASS_FILE           0     /* value persisted by the daemon   */
#define SETTING_CLASS_EPS            1     /* value lives in a driver via EPS */


/* =========================================================================
 *  Opcodes -- msg.code.  Range 420..439 reserved for DobSettings.
 *  All synchronous (send + wait for reply).
 * ========================================================================= */

/* --- Family A: owning program.  File deduced from sender PID. ------------ */

/* DECLARE_ENTRY -- declare, or update the schema of, one entry (one
 * setting) of the caller's own .setting file.  Idempotent: safe on
 * every boot.
 *
 *   arg0 = field_count    (>= 1; an EPS-class entry must use 1)
 *   arg1 = class          (SETTING_CLASS_FILE / SETTING_CLASS_EPS)
 *   arg2 = eps_read_op    } EPS class only; opcode on the EPS service.
 *   arg3 = eps_write_op   } Ignored (pass 0) for FILE class.
 *
 *   payload:
 *       key '\0' label '\0'
 *       [ service '\0' ]                 -- EPS class only
 *       then, field_count times, one field block:
 *           settings_field_hdr_t          -- type, option_count
 *           sublabel '\0' default '\0'
 *           option '\0' * option_count    -- enum fields only
 *
 *   reply.code = dob_status_t
 *
 * The wire carries each field's `default`, never its value: it is
 * structurally impossible to set a value through this call.
 *
 *   - entry absent  -> created; each field's value := its default.
 *   - entry present -> the schema (label, field list, types, defaults,
 *                      options, route) is updated; stored field values
 *                      are left untouched.  If a schema change makes a
 *                      field's value invalid (e.g. an enum option was
 *                      dropped, or the field count changed), that
 *                      field's value is reverted to its default.
 *
 * The daemon persists the file automatically after a successful call;
 * the program performs no file I/O.  The first DECLARE_ENTRY for a
 * program creates its .setting file and registers it.
 *
 * Declaring an EPS-class entry is privileged (see header comment); a
 * non-privileged caller receives DOB_ERR_DENIED. */
#define SETTINGS_DECLARE_ENTRY      420

/* GET_VALUE -- read the composite value of one entry of the caller's
 * own file.
 *   payload       = key '\0'
 *   reply.code    = dob_status_t  (DOB_ERR_NOT_FOUND if key undeclared)
 *   reply.payload = value '\0'    (on DOB_OK)
 *
 * The value is the entry's field values joined by SETTINGS_FIELD_SEP;
 * for a single-field entry it is just that field's value.  A declared
 * entry always has a value (defaults at creation). */
#define SETTINGS_GET_VALUE          421

/* SET_INVALIDATION -- declare one invalidation rule on an entry of the
 * caller's own file: when that entry's control holds `option_value`,
 * the listed target entries are invalidated (the editor greys out and
 * disables their rectangles).  The clause belongs to the OPTION, not
 * the entry: a dropdown declares one rule per option that disables
 * something; a checkbox declares a rule for "true" and/or "false".
 * Idempotent -- re-declaring the rule for the same (key, option_value)
 * replaces its target list.
 *
 *   payload    = key '\0' option_value '\0' target_key '\0' target_key '\0' ...
 *   reply.code = dob_status_t  (DOB_ERR_NOT_FOUND if `key` undeclared)
 *
 * `key` is the controlling entry; `option_value` is the value of its
 * first field that triggers the rule; the remaining strings (zero or
 * more, to the end of the payload) are the target entry keys.  An empty
 * target list removes the rule for that option_value.  Call this after
 * the controlling entry and all targets have been declared.  The daemon
 * only stores and transports the rule -- it enforces nothing; the
 * grey-out is the editor's, and the program, which reads the controlling
 * setting, honours the consequence itself at runtime. */
#define SETTINGS_SET_INVALIDATION   422

/* SET_OWN_VALUE -- the owning program writes the composite value of one
 * entry of ITS OWN file (deduced from sender PID, exactly like
 * GET_VALUE).  This is the single, deliberate exception to "only the
 * editor writes a value": it lets a program persist a USER-INITIATED
 * change made through its own UI -- a tray quick-toggle, a view-style
 * switch -- without routing the user through the editor.  The change is
 * still the user's; the program still reaches only its own file (it has
 * no way to name another's), so the isolation guarantee is intact.
 *
 *   payload    = key '\0' value '\0'
 *   reply.code = dob_status_t
 *
 * `value` is the composite (field values joined by SETTINGS_FIELD_SEP),
 * validated against the entry's schema EXACTLY as SET_VALUE is: a wrong
 * part count, a SETTING_BOOL part not "true"/"false", a SETTING_ENUM
 * part not among that field's options, or an over-long part is rejected
 * with DOB_ERR_INVALID and nothing is written.  FILE class only -- an
 * EPS-class entry returns DOB_ERR_INVALID (its live value lives in the
 * driver, written straight there).  DOB_ERR_NOT_FOUND if the key was
 * never declared by this program.  On success the file is persisted
 * atomically, the same temp-file + rename path SET_VALUE uses. */
#define SETTINGS_SET_OWN_VALUE      423

/* 424..429 reserved for the program family. */


/* --- Family B: editor only.  Caller must be SETTINGS_EDITOR_NAME. -------- */

/* LIST_FILES -- enumerate every registered .setting file, for the
 * editor's listbox.
 *   reply.arg0    = file count
 *   reply.payload = name '\0' name '\0' ...  (SETTINGS_MAX_NAME_LEN each) */
#define SETTINGS_LIST_FILES         430

/* READ_SCHEMA -- full schema of one .setting file.
 *   payload       = name '\0'        (target file)
 *   reply.arg0    = entry count
 *   reply.payload = entry record * count  (see "Schema wire" below)
 *
 * For a FILE-class entry a field record's value is the live stored
 * value.  For an EPS-class entry the daemon does not hold the value:
 * the value carries the declared default, which the editor uses as a
 * placeholder until its own EPS read of the device returns. */
#define SETTINGS_READ_SCHEMA        431

/* SET_VALUE -- write the composite value of one entry.  FILE-class only:
 * an EPS-class value is written by the editor straight to the device's
 * EPS service, not through the daemon.
 *   payload    = name '\0' key '\0' value '\0'
 *   reply.code = dob_status_t
 *
 * `value` is the composite (field values joined by SETTINGS_FIELD_SEP).
 * The daemon splits it into exactly field_count parts and validates
 * each against its field's schema: a SETTING_BOOL part not
 * "true"/"false", a SETTING_ENUM part not among that field's options,
 * an over-long part, or a wrong part count, is rejected with
 * DOB_ERR_INVALID and nothing is written.  On success the file is
 * persisted atomically.  "Reset to default" is simply a SET_VALUE
 * carrying the composite of the entry's per-field defaults -- no
 * dedicated opcode. */
#define SETTINGS_SET_VALUE          432

/* 433..437 reserved for the editor family. */

/* 438 reserved -- SETTINGS_SUBSCRIBE.  Future: a client subscribes a
 * port to SETTINGS_NOTIFY_CHANGED for live propagation of edits.  The
 * v1 model is apply-on-next-launch; this opcode and the notify bit
 * below are reserved so live update becomes an addition, not a
 * redesign. */


/* --- Family C: common. --------------------------------------------------- */

/* PING -- liveness check / reconnect after a daemon respawn.
 *   reply.code = DOB_OK */
#define SETTINGS_PING               439


/* =========================================================================
 *  Schema wire -- the record format of a READ_SCHEMA reply.
 *
 *  The reply payload is `entry_count` records packed back-to-back.
 *  A record is:
 *
 *      settings_entry_hdr_t                 -- class, field_count,
 *                                              inval_rule_count, route
 *      key '\0' label '\0'
 *      [ service '\0' ]                     -- iff class == EPS
 *      then field_count times, one field block:
 *          settings_field_hdr_t             -- type, option_count
 *          sublabel '\0' value '\0' default '\0'
 *          option '\0' * option_count
 *      then inval_rule_count times, one invalidation-rule block:
 *          option_value '\0'                -- the triggering value
 *          uint8_t target_count
 *          target_key '\0' * target_count
 *
 *  An invalidation-rule block says: while this entry's control holds
 *  option_value, the listed target entries are greyed out.  The count
 *  of rules is in the entry header; each rule self-sizes its targets.
 *
 *  Every count needed to walk the record sits in a header that precedes
 *  the data it sizes, so records need no separators.
 * ========================================================================= */

typedef struct
{
    uint8_t  setting_class;   /* SETTING_CLASS_FILE / SETTING_CLASS_EPS  */
    uint8_t  field_count;     /* controls in this entry; >= 1            */
    uint8_t  inval_rule_count;/* invalidation rules that follow the fields */
    uint8_t  reserved;        /* pad to 4; pass 0                        */
    uint32_t eps_read_op;     /* EPS class: read opcode;  0 for FILE     */
    uint32_t eps_write_op;    /* EPS class: write opcode; 0 for FILE     */
} settings_entry_hdr_t;

/* Per-field header.  Used both in the DECLARE_ENTRY payload and in a
 * READ_SCHEMA record, immediately before each field's string block. */
typedef struct
{
    uint8_t  type;            /* SETTING_BOOL / STRING / ENUM            */
    uint8_t  option_count;    /* enum options that follow; 0 otherwise   */
} settings_field_hdr_t;


/* ----- Async notification ------------------------------------------------
 * Reserved for the future live-update path (see opcode 438).  The daemon
 * would raise this bit on subscribed ports after a committed SET_VALUE.
 * Unused by the v1 apply-on-next-launch model. */
#define SETTINGS_NOTIFY_CHANGED     (1u << 0)


/* =========================================================================
 *  Live "Apply" -- the editor-to-program reload notification.
 *
 *  This is NOT a daemon opcode.  When the user presses Apply in the
 *  editor, the editor commits the values (SET_VALUE) and then sends a
 *  message with this `code` directly to the program that owns the file
 *  -- found in the registry under the program's own name (the same name
 *  as its `.setting` file).
 *
 *  It is sent as a fire-and-forget POST (dob_ipc_post): the editor does
 *  NOT block on it and the receiving program owes NO reply.  This is
 *  deliberate -- a synchronous call would freeze the editor on a slow,
 *  buggy, or non-replying reload handler.
 *
 *  A program opts in to live updates by (1) registering itself in the
 *  registry under its name and (2) handling this code.  An app-framework
 *  program receives it in event_request(); a server program receives it
 *  in its message handler.  In neither case does it reply.  What "apply"
 *  means is entirely the program's business: in the handler it re-reads
 *  its settings with getSetting() and does whatever it needs.  A program
 *  that does not register, or does not handle this code, simply picks
 *  the new values up on its next launch.
 *
 *  The value is a distinctive constant, well clear of the 420..439
 *  daemon range and unlikely to collide with a program's own request
 *  codes. */
#define SETTINGS_RELOAD_CALL        0x5E771140u

#endif /* MAINDOB_DOBSETTINGS_PROTOCOL_H */
