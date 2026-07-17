/* MainDOB Desktop Device Icon Protocol
 *
 * Between dobinterface (desktop shell, consumer) and any provider
 * module that wants to expose a device as a desktop icon.
 *
 * Flow:
 *   1. Provider calls dob_server_init("my_service").
 *   2. Provider looks up dobinterface via dob_registry_find.
 *   3. Provider posts GUI_DEVICE_ATTACH with a gui_device_attach_t
 *      payload (the bitmap is shipped by the provider, not embedded
 *      in dobinterface).
 *   4. Dobinterface places the icon (counter-clockwise from bottom-
 *      right) and listens for clicks.
 *   5. Single click → context panel "Apri" / "Espelli".
 *   6. Double click → "Apri" shortcut.
 *   7. "Espelli" → dobinterface posts PROVIDER_EJECT_REQ; provider
 *      cleans up and posts GUI_DEVICE_DETACH back to remove the icon.
 */

#ifndef MAINDOB_DOB_DEVICE_ICON_H
#define MAINDOB_DOB_DEVICE_ICON_H

#include <dob/types.h>

/* IPC codes received by dobinterface (incoming from providers).
 * 150..199 is the device-icon range. 100..142 is already used by
 * GUI_WIN_*, GUI_PANEL_*, GUI_WIDGET_*. */
#define GUI_DEVICE_ATTACH       150
#define GUI_DEVICE_DETACH       151

/* Query the WM for the current desktop device roster. Reply payload
 * is gui_device_list_t followed by `count` packed
 * gui_device_list_entry_t records. Used by DobFiles' "Monta" view
 * to surface mountable devices in-window without forcing the user
 * to interact with the desktop. */
#define GUI_LIST_DEVICES        152

/* Hard cap on devices reported in a single GUI_LIST_DEVICES reply.
 * Mirrors MAX_DESKTOP_ICONS in dobinterface; if either grows the
 * other should follow. */
#define DEV_LIST_MAX_ENTRIES    64

/* IPC code received by provider modules (incoming from dobinterface). */
#define PROVIDER_EJECT_REQ      300

/* IPC code received by hotplug (incoming from dobinterface): the user
 * double-clicked an icon. The payload is icon_activated_t with
 * device_id. Hotplug looks up which DAS matched this device and runs
 * that DAS's action {} sequence. */
#define ICON_ACTIVATED          301

/* IPC code received by hotplug (incoming from dobinterface): the user
 * picked one of the entries in the right-side context panel for a
 * device icon. The payload is menu_activated_t with device_id and
 * the zero-based index of the menu entry as declared in the DAS
 * file's menu {} section. Hotplug runs the matching primitive list. */
#define MENU_ACTIVATED          302

/* Maximum number of declarative menu entries a DAS may declare for
 * one device. Kept tight to bound gui_device_attach_t footprint. */
#define DEV_MENU_MAX_ITEMS      8
#define DEV_MENU_LABEL_LEN      32

/* Device kind — controls hotplug execution policy and UI visibility.
 *
 *   DEV_KIND_GUI    — icon on desktop; driver spawn deferred until the
 *                     user activates the icon (double-click or menu).
 *                     This is the path used today by floppy and CD-ROM.
 *   DEV_KIND_SYSTEM — no icon, no user interaction. The driver is
 *                     spawned automatically the moment hotplug matches
 *                     the device. Used for audio, video, NIC and any
 *                     device that must be live without user prompting.
 *
 * Default when a DAS file omits the `kind` line is DEV_KIND_GUI, so
 * pre-existing DAS files keep their current behaviour.
 *
 * The DEV_KIND_STORAGE / DEV_KIND_USB_HOST / DEV_KIND_UNKNOWN names are
 * kept as aliases so older DAS files (kind = storage) parse and behave
 * exactly as they did before — they all map onto DEV_KIND_GUI. */
#define DEV_KIND_GUI            1
#define DEV_KIND_SYSTEM         3

/* Backward-compatible aliases — semantically equivalent to GUI. */
#define DEV_KIND_UNKNOWN        DEV_KIND_GUI
#define DEV_KIND_STORAGE        DEV_KIND_GUI
#define DEV_KIND_USB_HOST       DEV_KIND_GUI

/* Icon bitmap formats */
#define ICON_FMT_1BPP_MASK      0   /* 1 bit per pixel mask, fg colour applied */

/* Icon bitmap dimensions — versioned cap so the struct stays stable */
#define ICON_MAX_W              64
#define ICON_MAX_H              64
#define ICON_DATA_MAX           (ICON_MAX_W * ICON_MAX_H / 8)   /* 512 bytes for 1bpp */

/* Packed bitmap shipped in GUI_DEVICE_ATTACH.
 *
 * For format 0 (1bpp mask):
 *   - data is row-major, MSB-first.
 *   - Each set bit is drawn using (fg_r, fg_g, fg_b) on the desktop
 *     blue background. Cleared bits are transparent.
 *   - Rows are padded up to the nearest byte: stride = (width + 7) / 8.
 *   - Required buffer size = stride * height bytes.
 */
typedef struct
{
    uint16_t width;
    uint16_t height;
    uint8_t  format;
    uint8_t  fg_r;
    uint8_t  fg_g;
    uint8_t  fg_b;
    uint8_t  data[ICON_DATA_MAX];
} icon_bitmap_t;

/* Payload of GUI_DEVICE_ATTACH.
 *
 * device_id:    opaque id chosen by the provider. Must be unique
 *               within the provider so DETACH can target it.
 * kind:         one of DEV_KIND_* (hint only).
 * label:        short human-readable label under the icon.
 * service_name: the name the provider registered with dob_server_init,
 *               used by dobinterface to resolve the provider's port
 *               when sending PROVIDER_EJECT_REQ.
 * bitmap:       the icon artwork.
 */
typedef struct
{
    uint32_t      device_id;
    uint32_t      kind;
    char          label[32];
    char          service_name[32];
    icon_bitmap_t bitmap;
    uint8_t       menu_count;
    char          menu_items[DEV_MENU_MAX_ITEMS][DEV_MENU_LABEL_LEN];
} gui_device_attach_t;

/* Payload of GUI_DEVICE_DETACH. Matches the device_id of an earlier
 * GUI_DEVICE_ATTACH. */
typedef struct
{
    uint32_t device_id;
} gui_device_detach_t;

/* Payload of PROVIDER_EJECT_REQ. The provider is being asked to tear
 * down the device identified by device_id and, once clean, post a
 * GUI_DEVICE_DETACH back to dobinterface. */
typedef struct
{
    uint32_t device_id;
} provider_eject_req_t;

/* Payload of ICON_ACTIVATED. The user activated (double-clicked or
 * chose "Apri") the desktop icon for device_id. Sent from dobinterface
 * to hotplug, which executes the matched DAS's action sequence. */
typedef struct
{
    uint32_t device_id;
    /* IPC port of the originator that wants to "own" the mount
     * resulting from this activation. Propagated unchanged through
     * hotplug → DAS → driver → OpenMount; the driver passes it as
     * arg0 of the OpenMount call, which uses it to IPC-hijack that
     * specific port instead of doing a registry lookup for the
     * generic "DobFiles" name.
     *
     *   0  → no specific target: OpenMount spawns a satellite
     *        (standard behaviour for desktop double-clicks)
     *  !=0 → target known: OpenMount IPC-calls that port directly
     *        (e.g. DobFiles' Monta view passing its own dobui_port)
     *
     * Backward-compatible: older senders that don't initialise this
     * field will have it at 0 from the zero-init of the struct,
     * which is the safe default. */
    uint32_t hijack_target_port;
} icon_activated_t;

/* Payload of MENU_ACTIVATED. The user picked menu entry `menu_idx`
 * (zero-based, into the menu_items array shipped in the original
 * gui_device_attach_t for device_id). Sent from dobinterface to
 * hotplug, which runs the corresponding primitive sequence from the
 * DAS file's menu {} section. */
typedef struct
{
    uint32_t device_id;
    uint32_t menu_idx;
} menu_activated_t;

/* One row of a GUI_LIST_DEVICES reply. Mirrors the visible fields of
 * a desktop icon: enough to render it inside a file explorer and to
 * fire an ICON_ACTIVATED at hotplug if the user clicks it. The
 * bitmap is included whole so callers draw the icon at natural size
 * (48x48 1bpp mask is typical). */
typedef struct
{
    uint32_t      device_id;
    uint32_t      kind;
    char          label[32];
    char          service_name[32];
    icon_bitmap_t bitmap;
} gui_device_list_entry_t;

/* Header preceding `count` packed gui_device_list_entry_t records
 * in the GUI_LIST_DEVICES reply payload. */
typedef struct
{
    uint32_t count;
} gui_device_list_t;

#endif /* MAINDOB_DOB_DEVICE_ICON_H */
