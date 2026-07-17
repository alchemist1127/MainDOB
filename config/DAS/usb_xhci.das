# USB xHCI controller — PCI class 0C:03 with prog_if 0x30.
#
# xHCI is the USB 3.x extensible host controller spec; also
# back-compatible with USB 2 / 1.1 devices on the same root hub.
# QEMU's nec-usb-xhci and qemu-xhci both report prog_if 0x30.

bus_type = pci
class    = 0x0C
subclass = 0x03
prog_if  = 0x30
kind     = system

driver  = /SYSTEM/DRIVERS/usb_xhci/usb_xhci.mdl
service = usb_xhci
label   = USB 3.x (xHCI)

category = usb
