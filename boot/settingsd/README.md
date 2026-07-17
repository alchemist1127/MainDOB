# MainDOB settings system

Decentralised, per-program settings.  There is no central settings
file: every program that needs configuration gets its own `.setting`
file, stored in its own folder.

## Components

- **`boot/settingsd/`** — the settings daemon (`settingsd.mdl`).  A
  `critical` OS service.  The *only* process that touches `.setting`
  files on disk: it parses them, validates writes, persists atomically
  (temp file + rename) and keeps a registry of known files.  Installed
  under `/SYSTEM/OperatingSystem/settingsd/`; listed in
  `Startup_modules` as `driver primary needs:DobFileSystem`.

- **`programs/DobSettings/`** — the editor (`DobSettings.mdl`).  An
  on-demand GUI.  It does not touch `.setting` files: it is a client
  of the daemon, and the only client allowed to write a value.  Left
  pane: a listbox of every `.setting` file.  Right pane: one rectangle
  per setting, scrollable.

- **`libdob/include/dob/dobsettings_protocol.h`** — the IPC wire
  contract shared by daemon, editor and stub.

- **`boot/settingsd/DobSettings.h` + `DobSettings_stub.c`** — the
  program-facing API.  A program includes `<DobSettings.h>` and never
  sees the wire.

## Security model

A program may **declare** its settings and **read** their values.  It
may also write **its own** value through `writeSetting()` — the single,
deliberate exception, meant for a user-initiated quick-toggle in the
program's own UI (a tray applet, a view-style switch) so the choice
persists without a trip through the editor.  The daemon scopes that
write to the caller's own `.setting` file, deduced from its identity:
a program can never name, address, or write **another** program's
file.  `DECLARE_ENTRY` still carries only defaults, never values, so
the editor remains the only way to set values across files, and the
write-own path is validated against the declared schema exactly as the
editor's writes are.

Caller identity is the program's home-folder basename, resolved from
its PID via `SYS_GET_HOME_DIR` — an exact match, the same
spoof-resistant scheme DobFileSystem's sandbox uses.  A program's
`.setting` file is *deduced* from its identity; it is never named, so
a program cannot address another's settings.  Only the editor's
identity may issue the editor-family opcodes.

## Entries and fields

One **entry** is one setting, drawn as one rectangle.  An entry has
one or more **fields**, each a control (checkbox / textbox /
dropdown).  A simple setting has one field; a composite one — e.g.
screen resolution = width + height — is still one entry / one
rectangle but has several fields, and the rectangle grows to fit them.

A fourth control type, `SETTING_SECRET`, renders as a **masked**
textbox (bullet echo, Copy/Cut refused).  The daemon accepts it only
inside an EPS-class entry: a secret must never be persisted in clear
in a `.setting` file.  Its canonical use is a change-secret handshake
— the editor sends the typed text straight to the owning service's
`write_op` on Apply (and only if the user actually typed something),
the service verifies and takes over; `read_op` replies an empty value
so nothing is ever echoed back.  First consumer: dobinterface's
`sicurezza.logon_password` (the logon password change flow).

The entry's stored value is the composite of its field values joined
by `SETTINGS_FIELD_SEP`.  `getSetting()` returns that composite;
`settingField()` splits it.  A single-field entry's composite is just
its one value.

## Two classes of entry

- **FILE class** — the daemon holds the schema and the value; the
  value lives in the `.setting` file.

- **EPS class** — the daemon holds only the schema, including a route
  (an EPS service name plus read/write opcodes).  The live value lives
  in a device driver.  The editor reads and writes it straight to that
  driver, bypassing the daemon.  Used for scalar device settings
  (audio volume, output device, power profile).  Declaring an EPS-class
  entry is driver-only, because it directs the editor's IPC.

## Program usage

```c
#include <DobSettings.h>

/* At startup -- idempotent, safe on every boot. */
declareSetting("ui.confirm_exit", SETTING_BOOL,
               "Confirm on exit", "true", 0);

static const setting_field_t res[] = {
    { SETTING_STRING, "Width",  "1024", 0 },
    { SETTING_STRING, "Height", "768",  0 },
};
declareSettingMulti("display.resolution", "Screen resolution", res, 2);

/* Any time after. */
const char *confirm = getSetting("ui.confirm_exit");

char w[32], h[32];
settingField("display.resolution", 0, w, sizeof(w));
settingField("display.resolution", 1, h, sizeof(h));
```
