# Intel GMA X3100 (Mobile Intel GM965 "Crestline") — PCI 8086:2A02.
#
# Primary onboard video of the Acer Extensa 5220. Integrated LVDS panel.
# PCI class 03:00 (VGA controller).
#
# REGISTER-PROBE build: this driver currently performs a read-only dump
# of the display-engine registers (as the VBIOS left them) over the
# serial console, then idles. It does NOT yet drive the display. See
# drivers/x3100/main.c.
#
# kind = system: spawned at match by hotplug, no desktop icon. Points at
# the shared x3100.mdl; the LT/variant selection (2A02 vs 2A03) is by
# device_id at attach time — same pattern as mach64_mobility/ltpro.das.

bus_type  = pci
vendor_id = 0x8086
device_id = 0x2A02
kind      = system

driver  = /SYSTEM/DRIVERS/x3100/x3100.mdl
service = x3100_probe
label   = Intel GMA X3100 (GM965) register probe

category = video
