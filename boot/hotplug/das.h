/* MainDOB Device Automation Script (DAS) — engine for hotplug
 *
 * A DAS is a plain-text file under /SYSTEM/CONFIG/DAS/ that tells
 * hotplug how to recognise a class of hardware, how to draw it on the
 * desktop, and what to do when the user activates its icon.
 *
 * A DAS file has up to four sections:
 *
 *   1. Device parameters (key = value)
 *        bus_type    = legacy_fdc | pci
 *        io_base     = 0x3F0                 (legacy_fdc)
 *        vendor_id   = 0x8086                (pci, exact device)
 *        device_id   = 0x100E                (pci, exact device)
 *        subsystem_vendor = 0x1014           (pci, board variant)
 *        subsystem_device = 0x0140           (pci, board variant)
 *        revision    = 0x02                  (pci, chip stepping)
 *        class       = 0x01                  (pci, top-level fallback)
 *        subclass    = 0x06                  (pci, top-level fallback)
 *        prog_if     = 0x30                  (pci, optional refinement)
 *        rank        = 65000                 (any, optional override)
 *        volume_fs   = fat32                 (subdevice — filesystem volume tag)
 *        kind        = system | GUI          (default: GUI)
 *        label       = Floppy
 *        driver      = /SYSTEM/DRIVERS/floppy/floppy.mdl
 *        service     = floppy0
 *
 *      kind = system tells hotplug to spawn the driver automatically
 *      at hardware-match time, with no desktop icon. kind = GUI keeps
 *      the spawn deferred to user activation and renders the icon.
 *
 *      driver / service are authoritative: when set, hotplug uses them
 *      to spawn the driver and to choose the registry name to wait on.
 *      A device with no matching DAS file is not spawned at all.
 *
 *      For PCI bus_type, vendor_id+device_id win when set; otherwise
 *      class+subclass(+prog_if) match. subsystem_vendor / subsystem_device
 *      narrow a vendor:device match to one specific board variant — set
 *      both for "this Creative Labs SB AWE32 specifically" or leave them
 *      zero for "any board with this chip". revision narrows further to
 *      one chip stepping (rare; default unset).
 *
 *      rank is an optional explicit specificity override: low values =
 *      more specific, high values = less specific. When unset, hotplug
 *      computes specificity from which fields the DAS sets (V:D > class
 *      > class+prog_if > class+SVID:SDID, etc.). Set rank explicitly
 *      only when writing a generic catch-all driver that should be the
 *      last resort — e.g. rank = 65000 for vbe.das so that any specific
 *      GPU driver beats it. Range 0..65535.
 *
 *   2. Icon clipart
 *        bitmap_w    = 48
 *        bitmap_h    = 48
 *        bitmap_fg_r = 255
 *        bitmap_fg_g = 255
 *        bitmap_fg_b = 0
 *        bitmap {
 *        ........###.........
 *        }
 *
 *   3. Action sequence — what to do on icon activation:
 *        action {
 *            spawn         /SYSTEM/DRIVERS/floppy/floppy.mdl   on_fail driver_crash
 *            wait_service  floppy$unit  timeout=3000           on_fail driver_crash
 *            ipc_call      floppy$unit  FLOPPY_PROBE_MEDIA     on_fail no_media
 *            spawn         /SYSTEM/PROGRAMS/DobFiles/DobFiles.mdl
 *            wait_service  DobFiles  timeout=2000
 *            ipc_post      DobFiles  FILES_CMD_NAVIGATE  "/FLOPPY$unit"
 *        }
 *
 *   4. Common errors — named handlers referenced by `on_fail`:
 *        errors {
 *            no_media     = popup_error "Floppy" "Nessun disco nel lettore."
 *            driver_crash = popup_error "Floppy" "Il driver floppy non risponde."
 *        }
 *
 * Variables resolved at execution time:
 *      $unit       — drive unit / device sub-index (0,1,...)
 *      $io_base    — legacy I/O base, hex
 *      $device_id  — opaque hotplug device id
 *
 * Any primitive that fails without an explicit `on_fail` triggers a
 * generic popup_error built from the device label, the failing keyword,
 * and a short diagnostic. Script execution stops on the first failure.
 */

#ifndef MAINDOB_HOTPLUG_DAS_H
#define MAINDOB_HOTPLUG_DAS_H

#include <dob/types.h>
#include <dob/hotplug_events.h>
#include <dob/device_icon.h>

/* Sizing — kept tight for footprint. */
#define DAS_MAX_ENTRIES         32
#define DAS_MAX_ACTIONS         16
#define DAS_MAX_ERRORS          8
#define DAS_MAX_TOKEN           96
#define DAS_MAX_TOKENS_PER_ACTION 6
#define DAS_MAX_MENU_ITEMS      DEV_MENU_MAX_ITEMS
#define DAS_MAX_MENU_PRIMS      6

/* One token in an action / error primitive — keyword and arguments. */
typedef struct
{
    char tokens[DAS_MAX_TOKENS_PER_ACTION][DAS_MAX_TOKEN];
    uint8_t token_count;
    char    on_fail[32];   /* empty string if none */
} das_primitive_t;

/* One named error handler. */
typedef struct
{
    char            name[32];
    das_primitive_t prim;
} das_error_t;

/* One declarative menu entry — a label plus a sequence of primitives
 * to run when the user picks it from the right-side context panel. */
typedef struct
{
    char            label[DEV_MENU_LABEL_LEN];
    das_primitive_t prims[DAS_MAX_MENU_PRIMS];
    uint8_t         prim_count;
} das_menu_entry_t;

/* A parsed DAS entry. */
typedef struct
{
    bool          used;

    /* Filename basename without .das extension — e.g. "floppy",
     * "cdrom_ide". Stable identifier across reboots; used by external
     * tools (the installer wizard, debug dumps) to refer to a DAS
     * independent of its display label. */
    char          name[32];

    /* Section 1: device parameters */
    uint8_t       bus_type;
    uint16_t      io_base;
    uint16_t      vendor_id;
    uint16_t      device_id;
    uint8_t       class_code;   /* BUS_TYPE_SUBDEVICE / PCI fallback match key */
    uint8_t       subclass;     /* BUS_TYPE_SUBDEVICE / PCI fallback match key */
    uint8_t       prog_if;      /* PCI refinement; 0xFF means "any" (unset) */
    uint16_t      subsystem_vendor;  /* PCI subsystem vendor; 0 = any */
    uint16_t      subsystem_device;  /* PCI subsystem device; 0 = any */
    int16_t       revision;     /* PCI chip stepping; -1 = any */
    int32_t       rank;         /* Specificity override; -1 = auto */
    uint8_t       volume_fs;    /* SUBDEVICE filesystem tag — VOLUME_FS_*; 0 = any */
    uint32_t      kind;
    char          label[32];

    /* Driver to spawn and service name to wait on at match time.
     * A DAS file with no `driver` line means "match the device but
     * don't spawn anything" — used for legacy bubbles (floppy,
     * CD-ROM) whose driver is launched lazily from the action block
     * on user interaction. */
    char          driver[128];
    char          service[32];

    /* Section 2: icon clipart */
    icon_bitmap_t bitmap;

    /* Section 3: action sequence (run on double-click via ICON_ACTIVATED) */
    das_primitive_t actions[DAS_MAX_ACTIONS];
    uint8_t         action_count;

    /* Section 4: error handlers */
    das_error_t errors[DAS_MAX_ERRORS];
    uint8_t     error_count;

    /* Section 5: declarative context-panel menu (run on single-click
     * entry pick via MENU_ACTIVATED) */
    das_menu_entry_t menu[DAS_MAX_MENU_ITEMS];
    uint8_t          menu_count;
} das_entry_t;

/* Variable substitution context — supplied at action execution time.
 *
 * Legacy fields (floppy/IDE): device_id, unit, io_base.
 * Sub-device fields (AHCI optical, future others): provider_service +
 * provider_token carry the identity the provider driver expects in the
 * payload of its SCSI PACKET calls. These are exposed to DAS files as
 * $provider and $token respectively. */
typedef struct
{
    uint32_t device_id;
    uint8_t  unit;
    uint16_t io_base;
    char     provider_service[32];
    uint32_t provider_token;
    /* Forwarded from ICON_ACTIVATED's icon_activated_t. Passed as
     * arg0 of every ipc_call DAS primitive so drivers performing
     * probe-and-mount can route the resulting OpenMount to a
     * specific window (DobFiles' Monta view) instead of spawning a
     * fresh satellite. 0 = no target (desktop double-click path). */
    uint32_t hijack_target_port;
} das_run_ctx_t;

/* Public API */

/* Scan /SYSTEM/CONFIG/DAS/ and load every *.das file. Idempotent —
 * safe to call once at hotplug boot. */
void das_load_all(void);

/* Number of currently loaded DAS entries. */
int das_count(void);

/* Pointer to entry by index, or NULL on out-of-range. */
const das_entry_t *das_get(int idx);

/* Find the DAS entry whose signature matches the given hardware.
 * Returns the entry index in [0, das_count()), or -1 if no match.
 * When several DAS files match, returns the most specific one
 * (highest score, with explicit `rank` overriding the auto-computed
 * specificity). */
int das_match(const hw_device_t *hw);

/* Same as das_match, but returns up to `max` matching indices in
 * `out_indices`, sorted from most specific to least specific. The
 * caller uses this when it needs the fallback chain — try the first,
 * if the driver fails to attach try the next, and so on. Returns the
 * number of matches written (0..max). */
int das_match_all(const hw_device_t *hw, int *out_indices, int max);

/* Fill a gui_device_attach_t payload from the given DAS entry plus the
 * runtime device_id and unit. Variable substitution is applied to the
 * label so that DAS files can use $unit1 (1-indexed unit, e.g. for
 * "Floppy 1" / "Floppy 2") or $unit (0-indexed). service_name is set
 * to "hotplug" so that dobinterface routes Eject/Activate back through
 * the hotplug pipeline. Returns 0 on success, -1 on bad index. */
int das_fill_attach_payload(int idx, uint32_t device_id, uint8_t unit,
                            gui_device_attach_t *out);

/* Execute the action sequence of the given DAS entry. Variable
 * references in arguments are resolved against ctx. On primitive
 * failure, the matching `on_fail` error is run; if none, a generic
 * popup_error is shown. Execution stops on first failure. */
void das_run_action(int idx, const das_run_ctx_t *ctx);

/* Execute the primitive sequence of one menu entry of the given DAS
 * entry. Same execution semantics as das_run_action — variable
 * substitution from ctx, on_fail handling, stop on first failure. */
void das_run_menu(int idx, int menu_idx, const das_run_ctx_t *ctx);

#endif /* MAINDOB_HOTPLUG_DAS_H */
