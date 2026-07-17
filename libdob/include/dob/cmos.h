/* MainDOB CMOS access — shared inline helpers.
 *
 * CMOS / NVRAM via two ISA I/O ports:
 *   0x70 — index register (write the register number here)
 *   0x71 — data register  (read or write the value)
 *
 * Two I/O instructions, fully synchronous, no IRQ. Used by the RTC
 * driver and by hotplug (register 0x10 — floppy drive types).
 */

#ifndef MAINDOB_DOB_CMOS_H
#define MAINDOB_DOB_CMOS_H

#include <unistd.h>
#include <dob/types.h>

#define CMOS_PORT_ADDR  0x70
#define CMOS_PORT_DATA  0x71

/* Standard CMOS register indices used across MainDOB. */
#define CMOS_REG_FLOPPY_TYPES   0x10  /* High nibble = drive 0, low nibble = drive 1 */

/* Floppy drive type encoding stored in CMOS register 0x10.
 * Each nibble holds one of these values; 0 means "no drive". */
#define CMOS_FLOPPY_NONE        0
#define CMOS_FLOPPY_360K_525    1
#define CMOS_FLOPPY_1200K_525   2
#define CMOS_FLOPPY_720K_350    3
#define CMOS_FLOPPY_1440K_350   4
#define CMOS_FLOPPY_2880K_350   5

/* Read one CMOS register. Synchronous, ~2 I/O cycles, never blocks. */
static inline uint8_t cmos_read(uint8_t reg)
{
    io_outb(CMOS_PORT_ADDR, reg);
    return io_inb(CMOS_PORT_DATA);
}

#endif /* MAINDOB_DOB_CMOS_H */
