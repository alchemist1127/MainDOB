/* MainDOB XHCI Driver - USB 3.0/3.1/3.2 (Extensible Host Controller Interface)
 * Modern USB host controller with command ring, event ring, transfer rings.
 *
 * Protocol: same as UHCI/EHCI
 *   code=1 GET_STATUS  code=2 PORT_RESET  code=3 CONTROL_TRANSFER
 *   code=4 BULK_TRANSFER  code=5 ENUMERATE */

#include <unistd.h>
#include <string.h>
#include <dob/server.h>
#include <dob/ipc.h>
#include <dob/types.h>
#include <dob/hotplug_driver.h>

/* PCI identification */
#define PCI_CLASS_SERIAL    0x0C
#define PCI_SUBCLASS_USB    0x03
#define PCI_PROGIF_XHCI     0x30

/* XHCI Capability Registers */
#define XHCI_CAPLENGTH      0x00    /* 8-bit */
#define XHCI_HCIVERSION     0x02    /* 16-bit */
#define XHCI_HCSPARAMS1     0x04
#define XHCI_HCSPARAMS2     0x08
#define XHCI_HCSPARAMS3     0x0C
#define XHCI_HCCPARAMS1     0x10
#define XHCI_DBOFF          0x14    /* Doorbell offset */
#define XHCI_RTSOFF         0x18    /* Runtime register offset */
#define XHCI_HCCPARAMS2     0x1C

/* XHCI Operational Registers (at base + CAPLENGTH) */
#define XHCI_USBCMD         0x00
#define XHCI_USBSTS         0x04
#define XHCI_PAGESIZE       0x08
#define XHCI_DNCTRL         0x14
#define XHCI_CRCR           0x18    /* Command Ring Control (64-bit) */
#define XHCI_DCBAAP          0x30    /* Device Context Base Address Array Pointer (64-bit) */
#define XHCI_CONFIG          0x38
#define XHCI_PORTSC(n)      (0x400 + (n) * 0x10)

/* USBCMD bits */
#define XHCI_CMD_RS         (1 << 0)    /* Run/Stop */
#define XHCI_CMD_HCRST      (1 << 1)    /* HC Reset */
#define XHCI_CMD_INTE       (1 << 2)    /* Interrupter Enable */
#define XHCI_CMD_HSEE       (1 << 3)    /* Host System Error Enable */

/* USBSTS bits */
#define XHCI_STS_HCH        (1 << 0)    /* HC Halted */
#define XHCI_STS_HSE        (1 << 2)    /* Host System Error */
#define XHCI_STS_EINT       (1 << 3)    /* Event Interrupt */
#define XHCI_STS_PCD        (1 << 4)    /* Port Change Detect */
#define XHCI_STS_CNR        (1 << 11)   /* Controller Not Ready */

/* PORTSC bits */
#define XHCI_PORT_CCS       (1 << 0)    /* Current Connect Status */
#define XHCI_PORT_PED       (1 << 1)    /* Port Enabled/Disabled */
#define XHCI_PORT_PR        (1 << 4)    /* Port Reset */
#define XHCI_PORT_PLS_MASK  (0xF << 5)  /* Port Link State */
#define XHCI_PORT_PP        (1 << 9)    /* Port Power */
#define XHCI_PORT_SPEED_MASK (0xF << 10) /* Port Speed */
#define XHCI_PORT_CSC       (1 << 17)   /* Connect Status Change */
#define XHCI_PORT_PRC       (1 << 21)   /* Port Reset Change */
#define XHCI_PORT_WRC       (1 << 19)   /* Warm Port Reset Change */

/* Port speed values */
#define XHCI_SPEED_FULL     1   /* USB 1.1 Full Speed (12 Mbps) */
#define XHCI_SPEED_LOW      2   /* USB 1.0 Low Speed (1.5 Mbps) */
#define XHCI_SPEED_HIGH     3   /* USB 2.0 High Speed (480 Mbps) */
#define XHCI_SPEED_SUPER    4   /* USB 3.0 SuperSpeed (5 Gbps) */
#define XHCI_SPEED_SUPER_PLUS 5 /* USB 3.1 SuperSpeed+ (10 Gbps) */

/* TRB (Transfer Request Block) types */
#define TRB_TYPE_NORMAL      1
#define TRB_TYPE_SETUP       2
#define TRB_TYPE_DATA        3
#define TRB_TYPE_STATUS      4
#define TRB_TYPE_LINK        6
#define TRB_TYPE_NOOP        8
#define TRB_TYPE_ENABLE_SLOT 9
#define TRB_TYPE_DISABLE_SLOT 10
#define TRB_TYPE_ADDRESS_DEV 11
#define TRB_TYPE_CONFIG_EP   12
#define TRB_TYPE_NOOP_CMD    23
#define TRB_TYPE_TRANSFER    32  /* Event */
#define TRB_TYPE_CMD_COMPL   33  /* Event */
#define TRB_TYPE_PORT_CHANGE 34  /* Event */

/* TRB structure */
typedef struct
{
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;
} __attribute__((packed, aligned(16))) xhci_trb_t;

/* Command ring, event ring, device context array */
#define CMD_RING_SIZE       16
#define EVT_RING_SIZE       16
#define MAX_SLOTS           32

static xhci_trb_t cmd_ring[CMD_RING_SIZE] __attribute__((aligned(64)));
static xhci_trb_t evt_ring[EVT_RING_SIZE] __attribute__((aligned(64)));

/* Event Ring Segment Table Entry */
typedef struct
{
    uint64_t base;
    uint32_t size;
    uint32_t reserved;
} __attribute__((packed)) xhci_erst_entry_t;

static xhci_erst_entry_t erst[1] __attribute__((aligned(64)));

/* Device Context Base Address Array */
static uint64_t dcbaa[MAX_SLOTS + 1] __attribute__((aligned(64)));

static volatile uint8_t  *mmio_base = NULL;
static volatile uint32_t *opregs = NULL;
static volatile uint32_t *rtregs = NULL;
static volatile uint32_t *dbregs = NULL;
static uint32_t num_ports = 0;
static uint32_t max_slots = 0;
static uint32_t irq_port = 0;
static uint8_t  xhci_irq = 0;
static uint32_t cmd_ring_idx = 0;
static uint32_t cmd_ring_cycle = 1;
static uint32_t evt_ring_idx = 0;
static uint32_t evt_ring_cycle = 1;

/* Timer-based delay: thread truly sleeps, zero CPU */
static void xhci_delay(uint32_t ms)
{
    int tid = timer_set(irq_port, ms, 0);
    dob_msg_t wm;
    memset(&wm, 0, sizeof(wm));
    dob_ipc_receive(irq_port, &wm);
    if (wm.type == 3) irq_done(xhci_irq);
    (void)tid;
}

static bool xhci_wait_reg(volatile uint32_t *reg, uint32_t mask,
                           uint32_t expected, uint32_t timeout_ms)
{
    for (uint32_t elapsed = 0; elapsed < timeout_ms; elapsed += 1)
    {
        if ((*reg & mask) == expected) return true;
        xhci_delay(1);
    }
    return (*reg & mask) == expected;
}

static void xhci_halt(void)
{
    opregs[XHCI_USBCMD / 4] &= ~XHCI_CMD_RS;
    xhci_wait_reg(&opregs[XHCI_USBSTS / 4], XHCI_STS_HCH, XHCI_STS_HCH, 100);
}

static void xhci_reset(void)
{
    xhci_halt();
    opregs[XHCI_USBCMD / 4] |= XHCI_CMD_HCRST;
    xhci_wait_reg(&opregs[XHCI_USBCMD / 4], XHCI_CMD_HCRST, 0, 100);
    xhci_wait_reg(&opregs[XHCI_USBSTS / 4], XHCI_STS_CNR, 0, 100);
}

static void xhci_ring_doorbell(uint32_t slot, uint32_t value)
{
    dbregs[slot] = value;
}

static void xhci_send_command(uint32_t param_lo, uint32_t param_hi,
                               uint32_t status, uint32_t type)
{
    xhci_trb_t *trb = &cmd_ring[cmd_ring_idx];
    trb->param_lo = param_lo;
    trb->param_hi = param_hi;
    trb->status = status;
    trb->control = (type << 10) | cmd_ring_cycle;

    cmd_ring_idx++;
    if (cmd_ring_idx >= CMD_RING_SIZE - 1)
    {
        /* Link TRB back to start */
        cmd_ring[cmd_ring_idx].param_lo = (uint32_t)cmd_ring;
        cmd_ring[cmd_ring_idx].param_hi = 0;
        cmd_ring[cmd_ring_idx].status = 0;
        cmd_ring[cmd_ring_idx].control = (TRB_TYPE_LINK << 10) | (1 << 1) | cmd_ring_cycle;
        cmd_ring_idx = 0;
        cmd_ring_cycle ^= 1;
    }

    /* Ring host controller doorbell (slot 0 = command ring) */
    xhci_ring_doorbell(0, 0);
}

static void xhci_port_reset(uint32_t port)
{
    if (port >= num_ports) return;

    volatile uint32_t *portsc = &opregs[XHCI_PORTSC(port) / 4];
    *portsc = (*portsc & 0x0E00C3E0) | XHCI_PORT_PR;

    xhci_wait_reg(portsc, XHCI_PORT_PRC, XHCI_PORT_PRC, 1000);

    *portsc |= XHCI_PORT_CSC | XHCI_PORT_PRC | XHCI_PORT_WRC;
}

static const char *speed_string(uint32_t speed)
{
    switch (speed)
    {
        case XHCI_SPEED_FULL:       return "Full (12 Mbps)";
        case XHCI_SPEED_LOW:        return "Low (1.5 Mbps)";
        case XHCI_SPEED_HIGH:       return "High (480 Mbps)";
        case XHCI_SPEED_SUPER:      return "Super (5 Gbps)";
        case XHCI_SPEED_SUPER_PLUS: return "Super+ (10 Gbps)";
        default: return "Unknown";
    }
}

static bool
xhci_init_hw(hotplug_device_t *dev)
{
    uint32_t mmio_phys = dev->bar[0] & 0xFFFFFFF0;
    if (!mmio_phys) return false;

    pci_enable_bus_master(dev);

    mmio_base = (volatile uint8_t *)mmap_phys(mmio_phys, 0x10000);
    if (!mmio_base) return false;

    uint8_t cap_length = mmio_base[XHCI_CAPLENGTH];
    uint32_t hcsparams1 = *(volatile uint32_t *)(mmio_base + XHCI_HCSPARAMS1);
    uint32_t dboff = *(volatile uint32_t *)(mmio_base + XHCI_DBOFF);
    uint32_t rtsoff = *(volatile uint32_t *)(mmio_base + XHCI_RTSOFF);

    max_slots = hcsparams1 & 0xFF;
    num_ports = (hcsparams1 >> 24) & 0xFF;
    if (max_slots > MAX_SLOTS) max_slots = MAX_SLOTS;

    opregs = (volatile uint32_t *)(mmio_base + cap_length);
    rtregs = (volatile uint32_t *)(mmio_base + rtsoff);
    dbregs = (volatile uint32_t *)(mmio_base + dboff);

    /* Create IRQ port BEFORE any hardware ops */
    irq_port = (uint32_t)port_create();
    xhci_irq = dev->irq_line ? dev->irq_line : 11;
    irq_register(xhci_irq, irq_port);

    xhci_reset();

    opregs[XHCI_CONFIG / 4] = max_slots;

    memset(dcbaa, 0, sizeof(dcbaa));
    opregs[XHCI_DCBAAP / 4] = (uint32_t)dcbaa;
    opregs[XHCI_DCBAAP / 4 + 1] = 0;

    memset(cmd_ring, 0, sizeof(cmd_ring));
    cmd_ring_idx = 0;
    cmd_ring_cycle = 1;
    opregs[XHCI_CRCR / 4] = (uint32_t)cmd_ring | cmd_ring_cycle;
    opregs[XHCI_CRCR / 4 + 1] = 0;

    memset(evt_ring, 0, sizeof(evt_ring));
    evt_ring_idx = 0;
    evt_ring_cycle = 1;

    erst[0].base = (uint64_t)(uint32_t)evt_ring;
    erst[0].size = EVT_RING_SIZE;
    erst[0].reserved = 0;

    volatile uint32_t *intr0 = (volatile uint32_t *)((volatile uint8_t *)rtregs + 0x20);
    intr0[1] = EVT_RING_SIZE;
    intr0[2] = (uint32_t)evt_ring;
    intr0[3] = 0;
    intr0[4] = (uint32_t)erst;
    intr0[5] = 0;
    intr0[0] = (1 << 1);

    opregs[XHCI_USBCMD / 4] = XHCI_CMD_RS | XHCI_CMD_INTE | XHCI_CMD_HSEE;

    xhci_delay(100);   /* Wait for ports to settle */

    xhci_send_command(0, 0, 0, TRB_TYPE_NOOP_CMD);

    return true;
}

static dob_status_t
handle_message(dob_msg_t *msg, dob_msg_t *reply)
{
    if (dob_driver_is_detach(msg))
    {
        dob_driver_released();
        _exit(0);
    }

    if (!opregs) return DOB_ERR_INTERNAL;

    switch (msg->code)
    {
        case 1: /* GET_STATUS */
            reply->arg0 = opregs[XHCI_USBSTS / 4];
            reply->arg1 = num_ports;
            reply->arg2 = max_slots;
            return DOB_OK;

        case 2: /* PORT_RESET */
            xhci_port_reset(msg->arg0);
            return DOB_OK;

        case 5: /* ENUMERATE */
        {
            reply->arg0 = num_ports;
            uint32_t connected = 0;
            for (uint32_t i = 0; i < num_ports; i++)
            {
                uint32_t portsc = opregs[XHCI_PORTSC(i) / 4];
                if (portsc & XHCI_PORT_CCS)
                {
                    connected++;
                    uint32_t speed = (portsc & XHCI_PORT_SPEED_MASK) >> 10;
                    (void)speed;
                    (void)speed_string;
                }
            }
            reply->arg1 = connected;
            return DOB_OK;
        }

        default:
            return DOB_ERR_INVALID;
    }
}

int
main(void)
{
    hotplug_device_t dev;

    dob_server_init("usb_xhci");

    if (dob_driver_attach(&dev))
    {
        if (xhci_init_hw(&dev))
            debug_print("[xhci] Initialized.\n");
        else
            debug_print("[xhci] Init failed.\n");
    }
    else
    {
        debug_print("[xhci] No device assigned.\n");
    }

    dob_server_register(handle_message);
    debug_print("[xhci] Ready.\n");
    dob_server_loop();
    return 0;
}
