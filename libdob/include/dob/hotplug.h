#ifndef MAINDOB_DOB_HOTPLUG_H
#define MAINDOB_DOB_HOTPLUG_H

/* Hotplug protocol: messages between hotplug server and device drivers.
 *
 * Lifecycle of a device "bubble":
 *
 *   [hardware detected] → spawn driver → driver calls dob_driver_attach()
 *                       → READY reply carries hotplug_device_t → [live, usable]
 *   [hardware removed]  → DETACH → driver cleanup → RELEASED → [bubble freed]
 *   [driver crashed]    → bubble reaped, hardware re-matched on next scan
 *   [yank without warning] → DETACH with 2s deadline, then force-kill
 */

#include <dob/types.h>

/* Messages FROM hotplug TO driver */
#define HOTPLUG_DETACH      101  /* Release this device. You have 2 seconds. */

/* Messages FROM driver TO hotplug */
#define HOTPLUG_READY       110  /* I'm operational. arg0 = my service port */
#define HOTPLUG_RELEASED    111  /* I've released all resources. Kill me. */
#define HOTPLUG_FAILED      112  /* I can't drive this hardware. */

/* Query messages from anyone TO hotplug */
#define HOTPLUG_LIST        200  /* List all live bubbles. Reply = hotplug_bubble_info_t[] */
#define HOTPLUG_FIND_DEVICE 201  /* arg0=vendor, arg1=device. Reply = bubble info */
#define HOTPLUG_RESCAN      202  /* Force immediate rescan */

/* Device info returned by hotplug in the HOTPLUG_READY reply */
typedef struct
{
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  bus, slot, func;
    uint32_t bar[6];
    uint8_t  irq_line;
    uint32_t bubble_id;      /* Unique ID for this bubble (driver sends it back in READY) */
} hotplug_device_t;

/* Bubble states */
#define BUBBLE_EMPTY      0
#define BUBBLE_SPAWNING   1
#define BUBBLE_ATTACHING  2
#define BUBBLE_LIVE       3
#define BUBBLE_DETACHING  4
#define BUBBLE_DEAD       5

/* Public info about a bubble (returned by HOTPLUG_LIST) */
typedef struct
{
    uint32_t bubble_id;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  state;
    int32_t  driver_pid;
    uint32_t service_port;   /* 0 if not yet LIVE */
    char     driver_name[32];
    char     service_name[32];
    /* DAS name (basename without .das) that hotplug matched against
     * this device, or empty string if no DAS matched. Used by the
     * installer wizard to mark "auto-detected" hardware. */
    char     das_name[32];
} hotplug_bubble_info_t;

#endif /* MAINDOB_DOB_HOTPLUG_H */
