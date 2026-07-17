/* subsedit.h -- modeless dialog to edit the global autocorrect list.
 *
 * Reuses the read-only dob_table_t (display + selection) for the list and
 * two text boxes ("Sostituisci:" / "Con:") for editing, plus Aggiungi /
 * Rimuovi / Chiudi.  It mutates the global autocorr list directly (no return
 * value).  One instance at a time; parent it on dobui_primary().
 */
#ifndef DOBWRITE_SUBSEDIT_H
#define DOBWRITE_SUBSEDIT_H

#include <app.h>

void subsedit_open(dobui_win_t *parent);

#endif /* DOBWRITE_SUBSEDIT_H */
