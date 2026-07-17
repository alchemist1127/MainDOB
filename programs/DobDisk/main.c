/* DobDisk — Gestore Unità e Partizioni di MainDOB.
 *
 * Enumera tutti i dischi visti dal block layer (ATA + SATA),
 * mostra le partizioni MBR del disco selezionato, e consente di
 * formattare, creare, eliminare, e ri-etichettare partizioni.
 * Il disco da cui MainDOB è bootato è protetto: nessuna azione
 * distruttiva è esposta nel pannello comandi mentre quel disco
 * è selezionato.
 *
 * Architettura GUI: il pannello universale destro (di sistema,
 * gestito da dobinterface) è agganciato al focus-manager singleton
 * di libdobui via dobfocus_attach_panel. publish_panel chiama
 * dobfocus_set_base_panel per ripubblicare le voci ad ogni cambio
 * di selezione. I widget (dropdown, listview, table) si
 * auto-registrano nel singleton al loro Init: tutti gli eventi
 * mouse/key/scroll/panel passano attraverso dobfocus_* invece di
 * chiamare i singoli OnClick — questo dà focus management
 * coerente, drag della tabella, e swap automatico di Incolla/
 * Copia/Pulisci nel pannello quando un textbox di InputBox prende
 * il focus. Conferme e input via DobPopup standard. Niente
 * bottoni dentro la finestra — la finestra contiene solo la
 * vista (dropdown disco + listview partizioni + progressbar uso
 * + table dettaglio).
 *
 * Note operative:
 *   - MBR only. Massimo 4 partizioni primarie per disco.
 *   - Solo FAT32 supportato per formattazione (fat32_ops in libdob/dob).
 *   - Resize: non implementato in v1. Verrà aggiunto quando il
 *     codice di compattazione FAT è pronto e testato.
 *   - Cambia label: riscrive solo il BPB della partizione, non
 *     richiede unmount.
 *   - Formatta / Elimina su partizione montata: la conferma
 *     avvisa l'utente. L'unmount cooperativo arriverà più avanti
 *     nel ciclo dell'OS (TODO architetturale del sistema).
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <app.h>
#include <DobInterface.h>
#include <DobPopup.h>
#include <DobFileSystem.h>
#include <DobTable.h>

#include <dob/block.h>
#include <dob/partition.h>
#include <dob/fs_ops.h>
#include <dob/types.h>
#include <dob/thread.h>     /* dob_thread_spawn — async DF worker */
#include <dob/ipc.h>        /* dob_ipc_post — wake the main loop */

#include <listview.h>
#include <table.h>
#include <dropdown.h>
#include <textbox.h>
#include <progressbar.h>
#include <focus.h>

/* ============================================================
 * Layout
 * ============================================================ */

#define WIN_W            720
#define WIN_H            520

#define PAD              12
#define DD_LABEL_X       PAD
#define DD_LABEL_W       60
#define DD_X             (DD_LABEL_X + DD_LABEL_W)
#define DD_Y             10
#define DD_W             420
#define DD_H             24

#define LIST_X           PAD
#define LIST_Y           48
#define LIST_W           (WIN_W - 2 * PAD)
#define LIST_H           140

#define PB_LABEL_X       PAD
#define PB_X             (PAD + 40)
#define PB_Y             (LIST_Y + LIST_H + 12)
#define PB_W             (WIN_W - 2 * PAD - 40)        /* full width to right edge */
#define PB_H             16
#define PB_TEXT_X        (PAD + 40)                    /* aligned with bar start */
#define PB_TEXT_Y        (PB_Y + PB_H + 4)             /* second line, below the bar */
/* Text line is ~12 px high; leave clearance before the table. */
#define TEXT_LINE_H      16

#define STATUS_Y         (WIN_H - 18)
#define TBL_X            PAD
#define TBL_Y            (PB_TEXT_Y + TEXT_LINE_H + 4)
#define TBL_W            (WIN_W - 2 * PAD)
/* Leave a clear gap between the table bottom and the status bar.
 * The widget draws its border at TBL_Y+TBL_H and a row may peek
 * one item-height below if the content is shorter than the box;
 * 32 px of buffer keeps that off the status text. */
#define TBL_H            (STATUS_Y - TBL_Y - 32)

#include <dobui_theme.h>
#define COL_BG           DOBUI_SURFACE
#define COL_LABEL        DOBUI_TEXT
#define COL_HINT         DOBUI_TEXT_ALT
#define COL_STATUS       DOBUI_TEXT
#define COL_STATUS_BG    DOBUI_RELIEF

/* ============================================================
 * Capacities
 * ============================================================ */

#define MAX_DISKS         BLOCK_MAX_DISKS    /* 32 */
#define MAX_ROWS         (MBR_MAX_PRIMARY * 2 + 1)  /* parts + gaps */
#define MAX_DETAIL_ROWS   20
#define PANEL_BUF_SIZE    256
#define PANEL_MAX_ITEMS   8

/* ============================================================
 * Selection model
 * ============================================================ */

typedef enum
{
    SEL_NONE = 0,
    SEL_PARTITION,
    SEL_FREE,
    SEL_DISK_HEADER,        /* (reserved for future "Header" row) */
} sel_kind_t;

typedef struct
{
    sel_kind_t kind;
    int        partition_index;      /* SEL_PARTITION: 0..3 */
    uint32_t   free_start_lba;       /* SEL_FREE */
    uint32_t   free_sectors;         /* SEL_FREE */
} selection_t;

/* ============================================================
 * Panel-command action codes
 *
 * Each item published in the right-side panel has an associated
 * action id, indexed by cmd_idx. event_panel(cmd_idx) dispatches
 * via panel_acts[cmd_idx].
 * ============================================================ */

typedef enum
{
    ACT_NONE = 0,
    ACT_FORMAT,
    ACT_DELETE,
    ACT_LABEL,
    ACT_CREATE,
    ACT_RESCAN,
    ACT_PROPS,
    ACT_REFRESH_USAGE,
    ACT_SMART,
    ACT_TRIM,
} action_id_t;

/* ============================================================
 * Row model — what the listview displays
 * ============================================================ */

typedef enum
{
    ROW_PARTITION,
    ROW_FREE,
} row_kind_t;

typedef struct
{
    row_kind_t kind;
    int        partition_index;
    uint32_t   start_lba;
    uint32_t   sectors;
    char       text[96];
} row_t;

/* ============================================================
 * Globals — state
 * ============================================================ */

static uint32_t       win_id;
static int            win_w   = WIN_W;
static int            win_h   = WIN_H;

/* Disk table mirror (a stable snapshot built from block_*) */
static block_disk_t   disks[MAX_DISKS];      /* copies, so block_enumerate
                                              * during a rescan doesn't
                                              * invalidate pointers we use */
static int            disk_count        = 0;
static int            cur_disk_idx      = 0;     /* index into disks[] */
static char           select_provider[24] = "";  /* --select argv, if any */
static int            boot_disk_idx     = -1;    /* -1 = unknown */
static uint32_t       boot_partition_lba = 0;    /* start_lba of the boot partition,
                                                  * 0 = unknown / not on this disk */

/* Mirror of disk dropdown labels, in disk-table order. */
static char           disk_dd_label[MAX_DISKS][96];
static const char    *disk_dd_label_ptr[MAX_DISKS];

/* Current disk's MBR snapshot. */
static mbr_table_t    cur_mbr;
static bool           cur_mbr_valid     = false;

/* Rows (partitions + free-space gaps) for the listview. */
static row_t          rows[MAX_ROWS];
static int            row_count         = 0;
static const char    *row_text_ptr[MAX_ROWS];

/* Detail table rows. */
static char           detail_keys[MAX_DETAIL_ROWS][32];
static char           detail_vals[MAX_DETAIL_ROWS][96];
static const char    *detail_keys_ptr[MAX_DETAIL_ROWS];
static const char    *detail_vals_ptr[MAX_DETAIL_ROWS];
static int            detail_count      = 0;

/* Selection. */
static selection_t    sel = { SEL_NONE, 0, 0, 0 };

/* Status text. */
static char           status_text[128]  = "Pronto.";

/* Cached DF result for the selected partition's usage bar.
 *
 * dobfs_DFOn does an O(N) scan of the FAT on the server side — too
 * expensive to call from a click handler. DobDisk only invokes it
 * via the explicit "Aggiorna uso" panel command (act_refresh_usage)
 * and caches the result here. rebuild_detail / update_progressbar
 * read these to render the usage row and the progressbar; they
 * never trigger a DF themselves. Cache is keyed by partition slot
 * and invalidated when sel.partition_index changes. */
static int      pb_cached_for_part = -1;     /* partition slot the cache
                                              * applies to; -1 = empty */
static uint64_t pb_cached_total_b  = 0;
static uint64_t pb_cached_used_b   = 0;

/* DF-worker plumbing.
 *
 * The DF call is blocking on the server side (it scans the entire
 * FAT). To keep the GUI responsive we spawn a worker thread that
 * does the call, then posts a custom message back to our own port
 * with the result. The main loop sees it as a non-GUI message and
 * dispatches it via event_request, where we update the cache and
 * redraw. The main thread is never blocked on IPC.
 *
 * df_busy_part: which partition slot is currently being scanned
 * (-1 = no scan in flight). Used both to gate UI feedback ("scan
 * in corso…") and to drop stale replies if the user has since
 * selected a different partition. */
#define DOBDISK_MSG_DF_RESULT  0x4000   /* outside GUI_EVT range */

typedef struct
{
    int       partition_index;
    uint64_t  total_b;
    uint64_t  used_b;
    int       ok;                       /* 1 = DF succeeded, 0 = failed */
    char      svc[32];                  /* service name to DF */
} df_worker_arg_t;

static int      df_busy_part = -1;

/* Panel state. */
static char           panel_buf[PANEL_BUF_SIZE];
static action_id_t    panel_acts[PANEL_MAX_ITEMS];
static int            panel_n           = 0;

/* Widgets. */
static dob_dropdown_t    disk_dd;
static dob_listview_t    part_lv;
static dob_progressbar_t use_pb;
static dob_table_t       detail_tbl;

/* ============================================================
 * Helpers — formatting
 * ============================================================ */

/* Human-readable size from a sector count (assumes 512 B sectors).
 *
 * MainDOB userspace links no compiler-rt, so __udivdi3 (64-bit
 * integer divide) isn't available. We convert sectors → KB by a
 * single >>1 shift (sectors is uint64_t but the shift is fine in
 * 32-bit code path), then narrow to uint32_t for everything after.
 * That caps the largest representable value at 4 TB which is far
 * beyond what we ship today. */
static void
fmt_sectors(uint64_t sectors, char *out, int n)
{
    /* 512 B sectors → KB needs >>1. */
    uint64_t kb64 = sectors >> 1;
    if (kb64 > 0xFFFFFFFFull) kb64 = 0xFFFFFFFFull;  /* clamp at 4 TB */
    uint32_t kb = (uint32_t)kb64;

    if (kb < 1024)
        snprintf(out, n, "%u KB", (unsigned)kb);
    else if (kb < 1024u * 1024u)
    {
        /* MB with one decimal: (kb*10)/1024 = tenths-of-MB. */
        uint32_t tenths = (kb * 10u) / 1024u;
        snprintf(out, n, "%u.%u MB", (unsigned)(tenths / 10),
                                     (unsigned)(tenths % 10));
    }
    else
    {
        /* GB with one decimal: (kb*10)/(1024*1024). 32-bit safe up
         * to ~4 TB which matches our clamp above. */
        uint32_t tenths = (kb / 1024u * 10u) / 1024u;
        snprintf(out, n, "%u.%u GB", (unsigned)(tenths / 10),
                                     (unsigned)(tenths % 10));
    }
}

static void
fmt_uint_grouped(uint32_t v, char *out, int n)
{
    /* "26 644 479" style — thousands grouped by space. */
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%u", (unsigned)v);
    int len = (int)strlen(tmp);
    int o = 0;
    for (int i = 0; i < len && o < n - 2; i++)
    {
        if (i > 0 && (len - i) % 3 == 0) out[o++] = ' ';
        out[o++] = tmp[i];
    }
    out[o] = '\0';
}

static const char *
bus_name(block_bus_t b)
{
    switch (b)
    {
        case BLOCK_BUS_ATA:  return "IDE";
        case BLOCK_BUS_SATA: return "SATA";
        case BLOCK_BUS_USB:  return "USB";
        default:             return "?";
    }
}

static const char *
mbr_type_label(uint8_t t)
{
    if (t == 0x00) return "(libera)";
    if (t == MBR_TYPE_EXFAT) return "exFAT";
    if (t == MBR_TYPE_FAT32_LBA) return "FAT32 LBA";
    if (t == MBR_TYPE_FAT32_CHS) return "FAT32 CHS";
    return "sconosciuto";
}

/* ============================================================
 * Disk discovery & boot-disk detection
 * ============================================================ */

static void
load_disks(void)
{
    block_enumerate();
    int n = block_count();
    disk_count = 0;
    for (int i = 0; i < n && disk_count < MAX_DISKS; i++)
    {
        const block_disk_t *d = block_get(i);
        if (!d) continue;
        disks[disk_count] = *d;     /* snapshot copy */
        char sz[24];
        fmt_sectors(d->total_sectors, sz, sizeof(sz));
        snprintf(disk_dd_label[disk_count], sizeof(disk_dd_label[0]),
                 "%s %d — %s · %s",
                 bus_name(d->bus), d->native_selector,
                 d->model[0] ? d->model : "(senza nome)", sz);
        disk_dd_label_ptr[disk_count] = disk_dd_label[disk_count];
        disk_count++;
    }
}

/* Identify which entry of disks[] is the boot disk by asking
 * the root DobFileSystem instance who its provider is. Also caches
 * the boot partition's start LBA so we can protect only that
 * partition (not the entire disk) from destructive ops. */
static void
probe_boot_disk(void)
{
    boot_disk_idx      = -1;
    boot_partition_lba = 0;
    dobfs_mounted_info_t info;
    if (dobfs_GetMountedOn("DobFileSystem", &info) != 0) return;
    if (!info.is_root_mount) return;

    block_bus_t expected = BLOCK_BUS_NONE;
    if      (strcmp(info.provider, "ata")  == 0) expected = BLOCK_BUS_ATA;
    else if (strncmp(info.provider, "ahci", 4) == 0) expected = BLOCK_BUS_SATA;  /* "ahci" e "ahci_N" */
    else if (strncmp(info.provider, "usbms_", 6) == 0) expected = BLOCK_BUS_USB;

    for (int i = 0; i < disk_count; i++)
        if (disks[i].bus == expected &&
            disks[i].native_selector == info.selector)
        {
            boot_disk_idx      = i;
            boot_partition_lba = info.partition_lba;
            break;
        }
}

/* True iff the (cur_disk_idx, partition_index) coordinate names
 * the partition holding the running root mount. Destructive ops
 * (Formatta, Elimina, Cambia label) are blocked only for this
 * specific partition. Other partitions on the same disk, and free
 * space on the same disk, remain fully editable. */
static bool
is_boot_partition(int partition_index)
{
    if (cur_disk_idx != boot_disk_idx) return false;
    if (partition_index < 0 || partition_index >= MBR_MAX_PRIMARY) return false;
    return cur_mbr.entries[partition_index].start_lba == boot_partition_lba
        && boot_partition_lba != 0;
}

/* ============================================================
 * MBR snapshot for the current disk
 * ============================================================ */

static void
load_current_mbr(void)
{
    cur_mbr_valid = false;
    if (cur_disk_idx < 0 || cur_disk_idx >= disk_count) return;

    uint8_t sec[BLOCK_SECTOR_SIZE];
    if (!block_read(cur_disk_idx, 0, 1, sec)) return;
    partition_mbr_parse(sec, &cur_mbr);
    cur_mbr_valid = true;

    /* An empty / never-initialised disk has no 0x55AA signature.
     * We still treat the parsed table as a valid empty one — the
     * "Crea partizione" path will initialise it on first write. */
    if (!cur_mbr.valid_signature)
    {
        memset(&cur_mbr, 0, sizeof(cur_mbr));
        cur_mbr.valid_signature = true;
    }
}

static bool
commit_current_mbr(void)
{
    if (cur_disk_idx < 0 || cur_disk_idx >= disk_count) return false;
    uint8_t sec[BLOCK_SECTOR_SIZE];
    /* Read first so partition_mbr_serialize preserves any existing
     * boot code at bytes 0..445. On a never-initialised disk the
     * read returns zeros which is fine — the signature still gets
     * written. */
    if (!block_read(cur_disk_idx, 0, 1, sec))
        memset(sec, 0, sizeof(sec));
    partition_mbr_serialize(&cur_mbr, sec);
    if (!block_write(cur_disk_idx, 0, 1, sec)) return false;
    block_flush(cur_disk_idx);   /* commit the MBR to media so a pulled USB stick keeps it */
    /* Ask the driver to re-emit SUBDEVICE_APPEARED / _GONE diffs. */
    block_rescan_partitions(cur_disk_idx);
    return true;
}

/* ============================================================
 * Row rebuild — partitions in MBR + free-space gaps
 * ============================================================ */

static int
mounted_service_for_partition(int part_index, char *out, int n)
{
    /* Service name is "dobfs_<token>" with token =
     * (part_index << 24) | native_selector (24-bit). */
    if (cur_disk_idx < 0 || cur_disk_idx >= disk_count) return -1;
    uint32_t selector = disks[cur_disk_idx].native_selector & 0x00FFFFFFu;
    uint32_t token    = ((uint32_t)part_index << 24) | selector;
    snprintf(out, n, "dobfs_%u", (unsigned)token);
    return 0;
}

/* Resolve which DobFileSystem service (if any) actually mounts the
 * partition at (cur_disk_idx, partition_index). Two candidates:
 *
 *   - The root mount, named "DobFileSystem". It serves whatever
 *     partition MainDOB booted from.
 *   - A secondary mount, named "dobfs_<token>". It exists only if
 *     the user double-clicked the partition's desktop icon.
 *
 * Returns true and fills `out_svc` (capacity n) on hit. Returns
 * false when nothing mounts this partition right now. */
static bool
resolve_mount_service(int partition_index, char *out_svc, int n)
{
    if (cur_disk_idx < 0 || cur_disk_idx >= disk_count) return false;
    if (partition_index < 0 || partition_index >= MBR_MAX_PRIMARY) return false;

    /* Try root first — the cheaper case. is_boot_partition compares
     * LBA, the same key the kernel uses internally, so this is
     * authoritative. */
    if (is_boot_partition(partition_index))
    {
        snprintf(out_svc, n, "DobFileSystem");
        return true;
    }

    /* Secondary candidate. The service may not exist (partition not
     * mounted); the caller decides what to do. */
    mounted_service_for_partition(partition_index, out_svc, n);
    dobfs_mounted_info_t probe;
    return (dobfs_GetMountedOn(out_svc, &probe) == 0);
}

static void
rebuild_rows(void)
{
    row_count = 0;
    if (!cur_mbr_valid) return;

    /* Sort partitions by start_lba so the gap walk is linear.
     * We need to preserve the original MBR index, so work on a
     * (index, entry) pair array — but since mbr_partition_t doesn't
     * carry its own index we use a small parallel array. */
    int    order[MBR_MAX_PRIMARY];
    int    n_active = 0;
    for (int i = 0; i < MBR_MAX_PRIMARY; i++)
        if (cur_mbr.entries[i].sectors > 0)
            order[n_active++] = i;
    /* Sort 'order' by start_lba. Tiny n, bubble. */
    for (int i = 0; i < n_active; i++)
        for (int j = i + 1; j < n_active; j++)
            if (cur_mbr.entries[order[i]].start_lba >
                cur_mbr.entries[order[j]].start_lba)
            { int t = order[i]; order[i] = order[j]; order[j] = t; }

    uint32_t total = (uint32_t)disks[cur_disk_idx].total_sectors;
    uint32_t cursor = 2048;   /* Conventional first usable LBA — leave
                               * the first 2048 sectors free for MBR +
                               * potential bootloader stub. */

    for (int k = 0; k < n_active && row_count < MAX_ROWS; k++)
    {
        const mbr_partition_t *p = &cur_mbr.entries[order[k]];

        /* Emit a "free" row if there's a gap before this partition. */
        if (p->start_lba > cursor + 1)
        {
            row_t *r = &rows[row_count];
            r->kind            = ROW_FREE;
            r->partition_index = -1;
            r->start_lba       = cursor;
            r->sectors         = p->start_lba - cursor;
            char sz[24];
            fmt_sectors(r->sectors, sz, sizeof(sz));
            snprintf(r->text, sizeof(r->text),
                     "— spazio libero — %s", sz);
            row_text_ptr[row_count] = r->text;
            row_count++;
            if (row_count >= MAX_ROWS) break;
        }

        /* Emit the partition row. */
        row_t *r = &rows[row_count];
        r->kind            = ROW_PARTITION;
        r->partition_index = order[k];
        r->start_lba       = p->start_lba;
        r->sectors         = p->sectors;
        char sz[24];
        fmt_sectors(p->sectors, sz, sizeof(sz));
        snprintf(r->text, sizeof(r->text),
                 "#%d  %-10s  %u sec.  %s",
                 order[k] + 1,
                 mbr_type_label(p->type),
                 (unsigned)p->sectors, sz);
        row_text_ptr[row_count] = r->text;
        row_count++;

        cursor = p->start_lba + p->sectors;
    }

    /* Trailing free space. */
    if (cursor < total && row_count < MAX_ROWS)
    {
        row_t *r = &rows[row_count];
        r->kind            = ROW_FREE;
        r->partition_index = -1;
        r->start_lba       = cursor;
        r->sectors         = total - cursor;
        char sz[24];
        fmt_sectors(r->sectors, sz, sizeof(sz));
        snprintf(r->text, sizeof(r->text),
                 "— spazio libero — %s", sz);
        row_text_ptr[row_count] = r->text;
        row_count++;
    }
}

/* ============================================================
 * Detail table rebuild — varies by selection
 * ============================================================ */

static void
detail_add(const char *key, const char *value)
{
    if (detail_count >= MAX_DETAIL_ROWS) return;
    strncpy(detail_keys[detail_count], key,   sizeof(detail_keys[0]) - 1);
    strncpy(detail_vals[detail_count], value, sizeof(detail_vals[0]) - 1);
    detail_keys[detail_count][sizeof(detail_keys[0]) - 1] = '\0';
    detail_vals[detail_count][sizeof(detail_vals[0]) - 1] = '\0';
    detail_keys_ptr[detail_count] = detail_keys[detail_count];
    detail_vals_ptr[detail_count] = detail_vals[detail_count];
    detail_count++;
}

static void
rebuild_detail(void)
{
    detail_count = 0;
    if (cur_disk_idx < 0 || cur_disk_idx >= disk_count) return;
    const block_disk_t *d = &disks[cur_disk_idx];

    char buf[96];

    /* Always include disk-level info. */
    snprintf(buf, sizeof(buf), "%s %d", bus_name(d->bus), d->native_selector);
    detail_add("Disco", buf);
    detail_add("Modello", d->model[0] ? d->model : "(senza nome)");
    fmt_sectors(d->total_sectors, buf, sizeof(buf));
    detail_add("Capacità disco", buf);
    fmt_uint_grouped((uint32_t)d->total_sectors, buf, sizeof(buf));
    detail_add("Settori totali disco", buf);

    if (cur_disk_idx == boot_disk_idx)
        detail_add("Disco contiene boot", "Sì");
    else
        detail_add("Disco contiene boot", "No");

    if (sel.kind == SEL_PARTITION)
    {
        const mbr_partition_t *p = &cur_mbr.entries[sel.partition_index];
        snprintf(buf, sizeof(buf), "%d di 4", sel.partition_index + 1);
        detail_add("MBR slot", buf);
        snprintf(buf, sizeof(buf), "0x%02X — %s",
                 p->type, mbr_type_label(p->type));
        detail_add("Tipo MBR", buf);
        fmt_uint_grouped(p->start_lba, buf, sizeof(buf));
        detail_add("LBA inizio", buf);
        fmt_uint_grouped(p->start_lba + p->sectors - 1, buf, sizeof(buf));
        detail_add("LBA fine", buf);
        fmt_uint_grouped(p->sectors, buf, sizeof(buf));
        detail_add("Settori partizione", buf);
        fmt_sectors(p->sectors, buf, sizeof(buf));
        detail_add("Capacità partizione", buf);
        detail_add("Avviabile", p->active == 0x80 ? "Sì" : "No");
        if (is_boot_partition(sel.partition_index))
            detail_add("Stato", "Partizione di sistema (protetta)");

        /* Mount status: try root first (cheap), fall back to a
         * secondary mount lookup. We use the cheap dob_registry_find
         * probe — no FAT scan. The "Uso" row reflects cached DF data
         * from the last "Aggiorna uso" request, if any. */
        char svc[32];
        if (resolve_mount_service(sel.partition_index, svc, sizeof(svc)))
        {
            snprintf(buf, sizeof(buf), "Sì — servizio %s", svc);
            detail_add("Montata", buf);
            if (pb_cached_for_part == sel.partition_index &&
                pb_cached_total_b > 0)
            {
                uint32_t used_kb  = (uint32_t)(pb_cached_used_b  >> 10);
                uint32_t total_kb = (uint32_t)(pb_cached_total_b >> 10);
                snprintf(buf, sizeof(buf), "%u MB / %u MB",
                         (unsigned)(used_kb  / 1024u),
                         (unsigned)(total_kb / 1024u));
                detail_add("Uso", buf);

                /* Raw sector counts: 512 B per sector. The percentage is
                 * intuitive but a disk utility should also expose the
                 * exact number of sectors written vs total. */
                uint32_t used_sec  = (uint32_t)(pb_cached_used_b  / BLOCK_SECTOR_SIZE);
                uint32_t total_sec = (uint32_t)(pb_cached_total_b / BLOCK_SECTOR_SIZE);
                char a[32], b2[32];
                fmt_uint_grouped(used_sec,  a,  sizeof(a));
                fmt_uint_grouped(total_sec, b2, sizeof(b2));
                snprintf(buf, sizeof(buf), "%s / %s", a, b2);
                detail_add("Settori usati", buf);
            }
        }
        else
        {
            detail_add("Montata", "No");
        }
    }
    else if (sel.kind == SEL_FREE)
    {
        fmt_uint_grouped(sel.free_start_lba, buf, sizeof(buf));
        detail_add("LBA inizio (free)", buf);
        fmt_uint_grouped(sel.free_sectors, buf, sizeof(buf));
        detail_add("Settori liberi", buf);
        fmt_sectors(sel.free_sectors, buf, sizeof(buf));
        detail_add("Spazio libero", buf);

        /* Count empty MBR slots. */
        int empty = 0;
        for (int i = 0; i < MBR_MAX_PRIMARY; i++)
            if (cur_mbr.entries[i].sectors == 0) empty++;
        snprintf(buf, sizeof(buf), "%d / 4", empty);
        detail_add("MBR slot liberi", buf);
    }

    dobtbl_SetRows(&detail_tbl, detail_keys_ptr, detail_vals_ptr, detail_count);
}

/* ============================================================
 * Progressbar — usage of the selected partition (if mounted)
 * ============================================================ */

static char pb_text[64] = "";

/* The progressbar shows partition usage IF we have a fresh DF
 * sample (see pb_cached_* file-scope globals). We do NOT call
 * dobfs_DFOn here, on the click path: DF on a multi-GB FAT32 means
 * scanning megabytes of FAT entries in the server, which makes
 * selection feel frozen. Instead the user triggers a DF refresh
 * explicitly via the "Aggiorna uso" panel command
 * (act_refresh_usage). The cached value persists until the
 * selection changes or another refresh is requested. */
static void
update_progressbar(void)
{
    pb_text[0] = '\0';
    dobpb_SetValue(&use_pb, 0);

    if (sel.kind != SEL_PARTITION)
    {
        pb_cached_for_part = -1;
        return;
    }
    /* Cache is per-partition. Switching partition invalidates it. */
    if (sel.partition_index != pb_cached_for_part)
    {
        pb_cached_for_part = -1;
        snprintf(pb_text, sizeof(pb_text),
                 "non calcolato — clicca \"Aggiorna uso\"");
        return;
    }
    if (pb_cached_total_b == 0) return;

    uint32_t used_kb  = (uint32_t)(pb_cached_used_b  >> 10);
    uint32_t total_kb = (uint32_t)(pb_cached_total_b >> 10);
    if (total_kb == 0) return;

    int pct = (int)((used_kb * 100u) / total_kb);
    if (pct > 100) pct = 100;
    dobpb_SetMax  (&use_pb, 100);
    dobpb_SetValue(&use_pb, pct);
    snprintf(pb_text, sizeof(pb_text),
             "%u / %u MB · %d%%",
             (unsigned)(used_kb  / 1024u),
             (unsigned)(total_kb / 1024u),
             pct);
}

/* ============================================================
 * Panel commands — published every time selection changes
 * ============================================================ */

static int  panel_pos = 0;

static void
panel_reset(void)
{
    panel_buf[0] = '\0';
    panel_pos    = 0;
    panel_n      = 0;
}

static void
panel_add(const char *text, action_id_t id)
{
    if (panel_n >= PANEL_MAX_ITEMS) return;
    int tlen = (int)strlen(text);
    int need = (panel_n > 0 ? 1 : 0) + tlen;
    if (panel_pos + need + 1 >= PANEL_BUF_SIZE) return;

    if (panel_n > 0) panel_buf[panel_pos++] = '\n';
    memcpy(panel_buf + panel_pos, text, (uint32_t)tlen);
    panel_pos += tlen;
    panel_buf[panel_pos] = '\0';
    panel_acts[panel_n++] = id;
}

static void
publish_panel(void)
{
    panel_reset();

    /* Always publish the verbs relevant to the current selection kind.
     * The decision "can I actually do this?" is taken inside each act_*
     * handler, which surfaces a clear popup error when the operation
     * is blocked (boot partition, table full, etc). Hiding verbs
     * "because they would fail" makes the UI harder to learn and
     * hides the reason the operation isn't available — the user has
     * to guess. Showing them + erroring on click is more honest. */
    if (sel.kind == SEL_PARTITION)
    {
        panel_add("Aggiorna uso",   ACT_REFRESH_USAGE);
        panel_add("Formatta…",      ACT_FORMAT);
        panel_add("Cambia label…",  ACT_LABEL);
        panel_add("Elimina…",       ACT_DELETE);
    }
    else if (sel.kind == SEL_FREE)
    {
        panel_add("Crea partizione…", ACT_CREATE);
        panel_add("TRIM regione…",    ACT_TRIM);
    }
    /* Sel == NONE → only the always-on verbs below. */

    panel_add("Proprietà",   ACT_PROPS);
    panel_add("SMART…",      ACT_SMART);
    panel_add("Riscansione", ACT_RESCAN);

    /* Re-publish via the focus manager. It owns the panel after
     * dobfocus_attach_panel in event_start, so contextual clipboard
     * commands (Incolla/Copia tutto/...) automatically appear in
     * front of our base list whenever an InputBox textbox holds
     * the focus. dobfocus_set_base_panel triggers a republish. */
    dobfocus_set_base_panel(panel_buf);
}

/* ============================================================
 * Selection change — wires together all view updates
 * ============================================================ */

static void
apply_selection_from_list(int row_idx)
{
    if (row_idx < 0 || row_idx >= row_count)
    {
        sel.kind = SEL_NONE;
        return;
    }
    const row_t *r = &rows[row_idx];
    if (r->kind == ROW_PARTITION)
    {
        sel.kind            = SEL_PARTITION;
        sel.partition_index = r->partition_index;
    }
    else
    {
        sel.kind           = SEL_FREE;
        sel.free_start_lba = r->start_lba;
        sel.free_sectors   = r->sectors;
    }
}

/* ============================================================
 * Actions
 * ============================================================ */

/* Worker thread body. Runs the blocking DF call, then posts the
 * result back to the main thread via dob_ipc_post. The main loop
 * picks it up in event_request() and updates the cache + redraws.
 *
 * Lifetime: arg is heap-allocated by act_refresh_usage and freed
 * here once the post is queued (the kernel snapshots the payload
 * during sys_post, so we can free our copy right after). */
static void
df_worker(void *raw_arg)
{
    df_worker_arg_t *a = (df_worker_arg_t *)raw_arg;
    dobfs_df_info_t df;
    if (dobfs_DFOn(a->svc, &df) == 0)
    {
        a->ok      = 1;
        a->total_b = df.total_bytes;
        a->used_b  = df.used_bytes;
    }
    else
    {
        a->ok = 0;
    }
    dob_msg_t m;
    memset(&m, 0, sizeof(m));
    m.code         = DOBDISK_MSG_DF_RESULT;
    m.payload      = a;
    m.payload_size = sizeof(*a);
    dob_ipc_post(dobui_port(), &m);
    free(a);
}

/* Triggered by the "Aggiorna uso" panel command. Spawns a worker
 * thread that runs the blocking DF call asynchronously. The user
 * sees a "Calcolo uso in corso…" status update immediately and can
 * keep interacting with the rest of DobDisk; the progressbar updates
 * when the worker's posted result arrives in event_request. */
static void
act_refresh_usage(void)
{
    if (sel.kind != SEL_PARTITION) return;
    char svc[32];
    if (!resolve_mount_service(sel.partition_index, svc, sizeof(svc)))
    {
        dobpopup_Info("Partizione non montata",
                      "Per leggere lo spazio usato, monta prima la"
                      " partizione (doppio clic sulla sua icona sul"
                      " desktop), poi riprova.");
        return;
    }
    if (df_busy_part >= 0)
    {
        dobpopup_Info("Calcolo già in corso",
                      "Una scansione dell'uso è già in corso."
                      " Attendi il suo completamento.");
        return;
    }

    df_worker_arg_t *a = (df_worker_arg_t *)malloc(sizeof(*a));
    if (!a)
    {
        dobpopup_Error("Errore", "Memoria insufficiente.");
        return;
    }
    memset(a, 0, sizeof(*a));
    a->partition_index = sel.partition_index;
    strncpy(a->svc, svc, sizeof(a->svc) - 1);

    if (dob_thread_spawn(df_worker, a) < 0)
    {
        free(a);
        dobpopup_Error("Errore", "Impossibile creare il thread di lavoro.");
        return;
    }
    df_busy_part = sel.partition_index;
    snprintf(status_text, sizeof(status_text),
             "Calcolo uso #%d in corso (la finestra resta usabile)…",
             sel.partition_index + 1);
}

/* Friendly name lookup for the most common SMART attribute IDs.
 * Covers both spinning disks and SSDs/flash (mSATA, the mSATA-to-IDE
 * adapter target included). Most SSD-specific IDs (0xAB-0xB6, 0xCA,
 * 0xE7-0xFA) simply do not exist on HDDs, so listing them creates no
 * conflict. For the few IDs whose meaning differs by media type, the
 * label names both senses rather than guessing the drive type.
 * Returns NULL when the id isn't recognised — the caller falls back
 * to a numeric label so the row still appears in the table. */
static const char *
smart_attr_name(uint8_t id)
{
    switch (id)
    {
        case 0x01: return "Read Error Rate";
        case 0x02: return "Throughput Performance";
        case 0x03: return "Spin-Up Time";
        case 0x04: return "Start/Stop Count";
        case 0x05: return "Reallocated Sectors/Blocks";
        case 0x07: return "Seek Error Rate";
        case 0x08: return "Seek Time Performance";
        case 0x09: return "Power-On Hours";
        case 0x0A: return "Spin Retry Count";
        case 0x0B: return "Calibration Retry Count";
        case 0x0C: return "Power Cycle Count";
        case 0xAA: return "Available Reserved Space";   /* SSD */
        case 0xAB: return "Program Fail Count";          /* SSD */
        case 0xAC: return "Erase Fail Count";            /* SSD */
        case 0xAD: return "Wear Leveling Count";         /* SSD */
        case 0xAE: return "Unexpected Power Loss";       /* SSD */
        case 0xAF: return "Power Loss Protection";       /* SSD */
        case 0xB0: return "Erase Fail Count (chip)";     /* SSD */
        case 0xB1: return "Wear Range Delta";            /* SSD */
        case 0xB3: return "Used Reserved Blocks";        /* SSD */
        case 0xB4: return "Unused Reserved Blocks";      /* SSD */
        case 0xB5: return "Program Fail Count (total)";  /* SSD */
        case 0xB6: return "Erase Fail Count (total)";    /* SSD */
        case 0xB7: return "SATA Downshift Count";
        case 0xB8: return "End-to-End Error";
        case 0xBB: return "Reported Uncorrectable";
        case 0xBC: return "Command Timeout";
        case 0xBD: return "High Fly Writes";
        case 0xBE: return "Airflow Temperature";
        case 0xBF: return "G-Sense Error Rate";
        case 0xC0: return "Power-Off Retract Count";
        case 0xC1: return "Load Cycle Count";
        case 0xC2: return "Temperature";
        case 0xC3: return "Hardware ECC Recovered";
        case 0xC4: return "Reallocation Events";
        case 0xC5: return "Current Pending Sectors";
        case 0xC6: return "Offline Uncorrectable";
        case 0xC7: return "UDMA CRC Errors";
        case 0xC8: return "Write Error Rate";
        case 0xC9: return "Soft Read Error Rate";
        case 0xCA: return "Media Wearout / Life Used";   /* SSD */
        case 0xCB: return "Run Out Cancel";
        case 0xCE: return "Flash Write Errors";          /* SSD */
        case 0xD1: return "Read Channel Margin";
        case 0xD2: return "Flash Reads (raw)";           /* SSD */
        case 0xDC: return "Disk Shift";
        case 0xE7: return "SSD Life Left";               /* SSD */
        case 0xE8: return "Available Reserved Space";    /* SSD */
        case 0xE9: return "Media Wearout Indicator";     /* SSD */
        case 0xEA: return "Total Erase Count";           /* SSD */
        case 0xEB: return "Erase/POR Count";             /* SSD */
        case 0xF0: return "Head Flying Hours";
        case 0xF1: return "Total LBAs Written";
        case 0xF2: return "Total LBAs Read";
        case 0xF3: return "Total LBAs Written (exp.)";
        case 0xF4: return "Total LBAs Read (exp.)";
        case 0xF5: return "Host Sectors Written (NAND)"; /* SSD */
        case 0xF6: return "Host Program Page Count";     /* SSD */
        case 0xF7: return "FTL Program Page Count";      /* SSD */
        case 0xF8: return "Min Spare Blocks";            /* SSD */
        case 0xF9: return "NAND Writes (1 GiB)";         /* SSD */
        case 0xFA: return "Read Retry Count";            /* SSD */
        case 0xFB: return "Min Spare Block Count";       /* SSD */
        case 0xFE: return "Free Fall Protection";
        default:   return NULL;
    }
}

static void
act_trim(void)
{
    if (sel.kind != SEL_FREE) return;
    if (cur_disk_idx < 0 || cur_disk_idx >= disk_count) return;

    const block_disk_t *d = &disks[cur_disk_idx];
    if (d->kind != BLOCK_KIND_SSD || !d->trim_supported)
    {
        dobpopup_Error("TRIM non disponibile",
                       "Il TRIM richiede un SSD che lo supporti. Questo\n"
                       "disco non è un SSD con TRIM, oppure il controller\n"
                       "non espone un percorso TRIM (es. SSD IDE non sul\n"
                       "canale primario master).");
        return;
    }
    if (sel.free_sectors == 0)
    {
        dobpopup_Error("TRIM", "La regione libera selezionata è vuota.");
        return;
    }

    char sz[24];
    fmt_sectors(sel.free_sectors, sz, sizeof(sz));
    char msg[400];
    snprintf(msg, sizeof(msg),
             "Eseguire il TRIM della regione libera?\n\n"
             "LBA %u … %u (%s).\n\n"
             "Comunica all'SSD che questi settori NON allocati sono\n"
             "liberi (utile per le prestazioni). Sicuro: la regione\n"
             "non contiene partizioni.",
             (unsigned)sel.free_start_lba,
             (unsigned)(sel.free_start_lba + sel.free_sectors - 1),
             sz);
    if (dobpopup_Show(POPUP_YESNO, "Conferma TRIM", msg) != 0)
        return;

    if (!block_trim(cur_disk_idx, sel.free_start_lba, sel.free_sectors))
    {
        dobpopup_Error("TRIM fallito",
                       "Il driver ha rifiutato o non ha completato il TRIM.");
        return;
    }

    snprintf(status_text, sizeof(status_text),
             "TRIM completato sulla regione libera (%s).", sz);
    dobpopup_Info("TRIM", "Operazione completata.");
}

static void
act_smart(void)
{
    /* SMART READ DATA returns one 512-byte sector. Layout (ATA-7):
     *   bytes 0..1    vendor structure revision
     *   bytes 2..361  30 vendor attributes × 12 bytes each
     *   bytes 362..   offline/test status (we ignore for now)
     * Each attribute:
     *   off 0      id          (0 = empty slot)
     *   off 1..2   status flags
     *   off 3      current normalised value
     *   off 4      worst normalised value
     *   off 5..10  raw value (vendor-specific; LE uint48)
     *   off 11     reserved
     */
    static uint8_t buf[512];
    uint32_t diag[4] = {0};
    if (!block_get_smart_diag(cur_disk_idx, buf, diag))
    {
        /* Log-less laptop workflow: this popup IS the diagnostic table.
         * reason/step come from the AHCI driver's cmd_diag (see
         * BLOCK_SMARTDIAG_* in dob/block.h); on a drive-side ABORT the
         * ATA Error register is PxTFD bits 15:8 (0x04 = ABRT). */
        static const char *reasons[] = {
            "nessuna info (trasporto o bus non-AHCI)",
            "slot comandi occupato",
            "timeout: il disco non ha risposto",
            "il disco ha ABORTITO il comando",
        };
        static const char *steps[] = {
            "-", "READ DATA", "ENABLE OPERATIONS", "READ DATA (retry)",
        };
        const char *r = (diag[0] < 4) ? reasons[diag[0]] : "?";
        const char *s = (diag[3] < 4) ? steps[diag[3]]   : "?";
        char m[256];
        snprintf(m, sizeof(m),
                 "Impossibile leggere SMART da questo disco.\n"
                 "Passo fallito: %s\n"
                 "Motivo: %s\n"
                 "PxTFD=0x%08x (ST=0x%02x ERR=0x%02x)\n"
                 "PxSERR=0x%08x\n"
                 "Fotografa questa finestra per il bug report.",
                 s, r,
                 (unsigned)diag[1],
                 (unsigned)(diag[1] & 0xFF),
                 (unsigned)((diag[1] >> 8) & 0xFF),
                 (unsigned)diag[2]);
        dobpopup_Error("SMART", m);
        return;
    }

    /* Build human strings before spawning so any allocation problems
     * surface here rather than mid-pipeline. Static arrays — we are
     * single-threaded for this action and the data lives only until
     * the IPC returns (DobTable copies internally). */
    static char  key_buf[30][32];
    static char  val_buf[30][64];
    static const char *keys[30];
    static const char *vals[30];
    int n = 0;

    for (int i = 0; i < 30; i++)
    {
        const uint8_t *a = buf + 2 + i * 12;
        uint8_t id = a[0];
        if (id == 0) continue;          /* unused slot */

        uint8_t cur   = a[3];
        uint8_t worst = a[4];
        /* Raw value: 6 LE bytes. Build the 48-bit value as low and
         * high 32-bit halves separately so the compiler never has to
         * synthesise __ashldi3 for runtime-variable 64-bit shifts. */
        uint32_t raw_lo = (uint32_t)a[5]
                       | ((uint32_t)a[6] << 8)
                       | ((uint32_t)a[7] << 16)
                       | ((uint32_t)a[8] << 24);
        uint32_t raw_hi = (uint32_t)a[9]
                       | ((uint32_t)a[10] << 8);
        /* raw == (raw_hi << 32) | raw_lo, but kept apart. */

        const char *name = smart_attr_name(id);
        if (name)
            snprintf(key_buf[n], sizeof(key_buf[0]),
                     "0x%02X %s", id, name);
        else
            snprintf(key_buf[n], sizeof(key_buf[0]),
                     "0x%02X (vendor)", id);

        /* Temperature attributes (0xC2 Temperature, 0xBE Airflow Temp):
         * the raw48 is a PACKED field, not a number — byte 0 is the
         * current °C and the upper words typically carry min/max. Printed
         * as a plain integer it becomes garbage like "raw=253403725866"
         * (which decodes to 42 °C). Show the temperature; append min/max
         * only when they look sane (non-zero, <=125, min<=cur<=max). */
        if (id == 0xC2 || id == 0xBE)
        {
            uint8_t t_now = a[5];
            uint8_t t_min = a[7];
            uint8_t t_max = a[9];
            if (t_min > 0 && t_max > 0 && t_max <= 125 &&
                t_min <= t_now && t_now <= t_max)
                snprintf(val_buf[n], sizeof(val_buf[0]),
                         "%u \xC2\xB0""C (min %u, max %u)  cur=%u worst=%u",
                         t_now, t_min, t_max, (unsigned)cur, (unsigned)worst);
            else
                snprintf(val_buf[n], sizeof(val_buf[0]),
                         "%u \xC2\xB0""C  cur=%u worst=%u",
                         t_now, (unsigned)cur, (unsigned)worst);

            keys[n] = key_buf[n];
            vals[n] = val_buf[n];
            n++;
            continue;
        }

        /* 64-bit decimal print without __udivmoddi4. MainDOB userspace
         * has no libgcc long-long helpers, so `raw / 10` and `raw % 10`
         * on a uint64_t would fail to link, and even helper routines
         * we write in C using uint64_t shifts compile down to the same
         * missing intrinsics. Instead, do long division by 10 over the
         * 48-bit raw value processed as three 16-bit limbs (high to
         * low), propagating the remainder forward. Every operation is
         * a strict 32-bit divide/modulo, well within compiler support.
         * Worst case: 15 decimal digits for a 48-bit value, fits in 24
         * bytes. */
        char raw_str[24];
        {
            /* Split the 48-bit raw into three 16-bit limbs, MS first. */
            uint16_t limb[3] = {
                (uint16_t)(raw_hi & 0xFFFFu),     /* bits 32..47 */
                (uint16_t)(raw_lo >> 16),         /* bits 16..31 */
                (uint16_t)(raw_lo & 0xFFFFu),     /* bits  0..15 */
            };
            char tmp[24];
            int  tn = 0;

            if ((limb[0] | limb[1] | limb[2]) == 0)
            {
                tmp[tn++] = '0';
            }
            else
            {
                while ((limb[0] | limb[1] | limb[2]) != 0 && tn < (int)sizeof(tmp))
                {
                    uint32_t rem = 0;
                    for (int li = 0; li < 3; li++)
                    {
                        uint32_t cur = (rem << 16) | limb[li];
                        limb[li]    = (uint16_t)(cur / 10u);
                        rem         = cur % 10u;
                    }
                    tmp[tn++] = (char)('0' + rem);
                }
            }
            int ri = 0;
            for (int k = tn - 1; k >= 0 && ri < (int)sizeof(raw_str) - 1; k--)
                raw_str[ri++] = tmp[k];
            raw_str[ri] = '\0';
        }

        snprintf(val_buf[n], sizeof(val_buf[0]),
                 "cur=%u worst=%u raw=%s",
                 (unsigned)cur, (unsigned)worst, raw_str);

        keys[n] = key_buf[n];
        vals[n] = val_buf[n];
        n++;
    }

    if (n == 0)
    {
        dobpopup_Error("SMART",
                       "Il disco ha risposto a SMART READ DATA, ma\n"
                       "tutti gli attributi sono vuoti. La virtualizzazione\n"
                       "(QEMU) spesso non popola la struttura.");
        return;
    }

    char svc[32];
    if (dobtable_Spawn(svc, sizeof(svc)) != 0)
    {
        dobpopup_Error("SMART",
                       "Impossibile aprire la tabella SMART.\n"
                       "Verifica che DobTable.mdl sia installato.");
        return;
    }

    char title[64];
    snprintf(title, sizeof(title), "SMART — %s %u",
             bus_name(disks[cur_disk_idx].bus),
             disks[cur_disk_idx].native_selector);
    dobtable_SetTitle(svc, title);
    dobtable_SetHeaders(svc, "Attributo", "Valore");
    dobtable_AddRows(svc, keys, vals, n);
    dobtable_Show(svc);

    snprintf(status_text, sizeof(status_text),
             "SMART: %d attributi letti.", n);
}

static void
act_rescan(void)
{
    snprintf(status_text, sizeof(status_text), "Riscansione in corso…");
    block_rescan_partitions(cur_disk_idx);
    load_current_mbr();
    rebuild_rows();
    doblv_SetItems  (&part_lv, row_text_ptr, row_count);
    doblv_SetSelected(&part_lv, -1);
    sel.kind = SEL_NONE;
    rebuild_detail();
    update_progressbar();
    publish_panel();
    snprintf(status_text, sizeof(status_text),
             "Riscansione completata. %d partizioni.", row_count);
}

static void
act_properties(void)
{
    char title[64];
    char body[256];
    if (sel.kind == SEL_PARTITION)
    {
        const mbr_partition_t *p = &cur_mbr.entries[sel.partition_index];
        snprintf(title, sizeof(title),
                 "Partizione #%d", sel.partition_index + 1);
        char sz[24];
        fmt_sectors(p->sectors, sz, sizeof(sz));
        snprintf(body, sizeof(body),
                 "Tipo: %s (0x%02X)\n"
                 "LBA: %u..%u\n"
                 "Settori: %u\n"
                 "Dim: %s\n"
                 "Avviabile: %s",
                 mbr_type_label(p->type), p->type,
                 (unsigned)p->start_lba,
                 (unsigned)(p->start_lba + p->sectors - 1),
                 (unsigned)p->sectors, sz, p->active == 0x80 ? "Sì" : "No");
    }
    else
    {
        snprintf(title, sizeof(title), "Disco %s %d",
                 bus_name(disks[cur_disk_idx].bus),
                 disks[cur_disk_idx].native_selector);
        char sz[24];
        fmt_sectors(disks[cur_disk_idx].total_sectors, sz, sizeof(sz));
        snprintf(body, sizeof(body),
                 "Modello: %s\nCapacità: %s\nSettori: %u\nAvvio: %s",
                 disks[cur_disk_idx].model[0]
                     ? disks[cur_disk_idx].model : "(senza nome)",
                 sz, (unsigned)disks[cur_disk_idx].total_sectors,
                 cur_disk_idx == boot_disk_idx ? "Sì" : "No");
    }
    dobpopup_Info(title, body);
}

/* ============================================================
 * Format dialog — a modal overlay drawn inside DobDisk's own
 * window. While g_fmt_open is set, every input event is routed
 * here (the main UI is inert) and draw_all() paints the box on
 * top. Widgets are driven manually (no focus-manager coupling).
 * ============================================================ */

static void draw_all(void);   /* forward decl */

#define FMT_BOX_W    448
#define FMT_BOX_H    372
/* The format dialog is now a real modal child window (see
 * fmt_dialog_open / fmt_vtbl). Its client area IS the box, so every
 * coordinate derived below is relative to the child window's origin:
 * the box sits at (0,0). */
#define FMT_BOX_X    0
#define FMT_BOX_Y    0
#define FMT_FIELD_X  (FMT_BOX_X + 150)
#define FMT_FIELD_W  280
#define FMT_DD_H     24
#define FMT_ROW_H    40
#define FMT_ROW0_Y   (FMT_BOX_Y + 100)
#define FMT_BTN_W    96
#define FMT_BTN_H    30
#define FMT_BTN_Y    (FMT_BOX_Y + FMT_BOX_H - FMT_BTN_H - 16)
#define FMT_OK_X     (FMT_BOX_X + FMT_BOX_W - 2 * FMT_BTN_W - 28)
#define FMT_CANCEL_X (FMT_BOX_X + FMT_BOX_W - FMT_BTN_W - 16)

#define COL_FMT_BG      DOBUI_SURFACE
#define COL_FMT_BORDER  DOBUI_RELIEF
#define COL_FMT_TEXT    DOBUI_TEXT
#define COL_FMT_WARN    DOBUI_DANGER
#define COL_BTN_BG      DOBUI_INSET
#define COL_BTN_OK_BG   DOBUI_SURFACE

static bool           g_fmt_open = false;
static int            g_fmt_part;
static char           g_fmt_info[96];
static char           g_fmt_warn[128];
static int            fmt_native_sec_idx = 0;  /* 0=512, 1=4096; from the disk */
static int            fmt_prev_fs_sel    = -1; /* detect FAT32->exFAT transition */

/* The dialog is a real modal child window owned by DobDisk's main
 * window.  fmt_win is the object; fmt_win_id is its WM id, the target
 * for every dialog draw call (the widgets and draw_fmt_dialog draw
 * into this window, NOT the main win_id). */
static dobui_win_t   *fmt_win    = NULL;
static uint32_t       fmt_win_id = 0;
static const dobui_win_vtbl_t fmt_vtbl;   /* defined after the handlers */

/* Not exported in focus.h, but defined in libdobui/focus.c: removes a widget
 * from the global focus manager it auto-registered into at _Init. */
extern void dobfocus_auto_unregister(void *ctrl);

static dob_dropdown_t fmt_fs_dd;
static dob_dropdown_t fmt_sec_dd;
static dob_dropdown_t fmt_clus_dd;
static dob_dropdown_t fmt_fats_dd;
static dob_textbox_t  fmt_label_tb;
static dob_focus_t    fmt_fm;          /* dialog-private; owns input while modal */

static const char *FMT_FS_ITEMS[]   = { "FAT32", "exFAT" };
static const char *FMT_SEC_ITEMS[]  = { "512 byte", "4096 byte" };
static const char *FMT_CLUS_ITEMS[] = { "Automatica", "4 KB", "8 KB", "16 KB",
                                        "32 KB", "64 KB", "128 KB" };
static const char *FMT_FATS_ITEMS[] = { "2 (predefinito)", "1" };
static const uint32_t FMT_CLUS_BYTES[] =
    { 0u, 4096u, 8192u, 16384u, 32768u, 65536u, 131072u };

/* exFAT uses a single FAT and supports 4096-byte sectors; FAT32 uses 512-byte
 * sectors only and lets the user pick the FAT count. Enforce that here after
 * every interaction so the controls never hold an impossible combination. */
static void
fmt_apply_constraints(void)
{
    if (fmt_fs_dd.selected == 0)        /* FAT32 */
    {
        if (fmt_sec_dd.selected != 0) dobdd_SetSelected(&fmt_sec_dd, 0);
        fmt_sec_dd.enabled  = false;
        fmt_fats_dd.enabled = true;
    }
    else                               /* exFAT */
    {
        fmt_sec_dd.enabled = true;
        /* On entry to exFAT (from FAT32, which pinned 512), preselect the
         * device's native sector size so a 4Kn stick formats aligned. The
         * user can still override. */
        if (fmt_prev_fs_sel == 0)
            dobdd_SetSelected(&fmt_sec_dd, fmt_native_sec_idx);
        if (fmt_fats_dd.selected != 1) dobdd_SetSelected(&fmt_fats_dd, 1);
        fmt_fats_dd.enabled = false;
    }
    fmt_prev_fs_sel = fmt_fs_dd.selected;
}

static void
fmt_dialog_init_widgets(void)
{
    int fx = FMT_FIELD_X, fw = FMT_FIELD_W;
    dobdd_Init(&fmt_fs_dd,    fmt_win_id, fx, FMT_ROW0_Y + 0 * FMT_ROW_H, fw, FMT_DD_H, FMT_FS_ITEMS,   2);
    dobdd_Init(&fmt_sec_dd,   fmt_win_id, fx, FMT_ROW0_Y + 1 * FMT_ROW_H, fw, FMT_DD_H, FMT_SEC_ITEMS,  2);
    dobdd_Init(&fmt_clus_dd,  fmt_win_id, fx, FMT_ROW0_Y + 2 * FMT_ROW_H, fw, FMT_DD_H, FMT_CLUS_ITEMS, 7);
    dobtb_Init(&fmt_label_tb, fmt_win_id, fx, FMT_ROW0_Y + 3 * FMT_ROW_H, fw, FMT_DD_H);
    dobdd_Init(&fmt_fats_dd,  fmt_win_id, fx, FMT_ROW0_Y + 4 * FMT_ROW_H, fw, FMT_DD_H, FMT_FATS_ITEMS, 2);

    /* Widgets auto-register into the global focus manager at _Init. Pull them
     * out and put them in a dialog-private manager, so the modal owns focus/
     * input while it is up (clicks set focus, the textbox accepts typing) and
     * never fights — or gets hit-tested alongside — the main UI's widgets. */
    dobfocus_auto_unregister(&fmt_fs_dd);
    dobfocus_auto_unregister(&fmt_sec_dd);
    dobfocus_auto_unregister(&fmt_clus_dd);
    dobfocus_auto_unregister(&fmt_label_tb);
    dobfocus_auto_unregister(&fmt_fats_dd);

    dobfocus_Init(&fmt_fm);
    dobfocus_Register(&fmt_fm, &fmt_fs_dd,    DOB_CTRL_DROPDOWN);
    dobfocus_Register(&fmt_fm, &fmt_sec_dd,   DOB_CTRL_DROPDOWN);
    dobfocus_Register(&fmt_fm, &fmt_clus_dd,  DOB_CTRL_DROPDOWN);
    dobfocus_Register(&fmt_fm, &fmt_label_tb, DOB_CTRL_TEXTBOX);
    dobfocus_Register(&fmt_fm, &fmt_fats_dd,  DOB_CTRL_DROPDOWN);
}

/* (Re)bind the widgets to the freshly-created child window and reset
 * them to defaults.  Called from on_start, once fmt_win_id is set. */
static void
fmt_dialog_reset_widgets(void)
{
    fmt_dialog_init_widgets();            /* binds to fmt_win_id (child) */
    dobdd_SetSelected(&fmt_fs_dd,   0);
    dobdd_SetSelected(&fmt_sec_dd,  0);
    dobdd_SetSelected(&fmt_clus_dd, 0);
    dobdd_SetSelected(&fmt_fats_dd, 0);
    dobtb_SetText(&fmt_label_tb, "DATI");
    fmt_fs_dd.open = fmt_sec_dd.open = fmt_clus_dd.open = fmt_fats_dd.open = false;
    fmt_prev_fs_sel = -1;
    fmt_apply_constraints();
}

static void
fmt_dialog_open(int part_index)
{
    if (g_fmt_open) return;               /* one dialog at a time */
    g_fmt_part = part_index;

    /* Native sector size of the selected disk drives the exFAT default. */
    fmt_native_sec_idx = (cur_disk_idx >= 0 && cur_disk_idx < disk_count &&
                          disks[cur_disk_idx].native_sector_size == 4096u) ? 1 : 0;

    const mbr_partition_t *p = &cur_mbr.entries[part_index];
    char sz[24];
    fmt_sectors(p->sectors, sz, sizeof(sz));
    snprintf(g_fmt_info, sizeof(g_fmt_info), "Partizione #%d   %s   %s",
             part_index + 1, mbr_type_label(p->type), sz);

    char svc[32];
    mounted_service_for_partition(part_index, svc, sizeof(svc));
    dobfs_mounted_info_t mi;
    if (dobfs_GetMountedOn(svc, &mi) == 0)
        snprintf(g_fmt_warn, sizeof(g_fmt_warn),
                 "ATTENZIONE: partizione montata - smontala prima di procedere.");
    else
        g_fmt_warn[0] = '\0';

    /* Open the real modal child window owned by the main window.  Its
     * on_start (fmt_on_start) binds the widgets and paints the body;
     * the WM blocks the main window until this dialog closes, and
     * rings the attention sound on a click into the blocked parent.
     * Fixed size -> NORESIZE | NOMAXIMIZE. */
    g_fmt_open = true;
    fmt_win = dobui_dialog_open(dobui_primary(), "Formatta partizione",
                                FMT_BOX_W, FMT_BOX_H, &fmt_vtbl, NULL,
                                /*modal=*/true,
                                DOBUI_WIN_NORESIZE | DOBUI_WIN_NOMAXIMIZE);
    if (!fmt_win)
    {
        g_fmt_open = false;
        dobpopup_Error("Errore",
                       "Impossibile aprire la finestra di formattazione.");
    }
}

static void
fmt_dialog_close(void)
{
    if (!fmt_win) { g_fmt_open = false; return; }
    fmt_fs_dd.open = fmt_sec_dd.open = fmt_clus_dd.open = fmt_fats_dd.open = false;

    dobui_win_t *w = fmt_win;
    fmt_win    = NULL;
    fmt_win_id = 0;
    g_fmt_open = false;
    dobui_win_close(w);   /* destroys the child window; unblocks the parent */
}

/* Read the controls, pick the right formatter and run mkfs. Mirrors the old
 * synchronous act_format() tail, now driven by the dialog's OK button. */
static void
fmt_dialog_commit(void)
{
    int part = g_fmt_part;
    const mbr_partition_t *p = &cur_mbr.entries[part];

    bool     is_exfat = (fmt_fs_dd.selected == 1);
    uint32_t bps      = (fmt_sec_dd.selected == 1) ? 4096u : 512u;
    uint32_t clus     = FMT_CLUS_BYTES[fmt_clus_dd.selected];
    uint8_t  nfat     = (fmt_fats_dd.selected == 1) ? 1u : 2u;
    if (!is_exfat && bps != 512u) bps = 512u;

    char label[16];
    {
        const char *t = dobtb_GetText(&fmt_label_tb);
        int i = 0;
        for (; i < 15 && t && t[i]; i++) label[i] = t[i];
        label[i] = '\0';
    }

    const fs_ops_t *ops      = is_exfat ? &exfat_ops : &fat32_ops;
    uint8_t         new_type = is_exfat ? MBR_TYPE_EXFAT : MBR_TYPE_FAT32_LBA;

    /* Tear the dialog window down (this unblocks the main window),
     * then point draws at the main window for the progress + result.
     * The blocking write and any error popup happen with the dialog
     * already gone. */
    fmt_dialog_close();
    dobui_SetActiveWindow(win_id);
    snprintf(status_text, sizeof(status_text), "Formattazione in corso…");
    draw_all();

    mkfs_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.label            = label;
    opts.num_fats         = nfat;
    opts.bytes_per_sector = bps;
    opts.cluster_size     = clus;

    if (!ops->mkfs(cur_disk_idx, p->start_lba, p->sectors, &opts))
    {
        dobpopup_Error("Formattazione fallita",
                       is_exfat
                       ? "Errore nella scrittura del filesystem exFAT."
                       : "Errore nella scrittura del filesystem FAT32. Nota: per"
                         " FAT32 la dimensione cluster massima e' 64 KB.");
        snprintf(status_text, sizeof(status_text), "Formattazione fallita.");
        draw_all();
        return;
    }

    /* Commit the freshly written filesystem to media before any rescan/eject:
     * on USB the structures are still in the stick's write cache otherwise. */
    block_flush(cur_disk_idx);

    if (cur_mbr.entries[part].type != new_type)
    {
        cur_mbr.entries[part].type = new_type;
        commit_current_mbr();
    }
    else
    {
        block_rescan_partitions(cur_disk_idx);
    }

    /* Rebuild the partition list so the row shows the new filesystem type
     * (and the freshly created partition appears), then reselect it so the
     * detail panel and usage bar refresh to the formatted volume. */
    load_current_mbr();
    rebuild_rows();
    doblv_SetItems(&part_lv, row_text_ptr, row_count);
    int sel_row = -1;
    for (int i = 0; i < row_count; i++)
        if (rows[i].kind == ROW_PARTITION && rows[i].partition_index == part)
        { sel_row = i; break; }
    doblv_SetSelected(&part_lv, sel_row);
    if (sel_row >= 0)
    {
        sel.kind            = SEL_PARTITION;
        sel.partition_index = part;
    }
    else
    {
        sel.kind = SEL_NONE;
    }

    snprintf(status_text, sizeof(status_text),
             "Partizione #%d formattata (%s \"%s\").",
             part + 1, is_exfat ? "exFAT" : "FAT32", label);
    rebuild_detail();
    update_progressbar();
    publish_panel();
    draw_all();
}

static bool
fmt_pt_in(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void
fmt_draw_button(int x, const char *text, bool primary)
{
    uint32_t bg = primary ? COL_BTN_OK_BG : COL_BTN_BG;
    uint32_t fg = primary ? DOBUI_TEXT_ALT : COL_FMT_TEXT;
    dobui_FillRect(fmt_win_id, x - 1, FMT_BTN_Y - 1, FMT_BTN_W + 2, FMT_BTN_H + 2, COL_FMT_BORDER);
    dobui_FillRect(fmt_win_id, x, FMT_BTN_Y, FMT_BTN_W, FMT_BTN_H, bg);
    int tw = (int)strlen(text) * 8;
    dobui_DrawText(fmt_win_id, x + (FMT_BTN_W - tw) / 2, FMT_BTN_Y + 8, text, fg, bg);
}

static void
draw_fmt_dialog(void)
{
    dobdd_ClearGhost(&fmt_fs_dd);   dobdd_ClearGhost(&fmt_sec_dd);
    dobdd_ClearGhost(&fmt_clus_dd); dobdd_ClearGhost(&fmt_fats_dd);

    int bx = FMT_BOX_X, by = FMT_BOX_Y, bw = FMT_BOX_W, bh = FMT_BOX_H, lx = FMT_BOX_X + 16;

    /* The window client area is the box; the WM draws the titlebar
     * ("Formatta partizione") and frame, so we only paint the body. */
    dobui_FillRect(fmt_win_id, bx, by, bw, bh, COL_FMT_BG);

    dobui_DrawText(fmt_win_id, lx, by + 42, g_fmt_info, COL_FMT_TEXT, COL_FMT_BG);
    dobui_DrawText(fmt_win_id, lx, by + 60, "Tutti i dati saranno cancellati in modo irreversibile.",
                   COL_FMT_WARN, COL_FMT_BG);
    if (g_fmt_warn[0])
        dobui_DrawText(fmt_win_id, lx, by + 78, g_fmt_warn, COL_FMT_WARN, COL_FMT_BG);

    dobui_DrawText(fmt_win_id, lx, FMT_ROW0_Y + 0 * FMT_ROW_H + 4, "Filesystem:",     COL_FMT_TEXT, COL_FMT_BG);
    dobui_DrawText(fmt_win_id, lx, FMT_ROW0_Y + 1 * FMT_ROW_H + 4, "Settore logico:", COL_FMT_TEXT, COL_FMT_BG);
    dobui_DrawText(fmt_win_id, lx, FMT_ROW0_Y + 2 * FMT_ROW_H + 4, "Dim. cluster:",   COL_FMT_TEXT, COL_FMT_BG);
    dobui_DrawText(fmt_win_id, lx, FMT_ROW0_Y + 3 * FMT_ROW_H + 4, "Etichetta:",      COL_FMT_TEXT, COL_FMT_BG);
    dobui_DrawText(fmt_win_id, lx, FMT_ROW0_Y + 4 * FMT_ROW_H + 4, "Numero di FAT:",  COL_FMT_TEXT, COL_FMT_BG);

    dobdd_Draw(&fmt_fs_dd);   dobdd_Draw(&fmt_sec_dd);   dobdd_Draw(&fmt_clus_dd);
    dobtb_Draw(&fmt_label_tb);
    dobdd_Draw(&fmt_fats_dd);

    fmt_draw_button(FMT_OK_X, "OK", true);
    fmt_draw_button(FMT_CANCEL_X, "Annulla", false);

    dobdd_FlushPopup(&fmt_fs_dd);   dobdd_FlushPopup(&fmt_sec_dd);
    dobdd_FlushPopup(&fmt_clus_dd); dobdd_FlushPopup(&fmt_fats_dd);
}

/* Repaint the dialog window after an interaction.  (The main window
 * is a separate window and never needs redrawing for dialog input.) */
static void
fmt_win_redraw(void)
{
    if (!fmt_win_id) return;
    draw_fmt_dialog();
    dobui_Invalidate(fmt_win_id);
}

static void
fmt_dialog_click(int x, int y)
{
    /* The dialog's focus manager hit-tests its widgets, sets focus on the one
     * clicked (so the textbox starts accepting keys) and drives its OnClick. */
    void *hit = dobfocus_OnClick(&fmt_fm, x, y);
    fmt_apply_constraints();

    if (!hit)   /* click landed outside every field — check the buttons */
    {
        if (fmt_pt_in(x, y, FMT_OK_X, FMT_BTN_Y, FMT_BTN_W, FMT_BTN_H))
        { fmt_dialog_commit(); return; }
        if (fmt_pt_in(x, y, FMT_CANCEL_X, FMT_BTN_Y, FMT_BTN_W, FMT_BTN_H))
        { fmt_dialog_close(); return; }
    }
    fmt_win_redraw();
}

static void
fmt_dialog_key(uint8_t key)
{
    if (key == 27) { fmt_dialog_close(); return; }            /* Esc = Annulla */
    if (key == '\n' || key == '\r')                           /* Enter         */
    {
        /* If a dropdown is open, Enter picks the highlighted item; otherwise
         * Enter is the OK shortcut. */
        if (fmt_fs_dd.open || fmt_sec_dd.open || fmt_clus_dd.open || fmt_fats_dd.open)
        {
            if (dobfocus_OnKey(&fmt_fm, key)) { fmt_apply_constraints(); fmt_win_redraw(); }
        }
        else
        {
            fmt_dialog_commit();
        }
        return;
    }
    if (dobfocus_OnKey(&fmt_fm, key)) { fmt_apply_constraints(); fmt_win_redraw(); }
}

static void
fmt_dialog_scroll(int delta)
{
    if (dobfocus_OnScroll(&fmt_fm, delta)) fmt_win_redraw();
}

/* ---- Dialog window callbacks (the vtable forward-declared above) ---- */

static void fmt_on_start(dobui_win_t *w)
{
    fmt_win_id = dobui_win_id(w);
    fmt_dialog_reset_widgets();   /* bind widgets to the child + defaults */
    draw_fmt_dialog();            /* dobui_dialog_open issues the Invalidate */
}

static void fmt_on_key(dobui_win_t *w, uint8_t key)
{ (void)w; fmt_dialog_key(key); }

static void fmt_on_click(dobui_win_t *w, int x, int y, uint8_t b)
{ (void)w; (void)b; fmt_dialog_click(x, y); }

static void fmt_on_release(dobui_win_t *w, int x, int y, uint8_t b)
{ (void)w; (void)x; (void)y; (void)b; dobfocus_OnRelease(&fmt_fm); fmt_win_redraw(); }

static void fmt_on_move(dobui_win_t *w, int x, int y, uint8_t b)
{ (void)w; (void)b; if (dobfocus_OnDrag(&fmt_fm, x, y)) fmt_win_redraw(); }

static void fmt_on_scroll(dobui_win_t *w, int delta)
{ (void)w; fmt_dialog_scroll(delta); }

static void fmt_on_close(dobui_win_t *w)
{ (void)w; fmt_dialog_close(); }

static const dobui_win_vtbl_t fmt_vtbl = {
    .on_start      = fmt_on_start,
    .on_key        = fmt_on_key,
    .on_click      = fmt_on_click,
    .on_rightclick = fmt_on_click,   /* right-click behaves like click (legacy) */
    .on_release    = fmt_on_release,
    .on_mousemove  = fmt_on_move,
    .on_scroll     = fmt_on_scroll,
    .on_close      = fmt_on_close,
};

static void
act_format(void)
{
    if (sel.kind != SEL_PARTITION) return;
    if (is_boot_partition(sel.partition_index))
    {
        dobpopup_Error("Operazione non permessa",
                       "Questa è la partizione di sistema attualmente"
                       " in uso. Formattarla mentre MainDOB ci sta"
                       " girando sopra causerebbe un crash immediato.");
        return;
    }
    fmt_dialog_open(sel.partition_index);
}

static void
act_delete(void)
{
    if (sel.kind != SEL_PARTITION) return;
    if (is_boot_partition(sel.partition_index))
    {
        dobpopup_Error("Operazione non permessa",
                       "Questa è la partizione di sistema attualmente"
                       " in uso e non può essere eliminata.");
        return;
    }
    const mbr_partition_t *p = &cur_mbr.entries[sel.partition_index];

    char svc[32];
    mounted_service_for_partition(sel.partition_index, svc, sizeof(svc));
    dobfs_mounted_info_t mi;
    bool mounted = (dobfs_GetMountedOn(svc, &mi) == 0);

    char sz[24];
    fmt_sectors(p->sectors, sz, sizeof(sz));
    char msg[400];
    snprintf(msg, sizeof(msg),
             "Eliminare la partizione #%d (%s, %s) dalla tabella MBR?\n\n"
             "L'operazione è irreversibile: i dati non saranno"
             " più accessibili da MainDOB.%s",
             sel.partition_index + 1,
             mbr_type_label(p->type), sz,
             mounted ? "\n\nLa partizione risulta montata."
                       " Smontarla prima è fortemente consigliato."
                     : "");
    if (dobpopup_Show(POPUP_YESNO, "Conferma eliminazione", msg) != 0)
        return;

    /* Zero out the MBR slot. */
    memset(&cur_mbr.entries[sel.partition_index], 0, sizeof(mbr_partition_t));
    if (!commit_current_mbr())
    {
        dobpopup_Error("Errore", "Impossibile scrivere l'MBR.");
        load_current_mbr();   /* reload to discard our in-memory change */
        return;
    }

    snprintf(status_text, sizeof(status_text),
             "Partizione #%d eliminata.", sel.partition_index + 1);
    rebuild_rows();
    doblv_SetItems  (&part_lv, row_text_ptr, row_count);
    doblv_SetSelected(&part_lv, -1);
    sel.kind = SEL_NONE;
    rebuild_detail();
    update_progressbar();
    publish_panel();
}

static void
act_create(void)
{
    if (sel.kind != SEL_FREE) return;

    /* Find a free MBR slot. */
    int slot = -1;
    for (int i = 0; i < MBR_MAX_PRIMARY; i++)
        if (cur_mbr.entries[i].sectors == 0) { slot = i; break; }
    if (slot < 0)
    {
        dobpopup_Error("Tabella piena",
                       "L'MBR ha tutti e 4 gli slot occupati."
                       " Elimina una partizione prima di crearne un'altra.");
        return;
    }

    /* Ask the user how large. Default = the whole free gap.
     * 1 MB = 2048 sectors (with 512 B sectors), so this stays in
     * 32-bit unsigned with the >>11 shift. */
    uint32_t max_mb = sel.free_sectors >> 11;
    char def[16], buf[16];
    snprintf(def, sizeof(def), "%u", (unsigned)max_mb);
    char prompt[160];
    snprintf(prompt, sizeof(prompt),
             "Dimensione in MB (massimo %u):", (unsigned)max_mb);
    if (dobpopup_InputBox("Nuova partizione", prompt, def, buf, sizeof(buf)) != 0)
        return;
    int requested_mb = atoi(buf);
    if (requested_mb <= 0)
    {
        dobpopup_Error("Valore non valido", "Inserisci un numero positivo.");
        return;
    }
    if ((uint32_t)requested_mb > max_mb) requested_mb = (int)max_mb;

    /* MB → sectors: each MB is 2048 sectors of 512 B. (uint32_t)
     * shifted left by 11 — capped above so no overflow concern. */
    uint32_t sectors = (uint32_t)requested_mb << 11;
    if (sectors > sel.free_sectors) sectors = sel.free_sectors;
    if (sectors < 1024)
    {
        dobpopup_Error("Troppo piccola",
                       "La partizione richiesta è inferiore a 512 KB.");
        return;
    }

    /* Build the new MBR entry. */
    cur_mbr.entries[slot].active        = 0;
    cur_mbr.entries[slot].type         = MBR_TYPE_FAT32_LBA;
    cur_mbr.entries[slot].start_lba    = sel.free_start_lba;
    cur_mbr.entries[slot].sectors      = sectors;
    /* CHS fields left zero — modern boot doesn't need them and the
     * partition_mbr_serialize emits zeros for the chs blocks. */

    if (!commit_current_mbr())
    {
        memset(&cur_mbr.entries[slot], 0, sizeof(mbr_partition_t));
        dobpopup_Error("Errore", "Impossibile scrivere l'MBR.");
        return;
    }

    /* The partition now exists. Refresh the list so its row is present, then
     * let the format dialog pick the filesystem and options — its OK handler
     * writes the filesystem and stamps the final MBR type byte. */
    load_current_mbr();
    rebuild_rows();
    doblv_SetItems   (&part_lv, row_text_ptr, row_count);
    doblv_SetSelected(&part_lv, -1);
    sel.kind = SEL_NONE;
    rebuild_detail();
    update_progressbar();
    publish_panel();

    snprintf(status_text, sizeof(status_text),
             "Partizione #%d creata — scegli come formattarla.", slot + 1);
    fmt_dialog_open(slot);
}

static void
act_label(void)
{
    if (sel.kind != SEL_PARTITION) return;
    if (is_boot_partition(sel.partition_index))
    {
        dobpopup_Error("Operazione non permessa",
                       "Modificare l'etichetta della partizione di"
                       " sistema in uso non è sicuro: il BPB è"
                       " attualmente bloccato.");
        return;
    }
    const mbr_partition_t *p = &cur_mbr.entries[sel.partition_index];

    /* Read the BPB to seed the input with the current label. */
    uint8_t bpb[BLOCK_SECTOR_SIZE];
    char def[16] = "";
    if (block_read(cur_disk_idx, p->start_lba, 1, bpb))
    {
        /* FAT32 BPB volume_label is 11 bytes at offset 71. */
        int copied = 0;
        for (int i = 0; i < 11 && copied < (int)sizeof(def) - 1; i++)
        {
            char c = (char)bpb[71 + i];
            if (c == ' ' && copied == 0) continue;  /* skip leading pad */
            def[copied++] = c;
        }
        while (copied > 0 && def[copied - 1] == ' ') copied--;
        def[copied] = '\0';
    }

    char buf[16];
    if (dobpopup_InputBox("Cambia label",
                          "Nuovo nome del volume (max 11 caratteri):",
                          def, buf, sizeof(buf)) != 0)
        return;

    /* Pad to 11 chars uppercase ASCII. */
    char pad[11];
    int i;
    for (i = 0; i < 11 && buf[i]; i++)
    {
        char c = buf[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        pad[i] = c;
    }
    for (; i < 11; i++) pad[i] = ' ';

    /* Rewrite the BPB sector with the new label only. */
    if (!block_read(cur_disk_idx, p->start_lba, 1, bpb))
    {
        dobpopup_Error("Errore", "Impossibile leggere il BPB.");
        return;
    }
    memcpy(bpb + 71, pad, 11);
    if (!block_write(cur_disk_idx, p->start_lba, 1, bpb))
    {
        dobpopup_Error("Errore", "Impossibile scrivere il BPB.");
        return;
    }

    snprintf(status_text, sizeof(status_text),
             "Label di #%d aggiornata.", sel.partition_index + 1);
    rebuild_detail();
}

/* ============================================================
 * Drawing
 * ============================================================ */

static void
draw_all(void)
{
    dobui_FillRect(win_id, 0, 0, win_w, win_h, COL_BG);
    dobui_DrawText(win_id, DD_LABEL_X, DD_Y + 4,
                   "Disco:", COL_LABEL, COL_BG);
    dobdd_ClearGhost(&disk_dd);

    /* Header above the listview. */
    char hdr[96];
    if (cur_disk_idx >= 0 && cur_disk_idx < disk_count)
        snprintf(hdr, sizeof(hdr), "Partizioni di %s %d  (%d voci)",
                 bus_name(disks[cur_disk_idx].bus),
                 disks[cur_disk_idx].native_selector,
                 row_count);
    else
        snprintf(hdr, sizeof(hdr), "Nessun disco selezionato");
    dobui_DrawText(win_id, LIST_X, LIST_Y - 16, hdr, COL_LABEL, COL_BG);

    doblv_Draw(&part_lv);

    /* Progressbar row — bar on its own line, usage caption underneath
     * on a second line so long text ("uso sconosciuto…") fits without
     * truncating against the right edge of the window. */
    dobui_DrawText(win_id, PB_LABEL_X, PB_Y + 1,
                   "Uso:", COL_LABEL, COL_BG);
    dobpb_Draw(&use_pb);
    if (pb_text[0])
        dobui_DrawText(win_id, PB_TEXT_X, PB_TEXT_Y,
                       pb_text, COL_LABEL, COL_BG);
    else
        dobui_DrawText(win_id, PB_TEXT_X, PB_TEXT_Y,
                       "(seleziona una partizione)", COL_HINT, COL_BG);

    /* Detail table — no header label, the table is self-explanatory
     * with its Proprietà / Valore column headers. */
    dobtbl_Draw(&detail_tbl);

    /* Status bar. */
    dobui_FillRect(win_id, 0, STATUS_Y, win_w, win_h - STATUS_Y, COL_STATUS_BG);
    dobui_DrawText(win_id, PAD, STATUS_Y + 2,
                   status_text, COL_STATUS, COL_STATUS_BG);

    /* Draw the dropdown last (its popup must overlay everything). */
    dobdd_Draw(&disk_dd);
    dobdd_FlushPopup(&disk_dd);

    dobui_Invalidate(win_id);
}

/* ============================================================
 * Event handlers
 * ============================================================ */

void
event_start(void)
{
    win_id = dobui_window();
    win_w  = dobui_width();
    win_h  = dobui_height();

    load_disks();
    probe_boot_disk();

    if (disk_count > 0) cur_disk_idx = 0;

    /* Honour --select <provider>: "usbms_<N>" maps to (BLOCK_BUS_USB,
     * selector N). Single pass, no retries: the DAS menu action runs
     * opcode 67 (WAIT_READY rendezvous) on the sub-driver BEFORE
     * spawning us, so by the time we enumerate, the stick is
     * guaranteed up — ordering by design, event-driven, the project
     * way. */
    if (select_provider[0] &&
        strncmp(select_provider, "usbms_", 6) == 0)
    {
        int want = atoi(select_provider + 6);
        for (int i = 0; i < disk_count; i++)
            if (disks[i].bus == BLOCK_BUS_USB &&
                (int)disks[i].native_selector == want)
            {
                cur_disk_idx = i;
                break;
            }
    }
    /* "sata_<porta>": icona desktop di un disco interno AHCI (HDD/SSD,
     * DAS disk_hdd/disk_ssd). Stessa semantica del ramo usbms. */
    else if (select_provider[0] &&
             strncmp(select_provider, "sata_", 5) == 0)
    {
        /* Il token delle icone-device AHCI e' (0x7D<<24)|porta (vedi
         * AHCI_DISK_ICON_TOKEN): i 24 bit bassi sono la porta. */
        int want = atoi(select_provider + 5) & 0x00FFFFFF;
        for (int i = 0; i < disk_count; i++)
            if (disks[i].bus == BLOCK_BUS_SATA &&
                (int)disks[i].native_selector == want)
            {
                cur_disk_idx = i;
                break;
            }
    }

    load_current_mbr();
    rebuild_rows();

    /* Widget init. */
    dobdd_Init(&disk_dd, win_id, DD_X, DD_Y, DD_W, DD_H,
               disk_dd_label_ptr, disk_count);
    dobdd_SetSelected(&disk_dd, cur_disk_idx);

    doblv_Init(&part_lv, win_id, LIST_X, LIST_Y, LIST_W, LIST_H);
    doblv_SetItems(&part_lv, row_text_ptr, row_count);

    dobpb_Init(&use_pb, win_id, PB_X, PB_Y, PB_W, PB_H);
    dobpb_SetMax(&use_pb, 100);

    dobtbl_Init(&detail_tbl, win_id, TBL_X, TBL_Y, TBL_W, TBL_H);
    dobtbl_SetHeaders(&detail_tbl, "Proprietà", "Valore");

    rebuild_detail();
    update_progressbar();

    /* Hand the panel to the focus-manager singleton. From now on,
     * focus transitions (especially when an InputBox grabs the
     * keyboard) automatically swap clipboard commands into the
     * panel and restore our base on focus loss. publish_panel()
     * just calls dobfocus_set_base_panel with the new base list. */
    dobfocus_attach_panel(win_id, "");
    publish_panel();
    draw_all();
}

/* Helper called when the disk dropdown selection changes — extracted
 * because the same logic runs from a click and from a keyboard pick. */
static void
on_disk_changed(void)
{
    const char *sel_text = dobdd_GetSelectedText(&disk_dd);
    int new_idx = -1;
    if (sel_text)
        for (int i = 0; i < disk_count; i++)
            if (strcmp(sel_text, disk_dd_label[i]) == 0)
            { new_idx = i; break; }
    if (new_idx < 0 || new_idx == cur_disk_idx) return;

    cur_disk_idx = new_idx;
    load_current_mbr();
    rebuild_rows();
    doblv_SetItems   (&part_lv, row_text_ptr, row_count);
    doblv_SetSelected(&part_lv, -1);
    sel.kind = SEL_NONE;
    rebuild_detail();
    update_progressbar();
    publish_panel();
    snprintf(status_text, sizeof(status_text),
             "Disco %s %d selezionato.",
             bus_name(disks[cur_disk_idx].bus),
             disks[cur_disk_idx].native_selector);
}

/* Helper called when the partition listview selection changes — same
 * fan-out from click and from keyboard. */
static void
on_part_selection_changed(void)
{
    int idx = doblv_GetSelectedIndex(&part_lv);
    apply_selection_from_list(idx);
    rebuild_detail();
    update_progressbar();
    publish_panel();
}

void
event_mouseclick(int x, int y, uint8_t buttons)
{
    (void)buttons;

    /* The focus manager hit-tests every registered widget (the
     * dropdown, listview and table all self-registered at _Init),
     * sets focus on the one that was clicked, refreshes the panel,
     * and returns a pointer so we can do widget-specific follow-up. */
    void *hit = dobfocus_click(x, y);

    if (hit == &disk_dd)
    {
        on_disk_changed();
    }
    else if (hit == &part_lv)
    {
        on_part_selection_changed();
    }
    /* Other clicks (table internal drag, header text, padding) are
     * a no-op at this level — just repaint and let widgets handle
     * their internals. */

    draw_all();
}

void
event_mousemove(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (dobfocus_drag(x, y)) draw_all();
}

void
event_mouserelease(int x, int y, uint8_t buttons)
{
    (void)x; (void)y; (void)buttons;
    dobfocus_release();
    draw_all();
}

void
event_dblclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    /* A double-click on a partition row is a convenient shortcut
     * to Properties. */
    void *hit = dobfocus_dblclick(x, y);
    if (hit == &part_lv)
    {
        on_part_selection_changed();
        act_properties();
    }
    draw_all();
}

void
event_key(uint8_t key)
{
    if (!dobfocus_key(key)) return;

    /* The focused widget may have moved its selection — update any
     * derived state. We can't tell *which* widget consumed the key
     * without re-checking, but only the dropdown and listview are
     * navigable, and their selections only change for arrow keys,
     * so doing both updates is harmless and cheap. */
    dob_ctrl_type_t t = dobfocus_get_focused_type();
    if (t == DOB_CTRL_DROPDOWN)       on_disk_changed();
    else if (t == DOB_CTRL_LISTVIEW)  on_part_selection_changed();
    draw_all();
}

void
event_scroll(int delta)
{
    if (dobfocus_scroll(delta)) draw_all();
}

void
event_panel(int cmd_idx)
{
    if (g_fmt_open) return;   /* contextual panel is inert while the dialog is modal */

    /* Contextual clipboard commands first — when an InputBox textbox
     * holds the focus, this is where Incolla / Pulisci / Copia tutto
     * land. We never see them as base commands. */
    if (dobfocus_panel(cmd_idx))
    {
        draw_all();
        return;
    }

    if (cmd_idx < 0 || cmd_idx >= panel_n) return;
    switch (panel_acts[cmd_idx])
    {
        case ACT_FORMAT:         act_format();          break;
        case ACT_DELETE:         act_delete();          break;
        case ACT_LABEL:          act_label();           break;
        case ACT_CREATE:         act_create();          break;
        case ACT_RESCAN:         act_rescan();          break;
        case ACT_PROPS:          act_properties();      break;
        case ACT_REFRESH_USAGE:  act_refresh_usage();   break;
        case ACT_SMART:          act_smart();           break;
        case ACT_TRIM:           act_trim();            break;
        default: break;
    }
    draw_all();
}

void
event_resize(int w, int h)
{
    win_w = w;
    win_h = h;
    draw_all();
}

void
event_close(void)
{
    dobui_quit();
}

/* event_request — destination of out-of-band IPC posts.
 *
 * The DF worker thread sends its result here via
 * dob_ipc_post(my_port, msg). libdobui's main loop sees the
 * out-of-GUI-range code and routes it to event_request. We
 * decode the payload, update the cache, and redraw. */
void
event_request(dob_msg_t *msg)
{
    if (msg->code == DOBDISK_MSG_DF_RESULT && msg->payload &&
        msg->payload_size >= sizeof(df_worker_arg_t))
    {
        const df_worker_arg_t *r = (const df_worker_arg_t *)msg->payload;
        if (r->ok)
        {
            /* Only honour the result if the user hasn't moved on to
             * another partition meanwhile. */
            if (sel.kind == SEL_PARTITION &&
                sel.partition_index == r->partition_index)
            {
                pb_cached_for_part = r->partition_index;
                pb_cached_total_b  = r->total_b;
                pb_cached_used_b   = r->used_b;
                snprintf(status_text, sizeof(status_text),
                         "Uso aggiornato per #%d.",
                         r->partition_index + 1);
                rebuild_detail();
                update_progressbar();
            }
            else
            {
                /* Result for a partition we're no longer showing —
                 * still cache it for when the user returns to it. */
                pb_cached_for_part = r->partition_index;
                pb_cached_total_b  = r->total_b;
                pb_cached_used_b   = r->used_b;
                snprintf(status_text, sizeof(status_text),
                         "Uso di #%d calcolato (selezione cambiata).",
                         r->partition_index + 1);
            }
        }
        else
        {
            snprintf(status_text, sizeof(status_text),
                     "Calcolo uso di #%d fallito.",
                     r->partition_index + 1);
        }
        df_busy_part = -1;
        draw_all();
    }
}

int
main(int argc, char **argv)
{
    /* --select <provider>: preselect a disk by its provider service
     * name (e.g. "usbms_0"). Sent by DAS menu items — the pendrive
     * icon's "Formatta..." spawns us pointed at the right stick. */
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], "--select") == 0 && i + 1 < argc)
            strncpy(select_provider, argv[i + 1],
                    sizeof(select_provider) - 1);

    /* No dobui_set_panel here — the focus manager takes ownership
     * via dobfocus_attach_panel inside event_start, and the very
     * first publish_panel call right after fills it with our real
     * base list. */
    dobui_run("DobDisk", WIN_W, WIN_H);
    return 0;
}
