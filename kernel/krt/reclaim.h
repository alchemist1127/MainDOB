#ifndef MAINDOB_KRT_RECLAIM_H
#define MAINDOB_KRT_RECLAIM_H

#include "lib/types.h"

/* Auto-pulizia delle cache di lunga durata ("il boot eterno non porta
 * cicatrici").
 *
 * Il problema che risolve: le cache del kernel — heap dei timer, depot
 * IPC, pagine slab vuote, buffer overflow IPC — crescono con i picchi
 * di lavoro e, pur LIMITATE, restano al loro high-water mark per
 * sempre. Dopo un uso intenso e caotico (copie massicce, format di
 * pendrive, giochi, GUI) il sistema porta le cicatrici del picco: un
 * servizio di lunga durata deve invece poter crescere e ritirarsi come
 * se nulla fosse accaduto.
 *
 * Il modello, fedele alla separazione logico/esecutiva:
 *   - ogni cache espone un VERBO ESECUTIVO di ritiro (X_trim), che
 *     riporta la struttura verso il suo stato di riposo rispettando un
 *     pavimento caldo (mai svuotare del tutto: il prossimo picco non
 *     deve ripagare tutto il warm-up) e ritorna i byte ritirati;
 *   - la LOGICA sta qui: il coordinatore decide QUANDO (sistema quieto:
 *     invocato dal blocco idle, un giro ogni RECLAIM_PERIOD in tutto il
 *     sistema, una sola CPU per giro) e delega ai verbi registrati.
 *
 * I verbi si registrano alle init dei rispettivi sottosistemi (fase di
 * boot, prima che l'idle giri): dopo il boot il registro e' in sola
 * lettura e la passeggiata e' lock-free. I verbi girano in contesto
 * thread idle (mai IRQ): possono prendere i propri lock e usare kfree. */

typedef uint32_t (*reclaim_fn_t)(void);     /* -> byte ritirati         */

void reclaim_register(const char *name, reclaim_fn_t fn);

/* Il coordinatore: chiamato dal blocco idle prima di dormire. Decide se
 * e' il momento (rate-limit globale) e nel caso esegue il giro. Costo a
 * vuoto: una lettura di clock e un confronto. */
void reclaim_idle_consider(void);

#endif /* MAINDOB_KRT_RECLAIM_H */
