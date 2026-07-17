# Bochs Graphics Adapter (BGA) — QEMU's `-vga std` device.
#
# Identifies as PCI vendor 0x1234 ("QEMU") device 0x1111 ("VGA Adapter").
# Linear framebuffer at BAR0; legacy VBE register window at I/O ports
# 0x01CE / 0x01CF.  No IRQ.
#
# kind = system: spawned at match time, no desktop icon.  The driver
# registers under the name "DobVideoControl" for the traditional
# control-plane API and under "video" for the dobVideo data-plane
# protocol.  Both names point to the same driver process.
#
# Specificity score: vendor:device match wins over any class:subclass
# generic VGA fallback that may be added later (rank=65000).

bus_type  = pci
vendor_id = 0x1234
device_id = 0x1111
kind      = system

driver  = /SYSTEM/DRIVERS/bga/bga.mdl
service = DobVideoControl
label   = QEMU Bochs Graphics Adapter

category = video
