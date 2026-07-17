# AC97 audio controller — class 04:01 on the PCI bus.
#
# kind = system: spawned at match, no icon. The audio service is
# consumed by audioplayer (system tray volume) which discovers it via
# the registry — see boot/audioplayer.

bus_type = pci
class    = 0x04
subclass = 0x01
kind     = system

driver  = /SYSTEM/DRIVERS/ac97/ac97.mdl
service = audio
label   = AC97 audio

category = audio
