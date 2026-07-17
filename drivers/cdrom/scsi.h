/* SCSI/ATAPI helpers for the MainDOB cdrom driver.
 *
 * The driver issues a small fixed set of SCSI commands over the ATAPI
 * PACKET transport: TEST UNIT READY (probe spin-up), REQUEST SENSE
 * (decode failures), READ(10) (one 2048-byte sector), and START STOP
 * UNIT (eject). Every CDB is 12 bytes — ATAPI PACKET length is fixed.
 *
 * Sense decoding turns the device's REQUEST SENSE reply into one of the
 * MainDOB-wide DOB_ERR_* codes the rest of the system already speaks.
 */

#ifndef MAINDOB_CDROM_SCSI_H
#define MAINDOB_CDROM_SCSI_H

#include <string.h>
#include <dob/types.h>

/* Opcodes — only the ones we actually issue. */
#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_REQUEST_SENSE    0x03
#define SCSI_START_STOP_UNIT  0x1B
#define SCSI_READ_10          0x28

/* Sense keys we discriminate on. Anything else is treated as hw fault. */
#define SENSE_NO_SENSE        0x00
#define SENSE_NOT_READY       0x02
#define SENSE_UNIT_ATTENTION  0x06

/* Additional sense codes we discriminate on. */
#define ASC_MEDIUM_NOT_PRESENT      0x3A
#define ASC_MEDIUM_MAY_HAVE_CHANGED 0x28

/* ===== CDB builders =====
 *
 * All CDBs are written into a 12-byte buffer the caller owns. The caller
 * passes that same buffer straight to the bus driver as the IPC payload,
 * so we never allocate. */

static inline void scsi_cdb_test_unit_ready(uint8_t cdb[12])
{
    memset(cdb, 0, 12);
    cdb[0] = SCSI_TEST_UNIT_READY;
}

static inline void scsi_cdb_request_sense(uint8_t cdb[12], uint8_t alloc)
{
    memset(cdb, 0, 12);
    cdb[0] = SCSI_REQUEST_SENSE;
    cdb[4] = alloc;
}

/* READ(10): one 2048-byte sector at `lba`. count=1 is the only shape we
 * ever issue — iso9660.mem walks one sector at a time. */
static inline void scsi_cdb_read10(uint8_t cdb[12], uint32_t lba)
{
    memset(cdb, 0, 12);
    cdb[0] = SCSI_READ_10;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)(lba);
    cdb[8] = 1;
}

/* START STOP UNIT with LoEj=1, Start=0 — eject the tray. */
static inline void scsi_cdb_eject(uint8_t cdb[12])
{
    memset(cdb, 0, 12);
    cdb[0] = SCSI_START_STOP_UNIT;
    cdb[4] = 0x02;
}

/* Decode an 18-byte REQUEST SENSE reply. Short replies are tolerated:
 * missing bytes read as zero. */
static inline void scsi_parse_sense(const uint8_t *sense, uint32_t len,
                                    uint8_t *key, uint8_t *asc)
{
    *key = (len > 2)  ? (uint8_t)(sense[2] & 0x0F) : 0;
    *asc = (len > 12) ?  sense[12]                 : 0;
}

#endif /* MAINDOB_CDROM_SCSI_H */
