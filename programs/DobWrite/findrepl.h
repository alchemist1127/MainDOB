/* findrepl.h -- modern Find & Replace window for DobWrite.
 *
 * Rebuilt from scratch. The window is pure UI (findrepl.c); all document work
 * lives in the host (main.c) and is reached through the findrepl_host_* calls
 * below -- direct extern functions, NO stored function pointers, so nothing in
 * the dialog state can be corrupted into a bad jump.
 */
#ifndef FINDREPL_H
#define FINDREPL_H

#include <app.h>
#include <stdbool.h>

/* Open the modeless Find & Replace window. A second open while one is up is a
 * no-op. */
void findrepl_open(dobui_win_t *parent);

/* Implemented by the host (main.c), invoked by the dialog. Each one switches
 * the drawing context to the main window, does its work, and repaints it. */
void findrepl_host_find_next(const char *needle, bool ci);
void findrepl_host_replace_all(const char *needle, const char *repl, bool ci);
void findrepl_host_closed(void);
int  findrepl_host_count(void);   /* matches from the last search (0 if none) */

#endif /* FINDREPL_H */
