/* MainDOB Hotplug Events Protocol
 *
 * Shared between hotplug (publisher) and any subscriber module
 * (dobinterface being the primary one — it drives desktop icons).
 *
 * Model:
 *   - Hotplug owns the hardware table and bubble lifecycle.
 *   - Subscribers register via HOTPLUG_SUBSCRIBE; they then receive
 *     GUI_DEVICE_ATTACH / GUI_DEVICE_DETACH for every bubble whose
 *     DAS yields a desktop icon. On subscribe, hotplug replays the
 *     ATTACH events for everything currently known so a late
 *     subscriber catches up.
 *   - PCI devices are truly hotpluggable and are surfaced event-driven.
 *   - The floppy controller is static: hotplug creates one legacy
 *     bubble at boot and never removes it. Media presence is the job
 *     of whatever mounts the drive on double-click.
 */

#ifndef MAINDOB_DOB_HOTPLUG_EVENTS_H
#define MAINDOB_DOB_HOTPLUG_EVENTS_H

#include <dob/types.h>

/* IPC codes — picked outside the ranges already used by GUI_* (100..151,
 * 200..222 for GUI events) and the existing HOTPLUG_* (these live in
 * hotplug_driver.h for the driver-side handshake and are below 100). */
#define HOTPLUG_SUBSCRIBE         240

/* Create a static legacy bubble from an external probe helper.
 * Lets boot-time floppy detection live in its own autoextinguishing
 * process (boot/floppyprobe) instead of inside hotplug's init path.
 * The helper sends one message per drive present, then exits.
 * Payload is a hotplug_legacy_create_t. */
#define HOTPLUG_CREATE_LEGACY_BUBBLE 243

/* Sub-device lifecycle. A "sub-device" is something a bus driver has
 * discovered inside itself and wants the desktop to surface as an
 * independent icon. The AHCI driver uses this for optical drives:
 * when a disc is inserted (IRQ + GESN async notification) ahci.mdl
 * sends SUBDEVICE_APPEARED; on eject it sends SUBDEVICE_GONE. Hotplug
 * turns each one into a child bubble shown on the desktop in real
 * time.
 *
 * The IDE bus uses CREATE_LEGACY_BUBBLE instead — IDE has no IRQ for
 * media-change, so IDE CDROMs show up as a static drive icon at boot
 * (same model as the floppy).
 *
 * APPEARED payload: subdev_appeared_t. GONE: hotplug_subdev_gone_t
 * (the token is what the provider assigned in APPEARED).
 *
 * These are sync request messages (dob_ipc_call), so the provider
 * learns immediately whether hotplug accepted the subdevice and a
 * quick APPEARED/GONE pair cannot be reordered. */
#define HOTPLUG_SUBDEVICE_APPEARED  244
#define HOTPLUG_SUBDEVICE_GONE      245

typedef struct
{
    uint8_t  bus_type;    /* BUS_TYPE_LEGACY_FDC, BUS_TYPE_LEGACY_IDE_ATAPI */
    uint8_t  unit;        /* sub-index (drive 0 or 1 for floppies, slot 0..3 for IDE) */
    uint16_t io_base;     /* primary I/O base (0x3F0 for FDC, channel base for IDE) */
} hotplug_legacy_create_t;

/* Sub-device descriptor — the facts the provider knows about the
 * inner device. Hotplug uses class_code/subclass to DAS-match it,
 * provider_service + provider_token to route menu/action IPC back
 * to the provider driver at activation time.
 *
 * ABI extension v1.0.0.420.60 (disk-utility step 6): volume_fs added
 * at the end. Filesystem-volume subdevices (FAT32 partitions emitted
 * by libdob/dob/partition) set this to VOLUME_FS_FAT32 and leave
 * class_code/subclass at 0; the DAS matcher treats volume_fs as an
 * orthogonal filter that's checked alongside class/subclass for any
 * BUS_TYPE_SUBDEVICE entry. Pre-step-6 callers (AHCI optical) leave
 * the field at zero (VOLUME_FS_NONE) and continue to match purely on
 * class/subclass — perfectly backward compatible. */
typedef struct
{
    uint32_t provider_token;       /* Opaque handle: provider's own device index */
    uint8_t  class_code;           /* PCI-style class (0x01 = storage, ...) */
    uint8_t  subclass;             /* 0x05 = optical (CD/DVD/BD) */
    uint8_t  pad[2];
    char     provider_service[32]; /* IPC service name: "ahci", future others */
    uint8_t  volume_fs;            /* VOLUME_FS_*, 0 if not a filesystem volume */
    uint8_t  _pad2[3];             /* Keep struct alignment tidy */

    /* ABI extension v1.0.0.420.60 (disk-utility step 9): for filesystem-
     * volume subdevices (volume_fs != 0), the partition's start LBA on
     * its disk. Lets hotplug compare against the root DobFileSystem's
     * partition_lba to identify and optionally hide the root-partition
     * desktop icon. Non-FS subdevices leave it at 0. */
    uint32_t partition_start_lba;
} sub_device_t;

/* volume_fs tag values. Zero = "this subdevice is not a filesystem
 * volume" (e.g. AHCI optical drives). Non-zero values identify a
 * filesystem type we know how to surface as a mountable icon.
 * Append-only, never reorder — these go on the wire. */
#define VOLUME_FS_NONE   0
#define VOLUME_FS_FAT32  1
#define VOLUME_FS_EXFAT  2
/* Future: VOLUME_FS_DOBFS = 3, VOLUME_FS_EXT4 = 4, ... */

typedef struct
{
    sub_device_t sub;
} hotplug_subdev_appeared_t;

typedef struct
{
    char     provider_service[32]; /* Same value passed in APPEARED */
    uint32_t provider_token;       /* Same value passed in APPEARED */
} hotplug_subdev_gone_t;

/* Bus type tag for hw_device_t. Future bus types append here. */
#define BUS_TYPE_PCI              0
#define BUS_TYPE_LEGACY_FDC       1
/* BUS_TYPE_LEGACY_IDE_ATAPI: an ATAPI (CDROM) drive found at boot on
 * the legacy IDE bus. Static bubble — no runtime appearance/removal,
 * because IDE has no hardware hotplug support. io_base carries the
 * channel base (0x1F0 primary, 0x170 secondary), unit carries the
 * full slot index 0..3 (channel*2 + master/slave). The drive icon
 * is permanent; media presence is discovered lazily on activation. */
#define BUS_TYPE_LEGACY_IDE_ATAPI 2
/* BUS_TYPE_SUBDEVICE: a dynamic sub-device discovered by a bus driver
 * (currently only ahci.mdl). DAS-matched by class_code/subclass.
 * Runtime lifecycle: appear on insertion, disappear on eject. */
#define BUS_TYPE_SUBDEVICE        3

/* Shared hardware descriptor. Previously local to boot/hotplug/main.c;
 * promoted here so subscribers can deserialize broadcasts without
 * duplicating the struct. The layout is identical to what hotplug
 * already uses, plus a bus_type tag at the end for legacy devices.
 *
 * For BUS_TYPE_PCI: vendor_id, device_id, class_code, subclass,
 *                   prog_if, revision, subsystem_vendor_id,
 *                   subsystem_device_id, bus, slot, func, bar[],
 *                   irq_line apply.
 * For BUS_TYPE_LEGACY_FDC: io_base carries the FDC primary I/O base
 *                         (typically 0x3F0). PCI fields are zeroed.
 *
 * IMPORTANT — ABI stability: when extending this struct, only ever
 * append new fields at the end. Inserting in the middle changes the
 * offsets of every subsequent field, which silently corrupts every
 * consumer that wasn't recompiled with the new header (the Makefile
 * does not track header dependencies). The original field block up
 * to and including io_base is the v59 ABI; revision and subsystem
 * IDs are the Phase-3 extensions, appended below. */
typedef struct
{
    /* === Original v59 ABI — DO NOT REORDER === */
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint32_t bar[6];
    uint8_t  irq_line;
    bool     present;
    uint8_t  bus_type;    /* BUS_TYPE_* */
    uint16_t io_base;     /* Legacy bus I/O base (e.g. 0x3F0 for FDC) */

    /* === Phase 3 extensions — appended === */
    uint8_t  revision;             /* PCI config offset 0x08, low byte */
    uint16_t subsystem_vendor_id;  /* PCI config offset 0x2C */
    uint16_t subsystem_device_id;  /* PCI config offset 0x2E */

    /* === Step-6 extension — appended ===
     * Mirror of sub_device_t.volume_fs. Populated when hotplug copies
     * a subdevice descriptor into its bubble's hw_device_t; zero on
     * every PCI / legacy hardware (they aren't filesystem volumes). */
    uint8_t  volume_fs;            /* VOLUME_FS_* */

    /* === Step-9 extension — appended ===
     * Mirror of sub_device_t.partition_start_lba. Lets hotplug
     * answer "is this bubble the root partition?" without re-asking
     * the driver. Zero on non-FS subdevices and non-subdevice hw. */
    uint32_t partition_start_lba;
} hw_device_t;

#endif /* MAINDOB_DOB_HOTPLUG_EVENTS_H */
