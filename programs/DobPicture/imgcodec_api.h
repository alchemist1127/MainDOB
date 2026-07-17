#ifndef DOBPICTURE_IMGCODEC_API_H
#define DOBPICTURE_IMGCODEC_API_H

/* API del .mem imgcodec (decoder+encoder PNG/JPEG) — vedi dob/mem.h.
 * L'HOST, subito dopo dob_mem_load, DEVE chiamare set_allocator con
 * il proprio malloc/free: il .mem e' self-contained e non ha una heap.
 * I buffer di output degli encoder e del decoder sono allocati con
 * quell'allocatore: li libera il chiamante col proprio free. */

#include <dob/types.h>

#define IMGCODEC_API_VERSION 1u

typedef struct
{
    uint32_t version;
    void (*set_allocator)(void *(*a)(uint32_t), void (*f)(void *));
    int  (*probe)(const uint8_t *head, int n);      /* 1=PNG 2=JPEG    */
    int  (*decode)(const uint8_t *data, uint32_t size, uint32_t **px,
                   int *w, int *h, int max_w, int max_h);
    int  (*encode_png)(const uint32_t *px, int w, int h,
                       uint8_t **out, uint32_t *out_n);
    int  (*encode_jpeg)(const uint32_t *px, int w, int h, int quality,
                        uint8_t **out, uint32_t *out_n);
} imgcodec_api_t;

#endif
