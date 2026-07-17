/* MainDOB USB-DAS matcher — shared by the USB host-controller drivers
 * (usb_uhci, usb_ehci, usb_xhci).
 *
 * A USB-DAS is a *device-level* automation script living in
 * /SYSTEM/CONFIG/DAS/USB/. It is the USB analogue of the PCI matching
 * hotplug does for controllers: when a host-controller driver enumerates a
 * freshly-inserted device and reads its descriptors, it matches the
 * device's USB class triple against every file in that directory and picks
 * the most specific one. On a match the driver emits
 * HOTPLUG_SUBDEVICE_APPEARED carrying the PCI-style subdev_class/subclass
 * the DAS specifies; hotplug then DAS-matches *that* to draw the desktop
 * icon and define the double-click action — the same SUBDEVICE pipeline
 * already used by AHCI optical media and FAT32 partitions. No new icon or
 * action machinery lives here.
 *
 * This library does no hardware access and no IPC. It only reads files
 * (via DobFileSystem) and compares bytes; the caller does the announce.
 */
#ifndef MAINDOB_USB_DAS_H
#define MAINDOB_USB_DAS_H

#include <dob/types.h>

/* Where USB-DAS files live. */
#define USB_DAS_DIR "/SYSTEM/CONFIG/DAS/USB"

/* "Any" wildcard for a class/subclass/protocol field in a DAS. A device
 * field never legitimately equals 0xFF for class (0xFF is the USB
 * vendor-specific class, which a DAS matches explicitly), so we use a
 * separate sentinel internally; see usb_das.c. On the wire 0xFF in a DAS
 * field means "match anything". */
#define USB_DAS_ANY 0xFF

/* match_on selector: which descriptor the DAS triple is compared against. */
typedef enum
{
    USB_DAS_MATCH_INTERFACE = 0,   /* default: bInterfaceClass/SubClass/Protocol */
    USB_DAS_MATCH_DEVICE           /* bDeviceClass/SubClass/Protocol */
} usb_das_match_on_t;

/* The facts the matcher needs from an enumerated device. The caller fills
 * this from the device descriptor and the (first) interface descriptor it
 * already parsed during enumeration. */
typedef struct
{
    uint8_t dev_class;        /* bDeviceClass */
    uint8_t dev_subclass;     /* bDeviceSubClass */
    uint8_t dev_protocol;     /* bDeviceProtocol */
    uint8_t if_class;         /* bInterfaceClass */
    uint8_t if_subclass;      /* bInterfaceSubClass */
    uint8_t if_protocol;      /* bInterfaceProtocol */
    uint16_t vid;             /* idVendor  (0 = unknown; VID 0 is invalid
                               * on the wire, so 0 safely means "unset") */
    uint16_t pid;             /* idProduct (meaningful only with vid!=0) */
} usb_das_device_t;

/* The result of a successful match: what to announce to hotplug. */
typedef struct
{
    uint8_t subdev_class;     /* PCI-style class for SUBDEVICE_APPEARED */
    uint8_t subdev_subclass;  /* PCI-style subclass */
    char    label[32];        /* human label (diagnostics) */
    bool    matched;          /* false => no DAS matched this device */
} usb_das_result_t;

/* Match an enumerated device against the USB DAS directory.
 *
 * Reads every *.das in USB_DAS_DIR, evaluates each against `dev`, and keeps
 * the MOST SPECIFIC match (a non-wildcard field is more specific than a
 * wildcard; ties keep the first seen). On success fills `out` with the
 * subdev class/subclass to announce and returns true; if no DAS matches,
 * sets out->matched=false and returns false.
 *
 * Self-contained: opens/reads/closes files via DobFileSystem. Safe to call
 * from a single-threaded driver after enumeration completes.
 */
bool usb_das_match(const usb_das_device_t *dev, usb_das_result_t *out);

/* Number of .das files the LAST usb_das_match call parsed. 0 means the
 * USB DAS directory was missing or empty (image-staging failure class). */
uint32_t usb_das_last_file_count(void);
int      usb_das_last_list_rc(void);
int      usb_das_last_open_fd(void);
int      usb_das_last_read_len(void);
uint32_t usb_das_last_dir_entries(void);

#endif /* MAINDOB_USB_DAS_H */
