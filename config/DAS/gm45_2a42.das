# Intel GMA 4500MHD (Mobile Intel GM45 "Cantiga") — PCI 8086:2A42.
#
# Primary onboard video of the Compaq Presario CQ62 (Pentium/Montevina
# variants; verified via lspci under Linux: "Mobile 4 Series Chipset
# Integrated Graphics Controller", driver i915). Integrated LVDS panel,
# native 1366x768. PCI class 03:00 (VGA controller).
#
# Served by the SAME x3100.mdl as the GM965 (Extensa 5220): Cantiga is
# the direct g4x-family successor of Crestline — identical BLT command
# set, same display-register offsets. The driver detects 2A42/2A43 at
# attach and switches the two hard deltas at runtime (GTT at BAR0+2MB
# instead of +512KB; LVDS pipe and native mode READ from the hardware
# instead of assumed). See x3100_hw_init() in x3100_modeset.c.
#
# The second display function (8086:2A43, class 03:80) has no DAS on
# purpose: one driver instance per GPU, attached to function 0.
#
# kind = system: spawned at match by hotplug, no desktop icon.

bus_type  = pci
vendor_id = 0x8086
device_id = 0x2A42
kind      = system

driver  = /SYSTEM/DRIVERS/x3100/x3100.mdl
service = x3100_probe
label   = Intel GMA 4500MHD (GM45)

category = video
