#ifndef MAINDOB_LIB_LIST_H
#define MAINDOB_LIB_LIST_H

#include "lib/types.h"

/* Lista doppia intrusiva, stile Linux — era gia'
 * il componente standard riusato ovunque (run-queue, wait-queue, code
 * IPC, timer) ed e' O(1) su insert/remove senza allocazioni.
 * list_remove reinizializza il nodo su se stesso: rimuovere due volte
 * e' innocuo, e list_node_is_linked() resta interrogabile. */

typedef struct list_node
{
    struct list_node *prev;
    struct list_node *next;
} list_node_t;

typedef struct
{
    list_node_t head;   /* Sentinella: head.next = primo, head.prev = ultimo */
} list_t;

#define LIST_INIT(name) { .head = { &(name).head, &(name).head } }

static inline void list_init(list_t *list)
{
    list->head.next = &list->head;
    list->head.prev = &list->head;
}

static inline void list_node_init(list_node_t *node)
{
    node->next = node;
    node->prev = node;
}

static inline bool list_empty(list_t *list)
{
    return list->head.next == &list->head;
}

/* Il nodo e' agganciato a una lista? (vero solo dopo list_node_init /
 * list_remove il nodo punta a se stesso) */
static inline bool list_node_is_linked(list_node_t *node)
{
    return node->next != node;
}

static inline void list_insert_after(list_node_t *pos, list_node_t *node)
{
    node->next = pos->next;
    node->prev = pos;
    pos->next->prev = node;
    pos->next = node;
}

static inline void list_insert_before(list_node_t *pos, list_node_t *node)
{
    list_insert_after(pos->prev, node);
}

static inline void list_push_back(list_t *list, list_node_t *node)
{
    list_insert_before(&list->head, node);
}

static inline void list_push_front(list_t *list, list_node_t *node)
{
    list_insert_after(&list->head, node);
}

static inline void list_remove(list_node_t *node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node;
    node->prev = node;
}

static inline list_node_t *list_pop_front(list_t *list)
{
    if (list_empty(list))
    {
        return NULL;
    }
    list_node_t *node = list->head.next;
    list_remove(node);
    return node;
}

static inline list_node_t *list_first(list_t *list)
{
    return list_empty(list) ? NULL : list->head.next;
}

static inline list_node_t *list_last(list_t *list)
{
    return list_empty(list) ? NULL : list->head.prev;
}

#define list_for_each(pos, list) \
    for (pos = (list)->head.next; pos != &(list)->head; pos = pos->next)

#define list_for_each_safe(pos, tmp, list) \
    for (pos = (list)->head.next, tmp = pos->next; \
         pos != &(list)->head; \
         pos = tmp, tmp = pos->next)

#define list_entry(ptr, type, member) container_of(ptr, type, member)

#endif /* MAINDOB_LIB_LIST_H */
