#!/bin/bash
# mkbootdisk.sh — Create a bootable MainDOB disk image
#
# MBR partitioned disk, FAT32 in partition 1 starting at sector 2048.
# GRUB goes in the MBR gap (sectors 0-2047), FAT32 is untouched.
# mtools with partition=1 for file copy -- no mount, no loop, no root.
#
# Usage: mkbootdisk.sh <disk.img> <size_mb> <build_dir> <root_dir>

set -e

DISK="$1"
SIZE_MB="${2:-32}"
BUILD_DIR="$3"
ROOT_DIR="$4"

if [ -z "$DISK" ] || [ -z "$BUILD_DIR" ] || [ -z "$ROOT_DIR" ]; then
    echo "Usage: $0 <disk.img> <size_mb> <build_dir> <root_dir>"
    exit 1
fi

cleanup() { rm -f /tmp/mdob_*_$$ 2>/dev/null; true; }
trap cleanup EXIT

GRUB_DIR=""
for dir in /usr/lib/grub/i386-pc /usr/share/grub/i386-pc; do
    [ -d "$dir" ] && GRUB_DIR="$dir" && break
done
MKIMAGE=""
for cmd in grub-mkimage grub2-mkimage; do
    command -v "$cmd" >/dev/null 2>&1 && MKIMAGE="$cmd" && break
done
if [ -z "$GRUB_DIR" ] || [ -z "$MKIMAGE" ]; then
    echo "[DISK] ERROR: GRUB i386-pc not found"; exit 1
fi

echo "[DISK] Creating ${SIZE_MB}MB bootable disk..."

# === 1. Create disk image with MBR + FAT32 partition at sector 2048 ===
TOTAL_SECTORS=$(( $SIZE_MB * 1024 * 1024 / 512 ))
PART_START=2048
PART_SECTORS=$(( $TOTAL_SECTORS - $PART_START ))

dd if=/dev/zero of="$DISK" bs=1M count="$SIZE_MB" 2>/dev/null

# Write MBR partition table with python
python3 -c "
import struct
mbr = bytearray(512)
mbr[510] = 0x55; mbr[511] = 0xAA
entry = struct.pack('<BBBBBBBBII', 0x80, 0xFE, 0xFF, 0xFF, 0x0C, 0xFE, 0xFF, 0xFF, $PART_START, $PART_SECTORS)
mbr[446:446+16] = entry
with open('$DISK', 'r+b') as f:
    f.write(bytes(mbr))
"

# Create FAT32 in a temp file, then dd into the disk at partition offset
dd if=/dev/zero of=/tmp/mdob_part_$$ bs=512 count=$PART_SECTORS 2>/dev/null
mkfs.fat -F32 /tmp/mdob_part_$$ >/dev/null 2>&1
dd if=/tmp/mdob_part_$$ of="$DISK" bs=512 seek=$PART_START conv=notrunc 2>/dev/null
rm -f /tmp/mdob_part_$$
echo "[DISK] MBR + FAT32 partition created."

# === 2. Configure mtools to access partition 1 ===
ABSDISK="$(cd "$(dirname "$DISK")" && pwd)/$(basename "$DISK")"
export MTOOLSRC="/tmp/mdob_rc_$$"
cat > "$MTOOLSRC" << MEOF
mtools_skip_check=1
drive c:
    file="$ABSDISK"
    partition=1
MEOF

# === 3. Create directories ===
echo "[DISK] Creating directories..."
mmd c:/boot c:/boot/grub 2>/dev/null || true
mmd c:/SYSTEM 2>/dev/null || true
mmd "c:/SYSTEM/OperatingSystem" 2>/dev/null || true
mmd c:/SYSTEM/PROGRAMS c:/SYSTEM/DRIVERS c:/SYSTEM/GAMES 2>/dev/null || true
mmd c:/SYSTEM/CONFIG 2>/dev/null || true
# Common files: shared, sandbox-free area. Ship any fonts dropped in the repo's
# common_files/fonts so DobWrite's live scanner finds them (kept in sync with
# mklive.sh).
mmd "c:/SYSTEM/CONFIG/common_files" 2>/dev/null || true
mmd "c:/SYSTEM/CONFIG/common_files/fonts" 2>/dev/null || true
if [ -d "$ROOT_DIR/common_files/fonts" ]; then
    for ff in "$ROOT_DIR/common_files/fonts"/*; do
        [ -f "$ff" ] || continue
        mcopy "$ff" "c:/SYSTEM/CONFIG/common_files/fonts/$(basename "$ff")" \
            && echo "[DISK] font: $(basename "$ff")"
    done
fi
mmd c:/DATA 2>/dev/null || true
mmd c:/DATA/Desktop c:/DATA/Documents c:/DATA/Downloads 2>/dev/null || true
mmd c:/DATA/Music c:/DATA/Video c:/DATA/Pictures c:/DATA/Screenshots 2>/dev/null || true

# === Audio test file ===
[ -f "$ROOT_DIR/boot/audioplayer/quack.mp2" ] && \
    mcopy "$ROOT_DIR/boot/audioplayer/quack.mp2" "c:/DATA/Music/quack.mp2"

# === 4. Copy kernel + GRUB config ===
echo "[DISK] Copying system files..."
mcopy "$BUILD_DIR/kernel.bin" c:/boot/kernel.bin
mcopy "$BUILD_DIR/kernel.bin" "c:/SYSTEM/OperatingSystem/kernel.bin"
mcopy "$ROOT_DIR/tools/grub_disk.cfg" c:/boot/grub/grub.cfg

# Helper
write_tmp() { echo "$1" > /tmp/mdob_tmp_$$; mcopy /tmp/mdob_tmp_$$ "$2"; }

# === 5. OS services ===
echo "[DISK] Copying OS services..."
OS_SVCLIST="console config hotplug DobFileSystem inputd dobinterface settingsd"
OS_SRCDIRS="boot/console boot/config boot/hotplug boot/DobFileSystem boot/inputd boot/dobinterface boot/settingsd"
set -- $OS_SRCDIRS
for svc in $OS_SVCLIST; do
    srcdir="$1"; shift
    for d in boot programs drivers; do
        if [ -f "$BUILD_DIR/$d/${svc}.mdl" ]; then
            mmd "c:/SYSTEM/OperatingSystem/$svc" 2>/dev/null || true
            mcopy "$BUILD_DIR/$d/${svc}.mdl" "c:/SYSTEM/OperatingSystem/$svc/${svc}.mdl"
            [ -f "$ROOT_DIR/$srcdir/manifest.dob" ] && \
                mcopy "$ROOT_DIR/$srcdir/manifest.dob" "c:/SYSTEM/OperatingSystem/$svc/manifest.dob"
            write_tmp "0" "c:/SYSTEM/OperatingSystem/$svc/Visible"
            break
        fi
    done
done

# audioplayer and floppyprobe are drivers (audio service / CMOS floppy
# probe): they live under /SYSTEM/DRIVERS, not /SYSTEM/OperatingSystem,
# even though their sources sit in boot/. Visible=0 keeps them off the
# desktop; the module manager still lists them under "Driver".
set -- "audioplayer boot/audioplayer" "floppyprobe boot/floppyprobe"
for pair in "audioplayer" "floppyprobe"; do
    case "$pair" in
        audioplayer) svc="audioplayer"; srcdir="boot/audioplayer" ;;
        floppyprobe) svc="floppyprobe"; srcdir="boot/floppyprobe" ;;
    esac
    if [ -f "$BUILD_DIR/boot/${svc}.mdl" ]; then
        mmd "c:/SYSTEM/DRIVERS/$svc" 2>/dev/null || true
        mcopy "$BUILD_DIR/boot/${svc}.mdl" "c:/SYSTEM/DRIVERS/$svc/${svc}.mdl"
        [ -f "$ROOT_DIR/$srcdir/manifest.dob" ] && \
            mcopy "$ROOT_DIR/$srcdir/manifest.dob" "c:/SYSTEM/DRIVERS/$svc/manifest.dob"
        write_tmp "0" "c:/SYSTEM/DRIVERS/$svc/Visible"
    fi
done

# === 6. Drivers ===
echo "[DISK] Copying drivers..."
for drv in rtc ata ahci usb_uhci usb_ehci usb_xhci usb_mass_storage ac97 maestro2e floppy cdrom bga mach64; do
    if [ -f "$BUILD_DIR/drivers/${drv}.mdl" ]; then
        mmd "c:/SYSTEM/DRIVERS/$drv" 2>/dev/null || true
        mcopy "$BUILD_DIR/drivers/${drv}.mdl" "c:/SYSTEM/DRIVERS/$drv/${drv}.mdl"
        [ -f "$ROOT_DIR/drivers/$drv/manifest.dob" ] && \
            mcopy "$ROOT_DIR/drivers/$drv/manifest.dob" "c:/SYSTEM/DRIVERS/$drv/manifest.dob"
        write_tmp "0" "c:/SYSTEM/DRIVERS/$drv/Visible"
    fi
done

# cdrom ships with a companion .mem that the driver loads via
# dob_mem_load at startup — ISO9660 parsing sits there, not in
# the driver binary. The .mem must land in the same folder
# because cdrom.mdl reads it with a sandbox-relative path.
if [ -f "$BUILD_DIR/drivers/cdrom/iso9660.mem" ]; then
    mcopy "$BUILD_DIR/drivers/cdrom/iso9660.mem" "c:/SYSTEM/DRIVERS/cdrom/iso9660.mem"
    echo "[DISK] iso9660.mem copied ($(stat -c %s "$BUILD_DIR/drivers/cdrom/iso9660.mem" 2>/dev/null || stat -f %z "$BUILD_DIR/drivers/cdrom/iso9660.mem") bytes)"
else
    echo "[DISK] WARNING: iso9660.mem MISSING — cdrom driver will fail at startup" >&2
fi

# exfat.mem — exFAT parser companion for DobFileSystem (full read-write + mkfs).
# Staged next to DobFileSystem.mdl; DobFileSystem loads it via dob_mem_load
# once filesystem routing lands (Phase 3). Inert until then, so a missing
# blob here is not fatal yet.
if [ -f "$BUILD_DIR/boot/DobFileSystem/exfat.mem" ]; then
    mcopy "$BUILD_DIR/boot/DobFileSystem/exfat.mem" "c:/SYSTEM/OperatingSystem/DobFileSystem/exfat.mem"
    echo "[DISK] exfat.mem copied ($(stat -c %s "$BUILD_DIR/boot/DobFileSystem/exfat.mem" 2>/dev/null || stat -f %z "$BUILD_DIR/boot/DobFileSystem/exfat.mem") bytes)"
else
    echo "[DISK] note: exfat.mem not present (exFAT support not built)"
fi

# === 7. Games ===
echo "[DISK] Copying games..."
for game in snake tetris minesweeper uidemo solitaire spider arrowsweeper; do
    if [ -f "$BUILD_DIR/programs/${game}.mdl" ]; then
        mmd "c:/SYSTEM/GAMES/$game" 2>/dev/null || true
        mcopy "$BUILD_DIR/programs/${game}.mdl" "c:/SYSTEM/GAMES/$game/${game}.mdl"
        [ -f "$ROOT_DIR/programs/$game/manifest.dob" ] && \
            mcopy "$ROOT_DIR/programs/$game/manifest.dob" "c:/SYSTEM/GAMES/$game/manifest.dob"
        write_tmp "1" "c:/SYSTEM/GAMES/$game/Visible"
    fi
done

# === 8. Programs ===
echo "[DISK] Copying programs..."
OS_SKIP="console config hotplug DobFileSystem inputd dobinterface snake tetris minesweeper uidemo solitaire spider arrowsweeper keymap settingsd"
for mdl in "$BUILD_DIR"/programs/*.mdl; do
    [ ! -f "$mdl" ] && continue
    name=$(basename "$mdl"); base="${name%.mdl}"; skip=false
    for s in $OS_SKIP; do [ "$base" = "$s" ] && skip=true; done
    if [ "$skip" = "false" ]; then
        mmd "c:/SYSTEM/PROGRAMS/$base" 2>/dev/null || true
        mcopy "$mdl" "c:/SYSTEM/PROGRAMS/$base/$name"
        [ -f "$ROOT_DIR/programs/$base/manifest.dob" ] && \
            mcopy "$ROOT_DIR/programs/$base/manifest.dob" "c:/SYSTEM/PROGRAMS/$base/manifest.dob"
        write_tmp "1" "c:/SYSTEM/PROGRAMS/$base/Visible"
        if [ "$base" = "DobPicture" ] && [ -f "$BUILD_DIR/programs/imgcodec.mem" ]; then
            mcopy "$BUILD_DIR/programs/imgcodec.mem" "c:/SYSTEM/PROGRAMS/DobPicture/imgcodec.mem"
        fi
    fi
done

# === 8b. keymap: tray applet + keyboard layout data files ===
# keymap is skipped above so we control its layout: the .mdl and every
# *.kbl layout file land in the same folder, and Visible="0" keeps the
# applet out of the program menu (it is launched from Startup_modules).
if [ -f "$BUILD_DIR/programs/keymap.mdl" ]; then
    echo "[DISK] Copying keymap (keyboard layout applet + layouts)..."
    mmd "c:/SYSTEM/PROGRAMS/keymap" 2>/dev/null || true
    mcopy "$BUILD_DIR/programs/keymap.mdl" "c:/SYSTEM/PROGRAMS/keymap/keymap.mdl"
    [ -f "$ROOT_DIR/programs/keymap/manifest.dob" ] && \
        mcopy "$ROOT_DIR/programs/keymap/manifest.dob" "c:/SYSTEM/PROGRAMS/keymap/manifest.dob"
    for kbl in "$ROOT_DIR"/programs/keymap/*.kbl; do
        [ -f "$kbl" ] && mcopy "$kbl" "c:/SYSTEM/PROGRAMS/keymap/$(basename "$kbl")"
    done
    write_tmp "0" "c:/SYSTEM/PROGRAMS/keymap/Visible"
fi

# === 9. Startup_modules config file ===
cat > /tmp/mdob_tmp_$$ << 'SMEOF'
# MainDOB Startup Modules
# Format: /path/to/module.mdl	flags
# Flags: driver, primary
# TAB separates path from flags. # disables a line.
# The kernel loads ALL modules listed here, in order.
# Registry is built into the kernel — services register via syscall.

# console (VGA text mode bubble) disabilitato: dobinterface è primary
# e riprogramma il framebuffer in modalità lineare, quindi il text
# mode 0xB8000 resta invisibile.  printf/puts delle app fanno fallback
# su debug_print (kernel serial) senza cambi visibili.
# Per riabilitarlo, scommenta la riga sotto.
# /SYSTEM/OperatingSystem/console/console.mdl	driver
/SYSTEM/OperatingSystem/inputd/inputd.mdl	driver
# rtc userspace bubble: il kernel ha già il suo rtc_monotonic_ms() per
# il clock, e nessun processo chiama dob_registry_find("rtc"). Lasciato
# sul disco ma non caricato. Scommenta se serve in futuro.
# /SYSTEM/DRIVERS/rtc/rtc.mdl	driver
# ATA disk driver — must be up before DobFileSystem so the filesystem
# service can mount the boot disk. Runs in standalone mode (no hotplug
# attach) because it is spawned directly from Startup_modules. Hotplug
# does not re-spawn it: there is no ata.das in /SYSTEM/CONFIG/DAS/, so
# the PCI scan finds class:01:01 unmatched and leaves the IDE
# controller alone.
/SYSTEM/DRIVERS/ata/ata.mdl	driver
/SYSTEM/DRIVERS/ahci/ahci.mdl	driver
/SYSTEM/OperatingSystem/DobFileSystem/DobFileSystem.mdl
# exfat_attach: one-shot at boot. Reads /SYSTEM/CONFIG/Definitive_volume
# (written by the installer) and, if present, attaches the designated exFAT
# "definitive" volume by spawning a secondary DobFileSystem mount on it.
# needs:DobFileSystem so the root FS is up to read the config; the disk
# drivers it relies on (ata/ahci) are already up above. No-op when the
# config file is absent, so it is harmless on plain FAT32-only installs.
/SYSTEM/PROGRAMS/exfat_attach/exfat_attach.mdl	needs:DobFileSystem
# settingsd: the settings daemon. Owns every program's .setting file.
# 'driver' grants the DobFileSystem sandbox bypass it needs to write
# those files inside other programs' folders; 'primary' makes the
# kernel respawn it if it ever dies (programs read settings at any
# time, so it must always be up); needs:DobFileSystem parks it until
# the filesystem is live, since its boot scan reads the disk.
/SYSTEM/OperatingSystem/settingsd/settingsd.mdl	driver primary needs:DobFileSystem
/SYSTEM/OperatingSystem/config/config.mdl
# Popups must be up before hotplug: the DAS action interpreter running
# inside hotplug calls dobpopup_Error/Info on failure paths and on
# popup_* primitives in action blocks.
# Hotplug loads the DAS database from /SYSTEM/CONFIG/DAS/*.das (needs
# DobFileSystem up), so it must come after DobFileSystem.
/SYSTEM/OperatingSystem/hotplug/hotplug.mdl	driver
# floppyprobe (v1.0.1.0): one-shot helper that reads CMOS to discover
# installed floppy drives and sends HOTPLUG_CREATE_LEGACY_BUBBLE to
# hotplug, then exits. needs:hotplug parks its main thread until the
# hotplug service is actually registered, so IPC is guaranteed to work.
/SYSTEM/DRIVERS/floppyprobe/floppyprobe.mdl	driver needs:hotplug
# dobinterface needs the video driver up before it runs: video_init()
# calls dv_vproc_attach via int 0x85, and the boomerang slot must be
# live at that point.  Declares needs:video — bga registers "video"
# only after BOTH its IPC port AND the boomerang slot are up, so the
# single clause covers both prerequisites.
#
# needs:hotplug used to be declared too but is now redundant: bga is
# spawned by hotplug, so the existence of a registered "video" service
# transitively guarantees hotplug is up.  Single-dependency parser in
# the kernel (see flag_extract_value) means only the first needs:
# clause is honoured anyway, and needs:video is the strictly stronger
# of the two.  dobinterface subscribes to hotplug at startup and gets
# the device-icon backlog via the subscribe replay.
/SYSTEM/OperatingSystem/dobinterface/dobinterface.mdl	driver primary needs:video
/SYSTEM/PROGRAMS/DobFiles/DobFiles.mdl	needs:dobinterface
# keymap: keyboard-layout tray applet. Loads after dobinterface
# (it needs the widget tray) and reaches inputd + config + the
# filesystem itself via the registry once it is up.
/SYSTEM/PROGRAMS/keymap/keymap.mdl	needs:dobinterface
# clock: tray clock + calendar (and agenda alarm). Widget-only in tray
# mode; needs the widget tray, hence needs:dobinterface. The agenda
# window is the same .mdl relaunched with "--app".
/SYSTEM/PROGRAMS/clock/clock.mdl	needs:dobinterface
/SYSTEM/DRIVERS/audioplayer/audioplayer.mdl	driver
SMEOF
mcopy /tmp/mdob_tmp_$$ "c:/SYSTEM/CONFIG/Startup_modules"

# === 10. Installed flag ===
write_tmp "installed" "c:/SYSTEM/CONFIG/installed"

# === 11. File type associations ===
cat > /tmp/mdob_tmp_$$ << 'ASSOCEOF'
# MainDOB File Type Associations
# Format: .ext=/full/path/to/program.mdl
# _spawn means launch the file directly as an executable MDL
#
# This file is the single source of truth for all file associations.
# The config server reads it at boot. Changes made at runtime are
# persisted back here.

.mdl=_spawn
.txt=/SYSTEM/PROGRAMS/editor/editor.mdl
.md=/SYSTEM/PROGRAMS/editor/editor.mdl
.cfg=/SYSTEM/PROGRAMS/editor/editor.mdl
.c=/SYSTEM/PROGRAMS/editor/editor.mdl
.h=/SYSTEM/PROGRAMS/editor/editor.mdl
.bmp=/SYSTEM/PROGRAMS/DobPicture/DobPicture.mdl
.png=/SYSTEM/PROGRAMS/DobPicture/DobPicture.mdl
.jpg=/SYSTEM/PROGRAMS/DobPicture/DobPicture.mdl
.wav=/SYSTEM/PROGRAMS/dobplayer/dobplayer.mdl
.mp2=/SYSTEM/PROGRAMS/dobplayer/dobplayer.mdl
.mp3=/SYSTEM/PROGRAMS/dobplayer/dobplayer.mdl
.tar=/SYSTEM/PROGRAMS/DobArchive/DobArchive.mdl
.zip=/SYSTEM/PROGRAMS/DobArchive/DobArchive.mdl
.dobw=/SYSTEM/PROGRAMS/DobWrite/DobWrite.mdl
.ttf=/SYSTEM/PROGRAMS/DobWrite/DobWrite.mdl
.otf=/SYSTEM/PROGRAMS/DobWrite/DobWrite.mdl
.dbp=/SYSTEM/PROGRAMS/DobInstaller/DobInstaller.mdl
ASSOCEOF
mcopy /tmp/mdob_tmp_$$ "c:/SYSTEM/CONFIG/Associations"

# === 12. Device Automation Scripts (DAS) ===
# Hotplug reads this directory at boot to discover every supported
# device. The flat /SYSTEM/CONFIG/Drivers manifest used in earlier
# builds is gone — DAS is now the sole authority.
mmd "c:/SYSTEM/CONFIG/DAS" 2>/dev/null || true
for das in "$ROOT_DIR"/config/DAS/*.das; do
    [ -f "$das" ] && mcopy "$das" "c:/SYSTEM/CONFIG/DAS/$(basename "$das")"
done
# USB device-level DAS live in a subdirectory (read by the USB host-
# controller drivers, not by hotplug). The glob above only covers the top
# level, so copy the USB/ subdir explicitly.
mmd "c:/SYSTEM/CONFIG/DAS/USB" 2>/dev/null || true
for das in "$ROOT_DIR"/config/DAS/USB/*.das; do
    [ -f "$das" ] && mcopy "$das" "c:/SYSTEM/CONFIG/DAS/USB/$(basename "$das")"
done
mmd c:/boot/grub/i386-pc 2>/dev/null || true
for mod in "$GRUB_DIR"/*.mod "$GRUB_DIR"/*.lst; do
    [ -f "$mod" ] && mcopy "$mod" "c:/boot/grub/i386-pc/$(basename "$mod")"
done

echo "[DISK] Files copied."

# === 15. Install GRUB in MBR gap (sectors 0-2047) ===
# Partition starts at 2048, so we have 1MB for GRUB — plenty.
cat > /tmp/mdob_embed_$$.cfg << 'EMBEDCFG'
set timeout=3
set default=0
set root=(hd0,msdos1)

menuentry "MainDOB" {
    multiboot /boot/kernel.bin
    boot
}
EMBEDCFG

$MKIMAGE -O i386-pc -d "$GRUB_DIR" -c /tmp/mdob_embed_$$.cfg \
    -p "(hd0,msdos1)/boot/grub" -o /tmp/mdob_core_$$.img \
    biosdisk fat part_msdos multiboot normal configfile ls

CORE_SIZE=$(stat -c%s /tmp/mdob_core_$$.img)
echo "[DISK] core.img: $CORE_SIZE bytes"

# Write boot.img to MBR (first 440 bytes, preserve partition table at 440-511)
python3 -c "
import struct

with open('$DISK', 'rb') as f:
    disk = bytearray(f.read(512))

with open('$GRUB_DIR/boot.img', 'rb') as f:
    boot = bytearray(f.read(512))

# Copy boot code (first 440 bytes) into MBR, keep partition table + signature
disk[0:440] = boot[0:440]

# Patch: core.img LBA at offset 92
struct.pack_into('<Q', disk, 92, 1)

with open('$DISK', 'r+b') as f:
    f.write(bytes(disk))

# Write core.img at sector 1 (in the MBR gap, before partition at 2048)
with open('/tmp/mdob_core_$$.img', 'rb') as f:
    core = f.read()
with open('$DISK', 'r+b') as f:
    f.seek(512)
    f.write(core)

print(f'[DISK] GRUB: boot.img in MBR + {len(core)} bytes core.img at sector 1')
"

rm -f "$MTOOLSRC"
echo "[DISK] Bootable disk created: $DISK (${SIZE_MB}MB)"
