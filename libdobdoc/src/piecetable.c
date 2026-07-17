/* libdobdoc -- piece table.
 *
 * The logical text is the concatenation of `pieces`, each a window into
 * either the immutable original buffer or the append-only add buffer.
 * Inserts append bytes to the add buffer and splice a piece in; deletes
 * trim/drop pieces. Large text never moves, and a piece-vector copy is a
 * cheap, complete undo snapshot.
 */

#include "doc_internal.h"
#include <string.h>
#include <stdlib.h>

void pt_init(piecetable *pt) { memset(pt, 0, sizeof(*pt)); }

void pt_free(piecetable *pt)
{
    free(pt->orig); free(pt->add); free(pt->pc);
    memset(pt, 0, sizeof(*pt));
}

static dd_result pc_reserve(piecetable *pt, uint32_t need)
{
    if (need <= pt->cap) return DD_OK;
    uint32_t nc = pt->cap ? pt->cap * 2 : 16;
    while (nc < need) nc *= 2;
    piece *np = (piece *)realloc(pt->pc, (size_t)nc * sizeof(piece));
    if (!np) return DD_ERR_NOMEM;
    pt->pc = np; pt->cap = nc;
    return DD_OK;
}

dd_result pt_set_original(piecetable *pt, const uint8_t *data, uint32_t len)
{
    free(pt->orig); pt->orig = NULL; pt->orig_len = 0;
    pt->npc = 0; pt->total = 0;

    if (len) {
        pt->orig = (uint8_t *)malloc(len);
        if (!pt->orig) return DD_ERR_NOMEM;
        memcpy(pt->orig, data, len);
        pt->orig_len = len;
        if (pc_reserve(pt, 1)) return DD_ERR_NOMEM;
        pt->pc[0].src = 0; pt->pc[0].off = 0; pt->pc[0].len = len;
        pt->npc = 1; pt->total = len;
    }
    return DD_OK;
}

uint32_t pt_total(const piecetable *pt) { return pt->total; }

/* locate the piece covering byte `pos`; for pos==total returns idx=npc */
static void pt_find(const piecetable *pt, uint32_t pos, uint32_t *idx, uint32_t *within)
{
    uint32_t acc = 0;
    for (uint32_t i = 0; i < pt->npc; i++) {
        if (pos < acc + pt->pc[i].len) { *idx = i; *within = pos - acc; return; }
        acc += pt->pc[i].len;
    }
    *idx = pt->npc; *within = 0;
}

static const uint8_t *pc_base(const piecetable *pt, const piece *p)
{
    return (p->src == 0) ? pt->orig : pt->add;
}

int pt_byte_at(const piecetable *pt, uint32_t pos)
{
    if (pos >= pt->total) return -1;
    uint32_t idx, within;
    pt_find(pt, pos, &idx, &within);
    const piece *p = &pt->pc[idx];
    return pc_base(pt, p)[p->off + within];
}

uint32_t pt_get_text(const piecetable *pt, uint32_t pos, uint32_t n,
                     char *buf, uint32_t cap)
{
    if (pos > pt->total) return 0;
    if (pos + n > pt->total) n = pt->total - pos;
    if (n > cap) n = cap;

    uint32_t idx, within, done = 0;
    pt_find(pt, pos, &idx, &within);
    for (uint32_t i = idx; i < pt->npc && done < n; i++) {
        const piece *p = &pt->pc[i];
        uint32_t avail = p->len - within;
        uint32_t take = (avail < n - done) ? avail : (n - done);
        memcpy(buf + done, pc_base(pt, p) + p->off + within, take);
        done += take; within = 0;
    }
    return done;
}

static dd_result add_append(piecetable *pt, const uint8_t *data, uint32_t n, uint32_t *off)
{
    if (pt->add_len + n > pt->add_cap) {
        uint32_t nc = pt->add_cap ? pt->add_cap * 2 : 256;
        while (nc < pt->add_len + n) nc *= 2;
        uint8_t *nb = (uint8_t *)realloc(pt->add, nc);
        if (!nb) return DD_ERR_NOMEM;
        pt->add = nb; pt->add_cap = nc;
    }
    *off = pt->add_len;
    memcpy(pt->add + pt->add_len, data, n);
    pt->add_len += n;
    return DD_OK;
}

dd_result pt_insert(piecetable *pt, uint32_t pos, const uint8_t *data, uint32_t n)
{
    if (pos > pt->total) return DD_ERR_RANGE;
    if (n == 0) return DD_OK;

    uint32_t add_off;
    if (add_append(pt, data, n, &add_off)) return DD_ERR_NOMEM;

    /* fast path: appending right after a contiguous add piece (typing) */
    uint32_t idx, within;
    pt_find(pt, pos, &idx, &within);

    if (idx > 0 && within == 0) {
        piece *prev = &pt->pc[idx - 1];
        if (prev->src == 1 && prev->off + prev->len == add_off) {
            prev->len += n; pt->total += n; return DD_OK;
        }
    }
    if (pos == pt->total && pt->npc > 0) {
        piece *last = &pt->pc[pt->npc - 1];
        if (last->src == 1 && last->off + last->len == add_off) {
            last->len += n; pt->total += n; return DD_OK;
        }
    }

    /* general: split the piece at pos and insert a new add piece */
    piece newp = { 1, add_off, n };

    if (idx >= pt->npc) {                          /* append at end */
        if (pc_reserve(pt, pt->npc + 1)) return DD_ERR_NOMEM;
        pt->pc[pt->npc++] = newp;
    } else if (within == 0) {                      /* at a piece boundary */
        if (pc_reserve(pt, pt->npc + 1)) return DD_ERR_NOMEM;
        memmove(&pt->pc[idx + 1], &pt->pc[idx], (pt->npc - idx) * sizeof(piece));
        pt->pc[idx] = newp; pt->npc++;
    } else {                                       /* split piece idx */
        if (pc_reserve(pt, pt->npc + 2)) return DD_ERR_NOMEM;
        piece orig = pt->pc[idx];
        piece left  = { orig.src, orig.off, within };
        piece right = { orig.src, orig.off + within, orig.len - within };
        memmove(&pt->pc[idx + 3], &pt->pc[idx + 1], (pt->npc - idx - 1) * sizeof(piece));
        pt->pc[idx]     = left;
        pt->pc[idx + 1] = newp;
        pt->pc[idx + 2] = right;
        pt->npc += 2;
    }
    pt->total += n;
    return DD_OK;
}

dd_result pt_delete(piecetable *pt, uint32_t pos, uint32_t n)
{
    if (pos > pt->total) return DD_ERR_RANGE;
    if (n == 0) return DD_OK;
    if (pos + n > pt->total) n = pt->total - pos;

    uint32_t i0, w0, i1, w1;
    pt_find(pt, pos, &i0, &w0);
    pt_find(pt, pos + n, &i1, &w1);

    /* build a fresh piece vector: [0,i0) + left frag + right frag + (i1..] */
    uint32_t cap = pt->npc + 2;                    /* capture BEFORE npc is reassigned */
    piece *np = (piece *)malloc((size_t)cap * sizeof(piece));
    if (!np) return DD_ERR_NOMEM;
    uint32_t m = 0;

    for (uint32_t i = 0; i < i0; i++) np[m++] = pt->pc[i];

    if (i0 < pt->npc && w0 > 0) {                  /* keep [0,w0) of piece i0 */
        piece p = pt->pc[i0];
        np[m].src = p.src; np[m].off = p.off; np[m].len = w0; m++;
    }
    if (i1 < pt->npc && w1 > 0) {                  /* keep [w1,len) of piece i1 */
        piece p = pt->pc[i1];
        np[m].src = p.src; np[m].off = p.off + w1; np[m].len = p.len - w1; m++;
    }
    /* When the cut ends exactly on a piece boundary (w1 == 0), piece i1 lies
     * wholly after the deleted range and the block above adds no fragment for
     * it -- so the tail must start AT i1, not past it, or i1 (and the count of
     * its bytes still in `total`) is silently orphaned. */
    for (uint32_t i = i1 + (w1 > 0 ? 1u : 0u); i < pt->npc; i++) np[m++] = pt->pc[i];

    free(pt->pc);
    pt->pc = np; pt->npc = m; pt->cap = cap;        /* cap = actual allocation (m always <= cap) */
    pt->total -= n;
    return DD_OK;
}

dd_result pt_snapshot(const piecetable *pt, piece **pc_out, uint32_t *npc_out,
                      uint32_t *total_out)
{
    piece *cp = NULL;
    if (pt->npc) {
        cp = (piece *)malloc((size_t)pt->npc * sizeof(piece));
        if (!cp) return DD_ERR_NOMEM;
        memcpy(cp, pt->pc, (size_t)pt->npc * sizeof(piece));
    }
    *pc_out = cp; *npc_out = pt->npc; *total_out = pt->total;
    return DD_OK;
}

void pt_restore(piecetable *pt, piece *pc, uint32_t npc, uint32_t total)
{
    free(pt->pc);
    if (npc) {
        pt->pc = (piece *)malloc((size_t)npc * sizeof(piece));
        if (pt->pc) memcpy(pt->pc, pc, (size_t)npc * sizeof(piece));
        pt->cap = npc;
    } else { pt->pc = NULL; pt->cap = 0; }
    pt->npc = npc; pt->total = total;
}
