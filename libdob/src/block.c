/* MainDOB Block Layer — implementation.
 *
 * See dob/block.h for the API surface. Internally we hold a static
 * table of up to BLOCK_MAX_DISKS entries, rebuilt by block_enumerate()
 * by polling each known disk-driver service in turn. Two adapters
 * ship today (ata, ahci) and a third slot in driver_classes[] is
 * ready for USB-MSC the day someone writes it.
 *
 * Per-driver adapter responsibilities:
 *   enumerate()  — call the driver's LIST_DISKS / LIST_PORTS opcode,
 *                   appending one block_disk_t per usable device into
 *                   the global table via block_alloc_slot().
 *   read/write/rescan — translate (selector, lba, count) into the
 *                       driver-native IPC arg layout, marshal payload.
 *
 * Why look up the service port on every call (no caching): the driver
 * lives in a hotplug bubble that can be torn down (sudden device
 * removal, driver crash) and respawned. A cached port becomes stale
 * silently; a fresh registry lookup costs one syscall and gives us
 * automatic recovery. If this ever shows up in a profile we'll add a
 * "cache + retry on DEAD" wrapper, but for the disk-utility class of
 * client (not on a sector-per-frame hot path) the cost is invisible.
 */

#include <string.h>
#include <dob/block.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/ata_protocol.h>
#include <dob/ahci_protocol.h>
#include <stdio.h>   /* snprintf (usbms service names) */

/* === Private types === */

typedef struct block_driver_class
{
    const char *display_name;       /* For diagnostics / future debug */
    const char *service_name;       /* Registry key */
    block_bus_t bus;
    bool (*enumerate)         (void);
    bool (*read)              (uint32_t sel, uint64_t lba, uint32_t cnt, void *buf);
    bool (*write)             (uint32_t sel, uint64_t lba, uint32_t cnt, const void *buf);
    bool (*rescan_partitions) (uint32_t sel);
    /* Optional. NULL means the driver does not (yet) expose SMART;
     * block_get_smart returns false without an IPC round-trip. */
    bool (*get_smart)         (uint32_t sel, void *buf512);
    /* Optional. NULL means the bus has no volatile write cache to commit
     * (or commits synchronously). block_flush() calls it to push a
     * removable device's cache to media before the user pulls it. */
    bool (*flush)             (uint32_t sel);
    /* Optional. NULL means the bus exposes no TRIM path. block_trim()
     * calls it once per <=65536-sector chunk. */
    bool (*trim)              (uint32_t sel, uint64_t lba, uint32_t cnt);
} block_driver_class_t;

/* === Global state === */

static block_disk_t g_disks[BLOCK_MAX_DISKS];
static int          g_disk_count = 0;

static block_disk_t *
block_alloc_slot(void)
{
    if (g_disk_count >= BLOCK_MAX_DISKS) return NULL;
    block_disk_t *d = &g_disks[g_disk_count];
    memset(d, 0, sizeof(*d));
    d->index       = g_disk_count;
    d->sector_size = BLOCK_SECTOR_SIZE;
    d->native_sector_size = BLOCK_SECTOR_SIZE;
    g_disk_count++;
    return d;
}

/* === Shared call helper ===
 *
 * Resolves the service port and issues one synchronous IPC call. The
 * caller hands in the message body (code + args + payload); we fill
 * in the reply struct. Returns true on DOB_OK from the driver. */
static bool
do_call(const char *service,
        uint32_t code,
        uint32_t a0, uint32_t a1, uint32_t a2,
        const void *in_payload, uint32_t in_size,
        dob_msg_t *reply)
{
    uint32_t port = dob_registry_find(service);
    if (!port) return false;

    dob_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.code         = code;
    msg.arg0         = a0;
    msg.arg1         = a1;
    msg.arg2         = a2;
    msg.payload      = (void *)in_payload;
    msg.payload_size = in_size;

    memset(reply, 0, sizeof(*reply));
    /* The contract above ("Returns true on DOB_OK from the driver") needs
     * BOTH conditions: the IPC round-trip must succeed AND the handler must
     * have returned DOB_OK in reply->code. sys_send returns only the
     * TRANSPORT status; the handler's verdict is copied into reply->code
     * separately. Checking transport alone turned every provider-side WRITE
     * error into a silent success — the read adapters were saved by their
     * payload check, the write adapters (ata/ahci/usbms) were not, producing
     * empty/truncated copies on a flaky stick and "formatted" volumes whose
     * sectors never landed. */
    if (dob_ipc_call(port, &msg, reply) != DOB_OK) return false;
    return reply->code == DOB_OK;
}

/* === Forward decls of the adapters === */
static const block_driver_class_t ata_class;
static const block_driver_class_t ahci_class;

/* === ATA adapter ===
 *
 * Wire convention (ata_protocol.h):
 *   READ/WRITE: arg0 = lba, arg1 = count, arg2 = disk-id (slot 0..3)
 *   IDENTIFY:                              arg2 = disk-id
 *   LIST_DISKS: no args, payload = ata_disk_info_t * ATA_MAX_DISKS
 *   RESCAN:     arg0 = disk-id
 */

static bool
ata_enumerate(void)
{
    dob_msg_t reply;
    if (!do_call("ata", ATA_OP_LIST_DISKS, 0, 0, 0, NULL, 0, &reply))
        return false;
    if (!reply.payload || reply.payload_size < sizeof(ata_disk_info_t))
        return false;

    const ata_disk_info_t *list = (const ata_disk_info_t *)reply.payload;
    uint32_t n = reply.arg0;
    if (n > ATA_MAX_DISKS) n = ATA_MAX_DISKS;

    for (uint32_t i = 0; i < n; i++)
    {
        if (!list[i].present) continue;
        block_disk_t *d = block_alloc_slot();
        if (!d) break;
        d->bus             = BLOCK_BUS_ATA;
        d->kind            = list[i].is_ssd ? BLOCK_KIND_SSD : BLOCK_KIND_HDD;
        d->trim_supported  = list[i].trim_supported != 0;
        d->total_sectors   = list[i].total_sectors;
        strncpy(d->model, list[i].model, sizeof(d->model) - 1);
        d->driver_class    = &ata_class;
        d->native_selector = i;
    }
    return true;
}

static bool
ata_read_op(uint32_t sel, uint64_t lba, uint32_t count, void *buf)
{
    /* ATA wire carries lba in 32 bits — caller (block_read) bounds-checks
     * against the disk's total_sectors so we don't actually exceed this. */
    if (lba > 0xFFFFFFFFu) return false;
    dob_msg_t reply;
    if (!do_call("ata", ATA_OP_READ,
                 (uint32_t)lba, count, sel,
                 NULL, 0, &reply))
        return false;
    uint32_t want = count * BLOCK_SECTOR_SIZE;
    if (!reply.payload || reply.payload_size < want) return false;
    memcpy(buf, reply.payload, want);
    return true;
}

static bool
ata_write_op(uint32_t sel, uint64_t lba, uint32_t count, const void *buf)
{
    if (lba > 0xFFFFFFFFu) return false;
    dob_msg_t reply;
    return do_call("ata", ATA_OP_WRITE,
                   (uint32_t)lba, count, sel,
                   buf, count * BLOCK_SECTOR_SIZE, &reply);
}

static bool
ata_rescan_op(uint32_t sel)
{
    dob_msg_t reply;
    return do_call("ata", ATA_OP_RESCAN_PARTITIONS,
                   sel, 0, 0, NULL, 0, &reply);
}

static bool
ata_smart_op(uint32_t sel, void *buf512)
{
    dob_msg_t reply;
    if (!do_call("ata", ATA_OP_GET_SMART,
                 0, 0, sel, NULL, 0, &reply))
        return false;
    if (!reply.payload || reply.payload_size < 512) return false;
    memcpy(buf512, reply.payload, 512);
    return true;
}

static bool
ata_trim_op(uint32_t sel, uint64_t lba, uint32_t count)
{
    /* ATA wire: arg0 = lba, arg1 = count, arg2 = disk-id. The driver only
     * has a DMA path (hence a DSM TRIM path) on slot 0, and rejects any
     * other selector itself. */
    if (lba > 0xFFFFFFFFu) return false;
    dob_msg_t reply;
    return do_call("ata", ATA_OP_TRIM,
                   (uint32_t)lba, count, sel, NULL, 0, &reply);
}

static const block_driver_class_t ata_class =
{
    .display_name      = "ATA/IDE",
    .service_name      = "ata",
    .bus               = BLOCK_BUS_ATA,
    .enumerate         = ata_enumerate,
    .read              = ata_read_op,
    .write             = ata_write_op,
    .rescan_partitions = ata_rescan_op,
    .get_smart         = ata_smart_op,
    .trim              = ata_trim_op,
};

/* === AHCI adapter ===
 *
 * Wire convention (ahci_protocol.h):
 *   READ/WRITE: arg0 = port, arg1 = lba, arg2 = count
 *   IDENTIFY:   arg0 = port
 *   LIST_PORTS: no args, payload = sata_port_info_t * N (only present
 *               ports), reply.arg0 = N
 *   RESCAN:     arg0 = port
 *
 * Note port_num is NOT the same as the array index — AHCI's LIST_PORTS
 * only returns present ports, but their port_num is the absolute port
 * number on the HBA (0..AHCI_MAX_PORTS-1). We carry port_num as the
 * native_selector so subsequent calls go to the right place even if
 * an HBA exposes ports non-contiguously.
 */

/* Multi-controller AHCI: one driver instance per controller, registered as
 * "ahci" (primary: the one with block devices) and "ahci_N" (secondaries).
 * The block layer namespaces the selector like usbms does: bits 8+ carry
 * the instance index, low 8 bits the AHCI port. Every op decodes it. */
#define AHCI_MAX_INSTANCES 4
static void ahci_svc_of_sel(uint32_t sel, char *out, int cap)
{
    uint32_t inst = sel >> 8;
    if (inst == 0) snprintf(out, (size_t)cap, "ahci");
    else           snprintf(out, (size_t)cap, "ahci_%u", (unsigned)inst);
}

static bool
ahci_enumerate_one(uint32_t inst)
{
    char svc[16];
    ahci_svc_of_sel(inst << 8, svc, sizeof(svc));
    if (!dob_registry_find(svc))          /* absent instance: skip quietly */
        return inst == 0 ? false : true;

    dob_msg_t reply;
    if (!do_call(svc, AHCI_OP_LIST_PORTS, 0, 0, 0, NULL, 0, &reply))
        return false;
    if (!reply.payload) return false;

    uint32_t n = reply.arg0;
    if (n > AHCI_MAX_PORTS) n = AHCI_MAX_PORTS;
    if (reply.payload_size < n * sizeof(sata_port_info_t)) return false;

    const sata_port_info_t *list = (const sata_port_info_t *)reply.payload;
    for (uint32_t i = 0; i < n; i++)
    {
        /* Only HDD/SSD are block devices — optical lives elsewhere. */
        if (list[i].type != AHCI_DEV_HDD && list[i].type != AHCI_DEV_SSD)
            continue;

        block_disk_t *d = block_alloc_slot();
        if (!d) break;
        d->bus             = BLOCK_BUS_SATA;
        d->kind            = (list[i].type == AHCI_DEV_SSD)
                             ? BLOCK_KIND_SSD : BLOCK_KIND_HDD;
        d->trim_supported  = list[i].trim_supported;
        d->total_sectors   = list[i].sector_count;
        strncpy(d->model, list[i].model, sizeof(d->model) - 1);
        d->driver_class    = &ahci_class;
        d->native_selector = (inst << 8) | list[i].port_num;
    }
    return true;
}

static bool
ahci_enumerate(void)
{
    bool any = false;
    for (uint32_t inst = 0; inst < AHCI_MAX_INSTANCES; inst++)
        any |= ahci_enumerate_one(inst);
    return any;
}

static bool
ahci_read_op(uint32_t sel, uint64_t lba, uint32_t count, void *buf)
{
    if (lba > 0xFFFFFFFFu) return false;
    char svc[16]; ahci_svc_of_sel(sel, svc, sizeof(svc));
    dob_msg_t reply;
    if (!do_call(svc, AHCI_OP_READ,
                 sel & 0xFFu, (uint32_t)lba, count,
                 NULL, 0, &reply))
        return false;
    uint32_t want = count * BLOCK_SECTOR_SIZE;
    if (!reply.payload || reply.payload_size < want) return false;
    memcpy(buf, reply.payload, want);
    return true;
}

static bool
ahci_write_op(uint32_t sel, uint64_t lba, uint32_t count, const void *buf)
{
    if (lba > 0xFFFFFFFFu) return false;
    char svc[16]; ahci_svc_of_sel(sel, svc, sizeof(svc));
    dob_msg_t reply;
    return do_call(svc, AHCI_OP_WRITE,
                   sel & 0xFFu, (uint32_t)lba, count,
                   buf, count * BLOCK_SECTOR_SIZE, &reply);
}

static bool
ahci_rescan_op(uint32_t sel)
{
    char svc[16]; ahci_svc_of_sel(sel, svc, sizeof(svc));
    dob_msg_t reply;
    return do_call(svc, AHCI_OP_RESCAN_PARTITIONS,
                   sel & 0xFFu, 0, 0, NULL, 0, &reply);
}

/* Last SMART failure diagnostics (see block_get_smart_diag). Written by
 * ahci_smart_op from the driver reply's args; single-threaded per the
 * library's existing usage contract (DobDisk's action handlers). */
static uint32_t g_smart_diag[4];

static bool
ahci_smart_op(uint32_t sel, void *buf512)
{
    char svc[16]; ahci_svc_of_sel(sel, svc, sizeof(svc));
    dob_msg_t reply;
    memset(g_smart_diag, 0, sizeof(g_smart_diag));
    if (!do_call(svc, AHCI_OP_GET_SMART,
                 sel & 0xFFu, 0, 0, NULL, 0, &reply))
    {
        /* do_call fills `reply` even on a handler error code — the AHCI
         * driver ships reason/PxTFD/PxSERR/step in the args. On pure
         * transport failure the memset above leaves NONE/zeros. */
        g_smart_diag[0] = reply.arg0;
        g_smart_diag[1] = reply.arg1;
        g_smart_diag[2] = reply.arg2;
        g_smart_diag[3] = reply.arg3;
        return false;
    }
    if (!reply.payload || reply.payload_size < 512) return false;
    memcpy(buf512, reply.payload, 512);
    return true;
}

static bool
ahci_trim_op(uint32_t sel, uint64_t lba, uint32_t count)
{
    /* AHCI wire: arg0 = port, arg1 = lba, arg2 = count. The driver
     * validates the port is an SSD that advertises TRIM. */
    if (lba > 0xFFFFFFFFu) return false;
    char svc[16]; ahci_svc_of_sel(sel, svc, sizeof(svc));
    dob_msg_t reply;
    return do_call(svc, AHCI_OP_TRIM,
                   sel & 0xFFu, (uint32_t)lba, count, NULL, 0, &reply);
}

static const block_driver_class_t ahci_class =
{
    .display_name      = "SATA/AHCI",
    .service_name      = "ahci",
    .bus               = BLOCK_BUS_SATA,
    .enumerate         = ahci_enumerate,
    .read              = ahci_read_op,
    .write             = ahci_write_op,
    .rescan_partitions = ahci_rescan_op,
    .get_smart         = ahci_smart_op,
    .trim              = ahci_trim_op,
};

/* === USB mass storage adapter ===
 *
 * One service per stick: "usbms_<port>" (the usb_mass_storage
 * sub-driver, one process per controller port — the cdrom model).
 * The sub-driver already normalizes ANY native block size (512/1024/
 * 2048/4096) to a 512-byte virtual view, so this adapter speaks plain
 * BLOCK_SECTOR_SIZE like everyone else. Argument layout differs from
 * ata/ahci: usbms takes arg0=lba, arg1=count (no selector — the
 * service IS the device). Per-request ceiling is 32 sectors, so reads
 * and writes chunk. */
/* usbms service tokens are (controller_instance << 4) | port — up to 8
 * UHCI instances (Extensa 5220: five on ICH8) x 2 root ports each. The
 * scan probes the sparse token space; registry misses are cheap. */
#define USBMS_MAX_TOKEN     ((7u << 4) | 1u)   /* 0..113, sparse */
#define USBMS_CHUNK_SECT    32

static void usbms_svc_name(uint32_t sel, char *out, int cap)
{
    snprintf(out, (size_t)cap, "usbms_%u", (unsigned)sel);
}

static bool
usbms_read_op(uint32_t sel, uint64_t lba, uint32_t count, void *buf)
{
    char svc[16];
    usbms_svc_name(sel, svc, sizeof(svc));
    uint8_t *dst = (uint8_t *)buf;
    while (count)
    {
        uint32_t n = (count > USBMS_CHUNK_SECT) ? USBMS_CHUNK_SECT : count;
        dob_msg_t reply;
        if (!do_call(svc, 1, (uint32_t)lba, n, 0, NULL, 0, &reply))
            return false;
        uint32_t want = n * BLOCK_SECTOR_SIZE;
        if (!reply.payload || reply.payload_size < want) return false;
        memcpy(dst, reply.payload, want);
        dst += want; lba += n; count -= n;
    }
    return true;
}

static bool
usbms_write_op(uint32_t sel, uint64_t lba, uint32_t count, const void *buf)
{
    char svc[16];
    usbms_svc_name(sel, svc, sizeof(svc));
    const uint8_t *src = (const uint8_t *)buf;
    while (count)
    {
        uint32_t n = (count > USBMS_CHUNK_SECT) ? USBMS_CHUNK_SECT : count;
        dob_msg_t reply;
        if (!do_call(svc, 2, (uint32_t)lba, n, 0,
                     src, n * BLOCK_SECTOR_SIZE, &reply))
            return false;
        src += n * BLOCK_SECTOR_SIZE; lba += n; count -= n;
    }
    return true;
}

static bool
usbms_rescan_op(uint32_t sel)
{
    /* USB volumes surface on the next pendrive-icon click (op 66
     * mounts on demand, the cdrom model) — nothing to push here. */
    (void)sel;
    return true;
}

static bool usbms_enumerate(void);

/* Push the stick's volatile write cache to NAND (SCSI SYNCHRONIZE CACHE via
 * usbms op 4). Best-effort; a failure must not abort a format. */
static bool
usbms_flush_op(uint32_t sel)
{
    char svc[16];
    usbms_svc_name(sel, svc, sizeof(svc));
    dob_msg_t reply;
    return do_call(svc, 4 /* FLUSH = SYNCHRONIZE CACHE */, 0, 0, 0, NULL, 0, &reply);
}

/* Traduzione handle opaco -> (servizio, selector nativo). Vive QUI,
 * accanto alle classi: ogni bus riusa il proprio naming esistente
 * (ahci_svc_of_sel, usbms_%u), l'encoding non esce mai dal layer. */
bool block_provider_binding(int i, char *service_out, uint32_t cap,
                            uint32_t *native_selector_out)
{
    const block_disk_t *d = block_get(i);
    if (d == NULL || service_out == NULL || cap == 0 ||
        native_selector_out == NULL)
    {
        return false;
    }

    switch (d->bus)
    {
    case BLOCK_BUS_SATA:
        /* Bit alti = istanza ("ahci"/"ahci_N"), bassi = porta reale. */
        ahci_svc_of_sel(d->native_selector, service_out, (int)cap);
        *native_selector_out = d->native_selector & 0xFFu;
        return true;

    case BLOCK_BUS_USB:
        /* Un servizio per stick: il selector e' gia' nativo (LUN 0). */
        snprintf(service_out, (size_t)cap, "usbms_%u",
                 (unsigned)d->native_selector);
        *native_selector_out = 0;
        return true;

    case BLOCK_BUS_ATA:
    default:
        snprintf(service_out, (size_t)cap, "%s",
                 d->driver_class->service_name);
        *native_selector_out = d->native_selector;
        return true;
    }
}

static const block_driver_class_t usbms_class =
{
    .enumerate         = usbms_enumerate,
    .read              = usbms_read_op,
    .write             = usbms_write_op,
    .rescan_partitions = usbms_rescan_op,
    .flush             = usbms_flush_op,
};

static bool
usbms_enumerate(void)
{
    for (uint32_t p = 0; p <= USBMS_MAX_TOKEN; p++)
    {
        if ((p & 0x0F) > 1) { p |= 0x0F; continue; } /* ports are 0..1 */
        char svc[16];
        usbms_svc_name(p, svc, sizeof(svc));
        if (!dob_registry_find(svc)) continue;

        dob_msg_t reply;
        if (!do_call(svc, 3, 0, 0, 0, NULL, 0, &reply)) continue;
        if (reply.arg0 == 0) continue;   /* bring-up incomplete */

        block_disk_t *d = block_alloc_slot();
        if (!d) break;
        d->bus             = BLOCK_BUS_USB;
        d->kind            = BLOCK_KIND_UNKNOWN;
        d->total_sectors   = reply.arg0;
        d->native_sector_size = (reply.arg1 == 4096u) ? 4096u : 512u;
        snprintf(d->model, sizeof(d->model),
                 "Pendrive USB (ctrl %u porta %u, blocco %u B)",
                 (unsigned)(p >> 4), (unsigned)(p & 0x0F),
                 (unsigned)reply.arg1);
        d->driver_class    = &usbms_class;
        d->native_selector = p;
    }
    return true;
}

/* === Driver class registry ===
 *
 * Extending: drop a new block_driver_class_t (with enumerate / read /
 * write / rescan_partitions populated) for the new bus type into this
 * array. No other changes needed in block.c — the rest of the code
 * is fully table-driven. */
static const block_driver_class_t *driver_classes[] =
{
    &ata_class,
    &ahci_class,
    &usbms_class,
    NULL,
};

/* === Public API === */

int
block_enumerate(void)
{
    g_disk_count = 0;
    for (int k = 0; driver_classes[k] != NULL; k++)
    {
        /* Missing service is fine — driver just isn't loaded right
         * now. Adapter returns false, we move on. */
        (void)driver_classes[k]->enumerate();
    }
    return g_disk_count;
}

int
block_count(void)
{
    return g_disk_count;
}

const block_disk_t *
block_get(int i)
{
    if (i < 0 || i >= g_disk_count) return NULL;
    return &g_disks[i];
}

bool
block_read(int i, uint64_t lba, uint32_t count, void *buf)
{
    const block_disk_t *d = block_get(i);
    if (!d || !d->driver_class || !d->driver_class->read) return false;
    if (count == 0)                                       return false;
    if (lba + count > d->total_sectors)                   return false;
    return d->driver_class->read(d->native_selector, lba, count, buf);
}

bool
block_write(int i, uint64_t lba, uint32_t count, const void *buf)
{
    const block_disk_t *d = block_get(i);
    if (!d || !d->driver_class || !d->driver_class->write) return false;
    if (count == 0)                                        return false;
    if (lba + count > d->total_sectors)                    return false;
    return d->driver_class->write(d->native_selector, lba, count, buf);
}

bool
block_rescan_partitions(int i)
{
    const block_disk_t *d = block_get(i);
    if (!d || !d->driver_class || !d->driver_class->rescan_partitions)
        return false;
    return d->driver_class->rescan_partitions(d->native_selector);
}

bool
block_flush(int i)
{
    const block_disk_t *d = block_get(i);
    if (!d || !d->driver_class) return false;
    if (!d->driver_class->flush) return true;   /* no volatile cache on this bus */
    return d->driver_class->flush(d->native_selector);
}

bool
block_get_smart(int i, void *buf512)
{
    if (!buf512) return false;
    const block_disk_t *d = block_get(i);
    if (!d || !d->driver_class || !d->driver_class->get_smart)
        return false;
    return d->driver_class->get_smart(d->native_selector, buf512);
}

bool
block_get_smart_diag(int i, void *buf512, uint32_t diag[4])
{
    if (diag) memset(diag, 0, 4 * sizeof(uint32_t));
    memset(g_smart_diag, 0, sizeof(g_smart_diag));
    bool ok = block_get_smart(i, buf512);
    if (!ok && diag)
        memcpy(diag, g_smart_diag, sizeof(g_smart_diag));
    return ok;
}

bool
block_trim(int i, uint64_t lba, uint32_t count)
{
    const block_disk_t *d = block_get(i);
    if (!d || !d->driver_class || !d->driver_class->trim) return false;
    if (count == 0)                     return false;
    if (lba + count > d->total_sectors) return false;   /* never past end */

    /* One DATA SET MANAGEMENT range entry covers at most 65536 sectors
     * (the 16-bit count field, 0 == 65536). Split a larger range into
     * back-to-back driver calls. */
    while (count)
    {
        uint32_t n = (count > 65536u) ? 65536u : count;
        if (!d->driver_class->trim(d->native_selector, lba, n))
            return false;
        lba   += n;
        count -= n;
    }
    return true;
}
