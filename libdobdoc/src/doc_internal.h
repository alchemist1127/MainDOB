/* libdobdoc -- internal structures (not public). */

#ifndef DOBDOC_INTERNAL_H
#define DOBDOC_INTERNAL_H

#include <dobdoc/dobdoc.h>

/* ====================================================================
 *  piece table
 * ==================================================================== */

typedef struct { uint8_t src; uint32_t off, len; } piece;   /* src: 0 orig, 1 add */

typedef struct
{
    uint8_t *orig; uint32_t orig_len;            /* immutable original buffer */
    uint8_t *add;  uint32_t add_len, add_cap;    /* append-only insert buffer */
    piece   *pc;   uint32_t npc, cap;            /* ordered pieces */
    uint32_t total;                              /* cached text length */
} piecetable;

void      pt_init(piecetable *pt);
void      pt_free(piecetable *pt);
dd_result pt_set_original(piecetable *pt, const uint8_t *data, uint32_t len);
uint32_t  pt_total(const piecetable *pt);
int       pt_byte_at(const piecetable *pt, uint32_t pos);            /* -1 if oob */
uint32_t  pt_get_text(const piecetable *pt, uint32_t pos, uint32_t n,
                      char *buf, uint32_t cap);
dd_result pt_insert(piecetable *pt, uint32_t pos, const uint8_t *data, uint32_t n);
dd_result pt_delete(piecetable *pt, uint32_t pos, uint32_t n);
/* undo support: copy/restore just the piece vector + total */
dd_result pt_snapshot(const piecetable *pt, piece **pc_out, uint32_t *npc_out, uint32_t *total_out);
void      pt_restore(piecetable *pt, piece *pc, uint32_t npc, uint32_t total);

/* ====================================================================
 *  generic attribute run list (payload of fixed esize per run)
 * ==================================================================== */

typedef struct
{
    uint32_t *len;       /* per-run byte length */
    uint8_t  *data;      /* n * esize payload bytes */
    uint32_t  n, cap, esize;
} runlist;

void      rl_init(runlist *rl, uint32_t esize);
void      rl_free(runlist *rl);
dd_result rl_reset_single(runlist *rl, uint32_t total, const void *payload);
uint32_t  rl_total(const runlist *rl);
void     *rl_payload(const runlist *rl, uint32_t i);
void      rl_find(const runlist *rl, uint32_t pos, uint32_t *idx, uint32_t *within);
void     *rl_payload_at(const runlist *rl, uint32_t pos);
dd_result rl_split_at(runlist *rl, uint32_t pos, uint32_t *out_idx);
void      rl_merge(runlist *rl);
dd_result rl_insert(runlist *rl, uint32_t pos, uint32_t n);
dd_result rl_delete(runlist *rl, uint32_t pos, uint32_t n);
dd_result rl_apply(runlist *rl, uint32_t pos, uint32_t n,
                   void (*fn)(void *payload, void *ctx), void *ctx);
dd_result rl_copy(const runlist *src, runlist *dst);   /* dst takes esize from src */
dd_result rl_push(runlist *rl, uint32_t len, const void *payload);  /* append a run (load) */

/* ====================================================================
 *  document
 * ==================================================================== */

typedef struct
{
    piece   *pc; uint32_t npc, total;     /* piece snapshot */
    runlist  cruns;                       /* CharFmt run snapshot */
    runlist  pruns;                       /* ParaFmt run snapshot */
    runlist  sruns;                       /* section-index run snapshot */
    PageSetup *sections; int nsections;   /* section page table snapshot */
} undo_state;

struct df_doc
{
    piecetable pt;
    runlist    cruns;     /* direct CharFmt overrides (payload = CharFmt, mask = set bits) */
    runlist    pruns;     /* direct ParaFmt overrides (payload = ParaFmt) */

    CharFmt    def_cf;    /* document default char fmt (fully set) */

    /* Sections: the document is partitioned into contiguous byte ranges, each
     * with its own page geometry.  `sruns` is a runlist whose payload is a
     * uint32 INDEX into `sections[]`; the runlist machinery keeps the byte
     * ranges aligned with the text across edits, and rl_merge coalesces runs
     * that share an index (same section) but never distinct indices.  Index 0
     * always exists (sections[0] = the document default), so an emptied
     * document (rl_delete zeroes the lone run) still resolves to a valid page. */
    runlist    sruns;     /* payload = uint32 section index */
    PageSetup *sections; int nsections, sections_cap;

    Style     *styles; int nstyles, styles_cap;

    char     **fam;    int nfam, fam_cap;   /* interned family names; id 0 reserved = "" */

    undo_state *ustack; int un, ucap;
    undo_state *rstack; int rn, rcap;
};

/* internal (document.c) -- append a page setup to the section table, idx or -1 */
int sect_add(df_doc *d, const PageSetup *p);

#endif /* DOBDOC_INTERNAL_H */
