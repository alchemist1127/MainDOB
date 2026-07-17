/* DobTable Entry Point — mountable two-column key/value viewer.
 *
 * DobTable.mdl is a standalone program that puts up its own window
 * and renders a scrollable key/value table inside it.  Callers spawn
 * an instance via dobtable_Spawn(), populate it via Set/Add calls,
 * and ask the user to look at it via Show.  The user can select a
 * row and copy it to the clipboard via the side panel; everything
 * else is read-only from the user's side.
 *
 * Typical session, e.g. a file manager showing properties of a file:
 *
 *     char svc[32];
 *     if (dobtable_Spawn(svc, sizeof(svc)) != 0) { ... }
 *     dobtable_SetTitle(svc, "Proprietà");
 *     dobtable_SetHeaders(svc, "Proprietà", "Valore");
 *
 *     const char *keys[]   = { "Nome", "Dimensione", "Modificato" };
 *     const char *values[] = { "report.pdf", "412 KB", "2 May 2026" };
 *     dobtable_AddRows(svc, keys, values, 3);
 *     dobtable_Show(svc);
 *
 *     // The DobTable instance now lives independently. The caller can
 *     // forget about it; the user closes it when done.  If the caller
 *     // wants to dismiss it programmatically (e.g. on its own exit):
 *     // dobtable_Close(svc);
 *
 * Multiple instances can coexist — each Spawn allocates a fresh
 * service name. */

#ifndef MAINDOB_DOBTABLE_H
#define MAINDOB_DOBTABLE_H

#include <dob/types.h>

/* Spawn a new DobTable instance. The unique service name used to
 * address it is written to `out_service` (NUL-terminated, fits in
 * out_cap bytes; at least 24 bytes recommended).
 *
 * Blocks until the new instance has registered itself in the
 * registry — at most ~5 s; returns -1 on timeout or spawn failure.
 * On success, returns 0 and `out_service` is the handle for all
 * subsequent calls. */
int dobtable_Spawn(char *out_service, int out_cap);

/* Set the window title. Pass before Show to give the user a
 * meaningful name in the title bar. */
int dobtable_SetTitle(const char *service, const char *title);

/* Set the optional header row.  Pass NULL,NULL to remove the
 * headers entirely (table jumps straight to data). */
int dobtable_SetHeaders(const char *service,
                        const char *key_header,
                        const char *value_header);

/* Append `count` rows.  `keys[i]` and `values[i]` are paired.
 * Either array may contain NULL entries (rendered as empty cell).
 * The server copies all strings, so the caller may free or reuse
 * the buffers immediately on return. */
int dobtable_AddRows(const char *service,
                     const char **keys,
                     const char **values,
                     int count);

/* Drop all rows currently displayed (headers and title untouched). */
int dobtable_Clear(const char *service);

/* Paint the current state and bring the window to the front. Call
 * after the data is fully populated; the window is blank-ish until
 * the first Show. */
int dobtable_Show(const char *service);

/* Ask the DobTable instance to close itself.  After this call,
 * subsequent calls on the same service will fail. */
int dobtable_Close(const char *service);

#endif
