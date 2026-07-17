# MainDOB Device Automation Script — USB mass-storage volume.
#
# Matches the subdevice a USB host-controller driver (usb_uhci, and later
# usb_ehci/usb_xhci) emits after enumerating a freshly-inserted device that
# matched config/DAS/USB/mass_storage.das. That USB-level match announces a
# HOTPLUG_SUBDEVICE_APPEARED carrying PCI-style class 0x01 / subclass 0x06
# (storage / SCSI-ish), distinct from 0x01:0x05 (optical) so a USB stick and
# a CD get different icons. Hotplug turns it into a child bubble; THIS file
# (a bus_type=subdevice DAS) decides how it looks on the desktop and what
# happens on double-click — the same pipeline AHCI optical media and FAT32
# partitions already use.
#
# The device identity is carried in the subdevice payload:
#   provider_service — "usb_uhci" (where to send mass-storage IPC)
#   provider_token   — the controller's port number for this device
# Both are exposed as $provider and $token in the action / label.
#
# NOTE ON THE ACTION (current phase):
# The block-level mass-storage sub-driver (USB Bulk-Only Transport + SCSI,
# the analogue of cdrom.mdl for optical) is not built yet — that is the next
# step. Until it exists, double-click shows the device's identity in a popup
# so the icon -> click -> action pipeline is verifiable on its own. When the
# sub-driver lands, this action becomes the familiar spawn/wait/mount trio:
#   spawn        .../usbstorage.mdl provider=$provider,token=$token  on_fail mount_failed
#   wait_service usbms_$token  timeout=3000                          on_fail mount_failed
#   ipc_call     usbms_$token  21                                    on_fail mount_failed
# (exactly mirroring partition_fat32.das / cdrom_ahci.das).

bus_type = subdevice
class    = 0x01
subclass = 0x06

label = Pendrive USB
kind  = storage

bitmap_w    = 48
bitmap_h    = 48
bitmap_fg_r = 120
bitmap_fg_g = 200
bitmap_fg_b = 150

# Stylised USB flash-drive icon: a connector tab at the left, the body to
# the right, and a small activity notch.
bitmap {
................................................
................................................
................................................
................................................
................................................
................................................
................................................
................................................
................................................
..........................................##....
.......................................##..##...
....................................##.....##...
.................................##........##...
..............................##...........##...
......######################................##..
....##......................##...............#..
....#........................#...............#..
....#..####################..#...............#..
....#..#..................#..#...............#..
....#..#..##..........##..#..#...............#..
....#..#..##..........##..#..################..
....#..#..................#..................#..
....#..#..##..........##..#..................#..
....#..#..##..........##..#..################..
....#..#..................#..#...............#..
....#..####################..#...............#..
....#........................#...............#..
....##......................##...............#..
......######################................##..
..............................##...........##...
.................................##........##...
....................................##.....##...
.......................................##..##...
..........................................##....
................................................
................................................
................................................
................................................
................................................
................................................
................................................
................................................
................................................
................................................
................................................
................................................
................................................
................................................
}

# Double-click action — the cdrom pattern: ONE icon, ONE click, ONE
# window. Every click runs the spawn (duplicate instances exit quietly,
# guard in the sub-driver), then:
#   - usbms_$token comes up: SET_CONFIGURATION, BOT INQUIRY/TUR/READ
#     CAPACITY, block service registered (512-byte virtual sectors over
#     ANY native block size: 512/1024/2048/4096);
#   - opcode 66 (PREPARE_VOLUME): the sub-driver locates the FAT32
#     volume (first MBR partition, or superfloppy at LBA 0) and spawns
#     the secondary DobFileSystem bound to itself, FIRE-AND-FORGET;
#   - hotplug (here) does ALL the waiting: the mount reads its sectors
#     FROM usbms, so usbms must be free to serve while we wait —
#     waiting inside the sub-driver would deadlock the chain;
#   - opcode 21 (OPEN_VIEW) on the mounted dobfs opens the DobFiles
#     window, exactly like partition_fat32.das does for AHCI.
action {
    spawn         /SYSTEM/DRIVERS/usb_mass_storage/usb_mass_storage.mdl $provider:$token   on_fail usbms_failed
    wait_service  usbms_$token  timeout=8000                                               on_fail usbms_failed
    ipc_call      usbms_$token  66                                                         on_fail novolume
    wait_service  dobfs_$token  timeout=8000                                               on_fail mount_failed
    ipc_call      dobfs_$token  21                                                         on_fail mount_failed
}

# Context-panel menu — shown when the icon is highlighted.
menu {
    item "Formatta..."
    {
        # Guarded spawn, then opcode 67 (WAIT_READY): a pure IPC
        # rendezvous — usbms's main is sequential, so the call is
        # served exactly when bring-up completes (or fails). THIS step
        # is what guarantees DobDisk's enumeration will see the stick:
        # ordering by design, not by retrying. All waiting happens
        # here in hotplug's action engine, as always.
        spawn         /SYSTEM/DRIVERS/usb_mass_storage/usb_mass_storage.mdl $provider:$token   on_fail usbms_failed
        wait_service  usbms_$token  timeout=8000                                               on_fail usbms_failed
        ipc_call      usbms_$token  67                                                         on_fail bringup_failed
        # open_program (NON run): lo spawn del programma GUI passa da
        # dobinterface (GUI_LAUNCH_PROGRAM), lo stesso identico percorso
        # del launcher "Tutti i programmi" — provato ogni giorno. Il
        # `run` diretto dal runner di hotplug in campo non apriva la
        # finestra (unità registrata, nessuna finestra, nessun popup).
        open_program  /SYSTEM/PROGRAMS/DobDisk/DobDisk.mdl --select usbms_$token               on_fail fmt_failed
    }
    item "Informazioni"
    {
        popup_info  "Pendrive USB"  "Provider: $provider, porta $token"
    }
}

errors {
    usbms_failed = popup_error "Pendrive USB" "Impossibile avviare il driver mass-storage. Apri usbdiag per la diagnostica."
    novolume     = popup_error "Pendrive USB" "Impossibile preparare il volume: nessun FAT32 (DobDisk per formattare) o bring-up fallito (usbdiag fase 4)."
    # Refined label (engine tries "<on_fail>_<N>" with N = -reply code
    # first): 9 = DOB_ERR_NO_MEDIA. An EMPTY internal card reader (the
    # CQ62's soldered SD slot is a permanent USB mass-storage device,
    # icon always on the desktop) lands here — informational popup, no
    # scary red "format it!" for a slot that simply has no card in it.
    novolume_9   = popup_info  "Lettore schede USB" "Nessun supporto inserito nel lettore. Inserisci una scheda e riprova."
    mount_failed = popup_error "Pendrive USB" "Volume trovato ma montaggio fallito. Apri usbdiag (fase 4)."
    fmt_failed     = popup_error "Pendrive USB" "Impossibile avviare DobDisk."
    bringup_failed = popup_error "Pendrive USB" "Inizializzazione del dispositivo fallita. Apri usbdiag (fase 4) per lo stadio."
    bringup_failed_9 = popup_info "Pendrive USB" "Nessun supporto presente nel dispositivo. Inserisci una scheda/supporto e riprova."
}

category = storage
