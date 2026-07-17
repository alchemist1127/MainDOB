/* MainDOB USB Mass Storage sub-driver (Bulk-Only Transport / SCSI).
 *
 * Spawned by the "Pendrive USB" DAS action with: --port <N>.
 * It is a CLIENT of the usb_uhci controller driver (ops CTRL/BULK of
 * dob/usb_uhci_protocol.h) and a SERVER towards the rest of the
 * system: it registers "usbms_<N>" and speaks the same block protocol
 * as ata/ahci (code 1 = read sectors, code 2 = write sectors), so
 * DobFileSystem secondary mounts and DobDisk work unmodified.
 *
 * === Sector-size independence ===
 * Real sticks report native block sizes of 512, 1024, 2048 or 4096
 * bytes (READ CAPACITY(10)). The whole MainDOB block ecosystem speaks
 * 512-byte sectors, so this driver exposes a 512-BYTE VIRTUAL VIEW of
 * the medium and translates internally:
 *   read : map LBA512 -> native blocks, READ(10) the covering blocks,
 *          copy out the requested slice;
 *   write: read-modify-write of the covering native blocks (skipping
 *          the read when the write is block-aligned and block-sized).
 * Consumers never see the native size; a 4096-byte-block stick mounts
 * and formats exactly like a 512-byte one.
 *
 * === Lifecycle ===
 * init: GET_DEVINFO from usb_uhci, read CONFIG descriptor to find the
 * bulk IN/OUT endpoints, SET_CONFIGURATION(1), BOT INQUIRY +
 * TEST UNIT READY (with REQUEST SENSE retries: sticks need spin-up
 * time) + READ CAPACITY(10); then register the service, scan the MBR
 * via libdob/dob/partition and announce FAT32 volumes to hotplug —
 * which DAS-matches them with the EXISTING partition_fat32.das: icon,
 * mount and DobFiles view are fully reused from the AHCI pipeline.
 *
 * On device removal usb_uhci posts us HOTPLUG_DETACH: we emit
 * SUBDEVICE_GONE for every volume we announced, forget the scanner
 * state and exit; the kernel cleans our registry entry. */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dob/server.h>
#include <dob/ipc.h>
#include <dob/types.h>
#include <dob/registry.h>
#include <dob/usb_uhci_protocol.h>
#include <dob/hotplug_events.h>
#include <dob/hotplug_driver.h>   /* HOTPLUG_DETACH */
#include <dob/partition.h>
#include <dob/spawn.h>

/* ===== Tunables ===== */
#define USBMS_VERSION    "1.5.34"
#define BOT_DATA_MAX     4096   /* per-CBW data ceiling (<= UHCI_XFER_MAX) */
#define CLIENT_MAX_SECT  128    /* per-IPC virtual sectors (128*512 = 64 KB,
                                 * the IPC payload ceiling). dobfs batches
                                 * cluster runs and flushes its FAT cache in
                                 * single calls well above 32 sectors — the
                                 * block contract must match ata/ahci, so we
                                 * loop over the wire-sized chunks INSIDE
                                 * vread/vwrite instead of refusing. */
#define TUR_RETRIES      20     /* TEST UNIT READY attempts at 200 ms */
/* Device-busy (NAK) patience: a flash stick NAKs the next CBW (and sometimes
 * the CSW) while it commits the previous write. We WAIT and retry, never reset
 * — a reset would abort the in-flight commit and wedge the device. 20 x 100 ms
 * = 2 s, well past any single-block commit. */
#define CBW_BUSY_RETRIES   20   /* CBW-busy retries (any command), 100 ms apart */
#define CSW_BUSY_RETRIES   20   /* CSW-busy re-reads, 100 ms apart */

/* ===== Controller link =====
 * Provider-agnostic, the cdrom lesson: the DAS passes "$provider:$token"
 * as a single argv and we talk the USB HOST TRANSPORT CONTRACT
 * (dob/usb_uhci_protocol.h ops 3/4/7/8/9) to WHATEVER controller driver
 * announced the device — usb_uhci today, usb_ehci/usb_xhci tomorrow,
 * same binary, zero patches here. */
static char     hc_name[32] = "usb_uhci";   /* provider service name */
static uint32_t hc_port    = 0;     /* provider service port */
static int      usb_port   = 0;     /* root port (token from the DAS) */

/* Zero-copy window: the controller's bulk DMA bounce mapped into OUR
 * address space (op GET_WINDOW + mmap_phys). When present, BULK data
 * moves controller<->us without IPC payload copies; the wire itself is
 * bus-master DMA on every host controller. NULL = fall back to
 * payload copies (works with any future controller that skips op 9). */
static uint8_t *win       = NULL;
static uint32_t win_size  = 0;
static uint8_t  ep_in      = 0;     /* bulk IN endpoint number */
static uint8_t  ep_out     = 0;     /* bulk OUT endpoint number */
static uint16_t ep_in_mps  = 64;    /* bulk EP max packet sizes from the
                                     * descriptors; full-speed allows
                                     * 8/16/32/64 and hardcoding 64
                                     * breaks packetization on the rest */
static uint16_t ep_out_mps = 64;

/* ===== Medium geometry ===== */
static uint32_t native_block  = 512;     /* 512 / 1024 / 2048 / 4096 */
static uint32_t native_count  = 0;       /* total native blocks */
static uint32_t virt_sectors  = 0;       /* total 512-byte sectors */
static uint32_t factor        = 1;       /* native_block / 512 */

/* ===== Buffers ===== */
static uint8_t  nat_buf[BOT_DATA_MAX];          /* native-block bounce */
static uint8_t  client_buf[CLIENT_MAX_SECT * 512]; /* reply assembly */
static uint32_t bot_tag = 1;

static char service_name[24];

/* Bring-up forensics: which stage did we reach? Exposed via opcode 3
 * (arg2) so usbdiag's fase 4 can display where a failed bring-up died
 * without anyone fishing for the serial log. The process stays ALIVE
 * after a failed bring-up exactly for this: a dead process answers no
 * questions. */
enum {
    STAGE_START = 0, STAGE_DEVINFO, STAGE_ENDPOINTS, STAGE_SETCONFIG,
    STAGE_INQUIRY, STAGE_TUR, STAGE_READCAP, STAGE_ONLINE
};
static uint8_t bringup_stage = STAGE_START;
static uint8_t bringup_ok    = 0;
/* Outcome of the LAST PREPARE_VOLUME (op 66), packed into op 3 arg2
 * bits 8..15 for usbdiag: 0 never ran, 1 ok, 2 no FAT32 volume,
 * 3 MBR read FAILED (transport problem, not a formatting problem). */
static uint8_t last_prepare  = 0;

/* Volumes we announced (mirror of the scanner's emission) so we can
 * retract them on detach. */
static uint8_t announced_parts[MBR_MAX_PRIMARY];

/* ======================================================================
 * Controller transport wrappers
 * ====================================================================== */

/* Set when the HC answers DOB_ERR_DEAD (port gate: device physically
 * gone). Every retry ladder below short-circuits on it: retrying a
 * ripped device costs minutes of wedge and produced the field "ghost
 * icon". The flag is per-process: a re-inserted stick gets a fresh
 * usbms instance anyway. */
static int transport_dead = 0;

static int hc_ctrl(const uint8_t setup[8], const void *out_data,
                   void *in_data, int in_cap)
{
    if (transport_dead) return -3;
    uint16_t wlen = (uint16_t)setup[6] | ((uint16_t)setup[7] << 8);
    static uint8_t pl[8 + 1024];
    memcpy(pl, setup, 8);
    uint32_t plen = 8;
    if (!(setup[0] & 0x80) && wlen)
    {
        if (wlen > 1024) return -1;
        memcpy(pl + 8, out_data, wlen);
        plen += wlen;
    }
    dob_msg_t m, r;
    memset(&m, 0, sizeof(m)); memset(&r, 0, sizeof(r));
    m.code = UHCI_OP_CTRL_XFER;
    m.payload = pl; m.payload_size = plen;
    if (dob_ipc_call(hc_port, &m, &r) != DOB_OK) return -1;
    if (r.code == DOB_ERR_DEAD) { transport_dead = 1; return -3; }
    if (r.code != DOB_OK) return -1;
    if ((setup[0] & 0x80) && in_data && r.payload)
    {
        int n = (int)r.arg0;
        if (n > in_cap) n = in_cap;
        memcpy(in_data, r.payload, (size_t)n);
        return n;
    }
    return (int)r.arg0;
}

/* Bulk transfer. dir_in: data device->host. Returns actual bytes,
 * -2 on endpoint STALL, -1 on other failure. */
static int hc_bulk(int dir_in, void *buf, uint32_t len)
{
    if (transport_dead) return -3;
    int via_win = (win != NULL && len <= win_size);
    dob_msg_t m, r;
    memset(&m, 0, sizeof(m)); memset(&r, 0, sizeof(r));
    m.code = UHCI_OP_BULK_XFER;
    m.arg0 = (uint32_t)((dir_in ? 0x80 : 0x00) | (dir_in ? ep_in : ep_out));
    m.arg1 = len;
    /* arg2 carried the (retired) window flag; since v1.5.20 it carries
     * the endpoint's max packet size so the HC packetizes correctly. */
    m.arg2 = dir_in ? ep_in_mps : ep_out_mps;
    if (!dir_in)
    {
        if (via_win) memcpy(win, buf, len);    /* into the shared window */
        else { m.payload = buf; m.payload_size = len; }
    }
    if (dob_ipc_call(hc_port, &m, &r) != DOB_OK) return -1;
    if (r.code == DOB_ERR_DENIED) return -2;          /* STALL */
    if (r.code == DOB_ERR_DEAD) { transport_dead = 1; return -3; }
    if (r.code == DOB_ERR_INTERNAL && r.arg1 == 1) return -4; /* device busy (NAK) */
    if (r.code != DOB_OK) return -1;
    if (dir_in && r.arg0)
    {
        uint32_t n = r.arg0; if (n > len) n = len;
        if (via_win)            memcpy(buf, win, n);
        else if (r.payload)     memcpy(buf, r.payload, n);
        else return -1;
        return (int)n;
    }
    return (int)r.arg0;
}

/* Map the controller's DMA window for zero-copy bulk. Optional. */
static void hc_map_window(void)
{
    dob_msg_t m, r;
    memset(&m, 0, sizeof(m)); memset(&r, 0, sizeof(r));
    m.code = UHCI_OP_GET_WINDOW;
    if (dob_ipc_call(hc_port, &m, &r) != DOB_OK || r.code != DOB_OK) return;
    if (!r.arg0 || !r.arg1) return;
    void *v = mmap_phys(r.arg0, r.arg1);
    if (!v) return;
    win = (uint8_t *)v;
    win_size = r.arg1;
    debug_print("[usbms] zero-copy window mapped\n");
}

/* CLEAR_FEATURE(ENDPOINT_HALT) + controller-side toggle reset. */
static void hc_clear_halt(int dir_in)
{
    uint8_t ep = (uint8_t)((dir_in ? 0x80 : 0x00) | (dir_in ? ep_in : ep_out));
    uint8_t setup[8] = { 0x02, 0x01, 0x00, 0x00, ep, 0x00, 0x00, 0x00 };
    hc_ctrl(setup, NULL, NULL, 0);
    dob_msg_t m, r;
    memset(&m, 0, sizeof(m)); memset(&r, 0, sizeof(r));
    m.code = UHCI_OP_RESET_TOGGLE;
    m.arg0 = (uint32_t)((dir_in ? 0x80 : 0x00) | (dir_in ? ep_in : ep_out));
    dob_ipc_call(hc_port, &m, &r);
}

/* ======================================================================
 * Bulk-Only Transport (CBW / data / CSW)
 * ====================================================================== */

/* Bulk-Only Mass Storage Reset Recovery (BOT 1.0, 5.3.4): reset the
 * device's command state machine and clear both bulk endpoint halts +
 * data toggles. Called when a transfer left the device mid-sequence (it
 * consumed the CBW but we never read a clean CSW): without it the next
 * command sends a fresh CBW while the device still owes the old CSW, the
 * two desync, and everything wedges until a physical replug. wIndex = 0:
 * single-interface BOT devices (all common flash sticks) use interface 0. */
static void bot_reset_recovery(void)
{
    if (transport_dead) return;
    uint8_t setup[8] = { 0x21, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    hc_ctrl(setup, NULL, NULL, 0);   /* Mass Storage Reset */
    hc_clear_halt(1);                /* bulk IN:  CLEAR_HALT + toggle reset */
    hc_clear_halt(0);                /* bulk OUT: CLEAR_HALT + toggle reset */
}

/* Fail a BOT round trip after the CBW was already on the wire: recover the
 * device first so the next command isn't doomed, then report the error. */
static int bot_fail(void)
{
    bot_reset_recovery();
    return -1;
}

/* One full BOT round trip. cb = SCSI command block (<= 16 B).
 * data/data_len: payload of the data phase (dir per data_in).
 * Returns SCSI status from the CSW (0 = good), or <0 on transport
 * failure. */
static int bot_xfer(const uint8_t *cb, int cb_len, int data_in,
                    void *data, uint32_t data_len, uint32_t *actual)
{
    uint8_t cbw[31];
    memset(cbw, 0, sizeof(cbw));
    cbw[0]='U'; cbw[1]='S'; cbw[2]='B'; cbw[3]='C';
    uint32_t tag = bot_tag++;
    memcpy(cbw + 4, &tag, 4);
    memcpy(cbw + 8, &data_len, 4);
    cbw[12] = data_in ? 0x80 : 0x00;
    cbw[13] = 0;                       /* LUN 0 */
    cbw[14] = (uint8_t)cb_len;
    memcpy(cbw + 15, cb, (size_t)cb_len);

    if (transport_dead) return -1;
    int c = hc_bulk(0, cbw, 31);
    /* CBW NAKed: the device is busy committing a previous write before it will
     * accept the next command. WAIT and re-send the SAME CBW — never reset, a
     * reset aborts the in-flight commit and wedges the device into permanent
     * NAK. Bounded so a truly stuck device still fails out instead of hanging. */
    for (int t = 0; c == -4 && t < CBW_BUSY_RETRIES; t++)
    {
        busy_wait_us(100000);          /* 100 ms */
        c = hc_bulk(0, cbw, 31);
    }
    if (c != 31) return bot_fail();

    uint32_t moved = 0;
    if (data_len)
    {
        int n = hc_bulk(data_in, data, data_len);
        if (n == -2) { hc_clear_halt(data_in); n = 0; } /* short/none */
        else if (n < 0) return bot_fail();
        moved = (uint32_t)n;
    }
    if (actual) *actual = moved;

    uint8_t csw[13];
    int n = hc_bulk(1, csw, 13);
    if (n == -2) { hc_clear_halt(1); n = hc_bulk(1, csw, 13); }
    /* CSW NAKed: the device is still committing before it reports status.
     * RE-READ it — never send a fresh CBW here (that violates Bulk-Only
     * Transport and wedges the device). Bounded so a truly stuck device still
     * fails out instead of hanging. */
    for (int t = 0; n == -4 && t < CSW_BUSY_RETRIES; t++)
    {
        busy_wait_us(100000);          /* 100 ms */
        n = hc_bulk(1, csw, 13);
    }
    if (n != 13) return bot_fail();
    if (memcmp(csw, "USBS", 4) != 0) return bot_fail();
    /* dCSWTag MUST echo the dCBWTag we sent (BOT 1.0, 6.3). A mismatch means
     * a STALE CSW from a previous, recovered command is still in the pipe —
     * accepting it pairs the wrong status with THIS command (a failed write
     * read back as "good", or vice versa). Recover and fail, never trust it. */
    if (memcmp(csw + 4, &tag, 4) != 0) return bot_fail();
    /* bCSWStatus == 2 is a Phase Error (BOT 1.0, 6.5/6.7): the device command
     * state machine is out of phase and the NEXT command will desync unless we
     * reset-recover NOW. Status 0 (good) and 1 (command failed) fall through. */
    if (csw[12] == 2) return bot_fail();
    return (int)csw[12];               /* bCSWStatus (0 = good, 1 = failed) */
}

/* ===== SCSI commands ===== */

/* Last REQUEST SENSE result (key + ASC), for medium-presence checks.
 * 0x02/0x3A = NOT READY / MEDIUM NOT PRESENT: the signature of an empty
 * card reader — the CQ62's soldered SD reader is a PERMANENT mass-storage
 * device, so "device present" no longer implies "medium present". */
static uint8_t last_sense_key = 0;
static uint8_t last_sense_asc = 0;

/* Bring-up latches for the lazy-retry path (see cases 66/67):
 * endpoints_ok  — bulk endpoint pair discovered (transport usable);
 * no_media_latch — bring-up stopped because the slot is EMPTY (sense
 * 2/3A), the normal state of an internal card reader. */
static uint8_t endpoints_ok   = 0;
static uint8_t no_media_latch = 0;

static int scsi_request_sense(void)
{
    uint8_t cb[6] = { 0x03, 0, 0, 0, 18, 0 };
    uint8_t sense[18];
    int st = bot_xfer(cb, 6, 1, sense, 18, NULL);
    if (st == 0)
    {
        last_sense_key = sense[2] & 0x0Fu;
        last_sense_asc = sense[12];
    }
    return st;
}

/* Medium presence: TUR ok -> present. TUR failed with sense 2/3A ->
 * ABSENT (empty reader). Any other failure is NOT treated as absent —
 * a flaky transport must keep reporting as a volume problem, not lie
 * about the medium. */
static int scsi_test_unit_ready(void);   /* defined just below */
static int scsi_medium_present(void)
{
    if (scsi_test_unit_ready() == 0) return 1;
    return !(last_sense_key == 0x02 && last_sense_asc == 0x3A);
}

static int scsi_test_unit_ready(void)
{
    uint8_t cb[6] = { 0x00, 0, 0, 0, 0, 0 };
    int st = bot_xfer(cb, 6, 0, NULL, 0, NULL);
    if (st != 0) scsi_request_sense();   /* clear pending sense */
    return st;
}

static int scsi_inquiry(void)
{
    uint8_t cb[6] = { 0x12, 0, 0, 0, 36, 0 };
    uint8_t inq[36];
    int st = bot_xfer(cb, 6, 1, inq, 36, NULL);
    if (st == 0)
    {
        /* MainDOB's sprintf has no %.Ns precision (the field log showed
         * the format string verbatim) — bounded copies instead. */
        char vend[9], prod[17], line[96];
        memcpy(vend, inq + 8, 8);   vend[8]  = 0;
        memcpy(prod, inq + 16, 16); prod[16] = 0;
        sprintf(line, "[usbms] INQUIRY: %s %s\n", vend, prod);
        debug_print(line);
    }
    return st;
}

/* SYNCHRONIZE CACHE(10): flush the device's volatile write cache.
 * QEMU is write-through so this is a no-op there, but real sticks
 * buffer — without it, a yank right after a "saved" file can lose
 * data the system already promised to the user. Issued after every
 * WRITE op; devices that don't implement it (CHECK CONDITION) set the
 * latch and are never asked again. */
static uint8_t sync_cache_unsupported = 0;

static void scsi_sync_cache(void)
{
    if (sync_cache_unsupported) return;
    uint8_t cb[16] = {0};
    cb[0] = 0x35;                       /* SYNCHRONIZE CACHE(10) */
    int st = bot_xfer(cb, 10, 0, NULL, 0, NULL);
    if (st != 0)
    {
        sync_cache_unsupported = 1;
        debug_print("[usbms] SYNC CACHE unsupported; relying on "
                    "device write-through\n");
    }
}

static int scsi_read_capacity(void)
{
    uint8_t cb[10] = { 0x25, 0,0,0,0,0,0,0,0,0 };
    uint8_t cap[8];
    int st = bot_xfer(cb, 10, 1, cap, 8, NULL);
    if (st != 0) return st;
    uint32_t last = ((uint32_t)cap[0] << 24) | ((uint32_t)cap[1] << 16) |
                    ((uint32_t)cap[2] << 8)  |  (uint32_t)cap[3];
    uint32_t bl   = ((uint32_t)cap[4] << 24) | ((uint32_t)cap[5] << 16) |
                    ((uint32_t)cap[6] << 8)  |  (uint32_t)cap[7];
    /* "Vari tagli di settore": accept the standard power-of-two sizes;
     * anything else is exotic enough to refuse rather than corrupt. */
    if (bl != 512 && bl != 1024 && bl != 2048 && bl != 4096) return -1;
    native_block = bl;
    native_count = last + 1;
    factor       = native_block / 512;
    virt_sectors = native_count * factor;
    char line[96];
    sprintf(line, "[usbms] capacity: %u blocks x %u B (%u virt sectors)\n",
            native_count, native_block, virt_sectors);
    debug_print(line);
    return 0;
}

/* Read `count` native blocks starting at native LBA into nat_buf.
 * count limited so count*native_block <= BOT_DATA_MAX. */
static bool scsi_read10(uint32_t nlba, uint32_t count)
{
    uint8_t cb[10];
    memset(cb, 0, sizeof(cb));
    cb[0] = 0x28;
    cb[2] = (uint8_t)(nlba >> 24); cb[3] = (uint8_t)(nlba >> 16);
    cb[4] = (uint8_t)(nlba >> 8);  cb[5] = (uint8_t)nlba;
    cb[7] = (uint8_t)(count >> 8); cb[8] = (uint8_t)count;
    uint32_t want = count * native_block, got = 0;
    int st = bot_xfer(cb, 10, 1, nat_buf, want, &got);
    return st == 0 && got == want;
}

static bool scsi_write10(uint32_t nlba, uint32_t count)
{
    uint8_t cb[10];
    memset(cb, 0, sizeof(cb));
    cb[0] = 0x2A;
    cb[2] = (uint8_t)(nlba >> 24); cb[3] = (uint8_t)(nlba >> 16);
    cb[4] = (uint8_t)(nlba >> 8);  cb[5] = (uint8_t)nlba;
    cb[7] = (uint8_t)(count >> 8); cb[8] = (uint8_t)count;
    uint32_t want = count * native_block, got = 0;
    int st = bot_xfer(cb, 10, 0, nat_buf, want, &got);
    return st == 0 && got == want;
}

/* ======================================================================
 * 512-byte virtual view
 * ====================================================================== */

/* Read `count` virtual sectors at lba512 into dst. */
static uint32_t cnt_vread_fail = 0;   /* diag: post-bring-up read health */

static bool vread(uint32_t lba512, uint32_t count, uint8_t *dst)
{
    if (lba512 + count > virt_sectors) { cnt_vread_fail++; return false; }
    while (count)
    {
        uint32_t nlba = lba512 / factor;
        uint32_t off  = (lba512 % factor) * 512;
        uint32_t max_nb = BOT_DATA_MAX / native_block;
        /* native blocks covering this chunk */
        uint32_t span_sect = max_nb * factor - (lba512 % factor);
        if (span_sect > count) span_sect = count;
        uint32_t nb = (off + span_sect * 512 + native_block - 1)
                      / native_block;
        if (!scsi_read10(nlba, nb)) { cnt_vread_fail++; return false; }
        memcpy(dst, nat_buf + off, span_sect * 512);
        dst    += span_sect * 512;
        lba512 += span_sect;
        count  -= span_sect;
    }
    return true;
}

/* Write `count` virtual sectors at lba512 from src (read-modify-write
 * on the covering native blocks when not aligned). */
static bool vwrite(uint32_t lba512, uint32_t count, const uint8_t *src)
{
    if (lba512 + count > virt_sectors) return false;
    while (count)
    {
        uint32_t nlba = lba512 / factor;
        uint32_t off  = (lba512 % factor) * 512;
        uint32_t max_nb = BOT_DATA_MAX / native_block;
        uint32_t span_sect = max_nb * factor - (lba512 % factor);
        if (span_sect > count) span_sect = count;
        uint32_t nb = (off + span_sect * 512 + native_block - 1)
                      / native_block;
        bool aligned = (off == 0) &&
                       ((span_sect * 512) % native_block == 0);
        if (!aligned && !scsi_read10(nlba, nb))   /* RMW: fetch first */
            return false;
        memcpy(nat_buf + off, src, span_sect * 512);
        if (!scsi_write10(nlba, nb)) return false;
        src    += span_sect * 512;
        lba512 += span_sect;
        count  -= span_sect;
    }
    return true;
}

/* ======================================================================
 * Volume location (the cdrom/floppy lesson: ONE icon, ONE click, ONE
 * window — the driver finds the volume and orchestrates the view)
 * ====================================================================== */

/* Where does the FAT32 volume start?
 *  - MBR present: first primary partition typed FAT32 -> its start LBA.
 *  - No MBR but LBA 0 carries a plausible FAT BPB ("superfloppy", very
 *    common on small/old sticks): the volume starts at LBA 0.
 * Returns true with *out_lba set, false if no mountable volume. */
static int find_volume_lba(uint32_t *out_lba, int *out_is_exfat)  /* 0 ok, -1 read err, -2 no fs */
{
    *out_is_exfat = 0;
    uint8_t sec[512];
    if (!vread(0, 1, sec)) return -1;

    /* exFAT superfloppy (exFAT volume at LBA 0, no MBR): the "EXFAT   "
     * signature at offset 3 is unique to an exFAT boot sector and never
     * appears in a real MBR, so test it BEFORE parsing sector 0 as an MBR.
     * Otherwise an exFAT boot sector whose boot-code region happens to look
     * like a partition entry (sectors != 0 + a FAT/exFAT type byte) would be
     * misread as a partitioned disk and yield a bogus start LBA. */
    if (sec[510] == 0x55 && sec[511] == 0xAA &&
        memcmp(sec + 3, "EXFAT   ", 8) == 0)
    {
        *out_lba = 0;
        *out_is_exfat = 1;
        return 0;
    }

    mbr_table_t t;
    partition_mbr_parse(sec, &t);
    if (t.valid_signature)
    {
        /* First recognised primary volume wins -- FAT32 or exFAT. */
        for (int i = 0; i < MBR_MAX_PRIMARY; i++)
        {
            if (!t.entries[i].sectors) continue;
            if (partition_type_is_fat32(t.entries[i].type))
            {
                *out_lba = t.entries[i].start_lba;
                *out_is_exfat = 0;
                return 0;
            }
            if (partition_type_is_exfat(t.entries[i].type))
            {
                *out_lba = t.entries[i].start_lba;
                *out_is_exfat = 1;
                return 0;
            }
        }
    }

    /* Superfloppy heuristic: jump opcode + sane BPB fields + 0xAA55.
     * (An MBR also ends with 0xAA55, but it has no BPB: bytes 11..12
     * read as garbage sector sizes there.) */
    uint16_t bps = (uint16_t)sec[11] | ((uint16_t)sec[12] << 8);
    uint8_t  spc = sec[13];
    int jump_ok  = (sec[0] == 0xEB || sec[0] == 0xE9);
    int bps_ok   = (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096);
    int spc_ok   = spc && ((spc & (spc - 1)) == 0);
    int sig_ok   = (sec[510] == 0x55 && sec[511] == 0xAA);
    if (jump_ok && bps_ok && spc_ok && sig_ok)
    {
        *out_lba = 0;
        *out_is_exfat = 0;
        return 0;
    }
    return -2;
}

/* PREPARE_VOLUME (op 66): locate the FAT32 volume and spawn the
 * secondary DobFileSystem bound to us — FIRE AND FORGET, then reply
 * at once. We must NOT wait for the mount here: the mount reads its
 * sectors FROM US, and we are busy serving this very request — the
 * waiting belongs to hotplug\'s action engine (wait_service dobfs_N),
 * exactly like the cdrom/partition flows it already runs. */
static dob_status_t op_prepare_volume(void)
{
    char dobfs_name[24];
    snprintf(dobfs_name, sizeof(dobfs_name), "dobfs_%d", usb_port);
    if (dob_registry_find(dobfs_name))
        return DOB_OK;                     /* already mounted: re-view */

    uint32_t lba = 0;
    int is_exfat = 0;
    int frc = find_volume_lba(&lba, &is_exfat);
    if (frc < 0)
    {
        /* Empty reader vs. real volume problem: an internal card reader
         * with no card must NOT produce the scary "no FAT32, format it!"
         * popup — the accurate answer is "insert a card". The DAS error
         * refinement (label novolume_9 = -DOB_ERR_NO_MEDIA) routes it. */
        if (!scsi_medium_present())
        {
            last_prepare = 4;
            debug_print("[usbms] no medium in reader (sense 2/3A)\n");
            return DOB_ERR_NO_MEDIA;
        }
        last_prepare = (frc == -1) ? 3 : 2;
        debug_print(frc == -1
                    ? "[usbms] MBR read FAILED (transport)\n"
                    : "[usbms] no mountable FAT32/exFAT volume found\n");
        return DOB_ERR_NOT_FOUND;
    }
    last_prepare = 1;

    char arg[96];
    snprintf(arg, sizeof(arg),
             "provider=%s,lba=%u,selector=0,id=%d,fs=%s",
             service_name, (unsigned)lba, usb_port,
             is_exfat ? "exfat" : "fat32");
    const char *av[3] = { "--mount", arg, NULL };
    /* Plain spawn_file: the secondary DobFileSystem runs as a NON-driver and
     * that is sufficient — it only needs IPC (sector reads from us, OPEN_VIEW
     * via the desktop spawner) plus the ability to load exfat.mem, which the
     * root mount now grants to any caller for shared .mem objects (see
     * DobFileSystem sandbox_check). No per-bus driver promotion needed. */
    spawn_file("/SYSTEM/OperatingSystem/DobFileSystem/DobFileSystem.mdl",
               av);

    char line[96];
    sprintf(line, "[usbms] %s volume at LBA %u, dobfs_%d spawning\n",
            is_exfat ? "exFAT" : "FAT32", (unsigned)lba, usb_port);
    debug_print(line);
    return DOB_OK;
}

/* ======================================================================
 * Bring-up
 * ====================================================================== */

/* Find the bulk IN/OUT endpoints in the configuration descriptor. */
static bool discover_endpoints(void)
{
    uint8_t cfg[256];
    /* wLength = 256 split LOW, HIGH. The original used
     * (uint8_t)sizeof(cfg) for the low byte: 256 truncates to 0, the
     * request went out as wLength=0, the device dutifully answered
     * with ZERO bytes and the parser found no endpoints. Field
     * signature: "[usbms] no bulk endpoint pair found" right after a
     * successful devinfo. One byte of bug, one stage of death. */
    uint8_t setup[8] = { 0x80, 0x06, 0x00, 0x02, 0x00, 0x00,
                         0x00, 0x01 };
    int n = hc_ctrl(setup, NULL, cfg, sizeof(cfg));
    if (n < 9) return false;
    int off = 0;
    while (off + 2 <= n)
    {
        uint8_t blen  = cfg[off];
        uint8_t btype = cfg[off + 1];
        if (blen < 2) break;
        if (btype == 0x05 && off + 7 <= n)          /* ENDPOINT */
        {
            uint8_t addr = cfg[off + 2];
            uint8_t attr = cfg[off + 3] & 0x03;
            if (attr == 0x02)                        /* bulk */
            {
                uint16_t mps = (uint16_t)cfg[off + 4] |
                               ((uint16_t)cfg[off + 5] << 8);
                if (mps < 8 || mps > 64) mps = 64;
                if (addr & 0x80)
                {
                    if (!ep_in)  { ep_in  = addr & 0x0F; ep_in_mps  = mps; }
                }
                else
                {
                    if (!ep_out) { ep_out = addr & 0x0F; ep_out_mps = mps; }
                }
            }
        }
        off += blen;
    }
    return ep_in && ep_out;
}

static bool device_bringup(void)
{
    no_media_latch = 0;   /* fresh attempt: re-decide from the hardware */

    /* SET_CONFIGURATION(1) — enumeration stopped at the address+descriptor
     * stage; the device only accepts class commands once configured. */
    bringup_stage = STAGE_SETCONFIG;
    uint8_t setcfg[8] = { 0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
    if (hc_ctrl(setcfg, NULL, NULL, 0) < 0)
    {
        debug_print("[usbms] SET_CONFIGURATION failed\n");
        return false;
    }

    bringup_stage = STAGE_INQUIRY;
    scsi_inquiry();

    /* Sticks need spin-up: TUR until ready. */
    bringup_stage = STAGE_TUR;
    /* Retry semantics matter here: st > 0 is the device saying "not
     * ready yet" (spin-up — keep knocking, that's what the retries are
     * for); st < 0 is the TRANSPORT failing (every attempt costs the
     * full transfer timeouts). Two transport failures in a row mean
     * the wire is dead and 18 more retries would only stack ~45 s of
     * bounded waits into the apparent system freeze the Armada showed.
     * Abort fast — the stage name in fase 4 does the explaining. */
    int ready = 0, transport_fails = 0;
    for (int i = 0; i < TUR_RETRIES; i++)
    {
        int st = scsi_test_unit_ready();
        if (st == 0) { ready = 1; break; }
        if (transport_dead)
        {
            debug_print("[usbms] TUR: device gone (port gate)\n");
            return false;
        }
        /* Empty slot (sense 2/3A MEDIUM NOT PRESENT): no amount of
         * spin-up retries will conjure a card. Latch and bail NOW —
         * the remaining ~4 s of knocking was pure boot-feel damage on
         * the CQ62's soldered reader. Cases 66/67 map the latch to
         * DOB_ERR_NO_MEDIA and retry the bring-up on the next click. */
        if (st > 0 && last_sense_key == 0x02 && last_sense_asc == 0x3A)
        {
            debug_print("[usbms] TUR: medium not present (empty slot)\n");
            no_media_latch = 1;
            return false;
        }
        if (st < 0)
        {
            if (++transport_fails >= 2)
            {
                debug_print("[usbms] TUR: transport dead, aborting\n");
                return false;
            }
        }
        else
            transport_fails = 0;
        busy_wait_us(100000); busy_wait_us(100000);   /* 200 ms */
    }
    if (!ready)
    {
        debug_print("[usbms] unit never became ready\n");
        return false;
    }

    bringup_stage = STAGE_READCAP;
    if (scsi_read_capacity() != 0)
    {
        debug_print("[usbms] READ CAPACITY failed/unsupported size\n");
        return false;
    }
    bringup_stage = STAGE_ONLINE;
    return true;
}

/* Lazy bring-up retry: an internal card reader is a PERMANENT device —
 * this process outlives an empty-slot boot, so the user's next click
 * (after inserting a card) must be able to complete the bring-up that
 * failed at boot. Only meaningful when the transport is fine
 * (endpoints discovered); with the empty-slot fast-exit above a retry
 * on a still-empty slot costs one TUR round, not 4 s of knocking. */
static void bringup_lazy_retry(void)
{
    if (bringup_ok || !endpoints_ok || transport_dead) return;
    debug_print("[usbms] lazy bring-up retry (op 66/67)\n");
    if (device_bringup()) bringup_ok = 1;
}

/* ======================================================================
 * Block service (ata/ahci-compatible: 1 = read, 2 = write)
 * ====================================================================== */

static dob_status_t handle_message(dob_msg_t *msg, dob_msg_t *reply)
{
    switch (msg->code)
    {
        case 1: {   /* READ: arg0 = lba512, arg1 = count, arg2 = selector */
            if (!bringup_ok) return DOB_ERR_INTERNAL;
            uint32_t lba = msg->arg0, count = msg->arg1;
            if (count == 0 || count > CLIENT_MAX_SECT) return DOB_ERR_INVALID;
            if (!vread(lba, count, client_buf)) return DOB_ERR_INTERNAL;
            reply->payload      = client_buf;
            reply->payload_size = count * 512;
            reply->arg0         = count * 512;
            return DOB_OK;
        }
        case 2: {   /* WRITE */
            if (!bringup_ok) return DOB_ERR_INTERNAL;
            uint32_t lba = msg->arg0, count = msg->arg1;
            if (count == 0 || count > CLIENT_MAX_SECT) return DOB_ERR_INVALID;
            if (!msg->payload || msg->payload_size < count * 512)
                return DOB_ERR_INVALID;
            if (!vwrite(lba, count, (const uint8_t *)msg->payload))
                return DOB_ERR_INTERNAL;
            /* No per-write SYNCHRONIZE CACHE here: forcing a NAND commit
             * after every data batch is exactly what made the device NAK
             * the next command — the mid-copy and 100% stalls. Durability
             * is now driven by the FS: it issues op 4 (FLUSH) on fat_flush,
             * i.e. once per close / metadata commit, not per data write. */
            return DOB_OK;
        }
        case 4: {   /* FLUSH: commit the device write cache to NAND on
                     * demand. The FS issues this at metadata-commit points
                     * (fat_flush, i.e. at close), so a pulled stick keeps
                     * its data + FAT now that the data write path no longer
                     * syncs every batch. Always OK (sync is best-effort). */
            scsi_sync_cache();
            return DOB_OK;
        }
        case 3: {   /* capacity query: arg0 = virt sectors, arg1 = native,
                     * arg2 = bring-up stage (forensics; 7 = online) */
            reply->arg0 = bringup_ok ? virt_sectors : 0;
            reply->arg1 = native_block;
            reply->arg2 = (uint32_t)bringup_stage |
                          ((uint32_t)last_prepare << 8) |
                          ((cnt_vread_fail > 255 ? 255u
                                                 : cnt_vread_fail) << 16);
            return DOB_OK;
        }
        case 67: {  /* WAIT_READY: pure rendezvous. main() is sequential
                     * (register -> bring-up -> loop), so this request
                     * PARKS in our queue and gets served exactly when
                     * bring-up has finished — the IPC reply IS the
                     * "device ready" event. Deterministic, no polling
                     * anywhere: callers (the DAS action engine) block
                     * on the call, bounded by the bring-up itself
                     * (TUR retries and transfer timeouts are finite). */
            if (!bringup_ok) bringup_lazy_retry();
            if (bringup_ok) return DOB_OK;
            return no_media_latch ? DOB_ERR_NO_MEDIA : DOB_ERR_INTERNAL;
        }

        case 66: {  /* PREPARE_VOLUME: locate FAT32, spawn dobfs_<port> */
            if (!bringup_ok) bringup_lazy_retry();
            if (!bringup_ok)
                return no_media_latch ? DOB_ERR_NO_MEDIA : DOB_ERR_INTERNAL;
            return op_prepare_volume();
        }
        default:
            return DOB_ERR_INVALID;
    }
}

int main(int argc, char **argv)
{
    /* argv: "<provider>:<token>" (the cdrom convention). Fallbacks keep
     * older invocations working: bare "--port N" assumes usb_uhci. */
    for (int i = 0; i < argc; i++)
    {
        char *colon = strchr(argv[i], ':');
        if (colon && argv[i][0] != '-')
        {
            *colon = '\0';
            strncpy(hc_name, argv[i], sizeof(hc_name) - 1);
            usb_port = atoi(colon + 1);
        }
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            usb_port = atoi(argv[i + 1]);
    }

    snprintf(service_name, sizeof(service_name), "usbms_%d", usb_port);

    /* Refuse a duplicate instance for this port (every icon click runs
     * the DAS spawn step; the extras exit quietly — cdrom pattern). */
    if (dob_registry_find(service_name)) return 0;

    debug_print("[usbms] v" USBMS_VERSION " starting\n");

    hc_port = dob_registry_wait(hc_name, 3000);
    if (!hc_port)
    {
        debug_print("[usbms] host controller service not found\n");
        return 1;
    }

    /* Zero-copy window DISABLED: the kernel (rightly) denies mmap_phys
     * of another process's allocated RAM — "[MMAP] Denied" in the
     * field log. The payload-copy path is the supported one; true
     * zero-copy needs a proper page-grant API (future kernel work).
     * hc_map_window() and the window plumbing stay for that day. */

    /* Register FIRST, then bring up. A bring-up failure used to exit
     * before registration: hotplug's wait_service timed out, the popup
     * said "driver non parte", and nobody could ask the corpse which
     * stage had died. Alive-but-degraded answers opcode 3 with the
     * stage; data opcodes refuse politely until bringup_ok. */
    /* VERIFIED registration: dob_server_init swallows the registry
     * error, and a silent name collision (e.g. a previous instance that
     * never died) produces exactly the field symptom "icon does nothing"
     * — the das resolves the OLD port, we serve a port nobody calls.
     * Register manually and fail LOUDLY instead. */
    {
        int32_t pr = dob_ipc_port_create();
        if (pr <= 0) { debug_print("[usbms] no port; exiting\n"); return 1; }
        if (dob_registry_register(service_name, (uint32_t)pr) != DOB_OK)
        {
            char m[96];
            sprintf(m, "[usbms] name %s ALREADY TAKEN (stale instance "
                       "alive?); exiting\n", service_name);
            debug_print(m);
            return 1;
        }
        dob_server_adopt_port((uint32_t)pr);
    }

    /* Cross-check the controller's view of the device (also moved
     * after registration: a failure here must leave a queryable
     * process, not a corpse). */
    bringup_stage = STAGE_DEVINFO;
    bool dev_ok = false;
    {
        dob_msg_t m, r;
        memset(&m, 0, sizeof(m)); memset(&r, 0, sizeof(r));
        m.code = UHCI_OP_GET_DEVINFO;
        if (dob_ipc_call(hc_port, &m, &r) == DOB_OK && r.code == DOB_OK &&
            r.payload && r.payload_size >= sizeof(uhci_devinfo_t))
        {
            const uhci_devinfo_t *di = (const uhci_devinfo_t *)r.payload;
            char line[96];
            sprintf(line,
                    "[usbms] device %04x:%04x on port %u, class %02x:%02x\n",
                    di->vid, di->pid, di->port, di->if_class,
                    di->if_subclass);
            debug_print(line);
            dev_ok = true;
        }
        else
            debug_print("[usbms] GET_DEVINFO failed\n");
    }

    if (dev_ok)
    {
        /* Two attempts: on real silicon the FIRST wire transfer can pay
         * the port-heal transient (PIIX4 auto-disable: the wake-side
         * re-enable may land a beat after the first TD already died).
         * Attempt #2 starts 500 ms later on a settled port. Bounded by
         * construction; QEMU simply succeeds on attempt #1. */
        for (int attempt = 0; attempt < 2 && !bringup_ok; attempt++)
        {
            if (attempt)
            {
                if (transport_dead) break;   /* gone is gone: no retry */
                debug_print("[usbms] bring-up retry (port settled)\n");
                busy_wait_us(250000); busy_wait_us(250000);
            }
            bringup_stage = STAGE_ENDPOINTS;
            if (!discover_endpoints())
            {
                debug_print("[usbms] no bulk endpoint pair found\n");
                continue;
            }
            endpoints_ok = 1;
            if (device_bringup())
                bringup_ok = 1;
        }
    }

    debug_print(bringup_ok ? "[usbms] online\n"
                           : "[usbms] bring-up FAILED, serving diagnostics only\n");

    dob_msg_t msg, reply;
    for (;;)
    {
        if (dob_ipc_receive(dob_server_get_port(), &msg) != 0) continue;

        /* Detach (posted by usb_uhci on disconnect, or sent by hotplug):
         * retract our volumes and leave. The kernel unregisters us. */
        if (msg.code == HOTPLUG_DETACH)
        {
            /* Take the mount down with us: DOBFS_SHUTDOWN (22) makes
             * the secondary DobFileSystem flush, notify its DobFiles
             * satellites (window closes) and exit — POSTED, never
             * called: it may be blocked mid-read on US right now. */
            {
                char dn[24];
                snprintf(dn, sizeof(dn), "dobfs_%d", usb_port);
                uint32_t dp = dob_registry_find(dn);
                if (dp)
                {
                    dob_msg_t um; memset(&um, 0, sizeof(um));
                    um.code = 22;   /* DOBFS_SHUTDOWN */
                    dob_ipc_post(dp, &um);
                }
            }
            debug_print("[usbms] detach: exiting\n");
            if (msg.type == 1)
            {
                memset(&reply, 0, sizeof(reply));
                reply.code = DOB_OK;
                dob_ipc_reply(msg.sender_tid, &reply);
            }
            _exit(0);
        }

        if (msg.type == 1)
        {
            memset(&reply, 0, sizeof(reply));
            reply.code = handle_message(&msg, &reply);
            dob_ipc_reply(msg.sender_tid, &reply);
        }
    }
}
