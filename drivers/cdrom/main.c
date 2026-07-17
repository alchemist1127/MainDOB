/* MainDOB CDROM driver — one process per ATAPI slot.
 *
 * Spawned by the DAS with a single argv: "<provider>:<token>" where
 * provider is "ata" or "ahci" and token is the slot index. From that
 * we derive bus port, SCSI opcode, and our own service name once in
 * main(). ISO9660 parsing is delegated to iso9660.mem. */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <dob/server.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/types.h>
#include <dob/mem.h>

#include <DobFileSystem.h>
#include <DobFiles.h>

#include "../../boot/DobFileSystem/dobfs_protocol.h"
#include "scsi.h"
#include "iso9660_api.h"

/* DobFS protocol extension — mirrors FLOPPY_PROBE_MEDIA so the DAS
 * action is identical for floppy and cdrom. */
#define CDROM_PROBE_MEDIA  64
#define CDROM_EJECT        65

#define MAX_OPEN_FDS       16
#define MAX_UNMOUNT_SUBS    4
#define MEM_BLOB_MAX    65536    /* iso9660.mem is ~10 KB; 64 K is plenty */

/* ===== Bus provider table ===== */

typedef struct
{
    const char *name;       /* registry name */
    uint32_t    opcode;     /* IPC opcode for ATAPI PACKET */
} bus_provider_t;

static const bus_provider_t providers[] = {
    { "ata",  100 },
    { "ahci",   5 },
};
#define PROVIDER_COUNT (sizeof(providers) / sizeof(providers[0]))

/* ===== State ===== */

typedef struct
{
    char                  service_name[32];   /* "cdrom_ata_2" / "cdrom_ahci_0" */
    const bus_provider_t *bus;
    char                  bus_name[16];   /* FULL provider as spawned ("ahci",
                                           * "ahci_1", ...): registry lookups
                                           * must use this, not bus->name — a
                                           * secondary AHCI instance registers
                                           * with a suffix. bus->name only
                                           * selects the opcode dialect. */
    uint32_t              bus_port;
    uint32_t              slot;
} identity_t;

typedef struct
{
    bool       in_use;
    bool       stale;
    iso_file_t f;
} cd_fd_t;

typedef struct
{
    bool          mounted;
    iso_volume_t *volume;
    cd_fd_t       fds[MAX_OPEN_FDS];
    uint32_t      sub_ports[MAX_UNMOUNT_SUBS];
    uint8_t       sub_count;
} mount_t;

static identity_t     id;
static mount_t        m;
static iso9660_api_t *iso;
static uint8_t        sector_buf[8192];

/* ===== SCSI transport ===== */

/* Ship one 12-byte CDB to the bus driver and copy any data-in back. */
static int scsi_issue(const uint8_t cdb[12], uint32_t alloc,
                      uint8_t *data_out, uint32_t *out_transferred)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code         = id.bus->opcode;
    msg.arg0         = id.slot;
    msg.arg1         = alloc;
    msg.payload      = (void *)cdb;
    msg.payload_size = 12;

    if (dob_ipc_call(id.bus_port, &msg, &reply) != DOB_OK) return -1;
    if ((int32_t)reply.code < 0) return -1;

    uint32_t got = reply.arg0;
    if (data_out && got > 0)
    {
        if (got > alloc) got = alloc;
        memcpy(data_out, reply.payload, got);
    }
    if (out_transferred) *out_transferred = got;
    return 0;
}

/* Translate the last failure into a DOB_ERR_* via REQUEST SENSE. */
static dob_status_t scsi_classify_sense(void)
{
    uint8_t cdb[12], sense[18];
    uint32_t got = 0;

    scsi_cdb_request_sense(cdb, 18);
    if (scsi_issue(cdb, 18, sense, &got) != 0) return DOB_ERR_HW_FAULT;
    if (got < 8) return DOB_ERR_HW_FAULT;

    uint8_t key, asc;
    scsi_parse_sense(sense, got, &key, &asc);

    if (key == SENSE_NOT_READY      && asc == ASC_MEDIUM_NOT_PRESENT)      return DOB_ERR_NO_MEDIA;
    if (key == SENSE_UNIT_ATTENTION && asc == ASC_MEDIUM_MAY_HAVE_CHANGED) return DOB_ERR_MEDIA_CHANGED;
    if (key == SENSE_NO_SENSE)                                              return DOB_OK;
    return DOB_ERR_HW_FAULT;
}

/* Spin TUR until the drive answers ready; ~5 s budget covers spin-up. */
static dob_status_t scsi_wait_ready(void)
{
    uint8_t cdb[12];
    dob_status_t last = DOB_ERR_HW_FAULT;

    for (int tries = 0; tries < 25; tries++)
    {
        scsi_cdb_test_unit_ready(cdb);
        if (scsi_issue(cdb, 0, NULL, NULL) == 0) return DOB_OK;

        last = scsi_classify_sense();
        if (last == DOB_ERR_MEDIA_CHANGED) continue;
        if (last == DOB_ERR_NO_MEDIA)
        {
            if (tries < 15) { sleep_ms(200); continue; }
            return DOB_ERR_NO_MEDIA;
        }
        sleep_ms(100);
    }
    return last == DOB_ERR_NO_MEDIA ? DOB_ERR_NO_MEDIA : DOB_ERR_HW_FAULT;
}

/* iso9660.mem callback: one ISO sector → one SCSI READ(10). */
static int rdsec_cb(void *ctx, uint32_t lba, void *buf)
{
    (void)ctx;
    uint8_t cdb[12];
    scsi_cdb_read10(cdb, lba);

    uint32_t got = 0;
    if (scsi_issue(cdb, ISO_SECTOR_SIZE, (uint8_t *)buf, &got) != 0) return -1;
    if (got < ISO_SECTOR_SIZE) return -1;
    return 0;
}

/* ===== Mount lifecycle ===== */

/* Fire-and-forget UNMOUNT_NOTIFY to every subscribed satellite. */
static void notify_unmount(void)
{
    for (uint8_t i = 0; i < m.sub_count; i++)
    {
        dob_msg_t n = {0};
        n.code = DOBFS_UNMOUNT_NOTIFY;
        n.arg0 = id.slot;
        (void)dob_ipc_post(m.sub_ports[i], &n);
    }
    m.sub_count = 0;
}

static void invalidate_fds(void)
{
    for (int i = 0; i < MAX_OPEN_FDS; i++)
        if (m.fds[i].in_use) m.fds[i].stale = true;
}

/* Drop all per-mount state. Idempotent. */
static void tear_down_mount(void)
{
    if (!m.mounted) return;
    notify_unmount();
    invalidate_fds();
    if (m.volume) iso->unmount(m.volume);
    m.volume  = NULL;
    m.mounted = false;
}

/* Tear down any prior mount, wait for the disc, hand it to iso9660. */
static dob_status_t do_mount(void)
{
    tear_down_mount();

    dob_status_t rc = scsi_wait_ready();
    if (rc != DOB_OK) return rc;

    iso_volume_t *v = iso->mount(rdsec_cb, NULL);
    if (!v) return DOB_ERR_BAD_FS;

    m.volume  = v;
    m.mounted = true;
    memset(m.fds, 0, sizeof(m.fds));
    return DOB_OK;
}

/* ===== DobFS request handlers ===== */

static dob_status_t op_open(dob_msg_t *msg, dob_msg_t *reply)
{
    if (!m.mounted) return DOB_ERR_NO_MEDIA;
    if (!msg->payload || msg->payload_size == 0) return DOB_ERR_INVALID;

    int fd = -1;
    for (int i = 0; i < MAX_OPEN_FDS; i++)
        if (!m.fds[i].in_use) { fd = i; break; }
    if (fd < 0) return DOB_ERR_NO_MEMORY;

    iso_file_t f;
    int rc = iso->open(m.volume, (const char *)msg->payload, &f);
    if (rc == -2) return DOB_ERR_HW_FAULT;
    if (rc != 0)  return DOB_ERR_INVALID;

    m.fds[fd].in_use = true;
    m.fds[fd].stale  = false;
    m.fds[fd].f      = f;
    reply->arg0 = (uint32_t)fd;
    return DOB_OK;
}

static dob_status_t op_read(dob_msg_t *msg, dob_msg_t *reply)
{
    int fd = (int)msg->arg0;
    if (fd < 0 || fd >= MAX_OPEN_FDS) return DOB_ERR_INVALID;
    if (!m.fds[fd].in_use)            return DOB_ERR_INVALID;
    if (m.fds[fd].stale)              return DOB_ERR_MEDIA_CHANGED;

    uint32_t n = msg->arg1;
    if (n > sizeof(sector_buf)) n = sizeof(sector_buf);

    int got = iso->read(m.volume, &m.fds[fd].f, sector_buf, n);
    if (got < 0) { m.fds[fd].stale = true; return DOB_ERR_HW_FAULT; }

    reply->payload      = sector_buf;
    reply->payload_size = (uint32_t)got;
    reply->arg0         = (uint32_t)got;
    return DOB_OK;
}

static dob_status_t op_close(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    int fd = (int)msg->arg0;
    if (fd < 0 || fd >= MAX_OPEN_FDS) return DOB_ERR_INVALID;
    m.fds[fd].in_use = false;
    return DOB_OK;
}

static dob_status_t op_stat(dob_msg_t *msg, dob_msg_t *reply)
{
    if (!m.mounted) return DOB_ERR_NO_MEDIA;
    if (!msg->payload || msg->payload_size == 0) return DOB_ERR_INVALID;

    iso_stat_t st;
    int rc = iso->stat(m.volume, (const char *)msg->payload, &st);
    if (rc == -2) return DOB_ERR_HW_FAULT;
    if (rc != 0)  return DOB_ERR_INVALID;

    reply->arg0 = st.size;
    reply->arg1 = st.is_dir ? 1 : 0;
    return DOB_OK;
}

/* Reply: "name\tD|F\tsize\n" lines, parsed by the DobFileSystem stub. */
static dob_status_t op_readdir(dob_msg_t *msg, dob_msg_t *reply)
{
    if (!m.mounted) return DOB_ERR_NO_MEDIA;
    if (!msg->payload || msg->payload_size == 0) return DOB_ERR_INVALID;

    static iso_dirent_t ents[128];
    int n = iso->readdir(m.volume, (const char *)msg->payload,
                         ents, (int)(sizeof(ents) / sizeof(ents[0])));
    if (n < 0) return DOB_ERR_HW_FAULT;

    static char out_buf[4096];
    uint32_t off = 0;
    for (int i = 0; i < n; i++)
    {
        int wrote = snprintf(out_buf + off, sizeof(out_buf) - off,
                             "%s\t%c\t%u\n",
                             ents[i].name,
                             ents[i].is_dir ? 'D' : 'F',
                             (unsigned)ents[i].size);
        if (wrote <= 0 || (uint32_t)wrote >= sizeof(out_buf) - off) break;
        off += (uint32_t)wrote;
    }

    reply->payload      = out_buf;
    reply->payload_size = off;
    reply->arg0         = (uint32_t)n;
    return DOB_OK;
}

static dob_status_t op_subscribe_unmount(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    uint32_t port = msg->arg0;
    if (!port) return DOB_ERR_INVALID;

    /* Idempotent — re-subscribing with the same port is a no-op. */
    for (uint8_t i = 0; i < m.sub_count; i++)
        if (m.sub_ports[i] == port) return DOB_OK;
    if (m.sub_count >= MAX_UNMOUNT_SUBS) return DOB_ERR_NO_MEMORY;
    m.sub_ports[m.sub_count++] = port;
    return DOB_OK;
}

/* DAS calls this on icon activation; mounts the disc and opens a
 * DobFiles satellite bound to our service. */
static dob_status_t op_probe_media(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    dob_status_t rc = do_mount();
    if (rc != DOB_OK) return rc;
    /* arg0 = hijack target port (0 = spawn fresh window, !=0 =
     * route the mount to that specific DobFiles instance — see
     * dobfiles_OpenMount comment). Forwarded from ICON_ACTIVATED
     * via the DAS ipc_call primitive. */
    (void)dobfiles_OpenMount(id.service_name, "/", msg->arg0);
    return DOB_OK;
}

static dob_status_t op_eject(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)msg; (void)reply;
    tear_down_mount();

    uint8_t cdb[12];
    scsi_cdb_eject(cdb);
    (void)scsi_issue(cdb, 0, NULL, NULL);
    return DOB_OK;
}

/* ===== Dispatcher ===== */

static dob_status_t cdrom_dispatch(dob_msg_t *msg, dob_msg_t *reply)
{
    switch (msg->code)
    {
        case DOBFS_OPEN:              return op_open(msg, reply);
        case DOBFS_READ:              return op_read(msg, reply);
        case DOBFS_CLOSE:             return op_close(msg, reply);
        case DOBFS_STAT:              return op_stat(msg, reply);
        case DOBFS_READDIR:           return op_readdir(msg, reply);
        case DOBFS_SUBSCRIBE_UNMOUNT: return op_subscribe_unmount(msg, reply);
        case CDROM_PROBE_MEDIA:       return op_probe_media(msg, reply);
        case CDROM_EJECT:             return op_eject(msg, reply);
        default:                      return DOB_ERR_NOT_FOUND;
    }
}

/* ===== Bootstrap ===== */

/* Look up provider by name. NULL if unknown. */
/* PREFIX match: a secondary AHCI controller registers as "ahci_1" etc.;
 * its opcode dialect is still "ahci". Exact-or-prefix-plus-'_' so a future
 * "ahcix" provider can't false-match. */
static const bus_provider_t *provider_lookup(const char *name, uint32_t name_len)
{
    for (uint32_t i = 0; i < PROVIDER_COUNT; i++)
    {
        const char *n = providers[i].name;
        uint32_t nl = (uint32_t)strlen(n);
        if (name_len >= nl && memcmp(n, name, nl) == 0 &&
            (name_len == nl || name[nl] == '_'))
            return &providers[i];
    }
    return NULL;
}

/* Parse "ata:2" / "ahci:0" into id. Returns false on malformed input. */
static bool parse_identity(const char *spec)
{
    if (!spec || !*spec) return false;
    const char *colon = strchr(spec, ':');
    if (!colon || colon == spec) return false;

    uint32_t plen = (uint32_t)(colon - spec);
    id.bus = provider_lookup(spec, plen);
    if (!id.bus) return false;
    if (plen >= sizeof(id.bus_name)) return false;
    memcpy(id.bus_name, spec, plen);
    id.bus_name[plen] = '\0';           /* FULL name for registry lookups */

    uint32_t tok = 0;
    for (const char *p = colon + 1; *p; p++)
    {
        if (*p < '0' || *p > '9') return false;
        tok = tok * 10 + (uint32_t)(*p - '0');
    }
    id.slot = tok;

    snprintf(id.service_name, sizeof(id.service_name),
             "cdrom_%s_%u", id.bus_name, (unsigned)tok);
    return true;
}

/* Read iso9660.mem in one shot into a fixed buffer. */
static uint32_t load_mem_blob(const char *path, uint8_t *buf, uint32_t cap)
{
    int fd = dobfs_Open(path, FS_READ);
    if (fd < 0) return 0;

    uint32_t total = 0;
    int got;
    while (total < cap && (got = dobfs_Read(fd, buf + total, cap - total)) > 0)
        total += (uint32_t)got;
    dobfs_Close(fd);

    /* If we filled the buffer to the brim, the file is bigger than we
     * planned for — reject rather than load a truncated .mem. */
    return (total == cap) ? 0 : total;
}

int main(int argc, char **argv)
{
    if (argc < 2 || !argv[1] || !parse_identity(argv[1]))
    {
        debug_print("[cdrom] missing/invalid identity argv\n");
        return 1;
    }

    /* Refuse a duplicate instance for this slot. */
    if (dob_registry_find(id.service_name)) return 0;

    id.bus_port = dob_registry_find(id.bus_name);
    if (!id.bus_port)
    {
        debug_print("[cdrom] bus driver not registered\n");
        return 1;
    }

    static uint8_t blob[MEM_BLOB_MAX];
    uint32_t blob_size = load_mem_blob(
        "/SYSTEM/DRIVERS/cdrom/iso9660.mem", blob, sizeof(blob));
    if (!blob_size)
    {
        debug_print("[cdrom] cannot read iso9660.mem\n");
        return 1;
    }

    iso = (iso9660_api_t *)dob_mem_load(blob, blob_size);
    if (!iso)
    {
        debug_print("[cdrom] dob_mem_load failed\n");
        return 1;
    }

    dob_server_init(id.service_name);
    dob_server_register(cdrom_dispatch);
    dob_server_loop();
    return 0;
}
