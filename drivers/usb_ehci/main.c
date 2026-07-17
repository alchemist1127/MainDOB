/* MainDOB EHCI Driver - USB 2.0 (Enhanced Host Controller Interface)
 *
 * CURRENT ROLE: GATEKEEPER, not a transfer stack. MainDOB has no EHCI
 * schedule/transfer engine yet; on real hardware this driver's job is to
 * (1) take the controller from the BIOS (USBLEGSUP handoff, see below),
 * (2) reset it, (3) leave CONFIGFLAG=0 so every port routes to the
 * companion UHCI controllers, where the full detection -> enumeration ->
 * DAS -> hotplug pipeline lives. Without this, a BIOS that left
 * CONFIGFLAG=1 (very common: legacy USB boot support) silently swallows
 * every inserted device on USB2-era machines (e.g. ICH8 laptops).
 *
 * Protocol (diagnostics only while gatekeeping):
 *   code=1 GET_STATUS  code=2 PORT_RESET  code=5 ENUMERATE */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <dob/server.h>
#include <dob/ipc.h>
#include <dob/types.h>
#include <dob/hotplug_driver.h>

/* PCI identification */
#define PCI_CLASS_SERIAL    0x0C
#define PCI_SUBCLASS_USB    0x03
#define PCI_PROGIF_EHCI     0x20

/* EHCI Capability registers (at MMIO base) */
#define EHCI_CAPLENGTH      0x00    /* 8-bit: offset to operational regs */
#define EHCI_HCIVERSION     0x02    /* 16-bit: interface version */
#define EHCI_HCSPARAMS      0x04    /* Structural parameters */
#define EHCI_HCCPARAMS      0x08    /* Capability parameters */

/* EHCI Operational registers (at MMIO base + CAPLENGTH) */
#define EHCI_USBCMD         0x00
#define EHCI_USBSTS         0x04
#define EHCI_USBINTR        0x08
#define EHCI_FRINDEX        0x0C
#define EHCI_CTRLDSSEGMENT  0x10
#define EHCI_PERIODICLIST   0x14
#define EHCI_ASYNCLIST      0x18
#define EHCI_CONFIGFLAG     0x40
#define EHCI_PORTSC(n)      (0x44 + (n) * 4)

/* USBCMD bits */
#define EHCI_CMD_RS         (1 << 0)    /* Run/Stop */
#define EHCI_CMD_HCRESET    (1 << 1)    /* Host Controller Reset */
#define EHCI_CMD_PSE        (1 << 4)    /* Periodic Schedule Enable */
#define EHCI_CMD_ASE        (1 << 5)    /* Async Schedule Enable */
#define EHCI_CMD_ITC_MASK   (0xFF << 16) /* Interrupt Threshold */

/* USBSTS bits */
#define EHCI_STS_USBINT     (1 << 0)
#define EHCI_STS_ERROR      (1 << 1)
#define EHCI_STS_PCD        (1 << 2)    /* Port Change Detect */
#define EHCI_STS_FLR        (1 << 3)    /* Frame List Rollover */
#define EHCI_STS_HSE        (1 << 4)    /* Host System Error */
#define EHCI_STS_ASS        (1 << 15)   /* Async Schedule Status */
#define EHCI_STS_HCH        (1 << 12)   /* HC Halted */

/* PORTSC bits */
#define EHCI_PORT_CCS       (1 << 0)    /* Current Connect Status */
#define EHCI_PORT_CSC       (1 << 1)    /* Connect Status Change */
#define EHCI_PORT_PE        (1 << 2)    /* Port Enable */
#define EHCI_PORT_PEC       (1 << 3)    /* Port Enable Change */
#define EHCI_PORT_OCA       (1 << 4)    /* Over-current Active */
#define EHCI_PORT_OCC       (1 << 5)    /* Over-current Change */
#define EHCI_PORT_FPR       (1 << 6)    /* Force Port Resume */
#define EHCI_PORT_SUSP      (1 << 7)    /* Suspend */
#define EHCI_PORT_RESET     (1 << 8)    /* Port Reset */
#define EHCI_PORT_LS_MASK   (3 << 10)   /* Line Status */
#define EHCI_PORT_PP        (1 << 12)   /* Port Power */
#define EHCI_PORT_PO        (1 << 13)   /* Port Owner (1=companion HC) */

/* Queue Head (simplified) */
typedef struct
{
    uint32_t horiz_link;
    uint32_t endpoint_chars;
    uint32_t endpoint_caps;
    uint32_t cur_qtd;
    /* Overlay area (transfer state) */
    uint32_t next_qtd;
    uint32_t alt_qtd;
    uint32_t token;
    uint32_t buffer[5];
} __attribute__((packed, aligned(32))) ehci_qh_t;

/* Queue Transfer Descriptor */
typedef struct
{
    uint32_t next;
    uint32_t alt;
    uint32_t token;
    uint32_t buffer[5];
} __attribute__((packed, aligned(32))) ehci_qtd_t;

static volatile uint32_t *mmio = NULL;
static volatile uint32_t *opregs = NULL;
static uint32_t num_ports = 0;
static uint32_t irq_port = 0;
static uint8_t  ehci_irq = 0;

/* ===== Real-hardware survival kit (EHCI edition) =====
 *
 * On real laptops the BIOS owns the EHCI controller through SMM, exactly
 * like the UHCI LEGSUP story: the ownership handshake lives in the
 * USBLEGSUP extended capability in PCI CONFIG space, found by walking the
 * capability chain whose first offset is HCCPARAMS[15:8] (EECP).
 *
 *   USBLEGSUP (EECP+0):  bit 16 = BIOS-owned semaphore
 *                        bit 24 = OS-owned semaphore
 *   USBLEGCTLSTS (EECP+4): SMI enables + R/WC SMI status
 *
 * Handoff (same as Linux ehci_bios_handoff in pci-quirks.c): set the
 * OS-owned bit, wait for the BIOS to drop its bit (it gets an SMI and
 * cleans up legacy keyboard/boot-device emulation), then write 0 to
 * USBLEGCTLSTS so no further SMI can fire. If the BIOS never lets go
 * within 1 s, force its bit clear — better a confused BIOS than a
 * controller we don't own.
 *
 * Why this driver exists at all in its current form: MainDOB has no real
 * EHCI transfer stack yet. Its ONLY job on real hardware is to take the
 * controller away from the BIOS, reset it, and leave CONFIGFLAG=0 so that
 * EVERY port routes to the companion UHCI controllers — where the fully
 * working UHCI driver enumerates devices, DAS-matches them and announces
 * them to hotplug. (BIOSes routinely leave CONFIGFLAG=1 behind for their
 * own legacy USB support; on an ICH8 laptop that single bit silently
 * swallows every inserted device.) Devices run at full speed instead of
 * high speed until a real EHCI stack lands; for detection + icon + mass
 * storage that is fully functional. */
#define EHCI_HCC_EECP(hcc)      (((hcc) >> 8) & 0xFF)
#define EHCI_CAP_ID_LEGSUP      0x01
#define EHCI_LEGSUP_BIOS_OWNED  (1u << 16)
#define EHCI_LEGSUP_OS_OWNED    (1u << 24)
#define EHCI_HANDOFF_TIMEOUT_MS 1000

static hotplug_device_t ehci_dev;       /* saved for PCI config access */

/* Timer-based delay: thread truly sleeps, zero CPU */
static void ehci_delay(uint32_t ms)
{
    int tid = timer_set(irq_port, ms, 0);
    dob_msg_t wm;
    memset(&wm, 0, sizeof(wm));
    dob_ipc_receive(irq_port, &wm);
    if (wm.type == 3) irq_done(ehci_irq);
    (void)tid;
}

/* Timer-based register wait */
static bool ehci_wait_reg(volatile uint32_t *reg, uint32_t mask,
                          uint32_t expected, uint32_t timeout_ms)
{
    for (uint32_t elapsed = 0; elapsed < timeout_ms; elapsed += 10)
    {
        if ((*reg & mask) == expected) return true;
        ehci_delay(10);
    }
    return (*reg & mask) == expected;
}

static void ehci_halt(void)
{
    opregs[EHCI_USBCMD / 4] &= ~EHCI_CMD_RS;
    ehci_wait_reg(&opregs[EHCI_USBSTS / 4], EHCI_STS_HCH, EHCI_STS_HCH, 100);
}

static void ehci_reset(void)
{
    ehci_halt();
    opregs[EHCI_USBCMD / 4] = EHCI_CMD_HCRESET;
    ehci_wait_reg(&opregs[EHCI_USBCMD / 4], EHCI_CMD_HCRESET, 0, 100);
}

static void ehci_port_reset(uint32_t port)
{
    if (port >= num_ports) return;

    volatile uint32_t *portsc = &opregs[EHCI_PORTSC(port) / 4];
    uint32_t val = *portsc;

    *portsc = val & ~EHCI_PORT_PE;
    ehci_delay(10);

    *portsc = (*portsc | EHCI_PORT_RESET) & ~EHCI_PORT_PE;
    ehci_delay(50);

    *portsc = *portsc & ~EHCI_PORT_RESET;
    ehci_delay(10);

    ehci_wait_reg(portsc, EHCI_PORT_PE, EHCI_PORT_PE, 100);

    *portsc |= EHCI_PORT_CSC | EHCI_PORT_PEC | EHCI_PORT_OCC;
}

/* Walk the EHCI extended-capability chain in PCI config space and perform
 * the BIOS->OS ownership handoff on the USBLEGSUP capability, then disable
 * every legacy SMI. Safe no-op when the capability is absent (QEMU) or the
 * BIOS never owned the controller. */
static void ehci_bios_handoff(hotplug_device_t *dev)
{
    uint32_t hcc  = mmio[EHCI_HCCPARAMS / 4];
    uint8_t  eecp = EHCI_HCC_EECP(hcc);
    int      guard = 0;     /* malformed chains must not loop forever */

    while (eecp >= 0x40 && guard++ < 8)
    {
        uint32_t cap = pci_config_read(dev->bus, dev->slot, dev->func, eecp);

        if ((cap & 0xFF) == EHCI_CAP_ID_LEGSUP)
        {
            char line[96];
            sprintf(line, "[ehci] USBLEGSUP@0x%02x = 0x%08x%s\n",
                    eecp, cap,
                    (cap & EHCI_LEGSUP_BIOS_OWNED) ? " (BIOS-owned)" : "");
            debug_print(line);

            /* Claim ownership: set the OS semaphore, keep the rest. */
            pci_config_write(dev->bus, dev->slot, dev->func, eecp,
                             cap | EHCI_LEGSUP_OS_OWNED);

            /* Wait for the BIOS to drop its semaphore (SMI handler runs). */
            uint32_t waited = 0;
            while (waited < EHCI_HANDOFF_TIMEOUT_MS)
            {
                cap = pci_config_read(dev->bus, dev->slot, dev->func, eecp);
                if (!(cap & EHCI_LEGSUP_BIOS_OWNED)) break;
                ehci_delay(10);
                waited += 10;
            }
            if (cap & EHCI_LEGSUP_BIOS_OWNED)
            {
                debug_print("[ehci] BIOS refused handoff; forcing it\n");
                pci_config_write(dev->bus, dev->slot, dev->func, eecp,
                                 (cap & ~EHCI_LEGSUP_BIOS_OWNED)
                                       | EHCI_LEGSUP_OS_OWNED);
            }

            /* Kill every legacy SMI source: enables off, R/WC status
             * acked (the status bits are write-1-to-clear, so writing
             * the register back to 0 in the enable half and 1s in the
             * status half clears everything; Linux just writes 0 to the
             * enable half — the pending bits become harmless once no
             * enable is set, which the dword write achieves). */
            pci_config_write(dev->bus, dev->slot, dev->func, eecp + 4, 0);
            return;
        }

        eecp = (cap >> 8) & 0xFF;   /* next capability pointer */
    }
}

static bool
ehci_init_hw(hotplug_device_t *dev)
{
    uint32_t mmio_phys = dev->bar[0] & 0xFFFFFFF0;
    if (!mmio_phys) return false;

    mmio = (volatile uint32_t *)mmap_phys(mmio_phys, 0x1000);
    if (!mmio) return false;

    uint8_t cap_length = ((volatile uint8_t *)mmio)[EHCI_CAPLENGTH];
    opregs = (volatile uint32_t *)((volatile uint8_t *)mmio + cap_length);

    uint32_t hcsparams = mmio[EHCI_HCSPARAMS / 4];
    num_ports = hcsparams & 0x0F;

    /* Port for the timer-based delays only. No irq_register: this driver
     * programs no schedules and enables no interrupts, so it owns no IRQ
     * line and never generates events. */
    irq_port = (uint32_t)port_create();

    ehci_dev = *dev;

    /* 1. Take the controller away from the BIOS (SMM). Must happen BEFORE
     *    any operational-register write: until the handoff, our writes race
     *    the SMM handler's. */
    ehci_bios_handoff(dev);

    /* 2. Halt + reset. HCRESET also clears CONFIGFLAG, but be explicit
     *    below — that single bit is the whole point of this driver. */
    ehci_halt();
    ehci_reset();

    /* 3. Quiet and out of the way: no interrupts, no schedules, RS=0,
     *    and CONFIGFLAG=0 so every port routes to the companion UHCI
     *    controllers. From here on the (fully working) UHCI driver sees
     *    connects/disconnects as resume events and runs the whole
     *    enumerate -> DAS -> hotplug-announce pipeline. */
    opregs[EHCI_USBINTR / 4]    = 0;
    opregs[EHCI_CONFIGFLAG / 4] = 0;

    {
        char line[96];
        sprintf(line, "[ehci] gatekeeper: %u ports routed to companion "
                      "UHCI (CONFIGFLAG=0)\n", num_ports);
        debug_print(line);
    }

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
            reply->arg0 = opregs[EHCI_USBSTS / 4];
            reply->arg1 = num_ports;
            return DOB_OK;

        case 2: /* PORT_RESET */
            ehci_port_reset(msg->arg0);
            return DOB_OK;

        case 5: /* ENUMERATE */
        {
            reply->arg0 = num_ports;
            uint32_t connected = 0;
            for (uint32_t i = 0; i < num_ports; i++)
            {
                if (opregs[EHCI_PORTSC(i) / 4] & EHCI_PORT_CCS)
                    connected++;
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

    debug_print("[ehci] v1.5.31 gatekeeper starting\n");

    /* Multi-instance: ICH8-era chipsets carry TWO EHCI functions. As a
     * singleton, the second instance died and its controller stayed
     * BIOS-owned with CONFIGFLAG possibly 1 — half the physical ports
     * silently swallowed. The registry arbitrates usb_ehci / usb_ehci_1
     * exactly as for the UHCI companions. */
    char svc[32];
    if (dob_server_init_unique("usb_ehci", svc, sizeof(svc)) != DOB_OK)
    {
        debug_print("[ehci] no free service slot; exiting\n");
        return 1;
    }

    /* Wait for hotplug's registry entry before asking it for our device —
     * the same guard usb_uhci / bga / ac97 carry. hotplug now registers
     * BEFORE spawning (see its step 1.5), so this normally returns
     * immediately; it remains as defence in depth against any future
     * reordering. Without it, on real ICH8 laptops this driver lost the
     * registration race, attach returned false, and — CONFIGFLAG never
     * cleared — the BIOS kept ownership of every physical port: the UHCI
     * companions saw empty ports forever (no icon, no USB). */
    dob_registry_wait("hotplug", 5000);

    if (dob_driver_attach(&dev))
    {
        if (ehci_init_hw(&dev))
            debug_print("[ehci] Initialized (gatekeeper: ports handed "
                        "to companion UHCI).\n");
        else
            debug_print("[ehci] Init failed.\n");
    }
    else
    {
        /* No device: we are a pure gatekeeper — without a controller there
         * is nothing to serve, and lingering in dob_server_loop() only
         * leaves a zombie ATTACHING bubble in hotplug. Exit; hotplug's
         * reaper handles a death-while-ATTACHING as "expected". */
        debug_print("[ehci] No device assigned; exiting.\n");
        _exit(1);
    }

    dob_server_register(handle_message);
    debug_print("[ehci] Ready.\n");
    dob_server_loop();
    return 0;
}
