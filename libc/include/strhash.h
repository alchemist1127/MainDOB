#ifndef MAINDOB_LIBC_STRHASH_H
#define MAINDOB_LIBC_STRHASH_H

/* Simple string-keyed hashtable for userspace.
 * Open addressing with linear probing. Fixed capacity.
 * Used by config server, registry server, etc. */

#include <string.h>

#define STRHASH_MAX_KEY 128
#define STRHASH_MAX_VAL 256

typedef struct
{
    char     key[STRHASH_MAX_KEY];
    char     value[STRHASH_MAX_VAL];
    uint8_t  used;
} strhash_entry_t;

typedef struct
{
    strhash_entry_t *entries;
    uint32_t         capacity;
    uint32_t         count;
} strhash_t;

static inline uint32_t _strhash_fn(const char *s, uint32_t cap)
{
    uint32_t h = 5381;
    while (*s) h = h * 33 + (uint8_t)*s++;
    return h & (cap - 1);
}

/* Entry states: 0 = empty (never used), 1 = occupied, 2 = tombstone (deleted).
 * find() skips tombstones but stops at empty.
 * set() can reuse tombstone slots.
 * Without tombstones, removing a mid-chain entry breaks find() for later entries. */
#define STRHASH_EMPTY     0
#define STRHASH_OCCUPIED  1
#define STRHASH_TOMBSTONE 2

static inline int strhash_init(strhash_t *ht, strhash_entry_t *storage, uint32_t capacity)
{
    ht->entries = storage;
    ht->capacity = capacity;
    ht->count = 0;
    memset(storage, 0, capacity * sizeof(strhash_entry_t));
    return 1;
}

static inline strhash_entry_t *strhash_find(strhash_t *ht, const char *key)
{
    uint32_t idx = _strhash_fn(key, ht->capacity);
    for (uint32_t i = 0; i < ht->capacity; i++)
    {
        uint32_t pos = (idx + i) & (ht->capacity - 1);
        if (ht->entries[pos].used == STRHASH_EMPTY)
            return (void *)0;  /* Empty slot: end of probe chain */
        if (ht->entries[pos].used == STRHASH_TOMBSTONE)
            continue;  /* Skip deleted slots, keep probing */
        if (strncmp(ht->entries[pos].key, key, STRHASH_MAX_KEY) == 0)
            return &ht->entries[pos];
    }
    return (void *)0;
}

static inline strhash_entry_t *strhash_set(strhash_t *ht, const char *key, const char *value)
{
    /* Update existing */
    strhash_entry_t *e = strhash_find(ht, key);
    if (e)
    {
        strncpy(e->value, value, STRHASH_MAX_VAL - 1);
        e->value[STRHASH_MAX_VAL - 1] = '\0';
        return e;
    }

    /* Insert new */
    if (ht->count >= ht->capacity * 3 / 4) return (void *)0; /* Load factor limit */

    uint32_t idx = _strhash_fn(key, ht->capacity);
    for (uint32_t i = 0; i < ht->capacity; i++)
    {
        uint32_t pos = (idx + i) & (ht->capacity - 1);
        if (ht->entries[pos].used != STRHASH_OCCUPIED)
        {
            strncpy(ht->entries[pos].key, key, STRHASH_MAX_KEY - 1);
            ht->entries[pos].key[STRHASH_MAX_KEY - 1] = '\0';
            strncpy(ht->entries[pos].value, value, STRHASH_MAX_VAL - 1);
            ht->entries[pos].value[STRHASH_MAX_VAL - 1] = '\0';
            ht->entries[pos].used = STRHASH_OCCUPIED;
            ht->count++;
            return &ht->entries[pos];
        }
    }
    return (void *)0;
}

static inline int strhash_remove(strhash_t *ht, const char *key)
{
    strhash_entry_t *e = strhash_find(ht, key);
    if (!e) return 0;
    e->used = STRHASH_TOMBSTONE;  /* Mark as tombstone, not empty */
    e->key[0] = '\0';
    ht->count--;
    return 1;
}

#endif /* MAINDOB_LIBC_STRHASH_H */
