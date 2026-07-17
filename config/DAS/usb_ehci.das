# USB EHCI controller — PCI class 0C:03 with prog_if 0x20.
#
# EHCI is the USB 2.0 high-speed host controller spec. QEMU's
# usb-ehci device reports prog_if 0x20.
#
# NOTE: the old /SYSTEM/CONFIG/Drivers manifest also routed prog_if
# 0x10 to this driver, but per the PCI class-code spec 0x10 is OHCI
# (a different USB 1.1 spec), not EHCI. The 0x10 line was almost
# certainly a historical mistake and is not reproduced here. If a
# real OHCI controller ever needs handling, write a separate
# usb_ohci.das (and a driver for it) — pointing OHCI hardware at
# the EHCI driver would crash on first MMIO access.

bus_type = pci
class    = 0x0C
subclass = 0x03
prog_if  = 0x20
kind     = system

driver  = /SYSTEM/DRIVERS/usb_ehci/usb_ehci.mdl
service = usb_ehci
label   = USB 2.0 (EHCI)

category = usb
