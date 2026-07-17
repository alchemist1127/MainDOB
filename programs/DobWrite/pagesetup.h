/* pagesetup.h -- modeless "Imposta pagina" dialog for DobWrite.
 *
 * Edits page dimensions and margins, displayed in a unit the user picks
 * (pixels / centimetres / inches). A scope control chooses whether the
 * geometry applies to the whole document or just the section at the caret.
 * Opens populated from `current` and reports the new geometry and scope
 * through on_apply (scope: 0 = whole document, 1 = current section). */

#ifndef DOBWRITE_PAGESETUP_H
#define DOBWRITE_PAGESETUP_H

#include <app.h>
#include <dobdoc/dobdoc.h>

#define PAGESETUP_SCOPE_DOC     0
#define PAGESETUP_SCOPE_SECTION 1

typedef void (*pagesetup_apply_cb)(const PageSetup *ps, int scope, void *ud);

void pagesetup_open(dobui_win_t *parent, const PageSetup *current,
                    pagesetup_apply_cb on_apply, void *ud);

#endif /* DOBWRITE_PAGESETUP_H */
