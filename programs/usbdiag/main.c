/* usbdiag — USB UHCI hotplug-pipeline diagnostic.
 *
 * Issues UHCI_OP_GET_DIAG (opcode 6) to the "usb_uhci" service and renders
 * the bring-up + enumeration + DAS-match snapshot in a DobTable. Same
 * reason atadiag exists: real hardware (Compaq Armada E500) without a
 * serial console — the driver's debug_print() trail is invisible once
 * dobinterface owns the screen, so the facts that decide "why is there no
 * pendrive icon" are latched in the driver and read back here.
 *
 * The decisive rows are:
 *   "IRQ ricevuti"        — 0 with a device plugged = dead IRQ delivery
 *   "LEGSUP prima/adesso" — whether the BIOS handoff happened and held
 *   "Stato FSM"           — where enumeration is parked right now
 *   "Ultimo errore"       — which step failed, in the driver's own words
 *   "File DAS USB letti"  — 0 = staging failure (DAS/USB not on the disk)
 *
 * Reading the verdict against the pipeline:
 *   driver missing -> hotplug match;  CCS=0 -> electrical;  IRQ=0 ->
 *   routing/handoff;  enum error -> reset/transfer;  no DAS -> staging;
 *   announce ok but no icon -> the bug is in hotplug/dobinterface.
 *
 * Same shape as atadiag: query, spawn a DobTable, populate, Show, exit.
 * Run it again after re-plugging the device for a fresh snapshot.
 *
 * v1.6.0: a "Sweep UHCI" table queries EVERY registered usb_uhci
 * instance up front (the ICH8 in the Extensa 5220 exposes five UHCI
 * companions); the detailed block then auto-targets the controller that
 * actually sees the device instead of always instance 0.
 */
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/types.h>
#include <dob/usb_uhci_protocol.h>
#include <dob/hotplug.h>
#include <DobFileSystem.h>
#include <DobTable.h>
#include <DobPopup.h>

/* Phased investigation. The driver is registered but mute, and every
 * pre-loop call in it is a bounded syscall EXCEPT the synchronous attach
 * to hotplug — so the suspects are exactly two: hotplug wedged, or the
 * driver wedged at a specific phase. Each phase below leaves an
 * unambiguous on-screen breadcrumb:
 *   - frozen on "FASE 1/3"          -> HOTPLUG itself is wedged: it is
 *                                      what blocks the driver too;
 *   - bubble table, state ATTACHING -> driver stuck in the READY
 *                                      handshake with hotplug;
 *   - bubble table, state LIVE      -> attach done; driver wedged inside
 *                                      uhci_init_hw or its event loop;
 *   - bubble table, no UHCI bubble  -> hotplug never matched/spawned it;
 *   - frozen on "FASE 3/3"          -> driver mute despite the bubble
 *                                      verdict above (read it!);
 *   - full table                    -> read the "Diagnosi" row. */

static const char *bubble_state_text(uint8_t st)
{
    switch (st)
    {
        case BUBBLE_SPAWNING:  return "SPAWNING (driver in avvio)";
        case BUBBLE_ATTACHING: return "ATTACHING (handshake READY non completato)";
        case BUBBLE_LIVE:      return "LIVE (attach completato)";
        case BUBBLE_DETACHING: return "DETACHING";
        case BUBBLE_DEAD:      return "DEAD";
        default:               return "?";
    }
}

static int usbdiag_now_rc    = -100;  /* dobfs_List of DAS/USB from THIS process */
static int usbdiag_now_count = 0;
static uint32_t usbdiag_first_size = 0xFFFFFFFFu; /* size of first .das */

/* Show a 2-row "phase" table. Returns 0 on success. */
static int phase_table(char *svc, int cap, const char *phase,
                       const char *if_stuck)
{
    if (dobtable_Spawn(svc, cap) != 0) return -1;
    const char *k[2] = { "Fase", "Se resta qui" };
    const char *v[2] = { phase, if_stuck };
    dobtable_SetTitle(svc, "Diagnostica USB (UHCI)");
    dobtable_SetHeaders(svc, "Parametro", "Valore");
    dobtable_AddRows(svc, k, v, 2);
    dobtable_Show(svc);
    return 0;
}

#define MAX_ROWS 28
static char  key_buf[MAX_ROWS][40];
static char  val_buf[MAX_ROWS][80];
static const char *keys[MAX_ROWS];
static const char *vals[MAX_ROWS];
static int   row_count = 0;

/* The main table is spawned EARLY and rows are published IMMEDIATELY:
 * a diagnostic tool that gathers everything first and renders last
 * shows NOTHING exactly when the system is sickest (field: a wedged
 * driver query left only the transient tables on screen). With live
 * publishing, a blocking query leaves its own sentinel row visible —
 * the freeze itself becomes readable. */
static char main_svc[32];
static int  main_table_up = 0;

static void add_row(const char *k, const char *v)
{
    if (row_count >= MAX_ROWS) return;
    snprintf(key_buf[row_count], sizeof(key_buf[0]), "%s", k);
    snprintf(val_buf[row_count], sizeof(val_buf[0]), "%s", v);
    keys[row_count] = key_buf[row_count];
    vals[row_count] = val_buf[row_count];
    const char *kp = keys[row_count], *vp = vals[row_count];
    row_count++;
    if (main_table_up)
        dobtable_AddRows(main_svc, &kp, &vp, 1);
}

static void main_table_open(void)
{
    if (main_table_up) return;
    if (dobtable_Spawn(main_svc, sizeof(main_svc)) != 0) return;
    dobtable_SetTitle(main_svc, "Diagnostica USB (UHCI)");
    dobtable_SetHeaders(main_svc, "Parametro", "Valore");
    if (row_count > 0)
        dobtable_AddRows(main_svc, keys, vals, row_count);
    dobtable_Show(main_svc);
    main_table_up = 1;
}

static void add_yesno(const char *k, uint8_t b)
{
    add_row(k, b ? "Si" : "No");
}

static const char *state_text(uint8_t st)
{
    switch (st)
    {
        case UHCI_ST_IDLE_SUSPENDED: return "In attesa (Global Suspend)";
        case UHCI_ST_PORT_RESETTING: return "Reset porta in corso";
        case UHCI_ST_SETADDR_SENT:   return "SET_ADDRESS inviato";
        case UHCI_ST_DEVDESC_SENT:   return "Lettura device descriptor";
        case UHCI_ST_CONFDESC_SENT:  return "Lettura config descriptor";
        case UHCI_ST_ENUM_DONE:      return "Enumerazione completata";
        case UHCI_ST_DEVICE_READY:   return "Dispositivo pronto (attivo)";
        case UHCI_ST_DEVICE_IDLE:    return "Dispositivo pronto (idle, RD armato)";
        case UHCI_ST_ENUM_ERROR:     return "Errore di enumerazione";
        case UHCI_ST_ENUM_ERROR + 1: return "Sveglia in corso (FGR)";
        case UHCI_ST_ENUM_ERROR + 2: return "Sveglia in corso (TRSMRCY)";
        case UHCI_ST_ENUM_ERROR + 3: return "Debounce connect (100 ms)";
        case UHCI_ST_ENUM_ERROR + 4: return "Verifica distacco (50 ms)";
        default:                     return "Sconosciuto";
    }
}

int main(void)
{
    char svc[32];

    /* Instance chosen for the detailed FASE-3 block. The sweep below
     * picks the UHCI companion that actually sees the device (CCS=1) or
     * any activity (resume/IRQ); falls back to instance 0 when none is
     * interesting (e.g. the device is captured by EHCI). */
    uint32_t detail_port = 0;
    char     detail_name[24] = "usb_uhci";

    /* Main table FIRST: it needs no queries. Everything below appends
     * live rows; any wedged subsystem leaves its sentinel as the last
     * visible row — the order of interrogation is now driver (always
     * answers), sub-driver, hotplug LAST: a catatonic hotplug (field,
     * Armada) can no longer blind the whole tool at step one. */
    main_table_open();

    /* Count + name every registered UHCI instance (one summary row).
     * Per-instance detail follows in the sweep block below. */
    {
        char insts[96] = ""; int ni = 0;
        for (int n = 0; n < 8; n++)
        {
            char nm[24];
            if (n == 0) snprintf(nm, sizeof(nm), "usb_uhci");
            else        snprintf(nm, sizeof(nm), "usb_uhci_%d", n);
            if (dob_registry_find(nm))
            {
                if (ni) strncat(insts, ", ", sizeof(insts)-strlen(insts)-1);
                strncat(insts, nm, sizeof(insts)-strlen(insts)-1);
                ni++;
            }
        }
        char row[112];
        snprintf(row, sizeof(row), "%d (%s)", ni, ni ? insts : "nessuno");
        add_row("Controller UHCI attivi", row);
    }

    /* ===== Sweep multi-controller: GET_DIAG EVERY usb_uhci instance =====
     * usbdiag historically detailed only instance 0; on the Extensa 5220
     * the ICH8 exposes FIVE UHCI companions and an inserted device lands
     * on whichever one fronts the physical port used — not necessarily
     * instance 0. This table queries every registered instance so the
     * controller that actually sees the connect is visible at a glance:
     * the row with "P1:DEV"/"P2:DEV", res>0 or irq>0 is the live one, and
     * the detailed block below auto-targets it. Read-only IPC per
     * instance — no polling added to any driver. */
    {
        static char sk[8][40], sv[8][80];
        static const char *skp[8], *svp[8];
        int sn = 0;
        for (int n = 0; n < 8 && sn < 8; n++)
        {
            char nm[24];
            if (n == 0) snprintf(nm, sizeof(nm), "usb_uhci");
            else        snprintf(nm, sizeof(nm), "usb_uhci_%d", n);
            uint32_t p = dob_registry_find(nm);
            if (!p) continue;

            dob_msg_t m = {0}, r = {0};
            m.code = UHCI_OP_GET_DIAG;
            int ok = (dob_ipc_call(p, &m, &r) == DOB_OK &&
                      r.payload && r.payload_size >= sizeof(uhci_diag_t));
            snprintf(sk[sn], sizeof(sk[sn]), "%s", nm);
            if (!ok)
                snprintf(sv[sn], sizeof(sv[sn]),
                         "GET_DIAG muto (driver non aggiornato?)");
            else
            {
                const uhci_diag_t *d = (const uhci_diag_t *)r.payload;
                int ccs0 = d->portsc[0] & 1, ccs1 = d->portsc[1] & 1;
                snprintf(sv[sn], sizeof(sv[sn]),
                         "%04X CMD=%04X STS=%04X P1:%s P2:%s "
                         "irq=%u res=%u shr=%u",
                         (unsigned)d->pci_device,
                         (unsigned)d->usbcmd, (unsigned)d->usbsts,
                         ccs0 ? "DEV" : "-", ccs1 ? "DEV" : "-",
                         (unsigned)d->cnt_irq, (unsigned)d->cnt_resume,
                         (unsigned)d->cnt_shared);
                /* First instance with a device, or any IRQ/resume since
                 * boot, wins the detailed block below. */
                if (!detail_port && (ccs0 || ccs1 ||
                                     d->cnt_resume || d->cnt_irq))
                {
                    detail_port = p;
                    snprintf(detail_name, sizeof(detail_name), "%s", nm);
                }
            }
            skp[sn] = sk[sn]; svp[sn] = sv[sn];
            sn++;
        }
        if (sn > 0)
        {
            char ssvc[32];
            if (dobtable_Spawn(ssvc, sizeof(ssvc)) == 0)
            {
                dobtable_SetTitle(ssvc, "Sweep UHCI — tutte le istanze");
                dobtable_SetHeaders(ssvc, "Istanza", "porte / IRQ");
                dobtable_AddRows(ssvc, skp, svp, sn);
                dobtable_Show(ssvc);
            }
        }
    }

    uint32_t port = detail_port ? detail_port
                                : dob_registry_find("usb_uhci");
    if (!port)
    {
        dobpopup_Error("usbdiag",
            "Servizio usb_uhci non trovato.\n"
            "Il driver non e' in esecuzione: o hotplug non ha\n"
            "trovato un controller UHCI su PCI, o usb_uhci.mdl\n"
            "manca dall'immagine, o il driver e' morto all'avvio.");
        return 1;
    }

    int have_wait = (phase_table(svc, sizeof(svc),
        "3/3 — interrogo il driver usb_uhci (GET_DIAG)...",
        "driver registrato ma NON risponde: leggi il verdetto "
        "nella tabella delle bubble") == 0);

    dob_msg_t msg = {0}, reply = {0};
    /* Sentinel: if the driver is wedged, this row is the LAST thing
     * published — and that is the diagnosis. */
    add_row("Fase 3", "interrogo usb_uhci (se resta qui: driver bloccato)");

    msg.code = UHCI_OP_GET_DIAG;
    int call_rc = dob_ipc_call(port, &msg, &reply);

    if (have_wait)
        dobtable_Close(svc);

    if (call_rc != DOB_OK)
    {
        dobpopup_Error("usbdiag",
            "Chiamata GET_DIAG fallita.\n"
            "Il driver usb_uhci caricato e' aggiornato?");
        return 1;
    }
    if (!reply.payload || reply.payload_size < sizeof(uhci_diag_t))
    {
        dobpopup_Error("usbdiag",
            "Opcode GET_DIAG non supportato.\n"
            "Ricompila e reinstalla il driver usb_uhci.");
        return 1;
    }

    const uhci_diag_t *d = (const uhci_diag_t *)reply.payload;
    char tmp[80];

    add_row("Versione usbdiag", "1.7.0 (modalita consegna IRQ)");
    add_row("Istanza in dettaglio", detail_name);
    add_yesno("Init hardware riuscito", d->init_ok);

    snprintf(tmp, sizeof(tmp), "%04X:%04X",
             (unsigned)d->pci_vendor, (unsigned)d->pci_device);
    add_row("Controller PCI", tmp);

    /* Interrupt delivery mode — the system-wide truth from the kernel,
     * independent of whether this controller has resolved its GSI yet. On
     * physical hardware this row is the reliable way to tell PIC from
     * IOAPIC when boot logs aren't reachable. */
    int ioapic = intr_delivery_mode();
    add_row("Consegna interrupt", ioapic ? "IOAPIC (nativo)" : "PIC (8259)");

    /* The IRQ number also confirms this controller's own resolution: a GSI
     * in the PCI range (>=16) means empirical resolution bound the device
     * to its real IOAPIC input; 1-15 is a legacy 8259 line; 0 under IOAPIC
     * means resolution is still pending (no interrupt has asserted yet —
     * plug a device and refresh). */
    if (ioapic && d->irq_line == 0)
        snprintf(tmp, sizeof(tmp), "0x%04X / IRQ in risoluzione",
                 (unsigned)d->io_base);
    else if (d->irq_line >= 16)
        snprintf(tmp, sizeof(tmp), "0x%04X / IRQ %u (GSI IOAPIC)",
                 (unsigned)d->io_base, (unsigned)d->irq_line);
    else
        snprintf(tmp, sizeof(tmp), "0x%04X / IRQ %u (linea 8259)",
                 (unsigned)d->io_base, (unsigned)d->irq_line);
    add_row("I/O base / IRQ", tmp);

    snprintf(tmp, sizeof(tmp), "0x%04X -> 0x%04X",
             (unsigned)d->legsup_before, (unsigned)d->legsup_now);
    add_row("LEGSUP prima -> adesso", tmp);

    snprintf(tmp, sizeof(tmp), "CMD=%04X STS=%04X INTR=%04X FRNUM=%04X",
             (unsigned)d->usbcmd, (unsigned)d->usbsts,
             (unsigned)d->usbintr, (unsigned)d->frnum);
    add_row("Registri controller", tmp);

    for (int p = 0; p < 2 && p < (int)d->num_ports; p++)
    {
        uint16_t v = d->portsc[p];
        snprintf(tmp, sizeof(tmp), "0x%04X (%s%s%s)", (unsigned)v,
                 (v & 0x0001) ? "dispositivo presente" : "vuota",
                 (v & 0x0004) ? ", abilitata" : "",
                 (v & 0x0100) ? ", low-speed" : "");
        char pkey[16];
        snprintf(pkey, sizeof(pkey), "Porta %d", p + 1);
        add_row(pkey, tmp);
    }

    snprintf(tmp, sizeof(tmp), "%u (resume %u, completam. %u, vicini %u)",
             (unsigned)d->cnt_irq, (unsigned)d->cnt_resume,
             (unsigned)d->cnt_complete, (unsigned)d->cnt_shared);
    add_row("IRQ ricevuti", tmp);

    /* Flight recorder: usbms's bring-up op sequence is deterministic,
     * so this count+last-op names the stage where a silent usbms died
     * — readable here even when fase 4 hangs on it. */
    snprintf(tmp, sizeof(tmp), "%u (ultimo opcode %u)",
             (unsigned)d->subdrv_ops, (unsigned)d->subdrv_last_op);
    add_row("Op servite al sub-driver", tmp);

    snprintf(tmp, sizeof(tmp), "%u (avanzati senza IRQ: %u)",
             (unsigned)d->cnt_timeout, (unsigned)d->cnt_noirq_adv);
    add_row("Timeout enumerazione", tmp);

    add_row("Stato FSM", state_text(d->enum_state));
    if (d->enum_port >= 0)
    {
        snprintf(tmp, sizeof(tmp), "%d", d->enum_port + 1);
        add_row("Porta in enumerazione", tmp);
    }

    add_row("Ultimo errore",
            d->last_error[0] ? d->last_error : "Nessuno");

    /* --- forensics latched at enum_fail time --- */
    if (d->last_error[0])
    {
        uint32_t td  = d->fail_td_status;
        uint32_t alen = td & 0x7FF;             /* ActLen, n-1 encoded */
        snprintf(tmp, sizeof(tmp),
                 "0x%08X %s CERR=%u%s%s%s%s%s ActLen=%s",
                 (unsigned)td,
                 (td & (1u << 23)) ? "ACTIVE" : "fermo",
                 (unsigned)((td >> 27) & 3),
                 (td & (1u << 22)) ? " STALL"   : "",
                 (td & (1u << 21)) ? " DBUF"    : "",
                 (td & (1u << 20)) ? " BABBLE"  : "",
                 (td & (1u << 19)) ? " NAK"     : "",
                 (td & (1u << 18)) ? " CRC/TO"  : "",
                 (alen == 0x7FF) ? "0 (mai scritto)" : "scritto");
        add_row("TD al fallimento", tmp);

        snprintf(tmp, sizeof(tmp),
                 "USBSTS=0x%04X%s%s%s%s  FRNUM=0x%04X  PORTSC=0x%04X",
                 (unsigned)d->fail_usbsts,
                 (d->fail_usbsts & (1u << 4)) ? " HCPE!"  : "",
                 (d->fail_usbsts & (1u << 3)) ? " HSE!"   : "",
                 (d->fail_usbsts & (1u << 1)) ? " ERRINT" : "",
                 (d->fail_usbsts & (1u << 0)) ? " USBINT" : "",
                 (unsigned)d->fail_frnum, (unsigned)d->fail_portsc);
        add_row("Registri al fallimento", tmp);
    }

    /* ELCR: the IRQ line of a PIRQ-routed device MUST be level-triggered;
     * an edge-mode line silently drops UHCI interrupts. */
    {
        /* ELCR is an 8259-only register: it sets trigger mode only when the
         * PIC delivers. Under IOAPIC the redirection entry carries
         * level/active-low instead, so the ELCR verdict is meaningless there
         * (and "elcr >> gsi" with a GSI >= 16 would be nonsense). Evaluate
         * it only on the PIC path; otherwise say so, to avoid a false
         * "EDGE/SBAGLIATO" alarm in the very test that matters. */
        if (ioapic)
        {
            snprintf(tmp, sizeof(tmp),
                     "non applicabile in modo IOAPIC (trigger nella RTE)");
        }
        else
        {
            int level = (d->elcr >> d->irq_line) & 1;
            snprintf(tmp, sizeof(tmp), "0x%04X -> IRQ %u in modo %s",
                     (unsigned)d->elcr, (unsigned)d->irq_line,
                     level ? "LEVEL (corretto per PIRQ)"
                           : "EDGE (SBAGLIATO: gli IRQ UHCI si perdono!)");
        }
        add_row("ELCR (8259)", tmp);
    }

    add_yesno("Enumerazione completata", d->enum_done);
    if (d->enum_done)
    {
        snprintf(tmp, sizeof(tmp), "%04X:%04X  classe if %02X:%02X:%02X",
                 (unsigned)d->vid, (unsigned)d->pid,
                 (unsigned)d->if_class, (unsigned)d->if_subclass,
                 (unsigned)d->if_protocol);
        add_row("Dispositivo enumerato", tmp);

        snprintf(tmp, sizeof(tmp), "%u", (unsigned)d->das_files);
        add_row("File DAS USB letti", tmp);

        /* Driver-side raw List outcome, latched at the LAST enum. */
        if (d->das_list_rc == -99)
            add_row("List (driver)", "mai eseguita dall'avvio del driver");
        else
        {
            snprintf(tmp, sizeof(tmp), "rc=%d, entry nella dir: %u",
                     (int)d->das_list_rc, (unsigned)d->das_dir_entries);
            add_row("List (driver)", tmp);
        }

        if (d->das_open_fd != -100)
        {
            snprintf(tmp, sizeof(tmp), "Open fd=%d, Read len=%d",
                     (int)d->das_open_fd, (int)d->das_read_len);
            add_row("Open/Read (driver)", tmp);
        }

        add_row("Match DAS",
                d->das_matched ? (d->das_label[0] ? d->das_label : "Si")
                               : "Nessuno");
        add_yesno("Annuncio a hotplug riuscito", d->announce_ok);
    }

    /* Independent ground truth: list the same directory from THIS
     * process, right now. Splits "files genuinely absent from the boot
     * volume" from "dobfs broken in the driver context" from "files
     * copied AFTER the last enumeration (stale latch: re-plug!)". */
    {
        static dobfs_dirent_t ue[32];
        uint32_t uc = 0;
        usbdiag_now_rc = dobfs_List("/SYSTEM/CONFIG/DAS/USB", ue, 32, &uc);
        if (usbdiag_now_rc < 0)
            add_row("List (usbdiag, ORA)",
                    "FALLITA: directory assente dal volume di boot");
        else
        {
            int shown = 0, k = 0;
            char names[56]; names[0] = '\0';
            for (uint32_t i = 0; i < uc; i++)
            {
                if (ue[i].type != FS_TYPE_FILE) continue;
                usbdiag_now_count++;
                if (usbdiag_first_size == 0xFFFFFFFFu)
                    usbdiag_first_size = ue[i].size;
                if (shown < 3)
                {
                    k += snprintf(names + k, sizeof(names) - k,
                                  "%s%s (%u B)", shown ? ", " : "",
                                  ue[i].name, (unsigned)ue[i].size);
                    shown++;
                }
            }
            snprintf(tmp, sizeof(tmp), "%u entry, %d file%s%s",
                     (unsigned)uc, usbdiag_now_count,
                     shown ? ": " : "", names);
            add_row("List (usbdiag, ORA)", tmp);

            /* Try to actually OPEN and READ the first .das from THIS
             * process: List passing while Open/Read fails (for everyone
             * or only for the driver) is the next split to make. */
            for (uint32_t i = 0; i < uc; i++)
            {
                if (ue[i].type != FS_TYPE_FILE) continue;
                char fpath[96];
                snprintf(fpath, sizeof(fpath),
                         "/SYSTEM/CONFIG/DAS/USB/%s", ue[i].name);
                int fd = dobfs_Open(fpath, FS_READ);
                if (fd < 0)
                {
                    snprintf(tmp, sizeof(tmp),
                             "negata (fd=%d) su %s [atteso: i programmi "
                             "sono fuori whitelist CONFIG]",
                             fd, ue[i].name);
                }
                else
                {
                    static char head[64];
                    int n = dobfs_Read(fd, head, sizeof(head) - 1);
                    dobfs_Close(fd);
                    if (n <= 0)
                        snprintf(tmp, sizeof(tmp),
                                 "Open ok, Read=%d (vuoto/errore)", n);
                    else
                    {
                        head[n] = '\0';
                        for (int j = 0; j < n; j++)
                            if (head[j] == '\n' || head[j] == '\r')
                                head[j] = ' ';
                        snprintf(tmp, sizeof(tmp), "%d byte: %.40s",
                                 n, head);
                    }
                }
                add_row("Open/Read (usbdiag)", tmp);
                break;   /* first file only */
            }
        }
    }

    /* One-line interpretation so the screen says where the pipeline broke.
     * Ordered from the bottom of the stack upwards. */
    const char *verdict;
    int dev_present = (d->portsc[0] & 1) || (d->portsc[1] & 1);
    if (!d->init_ok)
        verdict = "Init fallito - vedi 'Ultimo errore'.";
    else if (!dev_present && !d->enum_done)
        verdict = "Nessun dispositivo sulle porte: problema elettrico "
                  "o porta sbagliata.";
    else if (d->enum_done && d->das_matched && d->announce_ok)
        verdict = "Pipeline driver OK: se manca l'icona il problema e' "
                  "in hotplug/dobinterface (subdevice).";
    else if (d->enum_done && !d->das_matched && d->das_files == 0)
    {
        if (usbdiag_now_rc < 0 || usbdiag_now_count == 0)
            verdict = "I file DAS/USB NON sono sul volume di boot: "
                      "copiali in /SYSTEM/CONFIG/DAS/USB.";
        else if (d->das_list_rc < 0 && d->das_list_rc != -99)
            verdict = "File PRESENTI ma la List FALLISCE nel contesto "
                      "del driver: bug dobfs lato driver.";
        else if (d->das_list_rc == -99)
            verdict = "File presenti ma il matcher non e' mai stato "
                      "eseguito: reinserisci il dispositivo.";
        else if (d->das_dir_entries == 0)
            verdict = "File presenti ORA ma assenti all'ultima "
                      "enumerazione (latch vecchio): reinserisci "
                      "la pendrive.";
        else if (d->das_open_fd != -100 && d->das_open_fd < 0)
            verdict = "Il driver vede il file ma la OPEN fallisce "
                      "(fd<0): confronta con 'Open/Read (usbdiag)'.";
        else if (d->das_open_fd >= 0 && d->das_read_len <= 0 &&
                 usbdiag_first_size == 0)
            verdict = "Il file su disco E' a 0 byte: la copia ha "
                      "scritto nulla. Rigenera il supporto o usa il "
                      "live (mcopy scrive size corretta).";
        else if (d->das_open_fd >= 0 && d->das_read_len <= 0)
            verdict = "Open ok ma READ vuota nel driver con file NON "
                      "vuoto: bug nel read path, da scavare.";
        else
            verdict = "Il driver VEDE le entry ma nessun file supera i "
                      "filtri (.das / tipo): controlla i nomi.";
    }
    else if (d->enum_done && !d->das_matched)
        verdict = "Dispositivo enumerato ma nessun DAS corrisponde "
                  "alla sua classe.";
    else if (d->last_error[0])
    {
        /* Refine using the enum_fail forensics. */
        uint32_t ftd = d->fail_td_status;
        if ((ftd & (1u << 23)) && ((ftd >> 27) & 3) == 3 &&
            (ftd & 0x7FF) == 0x7FF)
        {
            if (d->fail_usbsts & (1u << 4))
                verdict = "HCPE: il controller ha RIFIUTATO lo schedule "
                          "(QH/TD malformati per questo silicio).";
            else if (d->fail_usbsts & (1u << 3))
                verdict = "HSE: errore DMA sul bus PCI - bus master o "
                          "indirizzi fisici dello schedule.";
            else
                verdict = "Il controller non ha MAI eseguito il TD "
                          "(CERR intatto, ActLen vuoto): lo schedule "
                          "non viene raggiunto. FRBASEADD/link QH.";
        }
        else if (ftd & (1u << 23))
            verdict = "TD toccato ma incompleto (CERR/NAK): il "
                      "dispositivo risponde male o lentamente.";
        else
            verdict = "Enumerazione fallita - vedi 'TD al fallimento'.";
    }
    else if (dev_present && d->cnt_irq == 0 && d->cnt_timeout == 0)
        verdict = "Dispositivo presente ma zero eventi: IRQ muto e "
                  "FSM mai partita (handoff LEGSUP? routing PIRQ?).";
    else if (d->cnt_irq == 0 && d->cnt_noirq_adv > 0)
        verdict = "Linea IRQ muta: avanzamento solo via timeout. "
                  "Controlla routing PIRQ.";
    else
        verdict = "In attesa di un evento. Reinserisci il dispositivo "
                  "e riapri usbdiag.";
    add_row("Diagnosi", verdict);

    add_row("Fase 4", "interrogo usbms (se resta qui: sub-driver bloccato)");

    /* ===== FASE 4: il sub-driver mass-storage e' vivo? =====
     * Se l'utente ha gia' cliccato l'icona pendrive, usbms_<porta> deve
     * essere registrato e rispondere all'opcode 3 (capacita' + blocco
     * nativo). Tentiamo le porte 0/1 (UHCI ha due porte root). Una
     * chiamata qui e' sicura: usbms serve solo richieste block ed e'
     * fuori da ogni catena che ci coinvolga. */
    {
        int found = 0;
        for (int p2 = 0; p2 < 2 && !found; p2++)
        {
            char svc[16];
            snprintf(svc, sizeof(svc), "usbms_%d", p2);
            uint32_t sp = dob_registry_find(svc);
            if (!sp) continue;
            found = 1;
            dob_msg_t qm, qr;
            memset(&qm, 0, sizeof(qm)); memset(&qr, 0, sizeof(qr));
            qm.code = 3;
            int qok = (dob_ipc_call(sp, &qm, &qr) == DOB_OK &&
                       qr.code == DOB_OK);
            if (qok && qr.arg0 > 0)
            {
                uint32_t mb = qr.arg0 / 2048;   /* settori 512B -> MB */
                static const char *prep_names[] = {
                    "mai eseguita", "ok", "NESSUN volume FAT32",
                    "lettura MBR FALLITA (trasporto)" };
                uint32_t prep = (qr.arg2 >> 8) & 0xFF;
                if (prep > 3) prep = 0;
                uint32_t rdko = (qr.arg2 >> 16) & 0xFF;
                snprintf(tmp, sizeof(tmp),
                         "%s attivo: %u sett. virt. (%u MB), blocco "
                         "nativo %u B, prepara-volume: %s, letture KO: %u",
                         svc, (unsigned)qr.arg0, (unsigned)mb,
                         (unsigned)qr.arg1, prep_names[prep],
                         (unsigned)rdko);
            }
            else if (qok)
            {
                static const char *stage_names[] = {
                    "avvio", "GET_DEVINFO dal controller",
                    "ricerca endpoint bulk", "SET_CONFIGURATION",
                    "INQUIRY", "TEST UNIT READY (spin-up)",
                    "READ CAPACITY", "online" };
                uint32_t st = qr.arg2 & 0xFF;
                if (st > 7) st = 0;
                snprintf(tmp, sizeof(tmp),
                         "%s vivo ma bring-up FALLITO allo stadio: %s",
                         svc, stage_names[st]);
            }
            else
                snprintf(tmp, sizeof(tmp),
                         "%s registrato ma non risponde all'opcode 3",
                         svc);
            add_row("Sub-driver (fase 4)", tmp);
        }
        if (!found)
            add_row("Sub-driver (fase 4)",
                    "usbms non registrato (icona non ancora cliccata, "
                    "o spawn fallito: vedi popup)");

        /* Is the sub-driver BINARY even on the boot volume? A silently
         * failed build leaves the staging `if [ -f ]` to skip it just
         * as silently; List bypasses the sandbox so we can look. */
        {
            static dobfs_dirent_t de[8];
            uint32_t dc = 0;
            int drc = dobfs_List("/SYSTEM/DRIVERS/usb_mass_storage",
                                 de, 8, &dc);
            if (drc < 0)
                add_row("usbms.mdl su disco",
                        "directory ASSENTE: build o staging falliti");
            else
            {
                int got = 0;
                for (uint32_t i = 0; i < dc; i++)
                    if (strcmp(de[i].name, "usb_mass_storage.mdl") == 0)
                    {
                        snprintf(tmp, sizeof(tmp), "presente, %u byte",
                                 (unsigned)de[i].size);
                        add_row("usbms.mdl su disco", tmp);
                        got = 1;
                        break;
                    }
                if (!got)
                    add_row("usbms.mdl su disco",
                            "directory presente ma .mdl MANCANTE");
            }
        }
    }


    add_row("Fase hotplug",
            "interrogo hotplug (se resta qui: hotplug bloccato — e' lui "
            "che blocca anche il resto)");

    uint32_t hp = dob_registry_find("hotplug");
    dob_msg_t lmsg = {0}, lreply = {0};
    int list_rc = -1;
    uint32_t nbub = 0;
    if (hp)
    {
        lmsg.code = HOTPLUG_LIST;
        list_rc = dob_ipc_call(hp, &lmsg, &lreply);   /* may freeze: FASE 1 */
        nbub = lreply.arg0;
    }
    /* ===== FASE 2: tabella delle bubble + verdetto sul driver UHCI ===== */
    if (!hp || list_rc != DOB_OK)
    {
        dobpopup_Error("usbdiag", !hp
            ? "hotplug non e' nel registry (mai partito o morto)."
            : "HOTPLUG_LIST ha risposto con errore.");
        /* still try the driver below — every clue counts */
    }
    else
    {
        char bsvc[32];
        if (dobtable_Spawn(bsvc, sizeof(bsvc)) == 0)
        {
            static char bk[MAX_ROWS][40], bv[MAX_ROWS][80];
            static const char *bkp[MAX_ROWS], *bvp[MAX_ROWS];
            int rows = 0;
            const hotplug_bubble_info_t *bi =
                (const hotplug_bubble_info_t *)lreply.payload;
            uint32_t navail = lreply.payload_size / sizeof(*bi);
            if (nbub > navail) nbub = navail;

            int uhci_state = -1;
            for (uint32_t i = 0; i < nbub && rows < MAX_ROWS - 2; i++)
            {
                snprintf(bk[rows], sizeof(bk[rows]), "#%u %s",
                         bi[i].bubble_id,
                         bi[i].service_name[0] ? bi[i].service_name
                                               : bi[i].driver_name);
                snprintf(bv[rows], sizeof(bv[rows]),
                         "%04X:%04X cl=%02X:%02X pid=%d  %s",
                         bi[i].vendor_id, bi[i].device_id,
                         bi[i].class_code, bi[i].subclass,
                         (int)bi[i].driver_pid,
                         bubble_state_text(bi[i].state));
                bkp[rows] = bk[rows]; bvp[rows] = bv[rows]; rows++;

                if (bi[i].class_code == 0x0C && bi[i].subclass == 0x03)
                    uhci_state = bi[i].state;
            }

            strcpy(bk[rows], "VERDETTO bubble UHCI");
            if (uhci_state < 0)
                strcpy(bv[rows], "NESSUNA bubble USB: hotplug non l'ha "
                                 "mai matchata/spawnata");
            else if (uhci_state == BUBBLE_ATTACHING)
                strcpy(bv[rows], "driver fermo nell'handshake READY "
                                 "con hotplug");
            else if (uhci_state == BUBBLE_LIVE &&
                     dob_registry_find("usb_uhci"))
                strcpy(bv[rows], "LIVE e registrato: driver sano "
                                 "(dettagli in fase 3)");
            else if (uhci_state == BUBBLE_LIVE)
                strcpy(bv[rows], "attach OK ma servizio non registrato: "
                                 "incagliato in init_hw o nel loop");
            else
                snprintf(bv[rows], sizeof(bv[rows]), "stato bubble: %s",
                         bubble_state_text((uint8_t)uhci_state));
            bkp[rows] = bk[rows]; bvp[rows] = bv[rows]; rows++;

            dobtable_SetTitle(bsvc, "Bubble di hotplug (fase 2/3)");
            dobtable_SetHeaders(bsvc, "Bubble", "Stato");
            dobtable_AddRows(bsvc, bkp, bvp, rows);
            dobtable_Show(bsvc);
        }
    }

    /* ===== FASE 3: il driver risponde? ===== */

    /* The main table has been live since after fase 1; if its spawn
     * failed back then (window/VRAM pressure on small adapters), try
     * once more now that the transient phase tables are gone. */
    if (!main_table_up)
    {
        main_table_open();
        if (!main_table_up)
        {
            dobpopup_Error("usbdiag", "Impossibile creare la tabella.");
            return 1;
        }
    }
    return 0;
}
