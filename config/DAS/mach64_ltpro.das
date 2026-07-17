# ATI 3D Rage LT Pro AGP 2x (PCI 1002:4C42) — Mach64-family video driver.
#
# Onboard video of the EARLY Compaq Armada E500 PCBs (subsys 0E11:B10E),
# replaced by the Rage Mobility-P on later revisions.  Same Mach64
# 2D engine, same register layout for CRTC / DAC / cursor / draw, but
# the PLL reference frequency differs (29.498 MHz vs 14.318 MHz on
# the Mobility-P) and the LCD_LT_GIO register exists only here.
#
# This DAS coexists with mach64_mobility.das — both point at the same
# mach64.mdl binary, which selects the variant at attach time by
# reading PCI device_id.  A given E500 PCB has only one of the two
# chips, so exactly one DAS matches per machine.

bus_type  = pci
vendor_id = 0x1002
device_id = 0x4C42
kind      = system

driver  = /SYSTEM/DRIVERS/mach64/mach64.mdl
service = DobVideoControl
label   = ATI Rage LT Pro

category = video
