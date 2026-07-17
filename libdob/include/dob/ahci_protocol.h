/* MainDOB AHCI / SATA driver — wire protocol shared with clients.
 *
 * The driver registers as IPC service "ahci". Opcodes:
 *
 *   1   READ
 *         arg0 = port (0..MAX_PORTS-1)
 *         arg1 = lba
 *         arg2 = sector count
 *         reply.payload = count*512 bytes
 *
 *   2   WRITE
 *         arg0 = port, arg1 = lba, arg2 = count
 *         payload = data
 *
 *   3   IDENTIFY
 *         arg0 = port
 *         reply.payload = 512 bytes raw IDENTIFY data
 *
 *   4   TRIM (SSD only)
 *         arg0 = port, arg1 = lba, arg2 = count
 *
 *   5   ATAPI_PACKET (optical only)
 *         arg0 = port
 *         payload = 12-byte CDB
 *         reply.payload = response data
 *
 *   10  LIST_PORTS
 *         No arguments.
 *         reply.payload      = N * sizeof(sata_port_info_t)
 *         reply.payload_size = N * sizeof(sata_port_info_t)
 *         reply.arg0         = N (number of present ports)
 *
 *   11  EJECT (optical only)
 *         arg0 = port
 *
 *   20  RESCAN_PARTITIONS
 *         arg0 = port
 *         Triggers MBR re-read and re-emission of HOTPLUG_SUBDEVICE_*
 *         events for partitions on the given port. Wired up in step 4
 *         of the disk-utility work (libdob/dob/partition.{h,c});
 *         stub returns DOB_OK today.
 *
 * Port numbering is whatever the HBA exposes — see LIST_PORTS for
 * the set of populated ports.
 */

#ifndef MAINDOB_AHCI_PROTOCOL_H
#define MAINDOB_AHCI_PROTOCOL_H

#include <dob/types.h>

#define AHCI_OP_READ                1
#define AHCI_OP_WRITE               2
#define AHCI_OP_IDENTIFY            3
#define AHCI_OP_TRIM                4
#define AHCI_OP_ATAPI_PACKET        5
#define AHCI_OP_LIST_PORTS         10
#define AHCI_OP_EJECT              11
#define AHCI_OP_RESCAN_PARTITIONS  20
#define AHCI_OP_GET_SMART          21  /* arg0 = port; reply.payload = 512 raw SMART */

/* Device-type tags reported in sata_port_info_t.type */
#define AHCI_DEV_NONE     0
#define AHCI_DEV_HDD      1
#define AHCI_DEV_SSD      2
#define AHCI_DEV_OPTICAL  3

/* Maximum AHCI ports per controller (HBA limit per AHCI spec). */
#define AHCI_MAX_PORTS   32

/* Public info for LIST_PORTS query. One entry per present port; the
 * reply only lists present ports (use reply.arg0 as the count). */
typedef struct
{
    uint8_t  port_num;
    uint8_t  type;            /* AHCI_DEV_HDD / _SSD / _OPTICAL */
    uint64_t sector_count;
    char     model[41];
    char     serial[21];
    bool     trim_supported;
} sata_port_info_t;

#endif /* MAINDOB_AHCI_PROTOCOL_H */
