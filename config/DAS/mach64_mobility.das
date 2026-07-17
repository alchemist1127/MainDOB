# ATI 3D Rage Mobility-P (PCI 1002:4C4D) — Mach64-family video driver.
#
# Primary onboard video of the Compaq Armada E500 (subsys 0E11:B160).
# 8 MB SGRAM, AGP 2x, integrated LVDS for 1024x768 TFT internal panel
# and DAC RGB for external CRT.  PCI class 03:00 (VGA controller).
#
# This DAS matches by exact vendor:device (score ~1000), beating any
# generic class 03:00 fallback that may be added later (score ~20).
# It points at the shared mach64.mdl binary, which detects the variant
# at attach time via PCI device_id and branches PLL / LCD code
# accordingly — same binary serves the LT Pro path via mach64_ltpro.das.
#
# kind = system: spawned at match, no desktop icon.  The driver
# registers under "DobVideoControl" (control plane) and "video" (data
# plane) — same process, two service names, identical pattern to the
# BGA driver it replaces on real hardware.

bus_type  = pci
vendor_id = 0x1002
device_id = 0x4C4D
kind      = system

driver  = /SYSTEM/DRIVERS/mach64/mach64.mdl
service = DobVideoControl
label   = ATI Rage Mobility-P

category = video
