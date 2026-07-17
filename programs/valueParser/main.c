/* valueParser — DobTable launcher with sample data.
 *
 * A minimal program that spawns a DobTable with a representative key/
 * value dataset so the widget can be exercised end-to-end without a
 * real-world caller.  Useful as both a smoke test for the DobTable
 * pipeline and as a worked example for callers that want to embed
 * the same pattern (file properties, DAS pairings, settings, ...).
 *
 * Things to try once the window appears:
 *   - click a row to select it (highlight blue)
 *   - "Copia" panel command → "<key>: <value>" goes to the clipboard
 *   - hover the column splitter → cursor turns into ↔
 *   - drag the splitter to rebalance the columns live
 *   - mouse wheel / arrows / PgUp / PgDn / Home / End
 *   - resize the window
 *
 * After Show, valueParser exits.  The DobTable instance lives on its
 * own until the user closes it via the panel "Chiudi" or the window
 * close button. */

#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <DobTable.h>
#include <DobPopup.h>

/* Surface failures via a popup. Without this the program would just
 * exit silently (it has no window of its own) and the user would see
 * exactly nothing — making debugging impossible. */
static int fail(const char *step)
{
    char body[160];
    snprintf(body, sizeof(body),
             "valueParser: il passo \"%s\" è fallito.\n"
             "Controlla che DobTable.mdl sia installato in\n"
             "/SYSTEM/PROGRAMS/DobTable/DobTable.mdl.", step);
    dobpopup_Error("valueParser", body);
    return 1;
}

int main(void)
{
    char svc[32];
    if (dobtable_Spawn(svc, sizeof(svc)) != 0)
        return fail("Spawn");

    if (dobtable_SetTitle(svc, "Proprietà file (demo)") != 0)
        return fail("SetTitle");

    if (dobtable_SetHeaders(svc, "Proprietà", "Valore") != 0)
        return fail("SetHeaders");

    /* Mix of short and long values to stress the truncation/ellipsis
     * path on the value column.  20+ rows so the scrollbar appears. */
    const char *keys[] = {
        "Nome",
        "Tipo",
        "Dimensione",
        "Modificato",
        "Creato",
        "Permessi",
        "Proprietario",
        "Gruppo",
        "Inode",
        "Hash SHA-256",
        "Path completo",
        "Numero di righe",
        "Codifica",
        "Newline",
        "MIME",
        "Estensione",
        "Volume",
        "Filesystem",
        "Punto di mount",
        "Spazio libero",
        "Frammentazione",
        "ACL",
        "xattr",
    };
    const char *vals[] = {
        "report_q4_2025_finale_revisionato.pdf",
        "PDF Document",
        "412 KB (421888 byte)",
        "2026-05-02 14:23:11",
        "2026-04-28 09:11:42",
        "rw-r--r--",
        "klavatren",
        "users",
        "0x1A2B3C4D",
        "8f3b2a1e9c4d7f6...",
        "/DATA/Documents/Lavoro/Q4-2025/report_q4_2025_finale_revisionato.pdf",
        "1842",
        "UTF-8",
        "LF (Unix)",
        "application/pdf",
        ".pdf",
        "MainDOB Boot",
        "FAT32",
        "/",
        "1.2 GB",
        "0%",
        "(nessuna)",
        "(nessuno)",
    };

    int n = sizeof(keys) / sizeof(keys[0]);
    if (dobtable_AddRows(svc, keys, vals, n) != 0)
        return fail("AddRows");

    if (dobtable_Show(svc) != 0)
        return fail("Show");

    /* DobTable now owns the window; valueParser is done. */
    return 0;
}
