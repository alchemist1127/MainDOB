/* MainDOB Partition Library — implementation.
 *
 * Layer A (MBR pure ops) is straightforward byte-fiddling on a
 * 512-byte buffer.
 *
 * Layer B (subdevice scanner) maintains a small per-disk table of
 * the last set of FAT32 partitions we announced to hotplug. Each
 * call to partition_scan_announce() reads sector 0 via the caller's
 * sector-reader, builds the new set, computes the (added, removed)
 * delta against the saved set, and fires SUBDEVICE_APPEARED for
 * additions and SUBDEVICE_GONE for removals.
 *
 * The token format we hand to hotplug encodes both the disk and the
 * partition index in 32 bits: high 8 bits = partition_index (0..3),
 * low 24 bits = native_selector. Hotplug treats it as opaque, but
 * keeping the format public-by-convention means future cleanup tools
 * can decode it.
 *
 *   token = (partition_index << 24) | (native_selector & 0xFFFFFF)
 */

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <dob/partition.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/hotplug_events.h>

/* === Layer A: pure MBR ops === */

static uint32_t
le32_read(const uint8_t *p)
{
    return ((uint32_t)p[0])
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void
le32_write(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void
partition_mbr_parse(const void *sector_buf, mbr_table_t *out)
{
    const uint8_t *s = (const uint8_t *)sector_buf;
    memset(out, 0, sizeof(*out));

    out->valid_signature = (s[510] == MBR_SIG_LO && s[511] == MBR_SIG_HI);

    const uint8_t *pt = s + MBR_PT_OFFSET;
    for (int i = 0; i < MBR_MAX_PRIMARY; i++)
    {
        const uint8_t *e = pt + i * 16;
        out->entries[i].active    = e[0];
        out->entries[i].type      = e[4];
        out->entries[i].start_lba = le32_read(e + 8);
        out->entries[i].sectors   = le32_read(e + 12);
    }
}

void
partition_mbr_serialize(const mbr_table_t *table, void *sector_buf)
{
    uint8_t *s = (uint8_t *)sector_buf;

    uint8_t *pt = s + MBR_PT_OFFSET;
    for (int i = 0; i < MBR_MAX_PRIMARY; i++)
    {
        uint8_t *e = pt + i * 16;
        memset(e, 0, 16);
        e[0] = table->entries[i].active;
        /* CHS fields (bytes 1..3 and 5..7) intentionally left zero.
         * Every modern BIOS/UEFI does LBA-only; CHS values are advisory
         * and zero is a well-understood "use LBA" marker. */
        e[4] = table->entries[i].type;
        le32_write(e + 8,  table->entries[i].start_lba);
        le32_write(e + 12, table->entries[i].sectors);
    }
    s[510] = MBR_SIG_LO;
    s[511] = MBR_SIG_HI;
}

void
partition_mbr_init_empty(void *sector_buf)
{
    uint8_t *s = (uint8_t *)sector_buf;
    memset(s, 0, MBR_SECTOR_SIZE);
    s[510] = MBR_SIG_LO;
    s[511] = MBR_SIG_HI;
}

bool
partition_type_is_fat32(uint8_t type)
{
    return type == MBR_TYPE_FAT32_CHS || type == MBR_TYPE_FAT32_LBA;
}

bool
partition_type_is_exfat(uint8_t type)
{
    return type == MBR_TYPE_EXFAT;
}

/* === Layer B: subdevice scanner ===
 *
 * Per-disk "last announced" state. Keyed by (provider_service,
 * native_selector). Fixed-size table because in our scope the upper
 * bound is small (4 ATA slots + 32 AHCI ports = 36 + headroom).
 */

#define DISK_STATE_MAX  48

typedef struct
{
    bool     in_use;
    char     provider_service[32];
    uint32_t native_selector;

    /* Last announced set — entries[i].sectors==0 means slot i is
     * empty in the saved set. type and start_lba identify the
     * partition for diff purposes; an MBR edit that moves a
     * partition (changes start_lba) is treated as remove+add. */
    mbr_partition_t entries[MBR_MAX_PRIMARY];
} disk_state_t;

static disk_state_t g_state[DISK_STATE_MAX];

static disk_state_t *
state_find(const char *service, uint32_t selector)
{
    for (int i = 0; i < DISK_STATE_MAX; i++)
    {
        if (!g_state[i].in_use) continue;
        if (g_state[i].native_selector != selector) continue;
        if (strncmp(g_state[i].provider_service, service,
                    sizeof(g_state[i].provider_service)) != 0)
            continue;
        return &g_state[i];
    }
    return NULL;
}

static disk_state_t *
state_get_or_alloc(const char *service, uint32_t selector)
{
    disk_state_t *s = state_find(service, selector);
    if (s) return s;
    for (int i = 0; i < DISK_STATE_MAX; i++)
    {
        if (g_state[i].in_use) continue;
        memset(&g_state[i], 0, sizeof(g_state[i]));
        g_state[i].in_use          = true;
        g_state[i].native_selector = selector;
        strncpy(g_state[i].provider_service, service,
                sizeof(g_state[i].provider_service) - 1);
        return &g_state[i];
    }
    return NULL;
}

/* (partition_index << 24) | (native_selector & 0xFFFFFF) */
static uint32_t
make_token(uint32_t native_selector, int partition_index)
{
    return ((uint32_t)partition_index << 24)
         | (native_selector & 0x00FFFFFF);
}

static bool
emit_appeared(uint32_t hp_port,
              const partition_scan_ctx_t *ctx,
              int partition_index,
              const mbr_partition_t *entry)
{
    hotplug_subdev_appeared_t req;
    memset(&req, 0, sizeof(req));
    req.sub.provider_token = make_token(ctx->native_selector, partition_index);
    /* Filesystem-volume subdevices: identify via volume_fs, leave the
     * PCI-style class/subclass at zero (those slots are for AHCI's
     * optical-drive descriptor and the like, not for FS volumes). */
    req.sub.class_code          = 0;
    req.sub.subclass            = 0;
    req.sub.volume_fs           = partition_type_is_exfat(entry->type)
                                  ? VOLUME_FS_EXFAT : VOLUME_FS_FAT32;
    req.sub.partition_start_lba = entry->start_lba;
    strncpy(req.sub.provider_service, ctx->provider_service,
            sizeof(req.sub.provider_service) - 1);

    dob_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.code         = HOTPLUG_SUBDEVICE_APPEARED;
    msg.payload      = &req;
    msg.payload_size = sizeof(req);
    /* Async post, not sync call: the caller is a disk driver and we
     * cannot allow it to block on hotplug. Hotplug, in turn, may
     * have to call getSetting()/dobfs_GetMountedOn() to decide what
     * to do with the announcement — those calls round-trip back
     * through the disk driver to read settingsd's storage. A sync
     * APPEARED would deadlock: driver → hotplug → settingsd → DobFS
     * → driver. Post breaks the cycle: the driver returns
     * immediately and hotplug processes when it's free. */
    return dob_ipc_post(hp_port, &msg) == DOB_OK;
}

static bool
emit_gone(uint32_t hp_port,
          const partition_scan_ctx_t *ctx,
          int partition_index)
{
    hotplug_subdev_gone_t req;
    memset(&req, 0, sizeof(req));
    req.provider_token = make_token(ctx->native_selector, partition_index);
    strncpy(req.provider_service, ctx->provider_service,
            sizeof(req.provider_service) - 1);

    dob_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.code         = HOTPLUG_SUBDEVICE_GONE;
    msg.payload      = &req;
    msg.payload_size = sizeof(req);
    /* Async post — same deadlock-avoidance reasoning as emit_appeared. */
    return dob_ipc_post(hp_port, &msg) == DOB_OK;
}

/* Two partition entries represent the same volume iff the type byte
 * matches AND the starting LBA matches. A partition that "moved" via
 * resize/repartition is treated as remove+add — its identity changed
 * from the user's perspective. */
static bool
entries_match(const mbr_partition_t *a, const mbr_partition_t *b)
{
    if (a->type == MBR_TYPE_EMPTY || b->type == MBR_TYPE_EMPTY) return false;
    if (a->type != b->type)                                     return false;
    if (a->start_lba != b->start_lba)                           return false;
    return true;
}

bool
partition_scan_announce(const partition_scan_ctx_t *ctx)
{
    if (!ctx || !ctx->read_sector || !ctx->provider_service) return false;

    uint32_t hp_port = dob_registry_find("hotplug");
    if (!hp_port) return false;

    uint8_t sector0[MBR_SECTOR_SIZE];
    if (!ctx->read_sector(ctx->ctx, MBR_SECTOR_LBA, sector0)) return false;

    mbr_table_t table;
    partition_mbr_parse(sector0, &table);

    /* Build the new "interesting" set: FAT32 partitions in valid
     * signature MBRs only. An invalid MBR is treated as "no
     * partitions" — we still proceed so that previously announced
     * partitions get GONE'd. */
    mbr_partition_t new_set[MBR_MAX_PRIMARY];
    memset(new_set, 0, sizeof(new_set));
    if (table.valid_signature)
    {
        for (int i = 0; i < MBR_MAX_PRIMARY; i++)
        {
            uint8_t pty = table.entries[i].type;
            if (!partition_type_is_fat32(pty) &&
                !partition_type_is_exfat(pty))                      continue;
            if (table.entries[i].sectors == 0)                      continue;
            new_set[i] = table.entries[i];
        }
    }

    disk_state_t *st = state_get_or_alloc(ctx->provider_service,
                                          ctx->native_selector);
    if (!st) return false;

    /* Diff: per slot index, decide APPEARED / GONE / unchanged. */
    for (int i = 0; i < MBR_MAX_PRIMARY; i++)
    {
        const mbr_partition_t *was = &st->entries[i];
        const mbr_partition_t *now = &new_set[i];
        bool had = was->type != MBR_TYPE_EMPTY && was->sectors != 0;
        bool has = now->type != MBR_TYPE_EMPTY && now->sectors != 0;

        if (had && has && entries_match(was, now))
            continue;                              /* No change */

        if (had) emit_gone(hp_port, ctx, i);
        if (has) emit_appeared(hp_port, ctx, i, now);
    }

    memcpy(st->entries, new_set, sizeof(new_set));
    return true;
}

void
partition_scan_forget(const char *provider_service, uint32_t native_selector)
{
    disk_state_t *st = state_find(provider_service, native_selector);
    if (st) st->in_use = false;
}
