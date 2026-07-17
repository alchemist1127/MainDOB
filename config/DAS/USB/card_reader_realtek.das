# MainDOB USB Device Automation Script — internal SD card reader (Realtek).
#
# USB-level DAS (read by the host-controller driver, like
# mass_storage.das). Matches the SAME class triple as a pendrive —
# 08:06:50, Mass Storage / SCSI / Bulk-Only — but REFINED by USB
# vendor id 0x0BDA (Integrated Circuit Solution Inc.), the controller
# family of the SD reader soldered inside the Compaq CQ62 (and many
# laptops of that era). The VID match weighs more than the whole class
# triple in the matcher, so for THIS product it always wins over the
# generic mass_storage.das; every other 08:06:50 device is untouched.
#
# The announced subdevice category is 0x01:0x07 (storage / card
# reader) — distinct from 0x01:0x06 (pendrive) — so hotplug draws the
# dedicated SD-card icon and uses reader-appropriate wording (an empty
# slot is the reader's NORMAL state, not an error).

usb_class    = 0x08
usb_subclass = 0x06
usb_protocol = 0x50
vendor_id    = 0x0BDA

subdev_class    = 0x01
subdev_subclass = 0x07

label = Lettore schede (Realtek)
