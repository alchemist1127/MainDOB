#ifndef MAINDOB_DOB_HOTPLUG_DRIVER_H
#define MAINDOB_DOB_HOTPLUG_DRIVER_H

/*
 * How a driver module participates in the Dob modular system.
 *
 *   int main(void)
 *   {
 *       hotplug_device_t dev;
 *
 *       dob_server_init("ahci");               // create port, register
 *       if (dob_driver_attach(&dev))           // ask hotplug for our device
 *       {
 *           pci_enable_bus_master(&dev);
 *           mmio = mmap_phys(dev.bar[0], size);
 *           irq_register(dev.irq_line, port);
 *       }
 *
 *       dob_server_register(handler);
 *       dob_server_loop();
 *   }
 *
 * dob_server_init creates the port and registers with registry.
 * dob_driver_attach sends HOTPLUG_READY(our_port) to hotplug and
 * receives the hotplug_device_t back (BAR, IRQ, bus:slot:func).
 * One port, one registration. No duplicates.
 */

#include <dob/hotplug.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/server.h>
#include <unistd.h>
#include <string.h>

/*
 * Ask hotplug for our device assignment.
 * Call AFTER dob_server_init (which creates our port and registers us).
 * Returns true if device info received, false if hotplug not running.
 */
static inline bool
dob_driver_attach(hotplug_device_t *dev)
{
    if (!dev) return false;

    uint32_t hp_port = dob_registry_find("hotplug");
    if (!hp_port) return false;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = HOTPLUG_READY;
    msg.arg0 = dob_server_get_port();

    if (dob_ipc_call(hp_port, &msg, &reply) != 0)
        return false;

    if (reply.payload && reply.payload_size >= sizeof(hotplug_device_t))
    {
        memcpy(dev, reply.payload, sizeof(hotplug_device_t));
        return true;
    }
    return false;
}

/*
 * Check if an IPC message is a DETACH from hotplug.
 * If true, clean up hardware and _exit(0).
 */
static inline bool
dob_driver_is_detach(dob_msg_t *msg)
{
    return msg->code == HOTPLUG_DETACH;
}

/*
 * Tell hotplug we've released all resources. Call before _exit.
 */
static inline void
dob_driver_released(void)
{
    uint32_t hp_port = dob_registry_find("hotplug");
    if (!hp_port) return;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = HOTPLUG_RELEASED;
    dob_ipc_call(hp_port, &msg, &reply);
}

/*
 * Enable PCI bus mastering for the device.
 * Most DMA-capable devices need this.
 */
static inline void
pci_enable_bus_master(hotplug_device_t *dev)
{
    uint32_t addr = (1u << 31) | ((uint32_t)dev->bus << 16) |
                    ((uint32_t)dev->slot << 11) |
                    ((uint32_t)dev->func << 8) | 0x04;
    io_outl(0xCF8, addr);
    uint32_t cmd = io_inl(0xCFC);
    cmd |= 0x06;
    io_outl(0xCF8, addr);
    io_outl(0xCFC, cmd);
}

#endif
