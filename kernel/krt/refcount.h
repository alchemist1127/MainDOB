#ifndef MAINDOB_KRT_REFCOUNT_H
#define MAINDOB_KRT_REFCOUNT_H

#include "lib/types.h"
#include "sync/atomic.h"

/* Reference count atomico standard. Riusato da porte IPC, SHM, e ogni
 * oggetto a vita condivisa: nel 1.0 la stessa logica (get/put/
 * dec_and_test) era reimplementata caso per caso — qui e' UNA sola. */

typedef struct
{
    volatile uint32_t value;
} refcount_t;

/* Valore di saturazione: raggiunto questo, il conteggio si CONGELA.
 * Un refcount che wrappa a 0 per un inc/dec sbilanciato (bug, o loop
 * ostile su una API che incrementa) e' un free prematuro -> use-after-
 * free. La politica del blocco e': meglio un oggetto immortale (leak
 * controllato, diagnosticabile) di una liberazione anticipata (memoria
 * riciclata sotto i piedi di chi la usa). Saturazione = l'oggetto non
 * muore mai piu': inc e dec diventano no-op. */
#define REFCOUNT_SATURATED  0xC0000000u

static inline void refcount_init(refcount_t *rc, uint32_t initial)
{
    rc->value = initial;
}

/* Incremento saturante via CAS: mai oltre REFCOUNT_SATURATED. Il loop
 * e' privo di contesa nel caso normale (un giro); satura solo su
 * conteggi patologici che nessun uso legittimo raggiunge. */
static inline void refcount_inc(refcount_t *rc)
{
    for (;;)
    {
        uint32_t cur = rc->value;
        if (unlikely(cur >= REFCOUNT_SATURATED))
        {
            return;                     /* congelato: oggetto immortale */
        }
        if (atomic_cas(&rc->value, cur, cur + 1u))
        {
            return;
        }
    }
}

/* true se ha raggiunto 0 (il chiamante fa il teardown, in esclusiva).
 * Un refcount saturo non scende MAI: ritorna sempre false — nessun
 * teardown possibile su un oggetto il cui conteggio non e' piu'
 * affidabile. */
static inline bool refcount_dec(refcount_t *rc)
{
    for (;;)
    {
        uint32_t cur = rc->value;
        if (unlikely(cur >= REFCOUNT_SATURATED))
        {
            return false;               /* congelato: mai il teardown   */
        }
        if (unlikely(cur == 0))
        {
            return false;               /* underflow: gia' a zero — bug
                                         * del chiamante, non propagato */
        }
        if (atomic_cas(&rc->value, cur, cur - 1u))
        {
            return cur == 1u;
        }
    }
}

static inline uint32_t refcount_read(const refcount_t *rc)
{
    return rc->value;
}

#endif
