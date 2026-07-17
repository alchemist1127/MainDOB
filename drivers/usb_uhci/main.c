/* MainDOB UHCI Driver - USB 1.0/1.1 (Universal Host Controller Interface)
 *
 * === BLOCK 1 (Phase 1, step 1): DMA frame list + Global Suspend +
 *     Resume-IRQ event detection. ===
 *
 * Intel-style USB 1.x host controller (I/O port access).
 *
 * Event-driven detection rationale (Intel UHCI 1.1 Design Guide):
 * connect/disconnect on a port is a *resume event*. With the
 * controller in Global Suspend (USBCMD.EGSM=1) and the Resume
 * Interrupt enabled (USBINTR bit 1), inserting or removing a device
 * sets Resume Detect (USBSTS bit 2) and raises a real IRQ. We idle in
 * Global Suspend and wake on the IRQ instead of polling PORTSC.
 * (Spec: USBCMD bit 4 description; Table 6 "Behavior During Resume
 * when Host is in Global Suspend State".)
 *
 * This block does NOT enumerate yet. On a resume IRQ it acks the
 * controller, leaves suspend, reads PORTSC to report which port
 * changed and whether a device is now present, then (for now) goes
 * back to suspend to await the next event. Enumeration + USB-DAS
 * matching + hotplug handoff arrive in later blocks.
 *
 * Legacy control protocol (diagnostics / compatibility):
 *   code=1 GET_STATUS  -> reply.arg0=USBSTS arg1=PORTSC1 arg2=PORTSC2
 *   code=2 PORT_RESET  arg0=port_num
 *   code=5 ENUMERATE   -> reply.arg0=num_ports arg1=ccs1 arg2=ccs2
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <dob/server.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/types.h>
#include <dob/hotplug_driver.h>
#include <dob/hotplug_events.h>
#include <dob/usb_uhci_protocol.h>
#include "usb_das.h"

/* ===== UHCI I/O register offsets (from BAR4 I/O base) ===== */
#define UHCI_CMD        0x00    /* USB Command */
#define UHCI_STS        0x02    /* USB Status */
#define UHCI_INTR       0x04    /* USB Interrupt Enable */
#define UHCI_FRNUM      0x06    /* Frame Number */
#define UHCI_FRBASEADD  0x08    /* Frame List Base Address (32-bit, phys) */
#define UHCI_SOFMOD     0x0C    /* Start of Frame Modify */
#define UHCI_PORTSC1    0x10    /* Port 1 Status/Control */
#define UHCI_PORTSC2    0x12    /* Port 2 Status/Control */

/* ===== USBCMD bits ===== */
#define UHCI_CMD_RS         (1 << 0)    /* Run/Stop */
#define UHCI_CMD_HCRESET    (1 << 1)    /* Host Controller Reset */
#define UHCI_CMD_GRESET     (1 << 2)    /* Global Reset */
#define UHCI_CMD_EGSM       (1 << 3)    /* Enter Global Suspend Mode */
#define UHCI_CMD_FGR        (1 << 4)    /* Force Global Resume */
#define UHCI_CMD_MAXP       (1 << 7)    /* Max Packet (64 bytes) */

/* ===== USBSTS bits (R/WC: write 1 to clear) ===== */
#define UHCI_STS_USBINT     (1 << 0)    /* USB Interrupt (IOC / short pkt) */
#define UHCI_STS_ERROR      (1 << 1)    /* USB Error Interrupt */
#define UHCI_STS_RD         (1 << 2)    /* Resume Detect */
#define UHCI_STS_HSE        (1 << 3)    /* Host System Error */
#define UHCI_STS_HCPE       (1 << 4)    /* Host Controller Process Error */
#define UHCI_STS_HCH        (1 << 5)    /* HC Halted */
#define UHCI_STS_ALL        0x003F

/* Real interrupt causes only (everything in USBSTS EXCEPT HC Halted, which
 * is a run-state status bit, not an interrupt source). Use this — never
 * UHCI_STS_ALL — to decide whether THIS controller actually raised the
 * interrupt: at boot every UHCI controller sits halted (STS=0x20), so a
 * test against UHCI_STS_ALL is true on all of them, which during empirical
 * GSI discovery lets an idle, device-less controller wrongly claim a
 * shared GSI ahead of the controller that really asserted it. */
#define UHCI_STS_INTR       (UHCI_STS_USBINT | UHCI_STS_ERROR | UHCI_STS_RD | \
                             UHCI_STS_HSE | UHCI_STS_HCPE)   /* 0x1F */

/* ===== USBINTR bits ===== */
#define UHCI_INTR_TIMEOUT   (1 << 0)    /* Timeout/CRC */
#define UHCI_INTR_RESUME    (1 << 1)    /* Resume Interrupt */
#define UHCI_INTR_IOC       (1 << 2)    /* Interrupt on Complete */
#define UHCI_INTR_SHORT     (1 << 3)    /* Short Packet */

/* ===== PORTSC bits ===== */
#define UHCI_PORT_CCS       (1 << 0)    /* Current Connect Status (RO) */
#define UHCI_PORT_CSC       (1 << 1)    /* Connect Status Change (R/WC) */
#define UHCI_PORT_PE        (1 << 2)    /* Port Enable */
#define UHCI_PORT_PEC       (1 << 3)    /* Port Enable Change (R/WC) */
#define UHCI_PORT_RD        (1 << 6)    /* Resume Detect */
#define UHCI_PORT_LSDA      (1 << 8)    /* Low Speed Device Attached (RO) */
#define UHCI_PORT_RESET     (1 << 9)    /* Port Reset */
#define UHCI_PORT_SUSP      (1 << 12)   /* Suspend */

/* R/WC change bits in PORTSC. When doing read-modify-write on a port
 * register we must NOT accidentally write a 1 into these (that would
 * clear a pending change). Mask them out of the read-back value before
 * OR-ing in our new control bits, unless we mean to ACK them. */
#define UHCI_PORT_CHANGE    (UHCI_PORT_CSC | UHCI_PORT_PEC)

/* PORTSC write hygiene. Two trap classes in one register:
 *   - CSC/PEC (bits 1,3) are R/WC: writing 1 CLEARS them, so a plain
 *     read-modify-write accidentally acks them — every RMW must drop them
 *     unless the ack is intended.
 *   - RD (bit 6) is R/W: hardware sets it when the port detects resume
 *     signalling in suspend, but SOFTWARE writing 1 DRIVES resume
 *     signalling on the port. After a real resume the bit reads 1; QEMU
 *     never sets it, so on QEMU writing it back is invisible — on real
 *     silicon every "io_inw | something" write-back was re-asserting
 *     resume K-state on a running port, corrupting enumeration transfers
 *     and re-triggering Resume Detect.
 * Every PORTSC write therefore starts from a read masked with
 * UHCI_PORT_WRMASK. */
#define UHCI_PORT_WRMASK    ((uint16_t)~(UHCI_PORT_CHANGE | UHCI_PORT_RD))

/* ===== Frame list ===== */
#define UHCI_FRAME_COUNT    1024

/* ===== UHCI Transfer Descriptor =====
 * HW uses the first 4 dwords (16 bytes); the remaining 4 are reserved
 * for software. Must be 16-byte aligned. The HW only ever copies link
 * pointers and updates the status dword — no arithmetic on pointers. */
typedef struct
{
    volatile uint32_t link;     /* DWORD0: next TD/QH (phys) or terminate */
    volatile uint32_t status;   /* DWORD1: control & status */
    volatile uint32_t token;    /* DWORD2: PID, addr, endp, toggle, maxlen */
    volatile uint32_t buffer;   /* DWORD3: data buffer (phys) */
    uint32_t sw[4];             /* reserved for software */
} __attribute__((packed, aligned(16))) uhci_td_t;

/* ===== UHCI Queue Head ===== (8 bytes used; 16-byte aligned) */
typedef struct
{
    volatile uint32_t head_link; /* horizontal: next QH (phys) or T */
    volatile uint32_t element;   /* vertical: first TD (phys) or T */
    uint32_t sw[2];
} __attribute__((packed, aligned(16))) uhci_qh_t;

/* Link-pointer control bits (low bits of a phys pointer) */
#define LP_TERMINATE    (1u << 0)   /* T: pointer invalid */
#define LP_QH           (1u << 1)   /* Q: 1=points to QH, 0=points to TD */
#define LP_DEPTH        (1u << 2)   /* Vf: depth-first (vertical) */

/* TD status dword (DWORD1) bits */
#define TD_STS_ACTIVE   (1u << 23)
#define TD_STS_STALLED  (1u << 22)
#define TD_STS_DBUFERR  (1u << 21)
#define TD_STS_BABBLE   (1u << 20)
#define TD_STS_NAK      (1u << 19)
#define TD_STS_CRCTO    (1u << 18)
#define TD_STS_BITSTUFF (1u << 17)
#define TD_CTRL_IOC     (1u << 24)  /* interrupt on complete */
#define TD_CTRL_LS      (1u << 26)  /* low-speed device */
#define TD_CTRL_SPD     (1u << 29)  /* short packet detect */
#define TD_CTRL_CERR3   (3u << 27)  /* 3 error retries */
#define TD_STS_ERRMASK  (TD_STS_STALLED | TD_STS_DBUFERR | TD_STS_BABBLE | \
                         TD_STS_CRCTO | TD_STS_BITSTUFF)
#define TD_ACTLEN_MASK  0x000007FFu /* DWORD1 [10:0], (n-1) encoding */

/* TD token dword (DWORD2) field shifts */
#define TD_TOK_PID_SHIFT     0
#define TD_TOK_ADDR_SHIFT    8
#define TD_TOK_ENDP_SHIFT    15
#define TD_TOK_DT_SHIFT      19  /* data toggle */
#define TD_TOK_MAXLEN_SHIFT  21  /* (len-1); 0x7FF means 0 bytes */

/* USB PIDs */
#define USB_PID_SETUP   0x2D
#define USB_PID_IN      0x69
#define USB_PID_OUT     0xE1

/* USB descriptor types */
#define USB_DT_DEVICE       0x01
#define USB_DT_CONFIG       0x02
#define USB_DT_INTERFACE    0x04

/* USB standard requests */
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_GET_DESCRIPTOR  0x06

/* bmRequestType */
#define USB_RT_DEV_IN       0x80    /* device->host, standard, device */
#define USB_RT_DEV_OUT      0x00    /* host->device, standard, device */

/* ===== USB descriptor wire structs ===== */
typedef struct
{
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_t;

typedef struct
{
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed)) usb_device_descriptor_t;

typedef struct
{
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed)) usb_config_descriptor_t;

typedef struct
{
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed)) usb_interface_descriptor_t;

/* ===== Driver state ===== */
static uint16_t io_base   = 0;
static uint32_t irq_port  = 0;
static uint8_t  uhci_irq  = 0;
static uint32_t num_ports = 2;

/* Frame list lives in DMA memory: UHCI is a bus-master that fetches it
 * by PHYSICAL address. The previous build passed a virtual address
 * (worked on QEMU's identity map by luck, undefined on real HW). */
static volatile uint32_t *frame_list      = 0;
static uint32_t           frame_list_phys = 0;

/* Per-port presence shadow: last known Current Connect Status. Lets the
 * resume handler report appeared-vs-removed without re-deriving it. */
static uint8_t port_present[2] = { 0, 0 };

static const uint16_t portsc_reg[2] = { UHCI_PORTSC1, UHCI_PORTSC2 };

/* ===== Real-hardware survival kit =====
 *
 * PCI LEGSUP (Legacy Support, config offset 0xC0, 16-bit).  On real
 * machines the BIOS emulates a PS/2 keyboard/mouse through SMM and
 * OWNS the UHCI controller via this register: port 60h/64h traps
 * raise SMIs and the SMM handler drives the controller behind the
 * OS's back.  Until we take ownership, our IRQs are stolen and the
 * controller state is not ours.  QEMU has no such SMM BIOS, which
 * is exactly why everything worked there and nothing happens on
 * metal.  Handoff sequence (same as Linux uhci_check_and_reset_hc):
 *   1. write 0x8F00  — ack all R/WC trap-status bits AND clear every
 *                      SMI/trap enable: SMM lets go;
 *   2. HCRESET       — (already done by uhci_reset_and_setup);
 *   3. write 0x2000  — USBPIRQDEN only: route the controller's
 *                      interrupt to PIRQ.  On PIIX, without this bit
 *                      the IRQ line NEVER asserts — no resume IRQ,
 *                      no completion IRQ, driver sleeps forever.
 * The register is 16-bit but SYS_PCI_WRITE is dword-only (offset
 * masked to 0xFFC), so we read-modify-write 32 bits preserving the
 * reserved upper half at 0xC2.
 *
 * With ownership taken and PIRQDEN set, the event-driven model (Global
 * Suspend + Resume Detect IRQ on connect/disconnect) is trusted as-is:
 * hotplug detection is intrinsic to USB and the driver deliberately
 * does NOT poll PORTSC. */
#define UHCI_LEGSUP         0xC0
#define UHCI_LEGSUP_RWC     0x8F00  /* all R/WC status + all enables off */
#define UHCI_LEGSUP_PIRQEN  0x2000  /* USBPIRQDEN: IRQ routed to PIRQ */

static hotplug_device_t uhci_dev;       /* saved for PCI config access */

/* ===== Diagnostics latches + counters (read back via GET_DIAG) =====
 *
 * Same philosophy as the ATA driver's ata_diag_t: on real hardware with
 * no serial console the debug_print() trail is invisible, so every fact
 * that decides "why is there no pendrive icon" is latched here and shown
 * on screen by programs/usbdiag. */
static uint8_t  diag_init_ok       = 0;
static uint16_t diag_legsup_before = 0xFFFF;
static uint32_t cnt_irq            = 0;  /* hardware IRQ messages (ours) */
static uint32_t cnt_shared         = 0;
/* Multi-controller support (Extensa 5220: FIVE UHCI functions on ICH8).
 * The registry arbitrates the instance name; instance_id feeds the
 * announce token so every (controller, port) pair downstream is unique.
 * The token also carries a per-connection epoch (see conn_epoch below) so
 * it is unique across REPLUGS too, not just across ports: built by
 * uhci_make_token(). OPAQUE to das/usbms/dobfs/DobDisk, which all treat
 * it as a name suffix. */
static char     uhci_service_name[32] = "usb_uhci";
static uint32_t uhci_instance_id      = 0;

/* Per-connection epoch.
 *
 * The provider_token we announce to hotplug becomes, downstream, the
 * usbms_<token> and dobfs_<token> service names every layer derives from
 * it. That token MUST be fresh for each physical connection: a replug of
 * the same port is a NEW world, not a resumption of the old one.
 *
 * (uhci_instance_id<<4)|port alone is STABLE across replugs. The teardown
 * of the previous world (DETACH -> usbms -> dobfs SHUTDOWN) is a chain of
 * fire-and-forget posts (deliberately, to avoid the provider->consumer
 * deadlock cycle), so it is asynchronous and can lose the race against a
 * quick replug + click — or fail outright. A survivor of the old world
 * then squats the EXACT name the new device needs:
 *   - usbms startup guard finds usbms_<token> and exits -> the DAS resolves
 *     the OLD (dead) port: "icon does nothing";
 *   - usbms PREPARE_VOLUME finds dobfs_<token> and returns "already mounted"
 *     -> the DAS opens the OLD dobfs bound to the dead old usbms.
 *
 * Folding a monotonic epoch into the token makes each connection's names
 * unique, so a survivor of a prior connection can no longer collide: it
 * becomes a harmless orphan (its icon is already gone) that the existing
 * orphan-self-shutdown logic reaps. "The port signature must be fresh,
 * not stale", applied at the source. NOTE: this bites on the full-removal
 * path (CCS drops -> uhci_device_gone -> re-enumerate); a fast re-seat
 * that never drops CCS lands in ST_DISCONNECT_CHECK's "kept" branch, which
 * does not tear down and so keeps the same world (and token). */
static uint32_t conn_epoch    = 0;  /* bumped once per connection, in device_gone */
static uint32_t active_token  = 0;  /* token announced for the live device.
                                     * 0 is a VALID token (epoch 0, instance 0,
                                     * port 0 — the common first plug), so it
                                     * must NOT double as a "none" sentinel:
                                     * use dev_announced for that. */
static int      dev_announced = 0;  /* 1 between a successful SUBDEVICE_APPEARED
                                     * and its matching device_gone. Gates the
                                     * GONE retract + usbms nudge so they fire
                                     * for EVERY announced device, token 0
                                     * included. */

/* Build the opaque provider_token for the device on `port`. Layout:
 *   bits  0..3  : port
 *   bits  4..15 : controller instance id
 *   bits 16..30 : connection epoch (15 bits; wraps harmlessly after 32767
 *                 replugs — only needs to differ from the PREVIOUS epoch)
 * The whole value stays a positive int (<= 0x7FFFFFFF) so usbms's atoi()
 * of $token, and every "%u"/"%d" rendering downstream, are safe. With
 * epoch 0 (first connection) the token is byte-identical to the old
 * (instance<<4)|port, so nothing changes in the common single-plug case. */
static uint32_t uhci_make_token(int port)
{
    return ((conn_epoch & 0x7FFFu) << 16)
         | ((uhci_instance_id & 0xFFFu) << 4)
         | ((uint32_t)port & 0xFu);
}
static uint8_t  rd_deferred        = 0;  /* RD arrived while FSM busy */
/* Wake FSM parking slot: ops 3/4 arriving while DEVICE_IDLE are stashed
 * here (payload copied: the receive buffer is reused) and served when
 * the resume sequence completes. The sub-driver is strictly serial, so
 * one slot is enough; a second 3/4 during wake gets DOB_ERR_BUSY. */
static dob_msg_t pending_req;
/* Lazily allocated on the FIRST park: a controller with no device never
 * parks an op, so idle instances (Extensa: four out of five) never pay
 * these 8 KB. Per-idle-instance RAM is a design budget here. */
static uint8_t  *pending_payload   = NULL;
static uint8_t   pending_valid     = 0;
static uint32_t  debounce_port     = 0;
static uint8_t   wake_reason_rd    = 0;  /* wake FSM entered for RD inspect */

static void uhci_process_deferred_rd(void);
static bool uhci_port_reenable(int p);
static dob_status_t handle_request(dob_msg_t *msg, dob_msg_t *reply);
/* Flight recorder for the sub-driver: usbms's bring-up issues a
 * DETERMINISTIC op sequence (8 devinfo, 3 getdesc, 3 setconfig, then
 * 4s for BOT). When usbms goes silent, the count+last-op served HERE
 * name the stage it died in — readable from fase 3 even when fase 4
 * hangs. */
static uint32_t subdrv_ops_served  = 0;
static uint8_t  subdrv_last_op     = 0;  /* IRQ msgs with USBSTS == 0: the
                                          * neighbour on a SHARED line woke
                                          * us (QEMU: ~1380 such hits during
                                          * boot disk traffic). Benign, but
                                          * counted apart so the diag stops
                                          * crying wolf. */
static uint32_t cnt_resume         = 0;  /* Resume Detect handled */
static uint32_t cnt_complete       = 0;  /* completion IRQs to the FSM */
static uint32_t cnt_timeout        = 0;  /* enumeration timeouts fired */
static uint32_t cnt_noirq_adv      = 0;  /* TD-complete-but-no-IRQ advances */
static uint8_t  diag_enum_done     = 0;
static uint8_t  diag_das_matched   = 0;
static uint8_t  diag_announce_ok   = 0;
static char     diag_das_label[32]  = "";
static char     diag_last_error[48] = "";
static uint16_t diag_fail_usbsts    = 0;
static uint16_t diag_fail_frnum     = 0;
static uint32_t diag_fail_td_status = 0;
static uint16_t diag_fail_portsc    = 0;
static int      diag_cur_td_idx     = -1;  /* TD the FSM is waiting on */

/* ===== Sub-driver transport (ops 3/4/7/8) =====
 * Synchronous transfer engine used ONLY in ST_DEVICE_READY, while the
 * enumeration FSM is parked. It builds chains in a dedicated TD pool
 * (the enum pool is too small for 8 KB bulk payloads), links the ctrl
 * QH into the schedule, and polls completion with bounded busy-waits.
 * Polling here is fine: the caller (sub-driver) issued a synchronous
 * IPC and is waiting anyway, the controller is otherwise idle, and
 * events keep queueing on our port for the loop to drain afterwards. */
#define XFER_MAX_TD     136      /* 8 KB / 64 B + setup/status margin */
static volatile uhci_td_t *xfer_td  = 0;
static uint32_t  xfer_td_phys       = 0;
static uint8_t  *xfer_buf           = 0;   /* DMA bounce, UHCI_XFER_MAX */
static uint32_t  xfer_buf_phys      = 0;
static uint16_t  toggle_in_map      = 0;   /* bit N = next toggle, EP N IN */
static uint16_t  toggle_out_map     = 0;
static int       ready_port         = -1;  /* port of the live device */
static int       xfer_activity      = 0;   /* transfers since last idle tick */
static int       pending_gone       = 0;   /* disconnect seen while serving a
                                            * request: handled from the LOOP,
                                            * never inline (see device_gone) */
#define DEVICE_IDLE_MS   2000
#define XFER_TD_PHYS(i)  (xfer_td_phys + (uint32_t)(i) * sizeof(uhci_td_t))

static void uhci_device_ready(void);   /* defined with the transport engine */
static void uhci_device_gone(void);

/* ===== Enumeration finite state machine =====
 *
 * The whole driver is event-driven: the main loop never blocks except
 * inside dob_ipc_receive. Enumeration is therefore a state machine that
 * the loop advances one step per completion IRQ. See the transition
 * table in the file header.
 *
 * Timeouts: on entering any *_SENT state we arm a one-shot timer
 * (ENUM_TIMEOUT_MS). The completion IRQ cancels it by bumping
 * enum_timer_gen; a timer callback whose generation is stale is a
 * late timer and is ignored. */
typedef enum
{
    ST_IDLE_SUSPENDED = 0,  /* in global suspend, awaiting resume IRQ */
    ST_PORT_RESETTING,      /* port-reset pulse in progress (timer) */
    ST_SETADDR_SENT,        /* SET_ADDRESS queued, awaiting USBINT */
    ST_DEVDESC_SENT,        /* GET_DESCRIPTOR(device) queued */
    ST_CONFDESC_SENT,       /* GET_DESCRIPTOR(config) queued */
    ST_ENUM_DONE,           /* class triple in hand */
    ST_DEVICE_READY,        /* enumerated device, controller RUNNING:
                             * sub-driver transfers in flight. Disconnect
                             * here surfaces as transfer errors (checked
                             * after every failed op) — an event, not a
                             * poll. An idle one-shot timer demotes to
                             * DEVICE_IDLE after 2 s without transfers. */
    ST_DEVICE_IDLE,         /* enumerated device, controller back in
                             * GLOBAL SUSPEND: zero CPU, zero polling, and
                             * the hardware Resume-Detect IRQ fires on
                             * DISCONNECT too — removal while idle is a
                             * pure event (the cdrom/floppy lesson:
                             * probe/wake on demand, never poll). First
                             * sub-driver op wakes us to DEVICE_READY
                             * (~50 ms, once per idle->active edge). */
    ST_ENUM_ERROR,          /* timeout/stall/err -> cleanup -> suspend */
    ST_WAKING_FGR,          /* wake FSM: FGR asserted, timer running —
                             * the requesting op is PARKED, the loop is
                             * free (event-driven resume, no busy wait) */
    ST_WAKING_SETTLE,       /* wake FSM: RS=1, TRSMRCY settling */
    ST_CONNECT_DEBOUNCE,    /* connect seen; mechanical contacts BOUNCE
                             * on real ports — re-check after 100 ms
                             * (timer) before spending an enumeration */
    ST_DISCONNECT_CHECK,    /* CSC with device present in idle: timed
                             * 50 ms re-check (removal bounces too) */
} enum_state_t;

#define ENUM_TIMEOUT_MS     100
#define USB_ENUM_ADDRESS    1       /* single device: always address 1 */

static enum_state_t enum_state = ST_IDLE_SUSPENDED;
static int          enum_port  = -1;    /* which port we're enumerating */
static int          enum_low_speed = 0; /* LSDA bit captured at reset */
static uint32_t     enum_timer_gen = 0; /* bumped to invalidate timers */

/* Timer messages carry no identity, so a stale one-shot (e.g. the 2 s
 * idle tick armed before a suspend) can fire INTO a wake/debounce state
 * and cut its dwell short — an FGR held 3 ms wakes nothing, and the
 * failure is maddeningly intermittent. Every timed state records its
 * entry time; the handler re-arms the REMAINDER when woken early.
 * Stale timers become harmless instead of being (impossibly) filtered. */
static uint32_t  state_t0          = 0;
static uint32_t  state_dwell_ms    = 0;

static void timed_state(enum_state_t st, uint32_t dwell_ms)
{
    enum_state     = st;
    state_t0       = clock_ms();
    state_dwell_ms = dwell_ms;
    timer_set(irq_port, dwell_ms, 0);
}

/* Returns 1 if the dwell elapsed; else re-arms the remainder. */
static int dwell_elapsed(void)
{
    uint32_t el = clock_ms() - state_t0;
    if (el + 2 >= state_dwell_ms) return 1;     /* 2 ms slack */
    timer_set(irq_port, state_dwell_ms - el, 0);
    return 0;
}

/* Results gathered during enumeration */
static usb_device_descriptor_t    enum_dev;
static usb_interface_descriptor_t enum_iface;

/* ===== DMA transfer resources =====
 * One QH anchored in the frame list, a small TD ring, a setup-packet
 * buffer and a data buffer — all in physical-addressable DMA memory.
 * Control transfers for enumeration are tiny and strictly sequential
 * (one device at a time), so a single shared QH/TD set is sufficient. */
#define ENUM_MAX_TD     4
#define ENUM_BUF_SIZE   256

static volatile uhci_qh_t *ctrl_qh        = 0;
static uint32_t            ctrl_qh_phys   = 0;
static volatile uhci_td_t *ctrl_td        = 0;  /* array[ENUM_MAX_TD] */
static uint32_t            ctrl_td_phys   = 0;
static volatile uint8_t   *setup_buf      = 0;
static uint32_t            setup_buf_phys = 0;
static volatile uint8_t   *data_buf       = 0;
static uint32_t            data_buf_phys  = 0;

/* phys address of TD i in the array */
#define TD_PHYS(i)   (ctrl_td_phys + (uint32_t)((i) * sizeof(uhci_td_t)))

/* ===== Small helpers ===== */


/* ===== Hardware bring-up ===== */

/* Global + host-controller reset, then program the DMA frame list.
 * Leaves the controller stopped (RS=0) with all frames terminated. */
static void uhci_reset_and_setup(void)
{
    /* Global reset pulse */
    io_outw(io_base + UHCI_CMD, UHCI_CMD_GRESET);
    busy_wait_us(50000);
    io_outw(io_base + UHCI_CMD, 0);

    /* Host controller reset; wait for self-clear */
    io_outw(io_base + UHCI_CMD, UHCI_CMD_HCRESET);
    for (int i = 0; i < 20; i++)
    {
        if (!(io_inw(io_base + UHCI_CMD) & UHCI_CMD_HCRESET)) break;
        busy_wait_us(5000);
    }

    /* Clear all pending status (write-1-to-clear) */
    io_outw(io_base + UHCI_STS, UHCI_STS_ALL);

    /* Frame list is LAZY (ensure_frame_list): an idle controller in EGSM
     * never fetches frames, so instances without a device skip the 4 KB
     * DMA allocation entirely. If it was already allocated (re-setup
     * path), just re-program the base register. */
    if (frame_list)
    {
        io_outl(io_base + UHCI_FRBASEADD, frame_list_phys);
        io_outw(io_base + UHCI_FRNUM, 0);
    }
    io_outb(io_base + UHCI_SOFMOD, 64);  /* default 1 ms frame */
}

/* Allocate + install the DMA frame list on first need (enumeration or
 * transfer). Idempotent; returns false only on allocation failure. */
static bool ensure_frame_list(void)
{
    if (frame_list) return true;
    frame_list = (volatile uint32_t *)
        dma_alloc(UHCI_FRAME_COUNT * sizeof(uint32_t), &frame_list_phys);
    if (!frame_list || !frame_list_phys)
    {
        debug_print("[uhci] dma_alloc for frame list FAILED\n");
        frame_list = NULL;
        return false;
    }
    for (int i = 0; i < UHCI_FRAME_COUNT; i++)
        frame_list[i] = 1u;  /* T-bit set, pointer invalid */
    io_outl(io_base + UHCI_FRBASEADD, frame_list_phys);
    io_outw(io_base + UHCI_FRNUM, 0);
    return true;
}

/* Enter Global Suspend with Resume Interrupt enabled — the event-wait
 * idle state. Per the spec, before setting EGSM the Run/Stop bit must
 * be cleared. In suspend the controller generates no SOF traffic but
 * remains sensitive to resume signalling (connect/disconnect/K-state),
 * which raises the Resume IRQ. */
static void uhci_enter_suspend(void)
{
    /* Stop the schedule. */
    uint16_t cmd = io_inw(io_base + UHCI_CMD);
    cmd &= ~UHCI_CMD_RS;
    io_outw(io_base + UHCI_CMD, cmd);

    /* Ack any latched status so a stale RD doesn't fire immediately. */
    io_outw(io_base + UHCI_STS, UHCI_STS_ALL);

    /* Enable ONLY the resume interrupt for the idle state. (IOC/short/
     * timeout get enabled later, around actual transfers.) */
    io_outw(io_base + UHCI_INTR, UHCI_INTR_RESUME);

    /* Enter global suspend. */
    cmd = io_inw(io_base + UHCI_CMD);
    cmd |= UHCI_CMD_EGSM;
    cmd &= ~UHCI_CMD_RS;
    io_outw(io_base + UHCI_CMD, cmd);
}

/* Leave Global Suspend. Spec sequence: drive Force Global Resume for
 * ~20 ms, then clear FGR and EGSM together, then re-run the schedule.
 * After this the controller is running again and ready for transfers
 * (used by the enumeration block). */
static void uhci_leave_suspend(void)
{
    uint16_t cmd = io_inw(io_base + UHCI_CMD);

    /* Fast path: not suspended (EGSM clear, schedule running) — do NOT
     * re-run the resume dance. Driving FGR on an ACTIVE bus injects
     * resume signalling into live traffic. */
    if (!(cmd & UHCI_CMD_EGSM) && (cmd & UHCI_CMD_RS))
        return;

    /* Force global resume on the bus. Spec floor is 20 ms; hold a bit
     * longer for old silicon. */
    cmd |= UHCI_CMD_FGR;
    io_outw(io_base + UHCI_CMD, cmd);
    busy_wait_us(25000);

    /* Drop FGR and EGSM in the same write, per spec, to terminate the
     * resume cleanly. */
    cmd = io_inw(io_base + UHCI_CMD);
    cmd &= ~(UHCI_CMD_FGR | UHCI_CMD_EGSM);
    io_outw(io_base + UHCI_CMD, cmd);
    busy_wait_us(10000);

    /* THE forgotten last step of the resume sequence: Run/Stop back to
     * 1. enter_suspend clears RS and nobody ever set it again — QEMU
     * executes TDs regardless (permissive), the PIIX4 does what the
     * spec says with RS=0: NOTHING. Field signature on the Armada:
     * boot enumeration fine (init_hw sets RS), then first click ->
     * every transfer times out (bring-up failed) and the driver is
     * buried under ~45 s of stacked bounded waits — the "catatonia". */
    cmd = io_inw(io_base + UHCI_CMD);
    cmd |= UHCI_CMD_RS;
    io_outw(io_base + UHCI_CMD, cmd);
    busy_wait_us(1000);

    /* Re-arm the FULL interrupt mask for the running state. enter_suspend
     * narrowed USBINTR to Resume-only; without this write the completion
     * IRQs (IOC/short) of a hot-plug enumeration stay MASKED and the FSM
     * advances only through the 100 ms timeout fallback — slow, and on
     * usbdiag it masquerades as a dead IRQ line (cnt_complete=0 with
     * cnt_noirq_adv>0 even though the line is fine). */
    io_outw(io_base + UHCI_INTR,
            UHCI_INTR_RESUME | UHCI_INTR_IOC |
            UHCI_INTR_SHORT  | UHCI_INTR_TIMEOUT);

    /* Resume the schedule. */
    cmd = io_inw(io_base + UHCI_CMD);
    cmd |= UHCI_CMD_RS | UHCI_CMD_MAXP;
    io_outw(io_base + UHCI_CMD, cmd);

    /* TRSMRCY: after resume signalling ends, the DEVICE is entitled to
     * 10 ms of recovery — with SOFs flowing — before it must accept
     * traffic. We were launching the first TD ~1 ms in: on QEMU
     * (timeless) it worked, on the PIIX4 the freshly-woken stick simply
     * didn't answer (no response -> 3 errors -> halt: "Op servite: 2,
     * ultimo opcode 3" with a perfectly enabled port). Wait the spec
     * floor with margin, schedule already running. */
    busy_wait_us(15000);
}

/* ===== Control-transfer engine ===== */

/* Build a single TD in slot `idx`.
 *   pid      : USB_PID_SETUP / IN / OUT
 *   toggle   : DATA0=0, DATA1=1
 *   addr     : USB device address (0 during first SET_ADDRESS)
 *   endp     : endpoint number (0 for control)
 *   len      : payload length in bytes (0..ENUM_BUF_SIZE)
 *   buf_phys : physical address of payload (0 if len==0)
 *   ioc      : set IOC so completion raises USBINT
 *   next_idx : slot of the next TD, or -1 to terminate the chain
 *
 * maxlen field encodes (len-1); a length of 0 is encoded as 0x7FF. */
static void build_td(int idx, uint8_t pid, int toggle, uint8_t addr,
                     uint8_t endp, uint32_t len, uint32_t buf_phys,
                     int ioc, int next_idx)
{
    volatile uhci_td_t *td = &ctrl_td[idx];

    if (next_idx >= 0)
        td->link = TD_PHYS(next_idx) | LP_DEPTH; /* vertical, TD */
    else
        td->link = LP_TERMINATE;

    uint32_t s = TD_STS_ACTIVE | TD_CTRL_CERR3;
    if (enum_low_speed) s |= TD_CTRL_LS;
    if (ioc)            s |= TD_CTRL_IOC;
    td->status = s;

    uint32_t maxlen = (len == 0) ? 0x7FFu : ((len - 1) & 0x7FFu);
    uint32_t tok = ((uint32_t)pid    << TD_TOK_PID_SHIFT)  |
                   ((uint32_t)addr   << TD_TOK_ADDR_SHIFT) |
                   ((uint32_t)endp   << TD_TOK_ENDP_SHIFT) |
                   ((uint32_t)(toggle ? 1u : 0u) << TD_TOK_DT_SHIFT) |
                   (maxlen           << TD_TOK_MAXLEN_SHIFT);
    td->token  = tok;
    td->buffer = buf_phys;
}

/* Link the prepared TD chain (starting at slot 0) into the control QH
 * and make sure the QH is reachable from frame 0. The controller, once
 * running, will pick it up at the next frame boundary. */
static void ctrl_submit(void)
{
    ctrl_qh->element   = TD_PHYS(0);              /* vertical -> first TD */
    ctrl_qh->head_link = LP_TERMINATE;            /* no following QH */

    /* Link the QH into EVERY frame-list entry. The controller executes
     * exactly frame_list[FRNUM & 0x3FF], one entry per millisecond; the
     * original code linked entry 0 only, so the QH was visited once per
     * 1024 ms. On real silicon the 100 ms transfer timeout always struck
     * first: "transfer timeout" with the TD still ACTIVE, CERR intact and
     * ActLen empty — never executed at all. QEMU masked this: its UHCI
     * caches a discovered queue and keeps servicing it across frames, so
     * a single visit was enough there. With LP_DEPTH already set on the
     * TD->TD links the whole chain completes in one visit, i.e. within
     * ~1 ms of submission. */
    for (int i = 0; i < UHCI_FRAME_COUNT; i++)
        frame_list[i] = ctrl_qh_phys | LP_QH;
}

/* Detach our QH from the schedule (after a transfer completes or aborts)
 * so a stale TD isn't re-executed. */
static void ctrl_unlink(void)
{
    for (int i = 0; i < UHCI_FRAME_COUNT; i++)
        frame_list[i] = LP_TERMINATE;
    ctrl_qh->element = LP_TERMINATE;
}

/* Arm the per-transfer timeout and return the generation it belongs to.
 * A completion handler bumps enum_timer_gen so a late timer is ignored. */
static void enum_arm_timeout(void)
{
    enum_timer_gen++;
    (void)timer_set(irq_port, ENUM_TIMEOUT_MS, 0);
}

/* Inspect the last TD's status after a completion IRQ. Returns:
 *   1  = success (not active, no error bits)
 *   0  = still active (spurious wake; keep waiting)
 *  -1  = error (stall/crc/babble/etc.) */
static int ctrl_check(int last_idx)
{
    uint32_t st = ctrl_td[last_idx].status;
    if (st & TD_STS_ACTIVE)  return 0;
    if (st & TD_STS_ERRMASK) return -1;
    return 1;
}

/* ===== Build the standard requests as TD chains ===== */

/* SET_ADDRESS(addr): SETUP(DATA0) + STATUS-IN(DATA1). No data stage. */
static void start_set_address(uint8_t addr)
{
    usb_setup_t *s = (usb_setup_t *)setup_buf;
    s->bmRequestType = USB_RT_DEV_OUT;
    s->bRequest      = USB_REQ_SET_ADDRESS;
    s->wValue        = addr;
    s->wIndex        = 0;
    s->wLength       = 0;

    /* device still answers on address 0 until this completes */
    build_td(0, USB_PID_SETUP, 0, 0, 0, sizeof(usb_setup_t),
             setup_buf_phys, 0, 1);
    build_td(1, USB_PID_IN,    1, 0, 0, 0, 0, 1, -1);  /* STATUS-IN, IOC */
    diag_cur_td_idx = 1;
    ctrl_submit();
}

/* GET_DESCRIPTOR: SETUP(DATA0) + DATA-IN(DATA1) + STATUS-OUT(DATA1).
 * type goes in the high byte of wValue, index in the low byte. */
static void start_get_descriptor(uint8_t addr, uint8_t type,
                                 uint8_t index, uint16_t len)
{
    usb_setup_t *s = (usb_setup_t *)setup_buf;
    s->bmRequestType = USB_RT_DEV_IN;
    s->bRequest      = USB_REQ_GET_DESCRIPTOR;
    s->wValue        = ((uint16_t)type << 8) | index;
    s->wIndex        = 0;
    s->wLength       = len;

    build_td(0, USB_PID_SETUP, 0, addr, 0, sizeof(usb_setup_t),
             setup_buf_phys, 0, 1);
    build_td(1, USB_PID_IN,    1, addr, 0, len, data_buf_phys, 0, 2);
    build_td(2, USB_PID_OUT,   1, addr, 0, 0, 0, 1, -1); /* STATUS-OUT, IOC */
    diag_cur_td_idx = 2;
    ctrl_submit();
}

/* ===== FSM helpers ===== */

static void enum_fail(const char *why)
{
    char line[96];
    sprintf(line, "[uhci] enum error: %s\n", why);
    debug_print(line);
    strncpy(diag_last_error, why, sizeof(diag_last_error) - 1);
    diag_last_error[sizeof(diag_last_error) - 1] = '\0';

    /* Photograph the scene BEFORE enter_suspend acks USBSTS and the
     * controller state is torn down — this is the only moment HSE/HCPE
     * and the raw TD write-back are still readable. */
    diag_fail_usbsts = io_inw(io_base + UHCI_STS);
    diag_fail_frnum  = io_inw(io_base + UHCI_FRNUM);
    if (ctrl_td && diag_cur_td_idx >= 0 && diag_cur_td_idx < ENUM_MAX_TD)
        diag_fail_td_status = ctrl_td[diag_cur_td_idx].status;
    if (enum_port >= 0 && enum_port < (int)num_ports)
        diag_fail_portsc = io_inw(io_base + portsc_reg[enum_port]);

    ctrl_unlink();
    enum_state = ST_ENUM_ERROR;
    enum_port  = -1;
    /* back to the low-power event-wait state */
    uhci_enter_suspend();
    enum_state = ST_IDLE_SUSPENDED;
}

/* Begin enumerating the device on `port`: drive a reset pulse, then a
 * timer (handled in ST_PORT_RESETTING) gives the device recovery time
 * before SET_ADDRESS. */
static void enum_begin(int port)
{
    if (!ensure_frame_list())
    {
        enum_state = ST_IDLE_SUSPENDED;
        return;
    }

    uint16_t reg = portsc_reg[port];

    enum_port = port;

    /* Assert reset (preserve non-change control bits). */
    uint16_t v = io_inw(io_base + reg) & UHCI_PORT_WRMASK;
    io_outw(io_base + reg, v | UHCI_PORT_RESET);

    enum_state = ST_PORT_RESETTING;
    enum_arm_timeout();  /* reuse timeout timer as the reset dwell timer */

    char line[64];
    sprintf(line, "[uhci] enumerating port %d (reset asserted)\n", port + 1);
    debug_print(line);
}

/* After the reset dwell: de-assert reset, enable the port, capture
 * low-speed, then issue SET_ADDRESS. */
static void enum_after_reset(void)
{
    uint16_t reg = portsc_reg[enum_port];

    /* De-assert reset. */
    uint16_t v = io_inw(io_base + reg) & UHCI_PORT_WRMASK;
    io_outw(io_base + reg, v & ~UHCI_PORT_RESET);

    /* Brief settle, then enable. These are micro-waits INSIDE an FSM
     * transition (we were entered from a timer event, not blocking on
     * the port): use busy_wait_us so we do NOT consume events from
     * irq_port — that would race with the FSM's own timers/IRQs. */
    busy_wait_us(2000);

    /* Enable the port — with retries. On real UHCI silicon (PIIX4
     * included) the first PE write after reset is routinely ignored
     * while the port still has CSC/PEC latched or the device is still
     * settling; QEMU accepts it first try, metal often does not. The
     * canonical sequence (same as Linux uhci-hub) is: write PE, read
     * back, and if it did not stick ack the change bits and try again. */
    int enabled = 0;
    for (int attempt = 0; attempt < 5 && !enabled; attempt++)
    {
        v = io_inw(io_base + reg) & UHCI_PORT_WRMASK;
        io_outw(io_base + reg, v | UHCI_PORT_PE);
        busy_wait_us(2000);
        v = io_inw(io_base + reg);
        if (v & UHCI_PORT_PE)
            enabled = 1;
        else
            io_outw(io_base + reg, (v & UHCI_PORT_WRMASK) | UHCI_PORT_CHANGE); /* ack, retry */
    }

    /* Confirm a device is actually present & enabled. */
    v = io_inw(io_base + reg);
    if (!(v & UHCI_PORT_CCS))
    {
        enum_fail("device vanished after reset");
        return;
    }
    if (!enabled)
    {
        enum_fail("port refused enable after reset");
        return;
    }
    enum_low_speed = (v & UHCI_PORT_LSDA) ? 1 : 0;

    /* ACK any change bits the reset raised. */
    io_outw(io_base + reg,
             (io_inw(io_base + reg) & UHCI_PORT_WRMASK) | UHCI_PORT_CHANGE);

    /* Reset recovery (USB 2.0 §7.1.7.3, TRSTRCY): the device may ignore
     * bus traffic for up to 10 ms after reset. QEMU devices answer
     * immediately; real flash drives frequently NAK or ignore the first
     * SETUP if it arrives inside the recovery window, which then reads
     * as a SET_ADDRESS timeout. Wait it out. */
    busy_wait_us(10000);

    start_set_address(USB_ENUM_ADDRESS);
    enum_state = ST_SETADDR_SENT;
    enum_arm_timeout();
}

/* A completion IRQ arrived while in an *_SENT state. Validate the TD
 * chain and advance. last_idx is the index of the IOC/status TD. */
static void enum_on_complete(void)
{
    int last_idx;
    switch (enum_state)
    {
        case ST_SETADDR_SENT:  last_idx = 1; break;
        case ST_DEVDESC_SENT:  last_idx = 2; break;
        case ST_CONFDESC_SENT: last_idx = 2; break;
        default: return;  /* not waiting on a transfer */
    }

    int r = ctrl_check(last_idx);
    if (r == 0) return;            /* spurious: still active, keep waiting */
    if (r < 0) { enum_fail("transfer stalled/errored"); return; }

    /* Success: cancel the pending timeout and advance. */
    enum_timer_gen++;
    ctrl_unlink();

    if (enum_state == ST_SETADDR_SENT)
    {
        debug_print("[uhci] SET_ADDRESS ok; reading device descriptor\n");
        /* SET_ADDRESS recovery (USB 2.0 §9.2.6.3): the device has 2 ms
         * to start answering on the new address. Real devices use it. */
        busy_wait_us(2000);
        start_get_descriptor(USB_ENUM_ADDRESS, USB_DT_DEVICE, 0,
                             sizeof(usb_device_descriptor_t));
        enum_state = ST_DEVDESC_SENT;
        enum_arm_timeout();
        return;
    }

    if (enum_state == ST_DEVDESC_SENT)
    {
        memcpy(&enum_dev, (const void *)data_buf,
               sizeof(usb_device_descriptor_t));
        /* Request config descriptor + first interface. We ask for a
         * chunk large enough to include the interface descriptor that
         * follows the 9-byte config header. */
        start_get_descriptor(USB_ENUM_ADDRESS, USB_DT_CONFIG, 0,
                             sizeof(usb_config_descriptor_t) +
                             sizeof(usb_interface_descriptor_t));
        enum_state = ST_CONFDESC_SENT;
        enum_arm_timeout();
        return;
    }

    if (enum_state == ST_CONFDESC_SENT)
    {
        /* Parse: config header first, then the interface descriptor.
         * We walk descriptors until we hit the first INTERFACE. */
        const uint8_t *p   = (const uint8_t *)data_buf;
        const uint8_t *end = p + ENUM_BUF_SIZE;
        int found_iface = 0;

        /* skip the config descriptor header (p[0] = bLength) */
        if (p[0] >= sizeof(usb_config_descriptor_t))
        {
            const uint8_t *q = p + p[0];
            while (q + 2 <= end && q[0] >= 2)
            {
                if (q[1] == USB_DT_INTERFACE &&
                    q[0] >= sizeof(usb_interface_descriptor_t))
                {
                    memcpy(&enum_iface, q,
                           sizeof(usb_interface_descriptor_t));
                    found_iface = 1;
                    break;
                }
                q += q[0];
            }
        }

        if (!found_iface)
        {
            enum_fail("no interface descriptor in config");
            return;
        }

        enum_state = ST_ENUM_DONE;

        /* BLOCK 2: report. Log the class triple + IDs so we can verify on
         * QEMU that enumeration reads correct descriptors. */
        char line[160];
        sprintf(line,
            "[uhci] ENUM DONE port %d: VID=%04x PID=%04x "
            "devClass=%02x | ifClass=%02x sub=%02x proto=%02x\n",
            enum_port + 1,
            (unsigned)enum_dev.idVendor, (unsigned)enum_dev.idProduct,
            (unsigned)enum_dev.bDeviceClass,
            (unsigned)enum_iface.bInterfaceClass,
            (unsigned)enum_iface.bInterfaceSubClass,
            (unsigned)enum_iface.bInterfaceProtocol);
        debug_print(line);

        /* BLOCK 3: DAS match + hotplug announce.
         *
         * Match the enumerated device's USB class triple against the
         * device-level DAS files in /SYSTEM/CONFIG/DAS/USB/ (shared matcher,
         * also used by the EHCI/xHCI drivers). On a match, emit
         * HOTPLUG_SUBDEVICE_APPEARED carrying the PCI-style class/subclass
         * the DAS specifies; hotplug then DAS-matches THAT (a
         * bus_type=subdevice entry) to draw the desktop icon and define the
         * double-click action — the same SUBDEVICE pipeline AHCI uses for
         * optical media and FAT32 partitions. No icon/action machinery here.
         *
         * provider_token carries the port number so a later REMOVAL (and the
         * activation IPC) can identify which device this was. */
        usb_das_device_t ud;
        ud.dev_class    = enum_dev.bDeviceClass;
        ud.dev_subclass = enum_dev.bDeviceSubClass;
        ud.dev_protocol = enum_dev.bDeviceProtocol;
        ud.if_class     = enum_iface.bInterfaceClass;
        ud.if_subclass  = enum_iface.bInterfaceSubClass;
        ud.if_protocol  = enum_iface.bInterfaceProtocol;
        ud.vid          = enum_dev.idVendor;    /* per DAS vid-specifici  */
        ud.pid          = enum_dev.idProduct;   /* (es. card reader CQ62) */

        diag_enum_done = 1;
        diag_last_error[0] = '\0';   /* a success clears the latch */

        usb_das_result_t res;
        if (usb_das_match(&ud, &res))
        {
            diag_das_matched = 1;
            strncpy(diag_das_label, res.label, sizeof(diag_das_label) - 1);
            diag_das_label[sizeof(diag_das_label) - 1] = '\0';
            char m[120];
            sprintf(m, "[uhci] DAS match: %s -> subdev class=%02x:%02x\n",
                    res.label[0] ? res.label : "(unnamed)",
                    (unsigned)res.subdev_class,
                    (unsigned)res.subdev_subclass);
            debug_print(m);

            uint32_t hp = dob_registry_find("hotplug");
            if (!hp)
                strncpy(diag_last_error, "hotplug service not found",
                        sizeof(diag_last_error) - 1);
            if (hp)
            {
                dob_msg_t am, ar;
                memset(&am, 0, sizeof(am));
                memset(&ar, 0, sizeof(ar));

                hotplug_subdev_appeared_t req;
                memset(&req, 0, sizeof(req));
                /* Fresh per-connection token. Stored so the later GONE and
                 * the usbms nudge retract the SAME identity we announced
                 * (the epoch is bumped only after device_gone, so it is
                 * stable across this connection's lifetime). */
                active_token = uhci_make_token(enum_port);
                req.sub.provider_token = active_token;
                dev_announced = 1;
                req.sub.class_code     = res.subdev_class;
                req.sub.subclass       = res.subdev_subclass;
                strncpy(req.sub.provider_service, uhci_service_name,
                        sizeof(req.sub.provider_service) - 1);

                am.code         = HOTPLUG_SUBDEVICE_APPEARED;
                am.payload      = &req;
                am.payload_size = sizeof(req);
                if (dob_ipc_call(hp, &am, &ar) == 0)
                    diag_announce_ok = 1;
                else
                    strncpy(diag_last_error, "announce IPC to hotplug failed",
                            sizeof(diag_last_error) - 1);
            }
        }
        else
        {
            debug_print("[uhci] no USB-DAS matched this device\n");
        }

        /* Device stays attached: park the FSM in DEVICE_READY with the
         * controller RUNNING, so sub-drivers can submit transfers at
         * once. Suspend would stop the schedule (no transfers possible)
         * and each wake costs ~50 ms of FGR/EGSM sequencing per op.
         * Disconnect is covered by the 1 Hz port watch armed here. */
        uhci_device_ready();
        return;
    }
}

/* ===== DEVICE_READY: running policy, port watch, sub-driver transport ===== */

/* Enter the DEVICE_READY parked state after a successful enumeration.
 * Controller keeps running (RS=1 already set by the enum path); arm the
 * 1 Hz one-shot port watch — re-armed on every tick while in this
 * state, so leaving the state stops the watch naturally. */
static void uhci_device_ready(void)
{
    ready_port     = enum_port;
    enum_port      = -1;
    toggle_in_map  = 0;
    toggle_out_map = 0;
    /* Park ENUMERATED in global suspend: the icon is on the desktop,
     * nobody is transferring yet, and in EGSM the Resume-Detect IRQ
     * fires on disconnect — removal is a pure event. The first
     * sub-driver op wakes us. */
    uhci_enter_suspend();
    enum_state = ST_DEVICE_IDLE;
    debug_print("[uhci] device ready (idle-suspended, RD armed)\n");
    uhci_process_deferred_rd();
}

/* Wake from DEVICE_IDLE to serve transfers. */
static void uhci_device_wake(void)
{
    uhci_leave_suspend();
    /* Port health before serving transfers: the suspend/resume cycle on
     * real silicon can auto-disable the port (see uhci_port_reenable).
     * Heal it here so the very first sub-driver transfer finds a live
     * wire instead of timing out against a dead port. */
    if (ready_port >= 0)
        uhci_port_reenable(ready_port);
    enum_state    = ST_DEVICE_READY;
    xfer_activity = 1;
    timer_set(irq_port, DEVICE_IDLE_MS, 0);
}

/* Idle one-shot fired in DEVICE_READY: demote to suspended idle if no
 * transfer happened since the last tick, else re-arm. Not a poll: it
 * runs only while ACTIVE, and only to decide when activity ended. */
static void uhci_idle_tick(void)
{
    if (xfer_activity)
    {
        xfer_activity = 0;
        timer_set(irq_port, DEVICE_IDLE_MS, 0);
        return;
    }

    /* Before parking in Global Suspend, confirm the device is still present.
     * An unplug DURING the awake window (between the last transfer and this
     * tick) is silent on UHCI: a running controller raises no IRQ on a port
     * change, it only latches CSC. Suspending blindly would arm Resume-Detect
     * on a device that is already gone — and RD never fires, because the change
     * already happened — so the desktop icon would linger forever and a click
     * would serve stale cached directory data. This is a single status read at
     * the suspend transition (NOT a poll); it closes the one hole the
     * event-driven suspend+RD model leaves open. An unplug that happens once we
     * ARE suspended is still caught by RD, as before. */
    if (ready_port >= 0)
    {
        uint16_t v = io_inw(io_base + portsc_reg[ready_port]);
        if (!(v & UHCI_PORT_CCS))
        {
            debug_print("[uhci] idle: device left during awake window — tearing down\n");
            uhci_device_gone();
            return;
        }
    }

    uhci_enter_suspend();
    enum_state = ST_DEVICE_IDLE;
    debug_print("[uhci] idle: back to suspend (RD armed)\n");
}

/* A transfer failed: was it the stick leaving? Pure event-driven
 * disconnect detection while ACTIVE. */
static void uhci_check_disconnect_after_error(void)
{
    if (ready_port < 0) return;
    uint16_t v = io_inw(io_base + portsc_reg[ready_port]);
    if (!(v & UHCI_PORT_CCS) || (v & UHCI_PORT_CSC))
    {
        /* We are INSIDE handle_request: the client (usbms) is waiting
         * for our reply, and above it the dobfs/hotplug chain may be
         * waiting on the client. Tearing down now — with its IPC to
         * hotplug and to usbms — from this stack frame is how cycles
         * are born. Defer: flag it, reply the error to the client, and
         * let the event loop run the teardown via a 10 ms one-shot. */
        pending_gone = 1;
        timer_set(irq_port, 10, 0);
    }
}

/* The attached device disappeared (port watch saw CSC/!CCS). Retract
 * the subdevice from hotplug, nudge the mass-storage sub-driver (if it
 * spawned) so it can retract its volumes and exit, then return to the
 * pure event-driven suspended idle. */
static void uhci_device_gone(void)
{
    /* Never strand a parked caller: if an op was parked for a wake that
     * ended in a disconnect, fail it now (DOB_ERR_DEAD) — a stranded
     * sync caller is the seed of every freeze we have ever chased. */
    if (pending_valid)
    {
        pending_valid = 0;
        dob_msg_t r; memset(&r, 0, sizeof(r));
        r.code = DOB_ERR_DEAD;
        dob_ipc_reply(pending_req.sender_tid, &r);
    }
    char m[96];
    sprintf(m, "[uhci] device on port %d disconnected\n", ready_port);
    debug_print(m);

    /* Ack the port change bits. */
    uint16_t reg = portsc_reg[ready_port];
    io_outw(io_base + reg,
            (io_inw(io_base + reg) & UHCI_PORT_WRMASK) | UHCI_PORT_CHANGE);
    port_present[ready_port] = 0;

    /* Retract the subdevice (same token as the APPEARED).
     *
     * POSTED, never called. A synchronous call here can close a deadlock
     * cycle: hotplug's action engine may be blocked (transitively) on a
     * dobfs -> usbms -> usb_uhci chain — i.e. on US — while we'd be
     * blocking on hotplug. Field signature: usbdiag frozen at FASE 1/3
     * ("hotplug bloccato") after the first volume mount attempt.
     * Providers must never call upward while anyone below can be
     * waiting on them; fire-and-forget breaks the cycle. */
    /* Retract the EXACT identity we announced (active_token), not a
     * freshly-recomputed (instance<<4)|port — those now differ across
     * connections. Gate on dev_announced, NOT on active_token: token 0 is
     * a real device (the first plug on instance 0 / port 0), and gating on
     * the value would skip its GONE — leaving the icon on the desktop. */
    uint32_t hp = dev_announced ? dob_registry_find("hotplug") : 0;
    if (hp)
    {
        static hotplug_subdev_gone_t gone;
        memset(&gone, 0, sizeof(gone));
        gone.provider_token = active_token;
        strncpy(gone.provider_service, uhci_service_name,
                sizeof(gone.provider_service) - 1);
        dob_msg_t gm;
        memset(&gm, 0, sizeof(gm));
        gm.code = HOTPLUG_SUBDEVICE_GONE;
        gm.payload = &gone; gm.payload_size = sizeof(gone);
        dob_ipc_post(hp, &gm);
    }

    /* Tell the sub-driver, if one is serving this device. Posted (not
     * called): if it is mid-transfer towards us a synchronous call
     * would deadlock. */
    if (dev_announced)
    {
        char svc[24];
        sprintf(svc, "usbms_%u", (unsigned)active_token);
        uint32_t sp = dob_registry_find(svc);
        if (sp)
        {
            dob_msg_t dm; memset(&dm, 0, sizeof(dm));
            dm.code = HOTPLUG_DETACH;
            dob_ipc_post(sp, &dm);
        }
    }

    diag_enum_done = 0; diag_das_matched = 0; diag_announce_ok = 0;
    /* This connection is over. Bump the epoch so the NEXT enumeration on
     * this (or any) port announces a token distinct from the one above —
     * the whole point of the fix. Clear active_token / dev_announced:
     * nothing is live. */
    conn_epoch    = (conn_epoch + 1) & 0x7FFFu;
    active_token  = 0;
    dev_announced = 0;
    ready_port = -1;
    uhci_enter_suspend();
    enum_state = ST_IDLE_SUSPENDED;
    uhci_process_deferred_rd();
}

/* Lazily allocate the transport TD pool + bounce buffer. */
static bool xfer_pool_init(void)
{
    if (!ensure_frame_list()) return false;

    if (xfer_td) return true;
    uint32_t phys = 0;
    void *mem = dma_alloc(XFER_MAX_TD * sizeof(uhci_td_t), &phys);
    if (!mem) return false;
    xfer_td = (volatile uhci_td_t *)mem; xfer_td_phys = phys;

    /* The bounce is shared with sub-drivers via GET_WINDOW+mmap_phys,
     * and mmap_phys maps whole PAGES: a non-page-aligned phys would
     * hand the sub-driver a skewed pointer — its CBWs would reach the
     * wire corrupted and every BOT exchange would die mysteriously.
     * Over-allocate one page and align up; deterministic by design. */
    mem = dma_alloc(UHCI_XFER_MAX + 4096, &phys);
    if (!mem) return false;
    {
        uint32_t skew = (4096u - (phys & 4095u)) & 4095u;
        xfer_buf      = (uint8_t *)mem + skew;
        xfer_buf_phys = phys + skew;
    }
    return true;
}

/* Build one TD in the transport pool. next<0 terminates the chain. */
static void xfer_build_td(int idx, uint8_t pid, int toggle, uint8_t endp,
                          uint32_t len, uint32_t buf_phys, int spd, int next)
{
    volatile uhci_td_t *td = &xfer_td[idx];
    td->link = (next >= 0) ? (XFER_TD_PHYS(next) | LP_DEPTH) : LP_TERMINATE;
    uint32_t st = TD_STS_ACTIVE | TD_CTRL_CERR3;
    if (enum_low_speed) st |= TD_CTRL_LS;
    if (spd)            st |= TD_CTRL_SPD;
    td->status = st;
    uint32_t maxlen = (len == 0) ? 0x7FFu : ((len - 1) & 0x7FFu);
    td->token = ((uint32_t)pid << TD_TOK_PID_SHIFT) |
                (1u            << TD_TOK_ADDR_SHIFT) |   /* addr 1 */
                ((uint32_t)endp << TD_TOK_ENDP_SHIFT) |
                ((uint32_t)(toggle ? 1u : 0u) << TD_TOK_DT_SHIFT) |
                (maxlen << TD_TOK_MAXLEN_SHIFT);
    td->buffer = buf_phys;
}

/* Run TDs [0..last] through the schedule and poll completion.
 * Returns: 1 done, 0 timeout, -1 device error (STALL & friends).
 * On short IN packets the chain stops early (SPD) — treated as done. */
static int xfer_run(int last_idx, uint32_t timeout_ms)
{
    /* We POLL completion: the IOC/short-packet IRQs would only flood
     * our own port with useless messages (field: 1374 IRQs for 3 real
     * completions on QEMU). Mask the controller's interrupt sources
     * for the duration and restore after — the level line stays quiet
     * and the event loop's queue stays clean. */
    uint16_t saved_intr = io_inw(io_base + UHCI_INTR);
    io_outw(io_base + UHCI_INTR, 0);

    ctrl_qh->element   = XFER_TD_PHYS(0);
    ctrl_qh->head_link = LP_TERMINATE;
    for (int i = 0; i < UHCI_FRAME_COUNT; i++)
        frame_list[i] = ctrl_qh_phys | LP_QH;

    int rc = 0;
    for (uint32_t waited = 0; waited < timeout_ms; waited++)
    {
        /* Chain done when the QH element advanced past the last TD
         * (terminate) or stopped on an inactive TD with error bits. */
        uint32_t el = ctrl_qh->element;
        if (el & LP_TERMINATE) { rc = 1; break; }
        /* find the TD the QH points at; if it is inactive with errors
         * the chain is dead; if inactive without errors and not
         * advancing it is the SPD stop (short packet) — done. */
        uint32_t idx = (el - xfer_td_phys) / sizeof(uhci_td_t);
        if (idx < (uint32_t)XFER_MAX_TD)
        {
            uint32_t st = xfer_td[idx].status;
            if (!(st & TD_STS_ACTIVE))
            {
                rc = (st & TD_STS_ERRMASK) ? -1 : 1;
                break;
            }
        }
        busy_wait_us(1000);
    }
    /* Timed out with the chain still ACTIVE: distinguish a device that is
     * NAKing — flow-control "busy", e.g. a flash stick committing a previous
     * write before it will accept the next command — from a genuinely dead
     * transfer. A NAK is not an error (CERR is not decremented), so the TD
     * sits ACTIVE with TD_STS_NAK and no error bits. Report BUSY (2) so the
     * caller WAITS and retries instead of resetting, which would abort the
     * in-flight commit and wedge the device into permanent NAK. */
    if (rc == 0)
    {
        uint32_t el  = ctrl_qh->element;
        uint32_t idx = (el - xfer_td_phys) / sizeof(uhci_td_t);
        if (idx < (uint32_t)XFER_MAX_TD)
        {
            uint32_t st = xfer_td[idx].status;
            if ((st & TD_STS_ACTIVE) && (st & TD_STS_NAK) && !(st & TD_STS_ERRMASK))
                rc = 2;
        }
    }
    (void)last_idx;
    /* Unlink */
    for (int i = 0; i < UHCI_FRAME_COUNT; i++)
        frame_list[i] = LP_TERMINATE;
    ctrl_qh->element = LP_TERMINATE;
    /* Drain the status bits our chain raised, then restore the
     * interrupt mask for the event-driven paths. */
    io_outw(io_base + UHCI_STS, UHCI_STS_USBINT | UHCI_STS_ERROR);
    io_outw(io_base + UHCI_INTR, saved_intr);
    return rc;
}

/* Generic control transfer: setup[8] (+ data in xfer_buf for OUT).
 * IN data lands in xfer_buf. Returns actual IN length, or <0. */
static int uhci_ctrl_xfer(const uint8_t setup[8])
{
    if (!xfer_pool_init()) return -1;
    uint16_t wlen = (uint16_t)setup[6] | ((uint16_t)setup[7] << 8);
    int dir_in = (setup[0] & 0x80) ? 1 : 0;
    if (wlen > 1024) return -1;            /* control payloads are small */

    /* SETUP packet goes through the bounce too (DMA needs phys). */
    uint8_t *sb = xfer_buf + UHCI_XFER_MAX - 16;
    uint32_t sb_phys = xfer_buf_phys + UHCI_XFER_MAX - 16;
    memcpy(sb, setup, 8);

    /* Packetize the data stage by the ENUMERATED EP0 max packet size.
     * Hardcoding 64 worked on QEMU (its sticks use 64) and broke on
     * any device with mps0 of 8/16/32 (legal at full speed): the
     * device's first short packet halted the chain at mps0 bytes, the
     * config-descriptor read returned <9 bytes, and the bring-up died
     * at "no bulk endpoint pair found" — the Armada/Verbatim stage. */
    uint32_t mps0 = enum_dev.bMaxPacketSize0;
    if (mps0 < 8 || mps0 > 64) mps0 = 8;   /* safe floor pre-descriptor */

    int n = 0;
    xfer_build_td(n, USB_PID_SETUP, 0, 0, 8, sb_phys, 0, n + 1); n++;
    uint32_t off = 0; int tog = 1;
    while (off < wlen)
    {
        uint32_t chunk = wlen - off; if (chunk > mps0) chunk = mps0;
        xfer_build_td(n, dir_in ? USB_PID_IN : USB_PID_OUT, tog, 0,
                      chunk, xfer_buf_phys + off, dir_in, n + 1);
        n++; tog ^= 1; off += chunk;
    }
    /* Status stage: opposite direction, DATA1 always. */
    xfer_build_td(n, dir_in ? USB_PID_OUT : USB_PID_IN, 1, 0, 0,
                  0, 0, -1);
    int last = n;
    int rc = xfer_run(last, 1000);
    if (rc <= 0) return -1;

    /* Short IN data phase: SPD halts the queue BEFORE the status TD —
     * the transfer would be left without its handshake (devices may
     * tolerate it once, then STALL everything after; real silicon is
     * strict). If the status TD never ran, run it alone now. */
    if (xfer_td[last].status & TD_STS_ACTIVE)
    {
        ctrl_qh->element   = XFER_TD_PHYS(last);
        ctrl_qh->head_link = LP_TERMINATE;
        uint16_t saved_intr = io_inw(io_base + UHCI_INTR);
        io_outw(io_base + UHCI_INTR, 0);
        for (int i = 0; i < UHCI_FRAME_COUNT; i++)
            frame_list[i] = ctrl_qh_phys | LP_QH;
        for (uint32_t w = 0; w < 500; w++)
        {
            if (!(xfer_td[last].status & TD_STS_ACTIVE)) break;
            busy_wait_us(1000);
        }
        for (int i = 0; i < UHCI_FRAME_COUNT; i++)
            frame_list[i] = LP_TERMINATE;
        ctrl_qh->element = LP_TERMINATE;
        io_outw(io_base + UHCI_STS, UHCI_STS_USBINT | UHCI_STS_ERROR);
        io_outw(io_base + UHCI_INTR, saved_intr);
        if (xfer_td[last].status & (TD_STS_ACTIVE | TD_STS_ERRMASK))
            return -1;
    }

    if (!dir_in) return 0;
    /* Actual IN length: sum the data TDs actlen. */
    int got = 0;
    for (int i = 1; i < last; i++)
    {
        uint32_t al = xfer_td[i].status & TD_ACTLEN_MASK;
        if (al != 0x7FFu) got += (int)(al + 1);
        if (xfer_td[i].status & TD_STS_ACTIVE) break; /* SPD stop */
    }
    return got;
}

/* Generic bulk transfer on endpoint ep (data in/out via xfer_buf).
 * Returns actual length, or <0 on device error / timeout.
 *
 * A bulk transfer is ceil(len/mps) packets, one TD each. mps is the
 * endpoint's max packet size and is LEGALLY 8/16/32/64 at full speed —
 * not always 64. At mps=8 a 4 KB data phase is 512 TDs, far past the
 * XFER_MAX_TD-entry DMA pool: the old single-shot build ran straight off
 * the end of xfer_td[] and corrupted whatever DMA structure followed
 * (the bounce buffer, the QH, the frame list). QEMU never tripped it —
 * its sticks use mps=64 — but real full-speed silicon with a small-mps
 * bulk endpoint corrupts memory on the first multi-sector transfer.
 *
 * Fix: schedule the transfer in runs of at most (XFER_MAX_TD-2) TDs.
 * The data toggle is persistent per endpoint (toggle_*_map), so it
 * carries across runs exactly as the wire requires. At mps=64 the whole
 * UHCI_XFER_MAX fits in one run — no behaviour change for the common
 * case; only small-mps devices split. */
static int uhci_bulk_xfer(uint8_t ep, int dir_in, uint32_t len,
                          uint32_t mps)
{
    if (!xfer_pool_init()) return -1;
    if (len == 0 || len > UHCI_XFER_MAX) return -1;
    if (mps < 8 || mps > 64) mps = 64;     /* full-speed bulk ceiling */
    uint16_t *map = dir_in ? &toggle_in_map : &toggle_out_map;

    /* Bytes we can cover with one scheduled run without overrunning the
     * TD pool. (XFER_MAX_TD-2) leaves a small margin; at mps=64 this is
     * >= UHCI_XFER_MAX so a single run always suffices. */
    const uint32_t max_run_bytes = ((uint32_t)XFER_MAX_TD - 2u) * mps;

    uint32_t total_got = 0;
    uint32_t off       = 0;
    while (off < len)
    {
        uint32_t run_len = len - off;
        if (run_len > max_run_bytes) run_len = max_run_bytes;

        int tog = (*map >> ep) & 1;
        int n = 0; uint32_t r = 0;
        while (r < run_len)
        {
            if (n >= XFER_MAX_TD) return -1;   /* backstop: never overrun */
            uint32_t chunk = run_len - r; if (chunk > mps) chunk = mps;
            xfer_build_td(n, dir_in ? USB_PID_IN : USB_PID_OUT, tog, ep,
                          chunk, xfer_buf_phys + off + r,
                          dir_in, (r + chunk < run_len) ? n + 1 : -1);
            n++; tog ^= 1; r += chunk;
        }

        int rc = xfer_run(n - 1, 3000);
        if (rc == 2) return -4;   /* NAK busy: caller waits/resets, no partial trusted */
        if (rc < 0)  return -2;   /* stall & friends: caller may CLEAR_HALT */
        if (rc == 0) return -1;   /* timeout */

        uint32_t got = 0; int executed = 0;
        for (int i = 0; i < n; i++)
        {
            uint32_t st = xfer_td[i].status;
            if (st & TD_STS_ACTIVE) break;     /* SPD stop: rest never ran */
            executed++;
            uint32_t al = st & TD_ACTLEN_MASK;
            if (al != 0x7FFu) got += (al + 1u);
        }
        /* Toggle advances once per EXECUTED packet, persisted for the EP. */
        int newtog = ((*map >> ep) & 1) ^ (executed & 1);
        *map = (uint16_t)((*map & ~(1u << ep)) | ((uint32_t)newtog << ep));

        total_got += got;
        off       += run_len;

        /* A short IN packet ends the transfer (device delivered all it
         * had); stop here rather than scheduling the remaining runs. */
        if (dir_in && got < run_len) break;
    }

    return dir_in ? (int)total_got : (int)len;
}

/* A timer fired. Its meaning depends on the state. We ignore timers
 * whose generation is stale (the matching transfer already completed
 * and bumped the generation). */
static void enum_on_timer(void)
{
    switch (enum_state)
    {
        case ST_WAKING_FGR: {
            if (!dwell_elapsed()) return;   /* stale timer absorbed */
            /* FGR held >= 25 ms: terminate resume, run the schedule. */
            uint16_t c = io_inw(io_base + UHCI_CMD);
            c &= ~(UHCI_CMD_FGR | UHCI_CMD_EGSM);
            io_outw(io_base + UHCI_CMD, c);
            io_outw(io_base + UHCI_INTR,
                    UHCI_INTR_RESUME | UHCI_INTR_IOC |
                    UHCI_INTR_SHORT  | UHCI_INTR_TIMEOUT);
            c = io_inw(io_base + UHCI_CMD);
            io_outw(io_base + UHCI_CMD, c | UHCI_CMD_RS | UHCI_CMD_MAXP);
            timed_state(ST_WAKING_SETTLE, 15);  /* TRSMRCY, SOFs flowing */
            return;
        }

        case ST_WAKING_SETTLE: {
            if (!dwell_elapsed()) return;

            if (wake_reason_rd)
            {
                /* Woken to INSPECT after an RD in idle: decide gone /
                 * bounce / spurious, all event-driven. */
                wake_reason_rd = 0;
                uint16_t v = io_inw(io_base + portsc_reg[ready_port]);
                if (!(v & UHCI_PORT_CCS))
                {
                    uhci_device_gone();
                    return;
                }
                if (v & UHCI_PORT_CSC)
                {
                    /* CSC with device present: ack and verify after a
                     * timed 50 ms — removal bounces too. */
                    io_outw(io_base + portsc_reg[ready_port],
                            (v & UHCI_PORT_WRMASK) | UHCI_PORT_CHANGE);
                    timed_state(ST_DISCONNECT_CHECK, 50);
                    return;
                }
                /* Spurious wiggle: ack every port, back to armed idle. */
                for (uint32_t p = 0; p < num_ports; p++)
                    io_outw(io_base + portsc_reg[p],
                            (io_inw(io_base + portsc_reg[p])
                                 & UHCI_PORT_WRMASK) | UHCI_PORT_CHANGE);
                if (ready_port >= 0) uhci_port_reenable(ready_port);
                uhci_enter_suspend();
                enum_state = ST_DEVICE_IDLE;
                return;
            }

            /* Woken to SERVE: heal the port if the chip auto-disabled
             * it, serve the parked request, arm the idle demotion. */
            if (ready_port >= 0)
                uhci_port_reenable(ready_port);
            enum_state    = ST_DEVICE_READY;
            xfer_activity = 1;
            timer_set(irq_port, DEVICE_IDLE_MS, 0);
            if (pending_valid)
            {
                pending_valid = 0;
                dob_msg_t r; memset(&r, 0, sizeof(r));
                r.code = handle_request(&pending_req, &r);
                dob_ipc_reply(pending_req.sender_tid, &r);
            }
            /* An RD that landed during the waking dwell (e.g. the stick
             * being ripped mid-wake) was deferred; pick it up NOW
             * instead of at some later stabilization point. */
            uhci_process_deferred_rd();
            return;
        }

        case ST_DISCONNECT_CHECK: { /* disconnect re-check (50 ms) */
            if (!dwell_elapsed()) return;
            uint16_t v = io_inw(io_base + portsc_reg[ready_port]);
            if (!(v & UHCI_PORT_CCS))
            {
                uhci_device_gone();
                return;
            }
            debug_print("[uhci] spurious CSC with device present; kept\n");
            uhci_port_reenable(ready_port);
            uhci_enter_suspend();
            enum_state = ST_DEVICE_IDLE;
            return;
        }

        case ST_CONNECT_DEBOUNCE: {
            if (!dwell_elapsed()) return;
            /* 100 ms after the first connect edge: still there? Real
             * contacts bounce; one physical insertion can flicker CCS
             * for tens of ms and a naive driver burns enumerations on
             * ghosts (field: repeated plug cycles left a zombie). */
            uint16_t v = io_inw(io_base + portsc_reg[debounce_port]);
            if (v & UHCI_PORT_CCS)
            {
                enum_begin((int)debounce_port);
                return;
            }
            uhci_process_deferred_rd();
            debug_print("[uhci] connect bounced away; back to suspend\n");
            for (uint32_t p = 0; p < num_ports; p++)
                io_outw(io_base + portsc_reg[p],
                        (io_inw(io_base + portsc_reg[p]) & UHCI_PORT_WRMASK)
                            | UHCI_PORT_CHANGE);
            uhci_enter_suspend();
            enum_state = ST_IDLE_SUSPENDED;
            return;
        }

        case ST_DEVICE_READY:
            if (pending_gone)
            {
                pending_gone = 0;
                uhci_device_gone();
                return;
            }
            uhci_idle_tick();
            return;

        case ST_DEVICE_IDLE:
            return;   /* stale idle timer after suspend: ignore */

        case ST_PORT_RESETTING:
            enum_after_reset();
            break;
        case ST_SETADDR_SENT:
        case ST_DEVDESC_SENT:
        case ST_CONFDESC_SENT:
        {
            /* Before declaring a timeout, look at the TD chain: on
             * machines where the completion IRQ is never delivered
             * (dead PIRQ routing) the transfer may have completed on
             * the wire long ago.  If so, advance the FSM exactly as
             * the IRQ handler would — enumeration then proceeds at
             * one ENUM_TIMEOUT_MS per step instead of dying. */
            int last_idx = (enum_state == ST_SETADDR_SENT) ? 1 : 2;
            cnt_timeout++;
            int r = ctrl_check(last_idx);
            if (r == 1)
            {
                debug_print("[uhci] transfer completed but no IRQ "
                            "arrived; advancing (IRQ line dead?)\n");
                cnt_noirq_adv++;
                enum_on_complete();
            }
            else
            {
                enum_fail("transfer timeout");
            }
            break;
        }
        default:
            break;  /* stray timer in a non-waiting state: ignore */
    }
}



/* Read the port, ACK its change bits, and log appeared/removed by
 * comparing Current Connect Status against our shadow. Returns 1 if a
 * device is now present on the port, 0 otherwise. */
static int uhci_service_port(int p)
{
    uint16_t v = io_inw(io_base + portsc_reg[p]);
    uint8_t  ccs = (v & UHCI_PORT_CCS) ? 1 : 0;
    int      changed = (v & UHCI_PORT_CHANGE) ? 1 : 0;

    /* ACK the change bits: write them back as 1 (R/WC), preserving the
     * rest. We OR the change bits onto a fresh read so we don't disturb
     * control bits. */
    if (changed)
    {
        uint16_t ack = (io_inw(io_base + portsc_reg[p]) & UHCI_PORT_WRMASK)
                       | UHCI_PORT_CHANGE;
        io_outw(io_base + portsc_reg[p], ack);
    }

    if (ccs != port_present[p])
    {
        char line[96];
        sprintf(line, "[uhci] port %d %s (PORTSC=0x%04x)\n",
                p + 1, ccs ? "DEVICE CONNECTED" : "device removed",
                (unsigned)v);
        debug_print(line);
        port_present[p] = ccs;
    }
    else if (changed)
    {
        char line[96];
        sprintf(line, "[uhci] port %d change, present=%d (PORTSC=0x%04x)\n",
                p + 1, ccs, (unsigned)v);
        debug_print(line);
    }

    return ccs;
}

/* A resume IRQ arrived (device connect/disconnect while suspended).
 * Leave suspend, scan ports. If a device is now present on a port and
 * we're idle, kick off enumeration of that port; the FSM takes over
 * from ST_PORT_RESETTING. If only removals happened, log and go back
 * to suspend. */
static void uhci_handle_resume(void);

/* UHCI ports AUTO-DISABLE on (perceived) disconnect. The PIIX4's EGSM
 * entry/exit wiggle that latches spurious CSC also drops Port Enable —
 * leaving CCS=1, PED=0: device present, port dead, every TD timing out
 * (field: Armada, "Op servite: 2, ultimo opcode 3", bring-up dead at
 * the first wire transfer). The device itself keeps its address and
 * configuration across a port disable, so simply re-enabling PE
 * restores traffic — no re-enumeration needed. */
static bool uhci_port_reenable(int p)
{
    uint16_t reg = portsc_reg[p];
    uint16_t v   = io_inw(io_base + reg);
    if (!(v & UHCI_PORT_CCS)) return false;        /* truly empty */
    if (v & UHCI_PORT_PE)     return true;         /* already fine */

    io_outw(io_base + reg,
            (v & UHCI_PORT_WRMASK) | UHCI_PORT_PE);
    busy_wait_us(10000);
    v = io_inw(io_base + reg);
    if (v & UHCI_PORT_PE)
    {
        debug_print("[uhci] port re-enabled after auto-disable\n");
        return true;
    }
    debug_print("[uhci] port re-enable FAILED\n");
    return false;
}

/* If an RD was deferred while the FSM was busy, the port latches were
 * deliberately left untouched. Once the FSM settles (device ready/idle
 * or back to waiting), look at the ports ONCE: a connect that happened
 * during the busy window must not stay deaf. */
static void uhci_process_deferred_rd(void)
{
    if (!rd_deferred) return;
    rd_deferred = 0;
    debug_print("[uhci] processing deferred RD\n");
    uhci_handle_resume();
}

static void uhci_handle_resume(void)
{
    /* Re-entrancy guard. On real silicon a SECOND Resume Detect routinely
     * fires while the FSM is mid-enumeration (RD re-latches around the
     * FGR sequence and on port events; QEMU never does this). Without the
     * guard this path ran leave_suspend + "nothing new to enumerate" +
     * enter_suspend WHILE a port reset / transfer was in flight: the
     * controller was stopped, the FSM forced back to IDLE, and the port
     * could be abandoned with RESET still asserted — a frozen port and a
     * driver that looks perfectly idle. If the FSM is busy, just ack the
     * port change bits and let it finish. */
    if (enum_state == ST_DEVICE_IDLE)
    {
        /* RD with an enumerated device parked in suspend: wake through
         * the SAME timer-driven FSM as the op path (wake_reason_rd=1),
         * then inspect ports at TRSMRCY-complete. No busy waits, no
         * synchronous FGR dance, no gone verdicts on a groggy bus. */
        wake_reason_rd = 1;
        uint16_t c = io_inw(io_base + UHCI_CMD);
        io_outw(io_base + UHCI_CMD, c | UHCI_CMD_FGR);
        timed_state(ST_WAKING_FGR, 25);
        return;
    }

    if (enum_state != ST_IDLE_SUSPENDED)
    {
        /* Defer WITHOUT touching PORTSC: uhci_service_port acks the
         * change bits, and an acked CSC is a swallowed event — the
         * field showed a re-plugged stick (CCS=1, CSC=0) with a driver
         * deaf forever because no change was left to raise RD again.
         * Leave the latches alone; the post-enum/idle path rescans. */
        debug_print("[uhci] resume IRQ while FSM busy; deferring\n");
        rd_deferred = 1;
        return;
    }

    debug_print("[uhci] resume IRQ — leaving suspend to inspect ports\n");

    uhci_leave_suspend();

    int connect_port = -1;
    for (uint32_t p = 0; p < num_ports; p++)
    {
        int present = uhci_service_port((int)p);
        if (present && connect_port < 0)
            connect_port = (int)p;
    }

    if (connect_port >= 0 && enum_state == ST_IDLE_SUSPENDED)
    {
        /* Don't spend an enumeration on a bouncing contact: park in the
         * debounce state and let the timer confirm the connect. The
         * controller stays running (we left suspend). */
        debounce_port = (uint32_t)connect_port;
        timed_state(ST_CONNECT_DEBOUNCE, 100);
        return;
    }

    /* Nothing to enumerate (only disconnects, or busy). Return to the
     * low-power event-wait state. */
    debug_print("[uhci] no new device to enumerate; re-entering suspend\n");
    uhci_enter_suspend();
    enum_state = ST_IDLE_SUSPENDED;
}

/* ===== Hardware init from hotplug-provided device facts ===== */
static bool uhci_init_hw(hotplug_device_t *dev)
{
    /* UHCI I/O base is in BAR4, low bit is the I/O-space indicator. */
    io_base = (uint16_t)(dev->bar[4] & 0xFFFC);
    if (!io_base)
    {
        debug_print("[uhci] BAR4 has no I/O base\n");
        return false;
    }

    pci_enable_bus_master(dev);

    /* Take the controller away from the BIOS (SMM legacy keyboard
     * emulation) BEFORE touching it: ack trap status, clear every
     * SMI/trap enable.  See "Real-hardware survival kit" above. */
    uhci_dev = *dev;
    {
        uint32_t legsup = pci_config_read(dev->bus, dev->slot, dev->func,
                                          UHCI_LEGSUP);
        diag_legsup_before = (uint16_t)(legsup & 0xFFFF);
        char line[80];
        sprintf(line, "[uhci] LEGSUP=0x%04x; taking ownership from BIOS\n",
                (unsigned)(legsup & 0xFFFF));
        debug_print(line);
        pci_config_write(dev->bus, dev->slot, dev->func, UHCI_LEGSUP,
                         (legsup & 0xFFFF0000u) | UHCI_LEGSUP_RWC);
    }

    /* IRQ port + line. Create the port BEFORE any hardware op so the
     * delay helper (which blocks on it) is usable during reset. */
    /* ONE port for everything. dob_server_init already created and
     * registered the service port — that is where every client call
     * (usbdiag's GET_DIAG, hotplug's DETACH, any future sub-driver)
     * lands. The event loop receives from `irq_port`, so IRQ, timers
     * and client requests MUST share that port; the loop already
     * dispatches by msg.type/code.
     *
     * The original code did `irq_port = port_create()` here: a second,
     * private port. IRQs and timers flowed and the enumeration pipeline
     * worked, but the registered service port was never read by anyone —
     * handle_request() was unreachable, every dob_ipc_call from a client
     * blocked forever (usbdiag frozen at "Interrogazione in corso", on
     * QEMU exactly as on metal), and hotplug's DETACH was silently
     * ignored. */
    irq_port = dob_server_get_port();

    /* Register for this controller's interrupt by device identity and let
     * the kernel resolve delivery. Under PIC delivery it returns our
     * Interrupt Line; under IOAPIC delivery it returns the GSI if already
     * known, or 0 meaning "pending" — we then learn the GSI from the first
     * interrupt's notify bitmask and claim it (see the event loop). */
    int reg = irq_register_pci(dev->bus, dev->slot, dev->func, irq_port);
    if (reg > 0)
    {
        uhci_irq = (uint8_t)reg;        /* resolved: PIC line or cached GSI */
    }
    else if (reg == 0)
    {
        uhci_irq = 0;                   /* IOAPIC: GSI pending until first IRQ */
    }
    else
    {
        /* PIC delivery but config space had no valid Interrupt Line. Keep
         * the historical fallback so BIOSes that leave the byte clear still
         * deliver. */
        uhci_irq = dev->irq_line ? dev->irq_line : 11;
        irq_register(uhci_irq, irq_port);
    }

    /* ELCR: PIIX delivers PCI INTx as a LEVEL on the 8259 line. The
     * kernel's pirq_wire_device() promotes a line to level-triggered,
     * but only when it REWIRES one — a BIOS-preassigned line (our case:
     * the BIOS routed PIRQ to this line and we use it as-is) is never
     * touched, and some BIOSes leave it edge ("PnP OS: Yes"). An
     * edge-mode line never sees a transition from a steady level:
     * cnt_irq stays 0 forever with PIRQDEN perfectly set. Promote it
     * here; idempotent, and never for system-reserved lines. */
    if (uhci_irq > 2 && uhci_irq != 8 && uhci_irq != 13 && uhci_irq < 16)
    {
        uint16_t eport = (uhci_irq < 8) ? 0x4D0 : 0x4D1;
        uint8_t  ebit  = (uint8_t)(1u << (uhci_irq & 7));
        uint8_t  ecur  = io_inb(eport);
        if (!(ecur & ebit))
        {
            io_outb(eport, ecur | ebit);
            debug_print("[uhci] ELCR: IRQ line promoted to level\n");
        }
    }

    /* Frame list: allocated lazily by ensure_frame_list() at the first
     * enumeration/transfer — idle controllers never pay the 4 KB. */

    /* Allocate DMA control-transfer resources. The QH must be 16-byte
     * aligned and the TD array too; dma_alloc returns page-aligned
     * memory so both are satisfied. We allocate each region separately
     * to keep their physical bases clean. */
    ctrl_qh = (volatile uhci_qh_t *)dma_alloc(sizeof(uhci_qh_t), &ctrl_qh_phys);
    ctrl_td = (volatile uhci_td_t *)
        dma_alloc(ENUM_MAX_TD * sizeof(uhci_td_t), &ctrl_td_phys);
    setup_buf = (volatile uint8_t *)dma_alloc(ENUM_BUF_SIZE, &setup_buf_phys);
    data_buf  = (volatile uint8_t *)dma_alloc(ENUM_BUF_SIZE, &data_buf_phys);
    if (!ctrl_qh || !ctrl_td || !setup_buf || !data_buf ||
        !ctrl_qh_phys || !ctrl_td_phys || !setup_buf_phys || !data_buf_phys)
    {
        debug_print("[uhci] dma_alloc for control resources FAILED\n");
        return false;
    }

    {
        char line[96];
        sprintf(line, "[uhci] io_base=0x%04x irq=%u framelist_phys=0x%08x\n",
                io_base, uhci_irq, frame_list_phys);
        debug_print(line);
    }

    uhci_reset_and_setup();

    /* HCRESET done: now (and only now) set USBPIRQDEN so the controller's
     * interrupt actually reaches the PIRQ.  All SMI/trap enables stay 0. */
    {
        uint32_t legsup = pci_config_read(dev->bus, dev->slot, dev->func,
                                          UHCI_LEGSUP);
        pci_config_write(dev->bus, dev->slot, dev->func, UHCI_LEGSUP,
                         (legsup & 0xFFFF0000u) | UHCI_LEGSUP_PIRQEN);
    }

    /* Seed the presence shadow from the current port state, and note whether
     * any port already has a device attached at boot.
     *
     * Connect debounce: on real silicon CCS takes time to re-latch after the
     * GRESET/HCRESET pair (the port state machine re-detects the pull-up;
     * QEMU shows it instantly). Sampling exactly once right after reset can
     * read CCS=0 for a stick that has been plugged in since power-on — and
     * that miss is unrecoverable on machines whose resume IRQ never fires.
     * So sample for up to 100 ms (10 x 10 ms) before concluding "no device
     * at boot". This is a one-time bring-up dwell, not a polling loop: after
     * it, detection stays purely event-driven. */
    int boot_port = -1;
    for (int tries = 0; tries < 10 && boot_port < 0; tries++)
    {
        for (uint32_t p = 0; p < num_ports; p++)
        {
            uint16_t v = io_inw(io_base + portsc_reg[p]);
            port_present[p] = (v & UHCI_PORT_CCS) ? 1 : 0;
            if (port_present[p] && boot_port < 0)
                boot_port = (int)p;
        }
        if (boot_port < 0)
            busy_wait_us(10000);
    }

    if (boot_port >= 0)
    {
        /* A device is already plugged in at boot. The resume-IRQ path only
         * fires on a CHANGE while suspended, so a statically-present device
         * would never be enumerated if we just went to sleep. Enumerate it
         * now.
         *
         * reset_and_setup leaves the controller STOPPED (RS=0). The normal
         * resume path runs the controller via uhci_leave_suspend before
         * enumerating; here we must do the equivalent: enable the completion
         * interrupts the FSM relies on (IOC + short packet, plus timeout/CRC)
         * and set Run/Stop, so the enumeration TDs actually get processed and
         * raise IRQs. */
        io_outw(io_base + UHCI_INTR,
                UHCI_INTR_RESUME | UHCI_INTR_IOC |
                UHCI_INTR_SHORT  | UHCI_INTR_TIMEOUT);
        uint16_t cmd = io_inw(io_base + UHCI_CMD);
        cmd |= UHCI_CMD_RS | UHCI_CMD_MAXP;
        io_outw(io_base + UHCI_CMD, cmd);

        char line[64];
        sprintf(line, "[uhci] device present at boot on port %d; enumerating\n",
                boot_port + 1);
        debug_print(line);
        enum_begin(boot_port);
        return true;   /* enumeration in flight; do NOT enter suspend */
    }

    /* No device at boot: ack the change bits the resets raised on the ports
     * (CSC/PEC are R/WC and survive into suspend otherwise), then enter the
     * low-power event-wait state and let a resume IRQ wake us when something
     * is plugged in. A connect arriving between the ack and EGSM re-latches
     * CSC and drives resume signalling, so the window is safe. */
    for (uint32_t p = 0; p < num_ports; p++)
        io_outw(io_base + portsc_reg[p],
                (io_inw(io_base + portsc_reg[p]) & UHCI_PORT_WRMASK)
                    | UHCI_PORT_CHANGE);
    uhci_enter_suspend();
    debug_print("[uhci] entered Global Suspend; waiting for resume IRQ\n");
    return true;
}

/* Fill the GET_DIAG snapshot: live registers + latched pipeline facts.
 * Static so the buffer outlives the reply (the kernel copies the payload
 * into the caller's IPC buffer during dob_ipc_reply). */
static uhci_diag_t diag_buf;
static void diag_fill(void)
{
    memset(&diag_buf, 0, sizeof(diag_buf));
    diag_buf.init_ok       = diag_init_ok;
    diag_buf.num_ports     = (uint8_t)num_ports;
    diag_buf.irq_line      = uhci_irq;
    diag_buf.io_base       = io_base;
    diag_buf.pci_vendor    = uhci_dev.vendor_id;
    diag_buf.pci_device    = uhci_dev.device_id;
    diag_buf.legsup_before = diag_legsup_before;
    diag_buf.fail_usbsts    = diag_fail_usbsts;
    diag_buf.fail_frnum     = diag_fail_frnum;
    diag_buf.fail_td_status = diag_fail_td_status;
    diag_buf.fail_portsc    = diag_fail_portsc;
    {
        int lrc = usb_das_last_list_rc();
        diag_buf.das_list_rc     = (int8_t)((lrc < -98) ? -99
                                            : (lrc < 0 ? -1 : lrc));
        diag_buf.das_dir_entries = (uint8_t)usb_das_last_dir_entries();
        diag_buf.das_open_fd     = (int16_t)usb_das_last_open_fd();
        diag_buf.das_read_len    = (int16_t)usb_das_last_read_len();
        diag_buf.cnt_shared      = cnt_shared;
        diag_buf.subdrv_ops      = subdrv_ops_served;
        diag_buf.subdrv_last_op  = subdrv_last_op;
    }
    /* ELCR: 8259 edge/level control. PIRQ-routed lines MUST be level
     * (bit set) — an edge-mode line silently loses UHCI interrupts,
     * which would explain cnt_irq==0 with PIRQDEN correctly set. */
    diag_buf.elcr           = (uint16_t)(io_inb(0x4D0)
                                         | ((uint16_t)io_inb(0x4D1) << 8));
    diag_buf.legsup_now    = (uint16_t)(pci_config_read(uhci_dev.bus,
                                 uhci_dev.slot, uhci_dev.func,
                                 UHCI_LEGSUP) & 0xFFFF);
    if (io_base)
    {
        diag_buf.usbcmd    = io_inw(io_base + UHCI_CMD);
        diag_buf.usbsts    = io_inw(io_base + UHCI_STS);
        diag_buf.usbintr   = io_inw(io_base + UHCI_INTR);
        diag_buf.frnum     = io_inw(io_base + UHCI_FRNUM);
        diag_buf.portsc[0] = io_inw(io_base + UHCI_PORTSC1);
        diag_buf.portsc[1] = io_inw(io_base + UHCI_PORTSC2);
    }
    diag_buf.cnt_irq       = cnt_irq;
    diag_buf.cnt_resume    = cnt_resume;
    diag_buf.cnt_complete  = cnt_complete;
    diag_buf.cnt_timeout   = cnt_timeout;
    diag_buf.cnt_noirq_adv = cnt_noirq_adv;
    diag_buf.enum_state    = (uint8_t)enum_state;
    diag_buf.enum_port     = (int8_t)enum_port;
    diag_buf.enum_done     = diag_enum_done;
    diag_buf.das_matched   = diag_das_matched;
    diag_buf.das_files     = (uint8_t)usb_das_last_file_count();
    diag_buf.announce_ok   = diag_announce_ok;
    if (diag_enum_done)
    {
        diag_buf.vid         = enum_dev.idVendor;
        diag_buf.pid         = enum_dev.idProduct;
        diag_buf.dev_class   = enum_dev.bDeviceClass;
        diag_buf.if_class    = enum_iface.bInterfaceClass;
        diag_buf.if_subclass = enum_iface.bInterfaceSubClass;
        diag_buf.if_protocol = enum_iface.bInterfaceProtocol;
    }
    strncpy(diag_buf.das_label, diag_das_label,
            sizeof(diag_buf.das_label) - 1);
    strncpy(diag_buf.last_error, diag_last_error,
            sizeof(diag_buf.last_error) - 1);
}

/* ===== IPC request handling (sync requests, msg.type==1) ===== */
static dob_status_t handle_request(dob_msg_t *msg, dob_msg_t *reply)
{
    if (dob_driver_is_detach(msg))
    {
        dob_driver_released();
        _exit(0);
    }

    switch (msg->code)
    {
        case 1: /* GET_STATUS */
            reply->arg0 = io_inw(io_base + UHCI_STS);
            reply->arg1 = io_inw(io_base + UHCI_PORTSC1);
            reply->arg2 = io_inw(io_base + UHCI_PORTSC2);
            return DOB_OK;

        case 2: /* PORT_RESET (manual, diagnostics) */
        {
            uint32_t pn = msg->arg0;
            if (pn >= num_ports) return DOB_ERR_INVALID;
            uint16_t reg = portsc_reg[pn];
            uint16_t v = io_inw(io_base + reg) & UHCI_PORT_WRMASK;
            io_outw(io_base + reg, v | UHCI_PORT_RESET);
            busy_wait_us(50000);
            v = io_inw(io_base + reg) & UHCI_PORT_WRMASK;
            io_outw(io_base + reg, v & ~UHCI_PORT_RESET);
            busy_wait_us(10000);
            v = io_inw(io_base + reg) & UHCI_PORT_WRMASK;
            io_outw(io_base + reg, v | UHCI_PORT_PE);
            return DOB_OK;
        }

        case 5: /* ENUMERATE (count + connect bits) */
            reply->arg0 = num_ports;
            reply->arg1 = (io_inw(io_base + UHCI_PORTSC1) & UHCI_PORT_CCS) ? 1 : 0;
            reply->arg2 = (io_inw(io_base + UHCI_PORTSC2) & UHCI_PORT_CCS) ? 1 : 0;
            return DOB_OK;

        case UHCI_OP_CTRL_XFER: { /* 3: sub-driver control transfer */
            /* PORT GATE: a rip during active I/O raises NO event (RD
             * exists only in suspend; a running controller just latches
             * a silent CSC). Detection by TD-timeout costs seconds per
             * chain and minutes per bring-up retry ladder — the field
             * "ghost icon": driver wedged in xfer_run, usbms wedged in
             * retries, hotplug parked on the action. One PORTSC read
             * makes it instant: empty port -> DEAD, zero TDs launched. */
            if (ready_port < 0 ||
                !(io_inw(io_base + portsc_reg[ready_port]) & UHCI_PORT_CCS))
                return DOB_ERR_DEAD;
            if (enum_state == ST_DEVICE_IDLE) uhci_device_wake();
            if (enum_state != ST_DEVICE_READY) return DOB_ERR_INVALID;
            xfer_activity = 1;
            if (!msg->payload || msg->payload_size < 8) return DOB_ERR_INVALID;
            const uint8_t *pl = (const uint8_t *)msg->payload;
            uint8_t setup[8]; memcpy(setup, pl, 8);
            uint16_t wlen = (uint16_t)setup[6] | ((uint16_t)setup[7] << 8);
            if (!xfer_pool_init()) return DOB_ERR_NO_MEMORY;
            if (!(setup[0] & 0x80))   /* host->device: data follows setup */
            {
                if (msg->payload_size < 8u + wlen) return DOB_ERR_INVALID;
                if (wlen) memcpy(xfer_buf, pl + 8, wlen);
            }
            int got = uhci_ctrl_xfer(setup);
            if (got < 0)
            {
                uhci_check_disconnect_after_error();
                return DOB_ERR_INTERNAL;
            }
            if (setup[0] & 0x80)
            {
                reply->payload      = xfer_buf;
                reply->payload_size = (uint32_t)got;
            }
            reply->arg0 = (uint32_t)got;
            return DOB_OK;
        }

        case UHCI_OP_BULK_XFER: { /* 4: sub-driver bulk transfer */
            if (ready_port < 0 ||
                !(io_inw(io_base + portsc_reg[ready_port]) & UHCI_PORT_CCS))
                return DOB_ERR_DEAD;   /* port gate: see CTRL_XFER */
            if (enum_state == ST_DEVICE_IDLE) uhci_device_wake();
            if (enum_state != ST_DEVICE_READY) return DOB_ERR_INVALID;
            xfer_activity = 1;
            uint8_t  ep     = (uint8_t)(msg->arg0 & 0x0F);
            int      dir_in = (msg->arg0 & 0x80) ? 1 : 0;
            uint32_t len    = msg->arg1;
            if (len == 0 || len > UHCI_XFER_MAX) return DOB_ERR_INVALID;
            if (!xfer_pool_init()) return DOB_ERR_NO_MEMORY;
            /* OUT data always arrives in the IPC payload: the zero-copy
             * window was retired (the kernel denies mmap of another
             * process's RAM), so arg2 now carries ONLY the EP max packet.
             * The old `arg2 == 1` window flag overlapped that value and is
             * gone — a real mps is 8/16/32/64, never 1. */
            if (!dir_in)
            {
                if (!msg->payload || msg->payload_size < len)
                    return DOB_ERR_INVALID;
                memcpy(xfer_buf, msg->payload, len);
            }
            int got = uhci_bulk_xfer(ep, dir_in, len,
                                     msg->arg2);  /* EP max packet (0=64) */
            if (got == -2) { reply->arg0 = 0; return DOB_ERR_DENIED; } /* STALL */
            if (got == -4)
            {
                /* Device NAKing the burst (flow-control busy: a flash stick
                 * committing a write). NOT a fault — skip the disconnect probe
                 * and do NOT let the sub-driver reset. Signal busy via arg1 so
                 * it WAITS and retries the same transfer. */
                reply->arg1 = 1;
                return DOB_ERR_INTERNAL;
            }
            if (got < 0)
            {
                uhci_check_disconnect_after_error();
                return DOB_ERR_INTERNAL;
            }
            if (dir_in)
            {
                reply->payload      = xfer_buf;
                reply->payload_size = (uint32_t)got;
            }
            reply->arg0 = (uint32_t)got;
            return DOB_OK;
        }

        case UHCI_OP_GET_WINDOW: { /* 9: zero-copy shared DMA window */
            if (!xfer_pool_init()) return DOB_ERR_NO_MEMORY;
            reply->arg0 = xfer_buf_phys;
            reply->arg1 = UHCI_XFER_MAX;
            return DOB_OK;
        }

        case UHCI_OP_RESET_TOGGLE: { /* 7: after CLEAR_FEATURE(HALT) */
            uint8_t ep = (uint8_t)(msg->arg0 & 0x0F);
            if (msg->arg0 & 0x80) toggle_in_map  &= (uint16_t)~(1u << ep);
            else                  toggle_out_map &= (uint16_t)~(1u << ep);
            return DOB_OK;
        }

        case UHCI_OP_GET_DEVINFO: { /* 8: facts from enumeration */
            /* Valid in BOTH post-enum states: the answer comes from
             * enumeration-time statics, no wire access needed, so we
             * do NOT wake a DEVICE_IDLE controller for it. Rejecting
             * DEVICE_IDLE here killed usbms pre-registration on its
             * very first call (the controller parks idle right after
             * enumerating): "Impossibile avviare il driver" with the
             * .mdl perfectly on disk. */
            if (enum_state != ST_DEVICE_READY &&
                enum_state != ST_DEVICE_IDLE) return DOB_ERR_INVALID;
            static uhci_devinfo_t di;
            memset(&di, 0, sizeof(di));
            di.dev_addr    = 1;
            di.port        = (uint8_t)ready_port;
            di.low_speed   = enum_low_speed ? 1 : 0;
            di.vid         = enum_dev.idVendor;
            di.pid         = enum_dev.idProduct;
            di.if_class    = enum_iface.bInterfaceClass;
            di.if_subclass = enum_iface.bInterfaceSubClass;
            di.if_protocol = enum_iface.bInterfaceProtocol;
            reply->payload      = &di;
            reply->payload_size = sizeof(di);
            return DOB_OK;
        }

        case UHCI_OP_GET_DIAG: /* 6: full bring-up + pipeline snapshot */
            diag_fill();
            reply->payload      = &diag_buf;
            reply->payload_size = sizeof(diag_buf);
            return DOB_OK;

        default:
            return DOB_ERR_INVALID;
    }
}

/* ===== Unified event loop = FSM dispatcher =====
 *
 * One blocking receive on irq_port; never blocks elsewhere. Each event
 * advances the enumeration state machine by exactly one step:
 *
 *   msg.type == 3 (hardware IRQ): read USBSTS, ack it, then route by
 *       cause AND current state:
 *         - Resume Detect (RD): a connect/disconnect woke us from
 *           suspend -> uhci_handle_resume (only meaningful when idle).
 *         - USBINT (IOC/short): a control transfer finished -> advance
 *           the FSM via enum_on_complete.
 *         - USB Error: failed transfer -> enum_fail.
 *   msg.code == 70 (timer): reset-dwell or per-transfer timeout ->
 *       enum_on_timer (stale timers are filtered by state).
 *   msg.type == 1 (sync IPC request): handle + reply.
 *
 * Between events the driver sleeps inside dob_ipc_receive: zero CPU,
 * no polling. */
static void uhci_event_loop(void)
{
    for (;;)
    {
        dob_msg_t msg, reply;
        memset(&msg, 0, sizeof(msg));
        memset(&reply, 0, sizeof(reply));

        if (dob_ipc_receive(irq_port, &msg) != 0)
            continue;

        if (msg.type == 3) /* hardware IRQ */
        {
            uint16_t sts = io_inw(io_base + UHCI_STS);

            /* Empirical GSI resolution (IOAPIC mode): while our GSI is
             * unknown (irq_register_pci returned 0), the kernel notifies us
             * for one candidate GSI at a time, the GSI carried in msg.arg0.
             * Recognise our own interrupt by USBSTS and claim that GSI;
             * ignore notifications that are not ours — we are not yet a
             * sharer and owe no irq_done. */
            if (uhci_irq == 0)
            {
                if (!(sts & UHCI_STS_INTR))
                    continue;           /* not our device — stay pending */
                uint32_t bits = msg.arg0;
                uint8_t  gsi  = 0;
                while (bits && !(bits & 1u)) { bits >>= 1; gsi++; }
                uhci_irq = gsi;
                irq_pci_claim(gsi);     /* bind; we are now a sharer */
                /* fall through to service this very interrupt */
            }

            if (!(sts & UHCI_STS_INTR))
            {
                /* Not ours: shared-line neighbour (or just halted with no
                 * pending cause). Ack the kernel and move on without
                 * polluting the real counter. */
                cnt_shared++;
                irq_done(uhci_irq);
                continue;
            }
            cnt_irq++;

            /* Ack all latched status first (write-1-to-clear). */
            if (sts & UHCI_STS_ALL)
                io_outw(io_base + UHCI_STS, sts & UHCI_STS_ALL);

            /* Route by cause. Order matters: a transfer that both
             * completes and errors sets USBINT+ERROR; treat as error. */
            if (sts & UHCI_STS_ERROR)
            {
                if (enum_state == ST_SETADDR_SENT ||
                    enum_state == ST_DEVDESC_SENT ||
                    enum_state == ST_CONFDESC_SENT)
                    enum_fail("USB error interrupt");
            }
            else if (sts & UHCI_STS_USBINT)
            {
                cnt_complete++;
                enum_on_complete();
            }

            if (sts & UHCI_STS_RD)
            {
                cnt_resume++;
                uhci_handle_resume();
            }

            irq_done(uhci_irq);
            continue;
        }

        if (msg.code == 70) /* timer */
        {
            enum_on_timer();
            continue;
        }

        if (dob_driver_is_detach(&msg))
        {
            /* hotplug is tearing down our bubble. Quiesce the controller
             * (stop schedule, mask interrupts) so it can't DMA or raise
             * IRQs into a dead process, ack the request, and exit.
             * Note: until the port unification above, DETACH landed on
             * the unread service port and was silently lost. */
            if (io_base)
            {
                io_outw(io_base + UHCI_INTR, 0);
                io_outw(io_base + UHCI_CMD,
                        io_inw(io_base + UHCI_CMD) & ~UHCI_CMD_RS);
            }
            reply.code = DOB_OK;
            dob_ipc_reply(msg.sender_tid, &reply);
            _exit(0);
        }

        if (msg.type == 1) /* sync request */
        {
            if (msg.code == 3 || msg.code == 4 ||
                msg.code == 7 || msg.code == 8)
            {
                subdrv_ops_served++;
                subdrv_last_op = (uint8_t)msg.code;
            }

            /* Wire ops against a sleeping controller: PARK the request
             * and run the resume as a timer-driven FSM. The caller (a
             * sync IPC anyway) waits the same wall time; the LOOP stays
             * free to serve IRQs and diagnostics instead of burning
             * 40 ms in busy waits. Event-driven resume, project rule. */
            if ((msg.code == 3 || msg.code == 4) &&
                enum_state == ST_DEVICE_IDLE)
            {
                pending_req = msg;
                pending_req.payload = NULL;
                if (!pending_payload)
                    pending_payload = (uint8_t *)malloc(UHCI_XFER_MAX);
                if (pending_payload && msg.payload && msg.payload_size &&
                    msg.payload_size <= UHCI_XFER_MAX)
                {
                    memcpy(pending_payload, msg.payload, msg.payload_size);
                    pending_req.payload = pending_payload;
                }
                pending_valid  = 1;
                wake_reason_rd = 0;
                uint16_t c = io_inw(io_base + UHCI_CMD);
                io_outw(io_base + UHCI_CMD, c | UHCI_CMD_FGR);
                timed_state(ST_WAKING_FGR, 25);
                continue;   /* no reply yet: parked */
            }
            if ((msg.code == 3 || msg.code == 4) &&
                (enum_state == ST_WAKING_FGR ||
                 enum_state == ST_WAKING_SETTLE))
            {
                reply.code = DOB_ERR_INTERNAL; /* busy: wake in flight */
                dob_ipc_reply(msg.sender_tid, &reply);
                continue;
            }

            reply.code = handle_request(&msg, &reply);
            dob_ipc_reply(msg.sender_tid, &reply);
            continue;
        }

        /* Anything else: ignore. */
    }
}


int main(void)
{
    debug_print("[uhci] v1.5.34 driver starting\n");
    hotplug_device_t dev;

    if (dob_server_init_unique("usb_uhci", uhci_service_name,
                               sizeof(uhci_service_name)) != DOB_OK)
    {
        debug_print("[uhci] no free service slot; exiting\n");
        return 1;
    }
    {
        const char *p = uhci_service_name;
        while (*p && *p != '_') p++;          /* skip "usb" */
        p++; while (*p && *p != '_') p++;     /* skip "uhci" */
        if (*p == '_') uhci_instance_id = (uint32_t)atoi(p + 1);
        if (uhci_instance_id)
        {
            char m[64];
            sprintf(m, "[uhci] instance %u as %s\n",
                    (unsigned)uhci_instance_id, uhci_service_name);
            debug_print(m);
        }
    }

    /* Wait for hotplug to be up before asking it for our device. hotplug
     * spawns us during its PCI scan, BEFORE it enters its service loop, so
     * dob_driver_attach (a synchronous call to hotplug) can race ahead of
     * hotplug being ready to answer. The other hotplug-spawned drivers
     * (bga, ac97) all dob_registry_wait("hotplug", ...) first for exactly
     * this reason; usb_uhci was missing it and exited with "no device
     * assigned" the instant the call came back empty. */
    dob_registry_wait("hotplug", 5000);

    if (dob_driver_attach(&dev))
    {
        if (uhci_init_hw(&dev))
            diag_init_ok = 1;
        else
        {
            debug_print("[uhci] init failed\n");
            strncpy(diag_last_error, "uhci_init_hw failed",
                    sizeof(diag_last_error) - 1);
        }
    }
    else
    {
        debug_print("[uhci] no device assigned by hotplug\n");
        /* Without a device we have no io_base/irq; nothing to serve. */
        _exit(1);
    }

    debug_print("[uhci] ready (Block 1: event-driven detect)\n");

    /* We do NOT use dob_server_loop(): that loop blocks waiting for IPC
     * requests only and would never see our IRQ notifications. Our own
     * loop multiplexes IRQ + IPC on the same port. */
    uhci_event_loop();
    return 0;
}
