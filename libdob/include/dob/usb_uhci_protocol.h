/* usb_uhci IPC protocol — opcodes + GET_DIAG snapshot.
 *
 * Opcodes (msg.code on the "usb_uhci" service port):
 *
 *    1  GET_STATUS   reply.arg0=USBSTS arg1=PORTSC1 arg2=PORTSC2
 *    2  PORT_RESET   arg0=port (manual, diagnostics)
 *    5  ENUMERATE    reply.arg0=num_ports arg1=ccs1 arg2=ccs2
 *    6  GET_DIAG
 *         No arguments.
 *         reply.payload = sizeof(uhci_diag_t)
 *         Full bring-up + hotplug-pipeline snapshot. Exists for the same
 *         reason as ATA_OP_GET_DIAG: real hardware with no serial console.
 *         The driver's debug_print() trail is invisible on a physical
 *         Armada E500 once dobinterface owns the screen, so every fact
 *         that decides "why is there no pendrive icon" is latched in the
 *         driver and read back by programs/usbdiag, on screen.
 *         Always returns DOB_OK.
 */

#ifndef MAINDOB_USB_UHCI_PROTOCOL_H
#define MAINDOB_USB_UHCI_PROTOCOL_H

#include <dob/types.h>

#define UHCI_OP_GET_STATUS   1
#define UHCI_OP_PORT_RESET   2
#define UHCI_OP_CTRL_XFER    3   /* control transfer for sub-drivers */
#define UHCI_OP_BULK_XFER    4   /* bulk transfer for sub-drivers */
#define UHCI_OP_ENUMERATE    5
#define UHCI_OP_GET_DIAG     6
#define UHCI_OP_RESET_TOGGLE 7   /* arg0 = ep | (dir_in << 7): clear data toggle */
#define UHCI_OP_GET_DEVINFO  8   /* reply payload = uhci_devinfo_t */
#define UHCI_OP_GET_WINDOW   9   /* reply arg0 = phys, arg1 = size: shared
                                  * DMA window for zero-copy bulk (below) */

/* ===== USB HOST TRANSPORT CONTRACT (ops 3/4/7/8/9) =====
 *
 * These opcodes are NOT UHCI-specific: they are the generic contract
 * between any USB host-controller driver and the class sub-drivers
 * (usb_mass_storage today; HID and others later). usb_ehci and
 * usb_xhci MUST implement the same opcodes with the same semantics —
 * the sub-driver receives the provider service name from the DAS
 * ($provider) and never assumes which controller it talks to, exactly
 * like the cdrom driver's ata/ahci provider table. When the second
 * implementor lands, this block graduates to dob/usb_host_protocol.h.
 *
 * Zero-copy (op 9): the controller exposes the PHYSICAL address of its
 * bulk DMA bounce. A sub-driver (drivers have mmap_phys) maps it once
 * and passes arg2=1 on BULK_XFER: OUT data is taken straight from the
 * window, IN data is left there — no IPC payload copies on the data
 * path. The wire itself is already bus-master DMA on every HC; this
 * removes the only remaining copies between controller and sub-driver.
 * Sub-drivers without the mapping simply omit arg2 and use payloads.
 *
 * The host-controller driver owns the wire; a sub-driver (e.g.
 * usb_mass_storage) is a CLIENT that submits transfers over IPC:
 *
 *   CTRL_XFER: payload = 8-byte SETUP packet, followed by wLength bytes
 *     of data for host->device requests. Direction comes from
 *     bmRequestType bit 7. Reply payload = IN data (device->host).
 *
 *   BULK_XFER: arg0 = endpoint | (0x80 if IN); arg1 = byte length
 *     (max UHCI_XFER_MAX). OUT: payload = data. IN: reply payload =
 *     data, reply arg0 = actual length (short packets are normal in
 *     BOT). Data toggles are tracked per-endpoint by the controller
 *     driver and survive across calls; RESET_TOGGLE clears one after
 *     a CLEAR_FEATURE(ENDPOINT_HALT).
 *
 *   Transfers are only accepted while a device is enumerated and the
 *   controller is in the DEVICE_READY running state. */
#define UHCI_XFER_MAX        8192

typedef struct
{
    uint8_t  dev_addr;        /* assigned USB address (1) */
    uint8_t  port;            /* root port the device sits on */
    uint8_t  low_speed;       /* 1 = low-speed device */
    uint8_t  _pad;
    uint16_t vid, pid;
    uint8_t  if_class, if_subclass, if_protocol;
    uint8_t  _pad2;
} uhci_devinfo_t;

/* FSM state values mirrored in uhci_diag_t.enum_state (keep in sync with
 * enum_state_t in drivers/usb_uhci/main.c). */
#define UHCI_ST_IDLE_SUSPENDED  0
#define UHCI_ST_PORT_RESETTING  1
#define UHCI_ST_SETADDR_SENT    2
#define UHCI_ST_DEVDESC_SENT    3
#define UHCI_ST_CONFDESC_SENT   4
#define UHCI_ST_ENUM_DONE       5
#define UHCI_ST_DEVICE_READY    6   /* enumerated, controller running */
#define UHCI_ST_DEVICE_IDLE     7   /* enumerated, suspended (RD armed) */
#define UHCI_ST_ENUM_ERROR      8

/* Reply payload for UHCI_OP_GET_DIAG. */
typedef struct
{
    /* --- identity / bring-up --- */
    uint8_t  init_ok;          /* 1 = uhci_init_hw completed */
    uint8_t  num_ports;
    uint8_t  irq_line;
    uint8_t  _pad0;
    uint16_t io_base;          /* BAR4 I/O base */
    uint16_t pci_vendor;       /* controller ids from hotplug ATTACH */
    uint16_t pci_device;
    uint16_t legsup_before;    /* PCI LEGSUP as found, BEFORE BIOS handoff */
    uint16_t legsup_now;       /* PCI LEGSUP read live at GET_DIAG time */
    uint16_t _pad1;

    /* --- live controller registers --- */
    uint16_t usbcmd;
    uint16_t usbsts;
    uint16_t usbintr;
    uint16_t frnum;
    uint16_t portsc[2];

    /* --- event counters since driver start --- */
    uint32_t cnt_irq;          /* hardware IRQ messages received */
    uint32_t cnt_resume;       /* Resume Detect events handled */
    uint32_t cnt_complete;     /* completion IRQs routed to the FSM */
    uint32_t cnt_timeout;      /* enumeration timeouts fired */
    uint32_t cnt_noirq_adv;    /* "TD complete but no IRQ" advances */

    /* --- enumeration / hotplug-pipeline latch --- */
    uint8_t  enum_state;       /* current FSM state (UHCI_ST_*) */
    int8_t   enum_port;        /* port being/last enumerated, -1 = none */
    uint8_t  enum_done;        /* 1 = at least one ENUM DONE since start */
    uint8_t  das_matched;      /* 1 = usb_das_match succeeded */
    uint8_t  das_files;        /* .das files the matcher saw (staging check) */
    uint8_t  announce_ok;      /* 1 = SUBDEVICE_APPEARED delivered to hotplug */
    uint16_t vid;              /* latched from the last ENUM DONE */
    uint16_t pid;
    uint8_t  dev_class;
    uint8_t  if_class;
    uint8_t  if_subclass;
    uint8_t  if_protocol;
    char     das_label[32];    /* label of the matched USB-DAS */
    char     last_error[48];   /* latched enum_fail reason ('' = none) */

    /* --- forensics latched INSIDE enum_fail (v3) ---
     * uhci_enter_suspend acks USBSTS, destroying HSE/HCPE evidence long
     * before any GET_DIAG can read the live registers; and the raw TD
     * write-back is the only witness of whether the controller ever
     * touched our schedule. So enum_fail photographs the scene first. */
    uint16_t fail_usbsts;      /* USBSTS read in enum_fail, pre-ack */
    uint16_t fail_frnum;       /* FRNUM at failure */
    uint32_t fail_td_status;   /* raw status dword of the waited-on TD */
    uint16_t fail_portsc;      /* PORTSC of the enumerating port */
    uint16_t elcr;             /* 8259 ELCR (0x4D0 | 0x4D1<<8): bit N set
                                * = IRQ N level-triggered. PIRQ lines MUST
                                * be level; an edge-mode line silently
                                * drops UHCI interrupts. */

    /* DAS-list forensics: raw outcome of the dobfs_List inside the last
     * usb_das_match, to split "List fails from the driver process"
     * (rc < 0) from "directory empty" (rc=0, entries=0) from "entries
     * present but filtered" (entries > 0, files = 0). rc -99 = the
     * matcher never ran since driver start. */
    int8_t   das_list_rc;
    uint8_t  das_dir_entries;
    /* Open/Read forensics of the LAST .das file the matcher attempted:
     * splits "Open fails" (open_fd < 0) from "Open ok, Read returns
     * 0/short" — -100 = never attempted. */
    int16_t  das_open_fd;
    int16_t  das_read_len;
    uint32_t cnt_shared;     /* shared-IRQ-line wakeups (USBSTS was 0) */
    uint32_t subdrv_ops;     /* transport ops served to the sub-driver */
    uint8_t  subdrv_last_op; /* last such opcode (3 ctrl, 4 bulk, 7, 8) */
} uhci_diag_t;

#endif /* MAINDOB_USB_UHCI_PROTOCOL_H */
