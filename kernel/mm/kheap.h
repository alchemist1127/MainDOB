#ifndef MAINDOB_MM_KHEAP_H
#define MAINDOB_MM_KHEAP_H

#include "lib/types.h"

void  kheap_init(void);
void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void  kfree(void *ptr);

/* Allocatore di pagine virtuali kernel (usato anche dai thread stack). */
uint32_t kpage_alloc(void);                     /* 1 pagina, 0 = OOM      */
uint32_t kpages_alloc(uint32_t count);
void     kpage_free(uint32_t virt);
void     kpages_free(uint32_t virt, uint32_t count);

bool kheap_selftest(void);

#endif
