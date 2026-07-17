# ESS Maestro-2E (ES1978) audio controller — PCI 125D:1978.
#
# Onboard audio of the Compaq Armada E500. PCI class 04:01 (multimedia
# audio), same as a generic AC97 controller — so this file matches by
# exact vendor:device instead of class. The matcher scores a V:D match
# at ~1000 against ~20 for ac97.das's class match, so when a real
# Maestro is present this DAS wins and ac97.mdl is never spawned for it.
#
# kind = system: spawned at match time, no desktop icon. The driver
# registers the "audio" service, consumed by audioplayer via the
# registry — exactly like the AC97 driver, so nothing else changes.

bus_type  = pci
vendor_id = 0x125D
device_id = 0x1978
kind      = system

driver  = /SYSTEM/DRIVERS/maestro2e/maestro2e.mdl
service = audio
label   = ESS Maestro-2E audio

category = audio
