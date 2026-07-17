/* atadiag — ATA/IDE DMA bring-up diagnostic.
 *
 * Issues ATA_OP_GET_DIAG (opcode 22) to the "ata" service and renders the
 * slot-0 DMA bring-up snapshot in a DobTable. The point of this program is
 * hardware without a serial console: the driver's debug_print() trail is
 * invisible on a real Compaq Armada E500, so the facts that decide
 * DMA-vs-PIO are latched in the driver and read back here, on screen.
 *
 * The decisive rows are:
 *   "DMA attivo?"       — is the fast path actually in use right now
 *   "Ultimo errore DMA" — which branch tripped (timeout / ATA err / BMIDE err)
 *   "Quirk controller"  — whether Level B had to enable UDMA on the chipset
 *
 * Same shape as enginediag: query, spawn a DobTable, populate, Show, exit.
 */
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/types.h>
#include <dob/ata_protocol.h>
#include <DobTable.h>
#include <DobPopup.h>

#define MAX_ROWS 24
static char  key_buf[MAX_ROWS][40];
static char  val_buf[MAX_ROWS][80];
static const char *keys[MAX_ROWS];
static const char *vals[MAX_ROWS];
static int   row_count = 0;

static void add_row(const char *k, const char *v)
{
    if (row_count >= MAX_ROWS) return;
    snprintf(key_buf[row_count], sizeof(key_buf[0]), "%s", k);
    snprintf(val_buf[row_count], sizeof(val_buf[0]), "%s", v);
    keys[row_count] = key_buf[row_count];
    vals[row_count] = val_buf[row_count];
    row_count++;
}

static void add_yesno(const char *k, uint8_t b)
{
    add_row(k, b ? "Si" : "No");
}

static const char *fail_text(uint8_t code)
{
    switch (code)
    {
        case 0:  return "Nessuno";
        case 1:  return "Timeout (engine non parte)";
        case 2:  return "Errore ATA (ERR/DF)";
        case 3:  return "Errore BMIDE (timing/bus)";
        default: return "Sconosciuto";
    }
}

/* Human-readable chipset name for the PCI ids we know about. */
static const char *chipset_name(uint16_t vendor, uint16_t device)
{
    switch (vendor)
    {
    case 0x8086:
        switch (device)
        {
            case 0x7010: return "Intel PIIX3 (82371SB)";
            case 0x7111: return "Intel PIIX4 (82371AB)";
            case 0x7199: return "Intel PIIX4 mobile (82371MB)";
            case 0x2411: return "Intel ICH IDE";
            case 0x2421: return "Intel ICH0 IDE";
            case 0x244A: return "Intel ICH2-M IDE";
            case 0x244B: return "Intel ICH2 IDE";
            default:     return "Intel IDE (variante)";
        }
    case 0x1106: return "VIA IDE (VT82Cxxx)";
    case 0x1039: return "SiS IDE (5513)";
    case 0x0000: return "Nessuno";
    default:     return "Sconosciuto";
    }
}

int main(void)
{
    uint32_t port = dob_registry_find("ata");
    if (!port)
    {
        dobpopup_Error("atadiag",
            "Servizio ATA non trovato.\n"
            "Il driver ata non e' registrato.");
        return 1;
    }

    dob_msg_t msg = {0}, reply = {0};
    msg.code = ATA_OP_GET_DIAG;
    if (dob_ipc_call(port, &msg, &reply) != DOB_OK)
    {
        dobpopup_Error("atadiag",
            "Chiamata GET_DIAG fallita.\n"
            "Il driver ata caricato e' aggiornato?");
        return 1;
    }
    if (!reply.payload || reply.payload_size < sizeof(ata_diag_t))
    {
        dobpopup_Error("atadiag",
            "Opcode GET_DIAG non supportato.\n"
            "Ricompila e reinstalla il driver ata.");
        return 1;
    }

    const ata_diag_t *d = (const ata_diag_t *)reply.payload;

    char tmp[80];

    add_yesno("DMA attivo?", d->dma_ok);

    if (d->udma_mode == 0xFF)
        add_row("Modo UDMA negoziato", "Nessuno (PIO)");
    else
    {
        snprintf(tmp, sizeof(tmp), "UDMA %u", (unsigned)d->udma_mode);
        add_row("Modo UDMA negoziato", tmp);
    }

    snprintf(tmp, sizeof(tmp), "UDMA %u (max controller)", (unsigned)d->udma_cap);
    add_row("Limite UDMA chipset", tmp);

    add_yesno("LBA48", d->lba48);
    add_row("Ultimo errore DMA", fail_text(d->last_dma_fail));

    snprintf(tmp, sizeof(tmp), "%u", (unsigned)d->dma_fail_streak);
    add_row("Fallimenti consecutivi", tmp);

    add_yesno("Controller PCI trovato", d->pci_found);

    if (d->pci_found)
    {
        add_row("Chipset", chipset_name(d->pci_vendor, d->pci_device));
        snprintf(tmp, sizeof(tmp), "%04X:%04X",
                 (unsigned)d->pci_vendor, (unsigned)d->pci_device);
        add_row("PCI vendor:device", tmp);
        snprintf(tmp, sizeof(tmp), "0x%04X", (unsigned)d->bm_base);
        add_row("BMIDE base (BAR4)", tmp);
        add_yesno("Bus master abilitato", d->bus_master_on);
    }

    /* Level-B controller quirk status. "Tried but not applied" most often
     * means the BIOS had already enabled UDMA (the common, healthy case),
     * NOT that the chip is unknown — so word it neutrally. */
    if (!d->quirk_tried)
        add_row("Quirk controller (Liv. B)", "Non necessario");
    else if (d->quirk_applied)
        add_row("Quirk controller (Liv. B)", "Applicato (UDMA abilitato)");
    else
        add_row("Quirk controller (Liv. B)", "Non applicato (gia' OK o chip ignoto)");

    /* One-line interpretation so the screen says what to do next. The model
     * is layered: Level A trusts the BIOS (no controller write), Level B sets
     * the chipset UDMA-enable bit on failure, Level C is PIO. */
    const char *verdict;
    if (d->udma_mode != 0xFF && d->udma_mode > d->udma_cap)
        verdict = "UDMA oltre il limite del chipset - errori attesi.";
    else if (d->dma_ok && d->last_dma_fail == 0 && !d->quirk_applied)
        verdict = "DMA OK (BIOS, Liv. A).";
    else if (d->dma_ok && d->quirk_applied)
        verdict = "DMA OK dopo quirk chipset (Liv. B).";
    else if (d->dma_ok && d->last_dma_fail == 2)
        verdict = "Errore ATA - possibile UDMA troppo alto o cavo.";
    else if (d->dma_ok && d->last_dma_fail != 0)
        verdict = "DMA attivo ma con errori intermittenti.";
    else if (!d->pci_found)
        verdict = "Nessun controller PCI - PIO.";
    else if (d->last_dma_fail == 3)
        verdict = "Errore BMIDE - bus master/timing del controller.";
    else if (d->last_dma_fail == 1)
        verdict = "Timeout - controlla IRQ / bus master.";
    else
        verdict = "DMA non attivo - in PIO.";
    add_row("Diagnosi", verdict);

    char svc[32];
    if (dobtable_Spawn(svc, sizeof(svc)) != 0)
    {
        dobpopup_Error("atadiag", "Impossibile creare la tabella.");
        return 1;
    }
    dobtable_SetTitle(svc, "Diagnostica DMA ATA");
    dobtable_SetHeaders(svc, "Parametro", "Valore");
    dobtable_AddRows(svc, keys, vals, row_count);
    dobtable_Show(svc);
    return 0;
}
