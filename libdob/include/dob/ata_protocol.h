/* MainDOB ATA driver — wire protocol shared between driver and clients.
 *
 * The driver registers as IPC service "ata". Opcodes:
 *
 *   1   READ
 *         arg0 = lba
 *         arg1 = sector count   (1..MAX_SECTORS)
 *         arg2 = disk id        (0..3, default 0 = primary master)
 *         reply.payload         = count*512 bytes
 *
 *   2   WRITE
 *         arg0 = lba
 *         arg1 = sector count
 *         arg2 = disk id
 *         payload = data
 *
 *   3   IDENTIFY
 *         arg2 = disk id
 *         reply.payload = 512 bytes of raw ATA IDENTIFY data
 *
 *   10  LIST_DISKS
 *         No arguments.
 *         reply.payload      = ATA_MAX_DISKS * sizeof(ata_disk_info_t)
 *         reply.payload_size = same
 *         reply.arg0         = ATA_MAX_DISKS (4)
 *
 *   20  RESCAN_PARTITIONS
 *         arg0 = disk id
 *         Triggers a fresh MBR read and re-emission of
 *         HOTPLUG_SUBDEVICE_APPEARED/GONE events for partitions on the
 *         given disk. Wired up in step 4 of the disk-utility work
 *         (libdob/dob/partition.{h,c}); stub returns DOB_OK today.
 *
 *   21  GET_SMART
 *         arg2 = disk id
 *         reply.payload      = 512 bytes raw SMART READ DATA structure
 *                              (ATA-7 layout — vendor attributes at
 *                              bytes 2..361). Empty/zero on drives that
 *                              don't support SMART.
 *         reply.payload_size = 512
 *         Returns DOB_ERR_INTERNAL if the drive aborts the command
 *         (SMART unsupported or disabled).
 *
 *   22  GET_DIAG
 *         No arguments.
 *         reply.payload      = sizeof(ata_diag_t)
 *         Snapshot of the slot-0 DMA bring-up state. Exists for
 *         diagnosing DMA-vs-PIO on real hardware that has no serial
 *         console: a userspace probe (enginediag) reads it and shows it
 *         on screen. Always returns DOB_OK.
 *
 *   100 ATA_PACKET — ATAPI SCSI passthrough (cdrom driver only)
 *         arg0 = atapi slot, arg1 = alloc length, payload = 12-byte CDB
 *
 * Backward compatibility: arg2 defaulting to 0 keeps every pre-step-1
 * caller (DobFileSystem, bootfs) working unchanged — they zero the
 * whole dob_msg_t before populating, so arg2 is implicitly 0 = primary
 * master = old behaviour.
 */

#ifndef MAINDOB_ATA_PROTOCOL_H
#define MAINDOB_ATA_PROTOCOL_H

#include <dob/types.h>

#define ATA_OP_READ                1
#define ATA_OP_WRITE               2
#define ATA_OP_IDENTIFY            3
#define ATA_OP_LIST_DISKS         10
#define ATA_OP_RESCAN_PARTITIONS  20
#define ATA_OP_GET_SMART          21
#define ATA_OP_GET_DIAG           22
#define ATA_OP_TRIM               23  /* SSD slot 0: arg0=lba, arg1=count, arg2=disk */
#define ATA_OP_ATAPI_PACKET      100

#define ATA_MAX_DISKS              4   /* 4 IDE slots: 2 channels * 2 drives */

/* One entry per IDE slot. Slots are indexed 0..3:
 *   0 = primary master    (0x1F0 / IRQ 14)
 *   1 = primary slave     (0x1F0 / IRQ 14)
 *   2 = secondary master  (0x170 / IRQ 15)
 *   3 = secondary slave   (0x170 / IRQ 15)
 *
 * `present` is set iff IDENTIFY succeeded on the slot, i.e. an ATA
 * HDD is there. ATAPI (optical) drives appear in the parallel ATAPI
 * scan, not here. */
typedef struct
{
    uint8_t  present;        /* 1 = ATA HDD present, 0 = absent or ATAPI */
    uint8_t  channel;        /* 0 = primary, 1 = secondary */
    uint8_t  drive;          /* 0 = master,  1 = slave */
    uint8_t  lba48;          /* 1 if drive supports LBA48 */
    uint8_t  is_ssd;         /* 1 = non-rotating media (IDENTIFY word 217 == 1) */
    uint8_t  trim_supported; /* 1 = DATA SET MANAGEMENT TRIM supported (word 169 bit 0) */
    uint64_t total_sectors;  /* From IDENTIFY (LBA48 if supported, else LBA28) */
    char     model[41];      /* ATA IDENTIFY model string, trimmed, NUL-terminated */
    uint8_t  _pad[7];        /* Pad to 64 bytes for alignment */
} ata_disk_info_t;

/* Reply payload for ATA_OP_GET_DIAG (opcode 22). Slot-0 DMA bring-up
 * snapshot for on-screen diagnosis where no serial log is available. */
typedef struct
{
    uint8_t  dma_ok;          /* 1 = DMA path is currently active */
    uint8_t  udma_mode;       /* negotiated UDMA mode, 0xFF = none */
    uint8_t  udma_cap;        /* controller's max sustainable UDMA mode */
    uint8_t  lba48;           /* 1 = LBA48 in use */
    uint8_t  quirk_tried;     /* 1 = Level-B controller quirk was attempted */
    uint8_t  quirk_applied;   /* 1 = Level-B wrote a chipset UDMA-enable bit */
    uint8_t  bus_master_on;   /* 1 = PCI bus-master bit confirmed set */
    uint8_t  last_dma_fail;   /* 0 none, 1 wait timeout, 2 ATA ERR/DF, 3 BMIDE ERR */
    uint8_t  dma_fail_streak; /* consecutive runtime DMA failures */
    uint8_t  pci_found;       /* 1 = IDE controller located on PCI */
    uint16_t pci_vendor;      /* controller PCI vendor id (0 if none) */
    uint16_t pci_device;      /* controller PCI device id */
    uint16_t bm_base;         /* BMIDE I/O base (BAR4 masked) */
    uint8_t  _pad[6];         /* pad */
} ata_diag_t;

#endif /* MAINDOB_ATA_PROTOCOL_H */
