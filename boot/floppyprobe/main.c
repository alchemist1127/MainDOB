/* MainDOB FloppyProbe — one-shot CMOS floppy detector.
 *
 * Asks the CMOS how many floppy drives are present and tells hotplug
 * to create one legacy bubble per drive. Then exits.
 *
 * Listed in Startup_modules with `needs:hotplug`, so by the time
 * main() runs, hotplug is reachable via dob_registry_find. Total
 * runtime: two ISA I/O reads plus up to two sync IPC calls, < 1 ms.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <dob/types.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/cmos.h>
#include <dob/hotplug_events.h>

/*  *  Primitive — send one legacy-bubble creation request to hotplug.
 *  Synchronous so the helper only exits after hotplug has acknowledged.
 */

static void
request_legacy_bubble(uint32_t hp_port, uint8_t bus_type,
                      uint16_t io_base, uint8_t unit)
{
    hotplug_legacy_create_t req;
    req.bus_type = bus_type;
    req.unit     = unit;
    req.io_base  = io_base;

    dob_msg_t msg, reply;
    memset(&msg,   0, sizeof(msg));
    memset(&reply, 0, sizeof(reply));
    msg.code         = HOTPLUG_CREATE_LEGACY_BUBBLE;
    msg.payload      = &req;
    msg.payload_size = sizeof(req);

    (void)dob_ipc_call(hp_port, &msg, &reply);
}

/*  *  Primitive — inspect CMOS register 0x10.
 *
 *  Bit layout, stable since the 1984 PC/AT:
 *     bits 7..4 = drive 0 type   (0 = absent, 1..5 = present)
 *     bits 3..0 = drive 1 type
 */

static void
cmos_read_floppy_types(uint8_t *drive0, uint8_t *drive1)
{
    uint8_t types = cmos_read(CMOS_REG_FLOPPY_TYPES);
    *drive0 = (uint8_t)((types >> 4) & 0x0F);
    *drive1 = (uint8_t)( types       & 0x0F);
}

/* Top-level block */

static int
floppyprobe_run(void)
{
    uint32_t hp_port = dob_registry_find("hotplug");
    if (!hp_port)
    {
        debug_print("[floppyprobe] hotplug not reachable, exiting\n");
        return 0;
    }

    uint8_t drive0, drive1;
    cmos_read_floppy_types(&drive0, &drive1);

    printf("[floppyprobe] CMOS floppy probe: drive0=%u drive1=%u\n",
           drive0, drive1);

    if (drive0 != CMOS_FLOPPY_NONE)
        request_legacy_bubble(hp_port, BUS_TYPE_LEGACY_FDC, 0x3F0, 0);
    if (drive1 != CMOS_FLOPPY_NONE)
        request_legacy_bubble(hp_port, BUS_TYPE_LEGACY_FDC, 0x3F0, 1);

    return 0;
}

int
main(void)
{
    return floppyprobe_run();
}
