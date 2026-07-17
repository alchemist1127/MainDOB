/* MainDOB DobInterface 2.0 — foglio: dirty.c
 *
 * Il "dirty state" del desktop: quali ragioni impongono un present al
 * prossimo giro del loop. UNICO proprietario dello stato che nell'1.x
 * (e fino a ieri nel 2.0) era quattro booleani globali — needs_repaint
 * / needs_panel_repaint / needs_content_blit / needs_cursor_redraw —
 * scritti da ~50 punti in 11 fogli e AZZERATI da tre fogli diversi
 * (compositor, main, input), concordi solo per convenzione. Non era un
 * data race (il loop e' mono-thread), ma stato mutabile senza un
 * proprietario: bastava toccare un'autorita' ignorando le altre per
 * lasciare una flag stantia (repaint spurio a ogni tick) o persa
 * (repaint mancato).
 *
 * Qui lo stato e' privato e ha UN proprietario, con tre verbi soli:
 *   - di_mark_dirty(reasons)  accende (OR-in): dichiarazione d'intento,
 *                             l'unico modo di sporcare;
 *   - di_dirty(mask)          interroga (il loop-spec in main.c);
 *   - di_dirty_clear(mask)    spegne: chiamato SOLO da chi ha appena
 *                             presentato, e solo per le ragioni che ha
 *                             presentato (compositor_repaint per il
 *                             frame pieno, compositor_drag_blit per il
 *                             drag, il ramo cursore del loop per il
 *                             cursore). Ogni ragione ha un solo
 *                             consumatore per tipo di present.
 *
 * Le ragioni sono un bitmask (non piu' booleani sciolti) apposta: e'
 * la base del dirty-rect present — una ragione potra' un giorno
 * portare con se' il rettangolo sporco, alimentando
 * compositor_repaint_rect (oggi spento). */

#include "di_internal.h"

/* Primo giro: repaint pieno (era needs_repaint = true all'avvio).
 * Il paint iniziale esplicito in main lo consuma prima del loop. */
static uint32_t s_dirty = DIRTY_FULL;

/* ================= verbi esecutivi: un proprietario, tre verbi ======== */

void di_mark_dirty(uint32_t reasons) { s_dirty |= reasons; }

bool di_dirty(uint32_t mask) { return (s_dirty & mask) != 0; }

void di_dirty_clear(uint32_t mask) { s_dirty &= ~mask; }
