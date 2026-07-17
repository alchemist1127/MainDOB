# AHCI / SATA controller — class 01:06 on the PCI bus.
#
# kind = system: the driver is spawned automatically the moment hotplug
# matches an AHCI controller. No desktop icon. The driver in turn
# announces individual SATA devices (disks, optical drives) via the
# subdevice mechanism, and those produce desktop icons through their
# own DAS files (cdrom_ahci.das, etc.).

bus_type = pci
class    = 0x01
subclass = 0x06
kind     = system

driver  = /SYSTEM/DRIVERS/ahci/ahci.mdl
service = ahci
label   = AHCI / SATA controller

category = storage
