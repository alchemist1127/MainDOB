#ifndef MAINDOB_KRT_HANDLE_TABLE_H
#define MAINDOB_KRT_HANDLE_TABLE_H

#include "lib/types.h"
#include "sync/spinlock.h"

/* Tabella a slot generica, riusata da: tabella processi, tabella porte,
 * tabella thread, SHM, event group, handle driver. Fornisce in un solo
 * posto — testato una volta — la logica che nel kernel 1.0 era
 * duplicata (e divergeva) in ogni sottosistema:
 *
 *   - allocazione O(1) da freelist con fallback a scansione
 *   - GENERATION per-slot fuori dal payload (difesa ABA: uno slot
 *     riciclato non viene scambiato per il vecchio oggetto)
 *   - lock unico per la tabella
 *
 * Non possiede gli oggetti: memorizza solo puntatori void*. Il ciclo
 * di vita del payload e' del chiamante (spesso via krt_refcount). */

/* POLITICA DI RIUSO degli id — contratto esplicito per tabella:
 *   - HT_REUSE_FIFO: ring — uno slot liberato torna in CODA e viene
 *     riassegnato solo dopo che tutti gli altri liberi hanno girato.
 *     Per i PID e' obbligatorio: il riuso immediato del LIFO era una
 *     pistola carica per ogni bookkeeping chiavato su PID (ABA visto
 *     dal ferro: hotplug consegnava all'uhci il device del gemello
 *     ahci appena morto, stesso PID).
 *   - HT_REUSE_LIFO: stack — riuso immediato, id densi e bassi. E' il
 *     comportamento storico e resta il default per le tabelle i cui id
 *     viaggiano nell'ecosistema con dinamiche accordate alla densita'
 *     (SHM: i buffer finestra nascono e muoiono di continuo e gli id
 *     restano piccoli e caldi in cache dei consumatori).
 *   - HT_REUSE_MONOTONIC: cursore rotante — un id liberato torna in gioco
 *     SOLO dopo che il cursore ha percorso l'intero spazio (come il
 *     next_pid del 1.0). Ritardo di riuso MASSIMO e costante (~capacity
 *     allocazioni), a prescindere da quanti slot siano liberi: un
 *     riferimento esterno stantio a un id morto (bolla hotplug) e'
 *     SEMPRE reaped prima che l'id venga riconsegnato. Per la tabella
 *     processi: e' la finestra provata del 1.0.
 * In entrambe la generation resta la guardia FORTE per chi usa
 * handle_ref_t; la politica decide solo QUANDO un id torna in gioco. */
typedef enum
{
    HT_REUSE_LIFO      = 0,
    HT_REUSE_FIFO      = 1,
    HT_REUSE_MONOTONIC = 2,
} ht_reuse_t;

typedef struct
{
    void    **slots;        /* slot[0] riservato = "invalido"             */
    uint32_t *generation;   /* bump a ogni alloc sullo slot               */
    uint32_t *freelist;     /* ring (FIFO) o stack (LIFO) degli id liberi */
    uint32_t  free_head;    /* FIFO: prossimo id da assegnare             */
    uint32_t  free_tail;    /* FIFO: dove torna un id liberato            */
    uint32_t  free_count;   /* comune a tutte le politiche                */
    uint32_t  next_scan;    /* MONOTONIC: cursore rotante (id da cui scandire) */
    uint32_t  capacity;
    ht_reuse_t reuse;
    spinlock_t lock;
} handle_table_t;

/* Riferimento opaco: id + generation, validabile senza dereferenziare. */
typedef struct
{
    uint32_t id;
    uint32_t gen;
} handle_ref_t;

bool  handle_table_init(handle_table_t *t, uint32_t capacity,
                        ht_reuse_t reuse);
void  handle_table_destroy(handle_table_t *t);

/* Inserisce obj, restituisce il ref (id>0) o {0,0} se pieno. */
handle_ref_t handle_table_insert(handle_table_t *t, void *obj);

/* Rimuove per id, restituisce l'oggetto sfrattato (o NULL). */
void *handle_table_remove(handle_table_t *t, uint32_t id);

/* Lookup semplice per id (nessun controllo generation). */
void *handle_table_get(handle_table_t *t, uint32_t id);

/* Lookup validato: NULL se lo slot e' stato riciclato (gen diversa). */
void *handle_table_get_checked(handle_table_t *t, handle_ref_t ref);

uint32_t handle_table_generation(handle_table_t *t, uint32_t id);

#endif
