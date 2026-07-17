# USB UHCI controller — PCI class 0C:03 with prog_if 0x00.
#
# UHCI is Intel's pre-EHCI USB 1.1 host controller spec, found on
# 440BX-era and later chipsets up to ICH6. QEMU's piix3-usb is UHCI.

bus_type = pci
class    = 0x0C
subclass = 0x03
prog_if  = 0x00
kind     = system

driver  = /SYSTEM/DRIVERS/usb_uhci/usb_uhci.mdl
service = usb_uhci
label   = USB 1.1 (UHCI)

category = usb
