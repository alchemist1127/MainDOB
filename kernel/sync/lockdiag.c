#include "sync/spinlock.h"
#include "kernel.h"

/* Diagnostica di lockup degli spinlock (dichiarata in spinlock.h).
 *
 * Uno spinlock kernel si tiene per micro-secondi: superare
 * SPINLOCK_LOCKUP_SPINS (~centinaia di ms anche sul ferro lento)
 * significa deadlock o corruzione, non contesa — e girare in silenzio
 * per sempre e' il peggior esito diagnostico possibile. Si va in panic
 * con l'indirizzo del lock e il chiamante: sul ferro (dove non c'e'
 * debugger) quelle due parole sulla seriale sono cio' che trasforma
 * "si e' congelato" in un bug localizzabile. */

void spinlock_lockup_report(volatile void *lock, void *ret_addr)
{
    kpanic("spinlock LOCKUP: lock %p mai rilasciato (chiamante %p)",
           (void *)lock, ret_addr);
}
