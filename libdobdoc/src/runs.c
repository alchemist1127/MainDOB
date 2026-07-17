/* libdobdoc -- generic attribute run list.
 *
 * A run = (byte length, payload). Runs partition the text; the sum of
 * lengths equals the text length (which may be 0 -> a single empty run
 * that still carries a payload, so typing into an empty span inherits a
 * format). The same machinery serves character runs (payload CharFmt)
 * and paragraph runs (payload ParaFmt).
 *
 * Payloads must be value-comparable with memcmp, so callers zero-init
 * them (padding included) before filling fields -- merging relies on it.
 */

#include "doc_internal.h"
#include <string.h>
#include <stdlib.h>

void rl_init(runlist *rl, uint32_t esize)
{
    memset(rl, 0, sizeof(*rl));
    rl->esize = esize;
}

void rl_free(runlist *rl)
{
    free(rl->len); free(rl->data);
    uint32_t es = rl->esize;
    memset(rl, 0, sizeof(*rl));
    rl->esize = es;
}

static dd_result rl_reserve(runlist *rl, uint32_t need)
{
    if (need <= rl->cap) return DD_OK;
    uint32_t nc = rl->cap ? rl->cap * 2 : 8;
    while (nc < need) nc *= 2;
    uint32_t *nl = (uint32_t *)realloc(rl->len, (size_t)nc * sizeof(uint32_t));
    if (!nl) return DD_ERR_NOMEM;
    rl->len = nl;
    uint8_t *nd = (uint8_t *)realloc(rl->data, (size_t)nc * rl->esize);
    if (!nd) return DD_ERR_NOMEM;
    rl->data = nd; rl->cap = nc;
    return DD_OK;
}

void *rl_payload(const runlist *rl, uint32_t i) { return rl->data + (size_t)i * rl->esize; }

uint32_t rl_total(const runlist *rl)
{
    uint32_t t = 0;
    for (uint32_t i = 0; i < rl->n; i++) t += rl->len[i];
    return t;
}

dd_result rl_reset_single(runlist *rl, uint32_t total, const void *payload)
{
    if (rl_reserve(rl, 1)) return DD_ERR_NOMEM;
    rl->len[0] = total;
    memcpy(rl_payload(rl, 0), payload, rl->esize);
    rl->n = 1;
    return DD_OK;
}

void rl_find(const runlist *rl, uint32_t pos, uint32_t *idx, uint32_t *within)
{
    uint32_t acc = 0;
    for (uint32_t i = 0; i < rl->n; i++) {
        if (pos < acc + rl->len[i]) { *idx = i; *within = pos - acc; return; }
        acc += rl->len[i];
    }
    *idx = rl->n; *within = 0;
}

void *rl_payload_at(const runlist *rl, uint32_t pos)
{
    uint32_t idx, within;
    rl_find(rl, pos, &idx, &within);
    if (idx >= rl->n) idx = rl->n ? rl->n - 1 : 0;
    return rl_payload(rl, idx);
}

dd_result rl_split_at(runlist *rl, uint32_t pos, uint32_t *out_idx)
{
    uint32_t idx, within;
    rl_find(rl, pos, &idx, &within);
    if (within == 0) { *out_idx = idx; return DD_OK; }   /* boundary already there */

    if (rl_reserve(rl, rl->n + 1)) return DD_ERR_NOMEM;
    /* open a slot at idx+1 */
    memmove(&rl->len[idx + 2], &rl->len[idx + 1], (size_t)(rl->n - idx - 1) * sizeof(uint32_t));
    memmove(rl_payload(rl, idx + 2), rl_payload(rl, idx + 1), (size_t)(rl->n - idx - 1) * rl->esize);

    uint32_t oldlen = rl->len[idx];
    memcpy(rl_payload(rl, idx + 1), rl_payload(rl, idx), rl->esize);  /* right keeps payload */
    rl->len[idx]     = within;
    rl->len[idx + 1] = oldlen - within;
    rl->n++;
    *out_idx = idx + 1;
    return DD_OK;
}

void rl_merge(runlist *rl)
{
    if (rl->n == 0) return;
    uint32_t w = 0;
    for (uint32_t r = 0; r < rl->n; r++) {
        if (rl->len[r] == 0 && rl->n > 1) continue;       /* drop empty runs */
        if (w > 0 && memcmp(rl_payload(rl, w - 1), rl_payload(rl, r), rl->esize) == 0) {
            rl->len[w - 1] += rl->len[r];
        } else {
            if (w != r) { rl->len[w] = rl->len[r]; memcpy(rl_payload(rl, w), rl_payload(rl, r), rl->esize); }
            w++;
        }
    }
    if (w == 0) { rl->len[0] = 0; w = 1; }                /* keep one empty run */
    rl->n = w;
}

dd_result rl_insert(runlist *rl, uint32_t pos, uint32_t n)
{
    if (n == 0) return DD_OK;
    if (rl->n == 0) {
        if (rl_reserve(rl, 1)) return DD_ERR_NOMEM;
        rl->len[0] = n; memset(rl_payload(rl, 0), 0, rl->esize); rl->n = 1;
        return DD_OK;
    }
    uint32_t idx, within;
    rl_find(rl, pos, &idx, &within);
    if (within == 0 && idx > 0) idx--;                    /* inherit format to the left */
    else if (idx >= rl->n) idx = rl->n - 1;               /* pos == total */
    rl->len[idx] += n;
    return DD_OK;
}

dd_result rl_delete(runlist *rl, uint32_t pos, uint32_t n)
{
    if (n == 0) return DD_OK;
    uint32_t total = rl_total(rl);
    if (pos >= total) return DD_OK;
    if (pos + n > total) n = total - pos;

    uint32_t idx, within, remaining = n;
    rl_find(rl, pos, &idx, &within);

    while (remaining > 0 && idx < rl->n) {
        uint32_t avail = rl->len[idx] - within;           /* bytes from pos to run end */
        uint32_t take  = (avail < remaining) ? avail : remaining;
        rl->len[idx] -= take;                             /* run is homogeneous */
        remaining    -= take;
        within = 0;
        if (rl->len[idx] == 0) {                          /* drop empty run, stay at idx */
            memmove(&rl->len[idx], &rl->len[idx + 1], (size_t)(rl->n - idx - 1) * sizeof(uint32_t));
            memmove(rl_payload(rl, idx), rl_payload(rl, idx + 1), (size_t)(rl->n - idx - 1) * rl->esize);
            rl->n--;
        } else {
            idx++;
        }
    }
    rl_merge(rl);
    if (rl->n == 0) {
        if (rl_reserve(rl, 1)) return DD_ERR_NOMEM;
        rl->len[0] = 0; memset(rl_payload(rl, 0), 0, rl->esize); rl->n = 1;
    }
    return DD_OK;
}

dd_result rl_apply(runlist *rl, uint32_t pos, uint32_t n,
                   void (*fn)(void *payload, void *ctx), void *ctx)
{
    if (n == 0) return DD_OK;
    uint32_t i0, i1;
    if (rl_split_at(rl, pos, &i0)) return DD_ERR_NOMEM;
    if (rl_split_at(rl, pos + n, &i1)) return DD_ERR_NOMEM;
    for (uint32_t r = i0; r < i1 && r < rl->n; r++) fn(rl_payload(rl, r), ctx);
    rl_merge(rl);
    return DD_OK;
}

dd_result rl_copy(const runlist *src, runlist *dst)
{
    rl_init(dst, src->esize);
    if (src->n == 0) return DD_OK;
    if (rl_reserve(dst, src->n)) return DD_ERR_NOMEM;
    memcpy(dst->len, src->len, (size_t)src->n * sizeof(uint32_t));
    memcpy(dst->data, src->data, (size_t)src->n * src->esize);
    dst->n = src->n;
    return DD_OK;
}

dd_result rl_push(runlist *rl, uint32_t len, const void *payload)
{
    if (rl_reserve(rl, rl->n + 1)) return DD_ERR_NOMEM;
    rl->len[rl->n] = len;
    memcpy(rl_payload(rl, rl->n), payload, rl->esize);
    rl->n++;
    return DD_OK;
}
