#ifndef MAINDOB_STUBS_DOBPOPUP_H
#define MAINDOB_STUBS_DOBPOPUP_H

/* DobPopup Entry Point — System popup messages
 *
 * Usage:
 *   #include <DobPopup.h>
 *
 *   dobpopup_Info("Titolo", "Operazione completata.");
 *   dobpopup_Warning("Attenzione", "Disco quasi pieno.");
 *   dobpopup_Error("Errore", "File non trovato.");
 *   int choice = dobpopup_YesNo("Conferma", "Eliminare il file?");
 *   if (choice == 0) { ... }  // 0=yes, 1=no
 */

#include <dob/types.h>

#define POPUP_INFO      0
#define POPUP_WARNING   1
#define POPUP_ERROR     2
#define POPUP_YESNO     3
#define POPUP_INPUT     4

/* Show a popup. Blocks until the user dismisses it.
 * Returns 0 for OK/Yes, 1 for No, -1 for cancel. */
int dobpopup_Show(int type, const char *title, const char *message);

/* InputBox: shows a text input dialog.
 * Returns 0 on confirm, -1 on cancel.
 * If confirmed, out_text contains user input. */
int dobpopup_InputBox(const char *title, const char *prompt,
                      const char *default_text,
                      char *out_text, uint32_t out_size);

/* Convenience wrappers */
static inline void dobpopup_Info(const char *title, const char *msg)
{
    dobpopup_Show(POPUP_INFO, title, msg);
}

static inline void dobpopup_Warning(const char *title, const char *msg)
{
    dobpopup_Show(POPUP_WARNING, title, msg);
}

static inline void dobpopup_Error(const char *title, const char *msg)
{
    dobpopup_Show(POPUP_ERROR, title, msg);
}

static inline int dobpopup_YesNo(const char *title, const char *msg)
{
    return dobpopup_Show(POPUP_YESNO, title, msg);
}

#endif /* MAINDOB_STUBS_DOBPOPUP_H */
