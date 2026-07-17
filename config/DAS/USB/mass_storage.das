# MainDOB USB Device Automation Script — USB Mass Storage class.
#
# This is a *USB-level* DAS, read by the USB host-controller driver
# (usb_uhci / usb_ehci / usb_xhci) — NOT by hotplug. It is the USB
# analogue of the PCI matching that hotplug does: when the controller
# enumerates a freshly-inserted device and reads its descriptors, it
# matches the device's USB class/subclass/protocol against every file
# in /SYSTEM/CONFIG/DAS/USB/ and picks the most specific one.
#
# A USB-DAS is deliberately thinner than a hotplug DAS. It carries no
# icon bitmap and no action/menu blocks: those live on the hotplug
# side. The controller's only job after a match is to tell hotplug
# "a sub-device of THIS category appeared", by emitting
# HOTPLUG_SUBDEVICE_APPEARED with the class_code / subclass below.
# Hotplug then DAS-matches *that* (against a bus_type=subdevice file)
# to draw the desktop icon and define the double-click action — the
# exact same SUBDEVICE pipeline already used by AHCI optical media
# and FAT32 disk partitions. No new icon/action machinery.
#
# === USB matching signature ===
#
# usb_class / usb_subclass / usb_protocol match the device's reported
# USB class triple. They follow the USB-IF Mass Storage class spec:
#   usb_class    = 0x08   Mass Storage
#   usb_subclass = 0x06   SCSI transparent command set
#   usb_protocol = 0x50   Bulk-Only Transport (BBB)
#
# A device whose interface advertises this triple is a standard USB
# flash drive / external disk that speaks SCSI over Bulk-Only — which
# is what the mass-storage sub-driver (spawned later, on icon click)
# knows how to talk to. 0xFF in any field means "any" (wildcard).
#
# match_on selects WHICH descriptor the usb_class/subclass/protocol
# triple is compared against. It is a per-DAS choice, so each device
# category picks the style that fits it:
#
#   match_on = interface  -> compare against the Interface descriptor
#                            (bInterfaceClass/SubClass/Protocol).
#                            Correct for mass-storage: a flash drive
#                            reports class 0x00 at the Device level and
#                            declares 0x08 only per-interface.
#   match_on = device     -> compare against the Device descriptor
#                            (bDeviceClass/SubClass/Protocol). Correct
#                            for devices that declare their class at the
#                            device level (e.g. hubs, some HID combos).
#
# The controller reads the Device descriptor always, and the
# Configuration+Interface descriptors as well, so either style is
# available; the DAS decides which one applies. Default when the line
# is absent: interface.

usb_class    = 0x08
usb_subclass = 0x06
usb_protocol = 0x50
match_on     = interface

# === What to announce to hotplug on match ===
#
# When a device matches, the controller emits a SUBDEVICE_APPEARED
# carrying these PCI-style class/subclass values. They are what
# hotplug feeds to its own subdevice DAS matcher. 0x01:0x06 = mass
# storage device (storage class, SCSI-ish subclass) — distinct from
# 0x01:0x05 (optical) so a USB stick and a CD get different icons.
#
# volume_fs is left unset here: at announce time the controller has
# NOT yet read the partition table (that needs the mass-storage
# sub-driver and SCSI reads, which only happen on click). The FAT32
# tagging that drives partition_fat32.das happens later, emitted by
# the sub-driver after it can actually read sector 0.

subdev_class    = 0x01
subdev_subclass = 0x06

# Human label, shown in logs / diagnostics. The desktop label comes
# from the hotplug-side subdevice DAS, not from here.
label = USB Mass Storage

category = usb
