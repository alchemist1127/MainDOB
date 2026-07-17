/* imgcodec.c — PNG e JPEG per DobPicture, da zero (vedi header).
 *
 * Struttura: blocchi ESECUTIVI (bit reader, inflate, defilter PNG,
 * Huffman/IDCT JPEG) in alto; gli ORCHESTRATORI dei due formati e il
 * dispatcher in fondo. */

#include "imgcodec_api.h"

/* Questo file e' un .MEM (vedi dob/mem.h): self-contained, niente
 * libc. memcpy/memset con nome vero (GCC li emette implicitamente),
 * allocazioni via callback del HOST (impostate con set_allocator
 * subito dopo il load — prima di allora decode/encode falliscono). */

void *memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int v, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)v;
    return dst;
}

static int memcmp_(const void *a, const void *b, uint32_t n)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (uint32_t i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}
#define memcmp memcmp_

static void *(*g_alloc)(uint32_t);
static void  (*g_free)(void *);
#define malloc(n) (g_alloc ? g_alloc((uint32_t)(n)) : (void *)0)
#define free(p)   do { if (g_free && (p)) g_free(p); } while (0)

static void set_allocator(void *(*a)(uint32_t), void (*f)(void *))
{
    g_alloc = a;
    g_free  = f;
}

/* ===================================================================
 * ESECUTIVI comuni
 * =================================================================== */

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static inline uint8_t clamp_u8(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* ===================================================================
 * INFLATE (RFC 1951) — per il PNG
 * =================================================================== */

typedef struct
{
    const uint8_t *d;
    uint32_t       size;
    uint32_t       pos;
    uint32_t       bitbuf;
    int            bitcnt;
} bitrd_t;

static int br_bits(bitrd_t *b, int n)       /* LSB-first, -1 = finito */
{
    while (b->bitcnt < n)
    {
        if (b->pos >= b->size)
        {
            return -1;
        }
        b->bitbuf |= (uint32_t)b->d[b->pos++] << b->bitcnt;
        b->bitcnt += 8;
    }
    int v = (int)(b->bitbuf & ((1u << n) - 1u));
    b->bitbuf >>= n;
    b->bitcnt  -= n;
    return v;
}

/* Huffman canonico: decodifica bit-a-bit contro i limiti per
 * lunghezza — 15 iterazioni al massimo per simbolo, zero tabelle
 * giganti: per un load one-shot e' il compromesso giusto. */
typedef struct
{
    uint16_t count[16];             /* codici per lunghezza 1..15       */
    uint16_t sym[288];              /* simboli in ordine canonico       */
} huff_t;

static void huff_build(huff_t *h, const uint8_t *lens, int n)
{
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++)
    {
        h->count[lens[i]]++;
    }
    h->count[0] = 0;
    uint16_t offs[16];
    uint16_t sum = 0;
    for (int l = 1; l < 16; l++)
    {
        offs[l] = sum;
        sum = (uint16_t)(sum + h->count[l]);
    }
    for (int i = 0; i < n; i++)
    {
        if (lens[i] != 0)
        {
            h->sym[offs[lens[i]]++] = (uint16_t)i;
        }
    }
}

static int huff_decode(bitrd_t *b, const huff_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++)
    {
        int bit = br_bits(b, 1);
        if (bit < 0)
        {
            return -1;
        }
        code |= bit;
        int cnt = h->count[len];
        if (code - first < cnt)
        {
            return h->sym[index + (code - first)];
        }
        index += cnt;
        first  = (first + cnt) << 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
    67,83,99,115,131,163,195,227,258 };
static const uint8_t LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t DST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t DST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int inflate_block_huff(bitrd_t *b, const huff_t *lit,
                              const huff_t *dst,
                              uint8_t *out, uint32_t cap, uint32_t *opos)
{
    for (;;)
    {
        int sym = huff_decode(b, lit);
        if (sym < 0)      return -1;
        if (sym < 256)
        {
            if (*opos >= cap) return -1;
            out[(*opos)++] = (uint8_t)sym;
            continue;
        }
        if (sym == 256)   return 0;             /* fine blocco          */
        sym -= 257;
        if (sym >= 29)    return -1;
        int e   = br_bits(b, LEN_EXTRA[sym]);
        if (e < 0)        return -1;
        int len = LEN_BASE[sym] + e;
        int ds  = huff_decode(b, dst);
        if (ds < 0 || ds >= 30) return -1;
        e = br_bits(b, DST_EXTRA[ds]);
        if (e < 0)        return -1;
        uint32_t dist = (uint32_t)DST_BASE[ds] + (uint32_t)e;
        if (dist > *opos || *opos + (uint32_t)len > cap) return -1;
        for (int i = 0; i < len; i++, (*opos)++)
        {
            out[*opos] = out[*opos - dist];
        }
    }
}

static int inflate(const uint8_t *in, uint32_t in_size,
                   uint8_t *out, uint32_t out_cap)
{
    bitrd_t b = { in, in_size, 0, 0, 0 };
    uint32_t opos = 0;
    huff_t lit, dst;
    uint8_t lens[320];

    for (;;)
    {
        int final = br_bits(&b, 1);
        int type  = br_bits(&b, 2);
        if (final < 0 || type < 0) return -1;

        if (type == 0)                          /* stored                */
        {
            b.bitbuf = 0;
            b.bitcnt = 0;
            if (b.pos + 4 > b.size) return -1;
            uint32_t len = (uint32_t)in[b.pos] | ((uint32_t)in[b.pos+1] << 8);
            b.pos += 4;                         /* len + ~len            */
            if (b.pos + len > b.size || opos + len > out_cap) return -1;
            memcpy(out + opos, in + b.pos, len);
            b.pos += len;
            opos  += len;
        }
        else if (type == 1)                     /* Huffman fisso         */
        {
            for (int i = 0;   i < 144; i++) lens[i] = 8;
            for (int i = 144; i < 256; i++) lens[i] = 9;
            for (int i = 256; i < 280; i++) lens[i] = 7;
            for (int i = 280; i < 288; i++) lens[i] = 8;
            huff_build(&lit, lens, 288);
            for (int i = 0; i < 30; i++) lens[i] = 5;
            huff_build(&dst, lens, 30);
            if (inflate_block_huff(&b, &lit, &dst, out, out_cap, &opos) < 0)
                return -1;
        }
        else if (type == 2)                     /* Huffman dinamico      */
        {
            int hlit  = br_bits(&b, 5);
            int hdist = br_bits(&b, 5);
            int hclen = br_bits(&b, 4);
            if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
            hlit += 257; hdist += 1; hclen += 4;
            static const uint8_t ORD[19] = {
                16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            uint8_t clens[19];
            memset(clens, 0, sizeof(clens));
            for (int i = 0; i < hclen; i++)
            {
                int v = br_bits(&b, 3);
                if (v < 0) return -1;
                clens[ORD[i]] = (uint8_t)v;
            }
            huff_t ch;
            huff_build(&ch, clens, 19);
            int n = 0;
            while (n < hlit + hdist)
            {
                int sym = huff_decode(&b, &ch);
                if (sym < 0) return -1;
                if (sym < 16)      lens[n++] = (uint8_t)sym;
                else if (sym == 16)
                {
                    int r = br_bits(&b, 2);
                    if (r < 0 || n == 0) return -1;
                    uint8_t prev = lens[n - 1];
                    for (int i = 0; i < r + 3 && n < 320; i++)
                        lens[n++] = prev;
                }
                else
                {
                    int r = br_bits(&b, sym == 17 ? 3 : 7);
                    if (r < 0) return -1;
                    r += sym == 17 ? 3 : 11;
                    for (int i = 0; i < r && n < 320; i++)
                        lens[n++] = 0;
                }
            }
            huff_build(&lit, lens, hlit);
            huff_build(&dst, lens + hlit, hdist);
            if (inflate_block_huff(&b, &lit, &dst, out, out_cap, &opos) < 0)
                return -1;
        }
        else
        {
            return -1;
        }

        if (final)
        {
            return (int)opos;
        }
    }
}

/* ===================================================================
 * PNG
 * =================================================================== */

static inline int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc)             return b;
    return c;
}

static int png_decode(const uint8_t *d, uint32_t size,
                      uint32_t **out_px, int *out_w, int *out_h,
                      int max_w, int max_h)
{
    static const uint8_t SIG[8] = { 137,80,78,71,13,10,26,10 };
    if (size < 45 || memcmp(d, SIG, 8) != 0) return -1;

    uint32_t w = 0, h = 0;
    int bitdepth = 0, ctype = -1, interlace = -1;
    uint8_t palette[256][3];
    int npal = 0;
    uint8_t *idat = NULL;
    uint32_t idat_n = 0, idat_cap = 0;
    int rc = -1;

    for (uint32_t p = 8; p + 12 <= size; )
    {
        uint32_t clen = be32(d + p);
        const uint8_t *ctyp = d + p + 4;
        const uint8_t *body = d + p + 8;
        if (p + 12 + clen > size) break;

        if (memcmp(ctyp, "IHDR", 4) == 0 && clen >= 13)
        {
            w = be32(body);
            h = be32(body + 4);
            bitdepth  = body[8];
            ctype     = body[9];
            interlace = body[12];
        }
        else if (memcmp(ctyp, "PLTE", 4) == 0)
        {
            npal = (int)(clen / 3u);
            if (npal > 256) npal = 256;
            memcpy(palette, body, (size_t)npal * 3u);
        }
        else if (memcmp(ctyp, "IDAT", 4) == 0)
        {
            if (idat_n + clen > idat_cap)
            {
                idat_cap = (idat_n + clen) * 2u;
                uint8_t *nb = (uint8_t *)malloc(idat_cap);
                if (nb == NULL) goto out;
                if (idat != NULL) memcpy(nb, idat, idat_n);
                free(idat);
                idat = nb;
            }
            memcpy(idat + idat_n, body, clen);
            idat_n += clen;
        }
        else if (memcmp(ctyp, "IEND", 4) == 0)
        {
            break;
        }
        p += 12 + clen;
    }

    /* Limiti dichiarati: 8 bit, niente Adam7. */
    int nch;
    switch (ctype)
    {
    case 0: nch = 1; break;                     /* gray                  */
    case 2: nch = 3; break;                     /* RGB                   */
    case 3: nch = 1; break;                     /* palette               */
    case 4: nch = 2; break;                     /* gray + alpha          */
    case 6: nch = 4; break;                     /* RGBA                  */
    default: goto out;
    }
    if (w == 0 || h == 0 || (int)w > max_w || (int)h > max_h ||
        bitdepth != 8 || interlace != 0 || idat == NULL)
    {
        goto out;
    }

    /* zlib: 2 byte di header, poi DEFLATE (Adler in coda ignorato). */
    uint32_t stride = w * (uint32_t)nch;
    uint32_t raw_n  = (stride + 1u) * h;
    uint8_t *raw    = (uint8_t *)malloc(raw_n);
    if (raw == NULL) goto out;
    if (idat_n < 2 ||
        inflate(idat + 2, idat_n - 2, raw, raw_n) != (int)raw_n)
    {
        free(raw);
        goto out;
    }

    /* Defilter per riga + conversione a 0x00RRGGBB (alpha su bianco). */
    uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4u);
    if (px == NULL)
    {
        free(raw);
        goto out;
    }
    uint8_t *prev = NULL;
    for (uint32_t y = 0; y < h; y++)
    {
        uint8_t *row = raw + y * (stride + 1u);
        uint8_t  f   = row[0];
        uint8_t *cur = row + 1;
        for (uint32_t i = 0; i < stride; i++)
        {
            int a = i >= (uint32_t)nch ? cur[i - nch] : 0;
            int b = prev != NULL ? prev[i] : 0;
            int c = (prev != NULL && i >= (uint32_t)nch)
                  ? prev[i - nch] : 0;
            int v = cur[i];
            switch (f)
            {
            case 1: v += a;                 break;
            case 2: v += b;                 break;
            case 3: v += (a + b) / 2;       break;
            case 4: v += paeth(a, b, c);    break;
            default:                        break;
            }
            cur[i] = (uint8_t)v;
        }
        for (uint32_t x = 0; x < w; x++)
        {
            const uint8_t *s = cur + x * (uint32_t)nch;
            int r, g, bch, al = 255;
            switch (ctype)
            {
            case 0:  r = g = bch = s[0];                       break;
            case 2:  r = s[0]; g = s[1]; bch = s[2];           break;
            case 3:
                if (s[0] >= npal) { r = g = bch = 0; }
                else
                {
                    r = palette[s[0]][0];
                    g = palette[s[0]][1];
                    bch = palette[s[0]][2];
                }
                break;
            case 4:  r = g = bch = s[0]; al = s[1];            break;
            default: r = s[0]; g = s[1]; bch = s[2]; al = s[3]; break;
            }
            if (al != 255)                  /* composizione su bianco    */
            {
                r   = (r   * al + 255 * (255 - al)) / 255;
                g   = (g   * al + 255 * (255 - al)) / 255;
                bch = (bch * al + 255 * (255 - al)) / 255;
            }
            px[y * w + x] = ((uint32_t)r << 16) | ((uint32_t)g << 8)
                          | (uint32_t)bch;
        }
        prev = cur;
    }
    free(raw);
    *out_px = px;
    *out_w  = (int)w;
    *out_h  = (int)h;
    rc = 0;
out:
    free(idat);
    return rc;
}

/* ===================================================================
 * JPEG baseline
 * =================================================================== */

typedef struct
{
    uint8_t  bits[17];              /* codici per lunghezza (1..16)     */
    uint8_t  vals[256];
    int      mincode[17], maxcode[18], valptr[17];
} jhuff_t;

static void jhuff_build(jhuff_t *h)
{
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++)
    {
        h->valptr[l]  = k;
        h->mincode[l] = code;
        code += h->bits[l];
        k    += h->bits[l];
        h->maxcode[l] = code - 1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
}

typedef struct
{
    const uint8_t *d;
    uint32_t size, pos;
    uint32_t bitbuf;
    int      bitcnt;
} jbits_t;                          /* MSB-first, con byte-stuffing FF00 */

static int jb_bit(jbits_t *b)
{
    if (b->bitcnt == 0)
    {
        if (b->pos >= b->size) return -1;
        uint8_t v = b->d[b->pos++];
        if (v == 0xFF)
        {
            if (b->pos >= b->size) return -1;
            uint8_t m = b->d[b->pos++];
            if (m != 0x00)
            {
                b->pos -= 2;        /* marker vero: fine dati            */
                return -1;
            }
        }
        b->bitbuf = v;
        b->bitcnt = 8;
    }
    b->bitcnt--;
    return (int)((b->bitbuf >> b->bitcnt) & 1u);
}

static int jhuff_decode(jbits_t *b, const jhuff_t *h)
{
    int code = 0;
    for (int l = 1; l <= 16; l++)
    {
        int bit = jb_bit(b);
        if (bit < 0) return -1;
        code = (code << 1) | bit;
        if (code <= h->maxcode[l])
        {
            return h->vals[h->valptr[l] + code - h->mincode[l]];
        }
    }
    return -1;
}

static int jb_receive_extend(jbits_t *b, int s)
{
    if (s == 0) return 0;
    int v = 0;
    for (int i = 0; i < s; i++)
    {
        int bit = jb_bit(b);
        if (bit < 0) return -100000;
        v = (v << 1) | bit;
    }
    if (v < (1 << (s - 1)))
    {
        v += 1 - (1 << s);          /* estensione di segno JPEG          */
    }
    return v;
}

static const uint8_t ZIGZAG[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63 };

/* IDCT 8x8 intera, forma matriciale a due passate con tabella di
 * coseni a punto fisso (12 bit): out = C^T · X · C. Non e' la
 * fattorizzazione piu' veloce, ma e' CORRETTA per costruzione — e per
 * un load one-shot 1024 moltiplicazioni per blocco non si sentono.
 * (La prima versione usava un butterfly ad-hoc sbagliato: collaudo su
 * host contro il riferimento l'ha bocciata, questa fa maxdiff ~1.) */
static const int IDCT_C[8][8] = {
    {  1448,   1448,   1448,   1448,   1448,   1448,   1448,   1448},
    {  2009,   1703,   1138,    400,   -400,  -1138,  -1703,  -2009},
    {  1892,    784,   -784,  -1892,  -1892,   -784,    784,   1892},
    {  1703,   -400,  -2009,  -1138,   1138,   2009,    400,  -1703},
    {  1448,  -1448,  -1448,   1448,   1448,  -1448,  -1448,   1448},
    {  1138,  -2009,    400,   1703,  -1703,   -400,   2009,  -1138},
    {   784,  -1892,   1892,   -784,   -784,   1892,  -1892,    784},
    {   400,  -1138,   1703,  -2009,   2009,  -1703,   1138,   -400},
};

static void idct8x8(int *blk)
{
    int tmp[64];
    /* Passata colonne: tmp[n][c] = sum_k blk[k][c] * C[k][n]. */
    for (int c = 0; c < 8; c++)
    {
        for (int n = 0; n < 8; n++)
        {
            long long acc = 0;
            for (int k = 0; k < 8; k++)
            {
                acc += (long long)blk[k * 8 + c] * IDCT_C[k][n];
            }
            tmp[n * 8 + c] = (int)(acc >> 12);
        }
    }
    /* Passata righe: out[n][m] = sum_k tmp[n][k] * C[k][m]. */
    for (int n = 0; n < 8; n++)
    {
        for (int m = 0; m < 8; m++)
        {
            long long acc = 0;
            for (int k = 0; k < 8; k++)
            {
                acc += (long long)tmp[n * 8 + k] * IDCT_C[k][m];
            }
            blk[n * 8 + m] = (int)(acc >> 12) + 128;    /* level shift */
        }
    }
}

typedef struct
{
    int      id, hs, vs, tq;
    int      td, ta;
    int      dc_pred;
    uint8_t *plane;                 /* piano pieno (w_pad x h_pad)       */
    int      pw, ph;
} jcomp_t;

static int jpeg_decode(const uint8_t *d, uint32_t size,
                       uint32_t **out_px, int *out_w, int *out_h,
                       int max_w, int max_h)
{
    if (size < 4 || d[0] != 0xFF || d[1] != 0xD8) return -1;

    uint16_t qt[4][64];
    jhuff_t  hdc[4], hac[4];
    bool     has_q[4] = { false }, has_dc[4] = { false },
             has_ac[4] = { false };
    jcomp_t  comp[3];
    int      ncomp = 0, W = 0, H = 0, restart = 0;
    int      rc = -1;
    uint32_t p = 2;

    memset(comp, 0, sizeof(comp));

    while (p + 4 <= size)
    {
        if (d[p] != 0xFF) { p++; continue; }
        uint8_t m = d[p + 1];
        p += 2;
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (p + 2 > size) goto out;
        uint32_t seg = ((uint32_t)d[p] << 8) | d[p + 1];
        if (seg < 2 || p + seg > size) goto out;
        const uint8_t *b = d + p + 2;
        uint32_t blen = seg - 2;

        if (m == 0xDB)                              /* DQT                */
        {
            uint32_t q = 0;
            while (q + 65 <= blen)
            {
                int prec = b[q] >> 4, id = b[q] & 15;
                if (prec != 0 || id > 3) goto out;  /* solo 8 bit         */
                for (int i = 0; i < 64; i++) qt[id][i] = b[q + 1 + i];
                has_q[id] = true;
                q += 65;
            }
        }
        else if (m == 0xC4)                         /* DHT                */
        {
            uint32_t q = 0;
            while (q + 17 <= blen)
            {
                int cls = b[q] >> 4, id = b[q] & 15;
                if (id > 3) goto out;
                jhuff_t *h = cls ? &hac[id] : &hdc[id];
                int total = 0;
                for (int i = 1; i <= 16; i++)
                {
                    h->bits[i] = b[q + i];
                    total += h->bits[i];
                }
                if (total > 256 || q + 17 + (uint32_t)total > blen + 1)
                    goto out;
                memcpy(h->vals, b + q + 17, (size_t)total);
                jhuff_build(h);
                if (cls) has_ac[id] = true; else has_dc[id] = true;
                q += 17 + (uint32_t)total;
            }
        }
        else if (m == 0xC0)                         /* SOF0 baseline      */
        {
            if (blen < 6 || b[0] != 8) goto out;
            H = ((int)b[1] << 8) | b[2];
            W = ((int)b[3] << 8) | b[4];
            ncomp = b[5];
            if ((ncomp != 1 && ncomp != 3) || W <= 0 || H <= 0 ||
                W > max_w || H > max_h || blen < 6 + (uint32_t)ncomp * 3)
                goto out;
            for (int i = 0; i < ncomp; i++)
            {
                comp[i].id = b[6 + i*3];
                comp[i].hs = b[7 + i*3] >> 4;
                comp[i].vs = b[7 + i*3] & 15;
                comp[i].tq = b[8 + i*3];
                if (comp[i].hs < 1 || comp[i].hs > 2 ||
                    comp[i].vs < 1 || comp[i].vs > 2 || comp[i].tq > 3)
                    goto out;
            }
        }
        else if (m == 0xC2)                         /* progressive: no    */
        {
            goto out;
        }
        else if (m == 0xDD)                         /* DRI                */
        {
            if (blen < 2) goto out;
            restart = ((int)b[0] << 8) | b[1];
        }
        else if (m == 0xDA)                         /* SOS -> scan        */
        {
            if (W == 0 || blen < 1 + (uint32_t)ncomp * 2) goto out;
            int ns = b[0];
            if (ns != ncomp) goto out;
            for (int i = 0; i < ns; i++)
            {
                int cid = b[1 + i*2];
                for (int c = 0; c < ncomp; c++)
                {
                    if (comp[c].id == cid)
                    {
                        comp[c].td = b[2 + i*2] >> 4;
                        comp[c].ta = b[2 + i*2] & 15;
                    }
                }
            }
            p += seg;                               /* inizio entropia    */

            int hmax = 1, vmax = 1;
            for (int c = 0; c < ncomp; c++)
            {
                if (comp[c].hs > hmax) hmax = comp[c].hs;
                if (comp[c].vs > vmax) vmax = comp[c].vs;
            }
            int mcux = (W + hmax * 8 - 1) / (hmax * 8);
            int mcuy = (H + vmax * 8 - 1) / (vmax * 8);
            for (int c = 0; c < ncomp; c++)
            {
                if (!has_q[comp[c].tq] || !has_dc[comp[c].td] ||
                    !has_ac[comp[c].ta])
                    goto out;
                comp[c].pw = mcux * comp[c].hs * 8;
                comp[c].ph = mcuy * comp[c].vs * 8;
                comp[c].plane =
                    (uint8_t *)malloc((size_t)comp[c].pw * comp[c].ph);
                if (comp[c].plane == NULL) goto out;
                comp[c].dc_pred = 0;
            }

            jbits_t jb = { d, size, p, 0, 0 };
            int mcu_cnt = 0;
            for (int my = 0; my < mcuy; my++)
            for (int mx = 0; mx < mcux; mx++)
            {
                if (restart != 0 && mcu_cnt == restart)
                {
                    /* RSTn: riallinea al byte, consuma il marker,
                     * azzera i predittori DC. */
                    jb.bitcnt = 0;
                    while (jb.pos + 1 < jb.size &&
                           !(jb.d[jb.pos] == 0xFF &&
                             jb.d[jb.pos+1] >= 0xD0 &&
                             jb.d[jb.pos+1] <= 0xD7))
                        jb.pos++;
                    if (jb.pos + 1 < jb.size) jb.pos += 2;
                    for (int c = 0; c < ncomp; c++) comp[c].dc_pred = 0;
                    mcu_cnt = 0;
                }
                for (int c = 0; c < ncomp; c++)
                for (int by = 0; by < comp[c].vs; by++)
                for (int bx = 0; bx < comp[c].hs; bx++)
                {
                    int blk[64];
                    memset(blk, 0, sizeof(blk));
                    int s = jhuff_decode(&jb, &hdc[comp[c].td]);
                    if (s < 0) goto scan_done;
                    int diff = jb_receive_extend(&jb, s);
                    if (diff == -100000) goto scan_done;
                    comp[c].dc_pred += diff;
                    blk[0] = comp[c].dc_pred * qt[comp[c].tq][0];
                    for (int k = 1; k < 64; )
                    {
                        int rs = jhuff_decode(&jb, &hac[comp[c].ta]);
                        if (rs < 0) goto scan_done;
                        int run = rs >> 4, sz = rs & 15;
                        if (sz == 0)
                        {
                            if (run != 15) break;   /* EOB               */
                            k += 16;
                            continue;
                        }
                        k += run;
                        if (k > 63) break;
                        int v = jb_receive_extend(&jb, sz);
                        if (v == -100000) goto scan_done;
                        blk[ZIGZAG[k]] = v * qt[comp[c].tq][k];
                        k++;
                    }
                    idct8x8(blk);
                    int px0 = (mx * comp[c].hs + bx) * 8;
                    int py0 = (my * comp[c].vs + by) * 8;
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x++)
                            comp[c].plane[(py0 + y) * comp[c].pw
                                          + px0 + x] =
                                clamp_u8(blk[y * 8 + x]);
                }
                mcu_cnt++;
            }
scan_done:
            /* Ricostruzione RGB con upsampling nearest. */
            {
                uint32_t *px =
                    (uint32_t *)malloc((size_t)W * H * 4u);
                if (px == NULL) goto out;
                for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                {
                    int Y = comp[0].plane[
                        (y * comp[0].vs / vmax) * comp[0].pw
                        + x * comp[0].hs / hmax];
                    int r, g, bb;
                    if (ncomp == 1)
                    {
                        r = g = bb = Y;
                    }
                    else
                    {
                        int cb = comp[1].plane[
                            (y * comp[1].vs / vmax) * comp[1].pw
                            + x * comp[1].hs / hmax] - 128;
                        int cr = comp[2].plane[
                            (y * comp[2].vs / vmax) * comp[2].pw
                            + x * comp[2].hs / hmax] - 128;
                        r  = Y + ((91881  * cr) >> 16);
                        g  = Y - ((22554  * cb + 46802 * cr) >> 16);
                        bb = Y + ((116130 * cb) >> 16);
                    }
                    px[y * W + x] =
                        ((uint32_t)clamp_u8(r) << 16) |
                        ((uint32_t)clamp_u8(g) << 8)  |
                        (uint32_t)clamp_u8(bb);
                }
                *out_px = px;
                *out_w  = W;
                *out_h  = H;
                rc = 0;
            }
            goto out;
        }
        p += seg;
    }
out:
    for (int c = 0; c < 3; c++)
    {
        free(comp[c].plane);
    }
    return rc;
}


/* ===================================================================
 * ENCODER PNG — deflate a Huffman FISSO con LZ greedy (hash a 3 byte,
 * sonda singola): compressione onesta senza l'albero dinamico. CRC32
 * e Adler32 come da specifica.
 * =================================================================== */

static uint32_t crc_table[256];
static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++)
    {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc32(uint32_t c, const uint8_t *p, uint32_t n)
{
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

typedef struct { uint8_t *d; uint32_t n, cap; uint32_t bb; int bc; } bw_t;

static int bw_grow(bw_t *w, uint32_t need)
{
    if (w->n + need <= w->cap) return 0;
    uint32_t nc = (w->n + need) * 2u;
    uint8_t *nb = (uint8_t *)malloc(nc);
    if (nb == NULL) return -1;
    memcpy(nb, w->d, w->n);
    free(w->d);
    w->d = nb;
    w->cap = nc;
    return 0;
}
static int bw_byte(bw_t *w, uint8_t v)
{
    if (bw_grow(w, 1) < 0) return -1;
    w->d[w->n++] = v;
    return 0;
}
static int bw_bits(bw_t *w, uint32_t v, int n)     /* LSB-first        */
{
    w->bb |= v << w->bc;
    w->bc += n;
    while (w->bc >= 8)
    {
        if (bw_byte(w, (uint8_t)w->bb) < 0) return -1;
        w->bb >>= 8;
        w->bc  -= 8;
    }
    return 0;
}
static uint32_t rev_bits(uint32_t v, int n)        /* Huffman: MSB     */
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}
static int put_lit(bw_t *w, int sym)               /* Huffman fisso    */
{
    if (sym < 144)  return bw_bits(w, rev_bits(0x30 + sym, 8), 8);
    if (sym < 256)  return bw_bits(w, rev_bits(0x190 + sym - 144, 9), 9);
    if (sym < 280)  return bw_bits(w, rev_bits(sym - 256, 7), 7);
    return bw_bits(w, rev_bits(0xC0 + sym - 280, 8), 8);
}

static int deflate_fixed(bw_t *w, const uint8_t *src, uint32_t n)
{
    if (bw_bits(w, 1, 1) < 0 || bw_bits(w, 1, 2) < 0) return -1;
#define HBITS 13
    int32_t *head = (int32_t *)malloc((1u << HBITS) * 4u);
    if (head == NULL) return -1;
    for (uint32_t i = 0; i < (1u << HBITS); i++) head[i] = -1;

    uint32_t p = 0;
    while (p < n)
    {
        int len = 0, dist = 0;
        if (p + 3 <= n)
        {
            uint32_t hsh = ((uint32_t)src[p] * 506832829u
                          ^ (uint32_t)src[p+1] * 2654435761u
                          ^ (uint32_t)src[p+2] * 40503u) >> (32 - HBITS);
            int32_t cand = head[hsh];
            head[hsh] = (int32_t)p;
            if (cand >= 0 && p - (uint32_t)cand <= 32768u &&
                memcmp(src + cand, src + p, 3) == 0)
            {
                len = 3;
                uint32_t max = n - p;
                if (max > 258) max = 258;
                while ((uint32_t)len < max &&
                       src[cand + len] == src[p + len])
                    len++;
                dist = (int)(p - (uint32_t)cand);
            }
        }
        if (len >= 3)
        {
            int li = 28;
            while (li > 0 && LEN_BASE[li] > (uint16_t)len) li--;
            if (put_lit(w, 257 + li) < 0 ||
                bw_bits(w, (uint32_t)(len - LEN_BASE[li]),
                        LEN_EXTRA[li]) < 0)
                goto fail;
            int di = 29;
            while (di > 0 && DST_BASE[di] > (uint16_t)dist) di--;
            if (bw_bits(w, rev_bits((uint32_t)di, 5), 5) < 0 ||
                bw_bits(w, (uint32_t)(dist - DST_BASE[di]),
                        DST_EXTRA[di]) < 0)
                goto fail;
            p += (uint32_t)len;
        }
        else
        {
            if (put_lit(w, src[p]) < 0) goto fail;
            p++;
        }
    }
    if (put_lit(w, 256) < 0) goto fail;            /* fine blocco       */
    if (w->bc > 0 && bw_bits(w, 0, 8 - w->bc) < 0) goto fail;
    free(head);
    return 0;
fail:
    free(head);
    return -1;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static int png_encode(const uint32_t *px, int w, int h,
                      uint8_t **out, uint32_t *out_n)
{
    crc_init();
    uint32_t stride = (uint32_t)w * 3u;
    uint32_t raw_n  = (stride + 1u) * (uint32_t)h;
    uint8_t *raw = (uint8_t *)malloc(raw_n);
    if (raw == NULL) return -1;
    uint32_t a1 = 1, a2 = 0;            /* adler32                       */
    for (int y = 0; y < h; y++)
    {
        uint8_t *row = raw + (uint32_t)y * (stride + 1u);
        row[0] = 0;                     /* filtro none                   */
        for (int x = 0; x < w; x++)
        {
            uint32_t c = px[y * w + x];
            row[1 + x*3 + 0] = (uint8_t)(c >> 16);
            row[1 + x*3 + 1] = (uint8_t)(c >> 8);
            row[1 + x*3 + 2] = (uint8_t)c;
        }
        for (uint32_t i = 0; i < stride + 1u; i++)
        {
            a1 = (a1 + row[i]) % 65521u;
            a2 = (a2 + a1)     % 65521u;
        }
    }

    bw_t z = { 0 };
    if (bw_byte(&z, 0x78) < 0 || bw_byte(&z, 0x01) < 0 ||
        deflate_fixed(&z, raw, raw_n) < 0)
    {
        free(raw); free(z.d); return -1;
    }
    free(raw);
    uint32_t adler = (a2 << 16) | a1;

    uint32_t total = 8 + 25 + (12 + z.n + 4) + 12;
    uint8_t *o = (uint8_t *)malloc(total);
    if (o == NULL) { free(z.d); return -1; }
    uint8_t *p = o;
    static const uint8_t SIG[8] = { 137,80,78,71,13,10,26,10 };
    memcpy(p, SIG, 8); p += 8;
    put_be32(p, 13); memcpy(p+4, "IHDR", 4);
    put_be32(p+8, (uint32_t)w); put_be32(p+12, (uint32_t)h);
    p[16]=8; p[17]=2; p[18]=0; p[19]=0; p[20]=0;   /* 8bit RGB          */
    put_be32(p+21, crc32(0, p+4, 17)); p += 25;
    put_be32(p, z.n + 4); memcpy(p+4, "IDAT", 4);
    memcpy(p+8, z.d, z.n);
    put_be32(p+8+z.n, adler);
    put_be32(p+12+z.n, crc32(0, p+4, z.n + 8)); p += 12 + z.n + 4;
    put_be32(p, 0); memcpy(p+4, "IEND", 4);
    put_be32(p+8, crc32(0, p+4, 4)); p += 12;
    free(z.d);
    *out   = o;
    *out_n = (uint32_t)(p - o);
    return 0;
}

/* ===================================================================
 * ENCODER JPEG — baseline 4:4:4, tabelle standard (Annex K) scalate
 * per qualita', FDCT con la STESSA matrice dell'IDCT (trasposta).
 * =================================================================== */

static const uint8_t JQ_LUMA[64] = {
    16,11,10,16,24,40,51,61, 12,12,14,19,26,58,60,55,
    14,13,16,24,40,57,69,56, 14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77, 24,35,55,64,81,104,113,92,
    49,64,78,87,103,121,120,101, 72,92,95,98,112,100,103,99 };
static const uint8_t JQ_CHROMA[64] = {
    17,18,24,47,99,99,99,99, 18,21,26,66,99,99,99,99,
    24,26,56,99,99,99,99,99, 47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99 };
/* Huffman standard: (bits[16], vals). */
static const uint8_t HB_DCL[16]={0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t HV_DCL[12]={0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t HB_DCC[16]={0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t HV_DCC[12]={0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t HB_ACL[16]={0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const uint8_t HV_ACL[162]={
  0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,
  0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,
  0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,
  0x26,0x27,0x28,0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,
  0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,
  0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,
  0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,
  0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,
  0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,
  0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,
  0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa };
static const uint8_t HB_ACC[16]={0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
static const uint8_t HV_ACC[162]={
  0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,
  0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,
  0x52,0xf0,0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,
  0x19,0x1a,0x26,0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,
  0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,
  0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,
  0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,
  0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,
  0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,
  0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,
  0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa };

typedef struct { uint16_t code; uint8_t len; } jenc_code_t;

static void jenc_build(const uint8_t *bits, const uint8_t *vals,
                       jenc_code_t *tab)
{
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++)
    {
        for (int i = 0; i < bits[l - 1]; i++)
        {
            tab[vals[k]].code = (uint16_t)code;
            tab[vals[k]].len  = (uint8_t)l;
            k++;
            code++;
        }
        code <<= 1;
    }
}

typedef struct { bw_t w; uint32_t bb; int bc; } jw_t;   /* MSB + stuff  */

static int jw_bits(jw_t *j, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--)
    {
        j->bb = (j->bb << 1) | ((v >> i) & 1u);
        j->bc++;
        if (j->bc == 8)
        {
            uint8_t b = (uint8_t)j->bb;
            if (bw_byte(&j->w, b) < 0) return -1;
            if (b == 0xFF && bw_byte(&j->w, 0x00) < 0) return -1;
            j->bb = 0;
            j->bc = 0;
        }
    }
    return 0;
}

static int jenc_magnitude(int v)        /* categoria SSSS               */
{
    int a = v < 0 ? -v : v, s = 0;
    while (a) { a >>= 1; s++; }
    return s;
}

static void fdct8x8(int *blk)           /* out = C · X · C^T, x8        */
{
    int tmp[64];
    for (int c = 0; c < 8; c++)
        for (int n = 0; n < 8; n++)
        {
            long long acc = 0;
            for (int k = 0; k < 8; k++)
                acc += (long long)blk[k * 8 + c] * IDCT_C[n][k];
            tmp[n * 8 + c] = (int)(acc >> 12);
        }
    for (int n = 0; n < 8; n++)
        for (int m = 0; m < 8; m++)
        {
            long long acc = 0;
            for (int k = 0; k < 8; k++)
                acc += (long long)tmp[n * 8 + k] * IDCT_C[m][k];
            /* La matrice e' ORTONORMALE (la IDCT e' la trasposta pura):
             * nessun fattore 8 da compensare — il primo tentativo con
             * >>15 produceva coefficienti 8x troppo piccoli, quasi
             * tutto quantizzato a zero (collaudo host bocciato). */
            blk[n * 8 + m] = (int)(acc >> 12);
        }
}

static int jenc_block(jw_t *j, int *blk, const uint8_t *q,
                      const jenc_code_t *dc, const jenc_code_t *ac,
                      int *dc_pred)
{
    fdct8x8(blk);
    int zz[64];
    for (int i = 0; i < 64; i++)
    {
        int v = blk[ZIGZAG[i]];
        int qv = q[i];
        zz[i] = v >= 0 ? (v + qv / 2) / qv : -((-v + qv / 2) / qv);
    }
    int diff = zz[0] - *dc_pred;
    *dc_pred = zz[0];
    int s = jenc_magnitude(diff);
    if (jw_bits(j, dc[s].code, dc[s].len) < 0) return -1;
    if (s && jw_bits(j, (uint32_t)(diff < 0 ? diff - 1 + (1 << s)
                                            : diff), s) < 0)
        return -1;
    int run = 0;
    for (int k = 1; k < 64; k++)
    {
        if (zz[k] == 0) { run++; continue; }
        while (run > 15)
        {
            if (jw_bits(j, ac[0xF0].code, ac[0xF0].len) < 0) return -1;
            run -= 16;
        }
        s = jenc_magnitude(zz[k]);
        int rs = (run << 4) | s;
        if (jw_bits(j, ac[rs].code, ac[rs].len) < 0) return -1;
        if (jw_bits(j, (uint32_t)(zz[k] < 0 ? zz[k] - 1 + (1 << s)
                                            : zz[k]), s) < 0)
            return -1;
        run = 0;
    }
    if (run > 0 && jw_bits(j, ac[0x00].code, ac[0x00].len) < 0)
        return -1;
    return 0;
}

static int seg(bw_t *w, uint8_t m, const uint8_t *body, uint32_t n)
{
    if (bw_byte(w, 0xFF) < 0 || bw_byte(w, m) < 0) return -1;
    if (bw_byte(w, (uint8_t)((n + 2) >> 8)) < 0 ||
        bw_byte(w, (uint8_t)(n + 2)) < 0)
        return -1;
    for (uint32_t i = 0; i < n; i++)
        if (bw_byte(w, body[i]) < 0) return -1;
    return 0;
}

static int jpeg_encode(const uint32_t *px, int w, int h, int quality,
                       uint8_t **out, uint32_t *out_n)
{
    if (quality < 1)   quality = 1;
    if (quality > 100) quality = 100;
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    uint8_t ql[64], qc[64];
    for (int i = 0; i < 64; i++)
    {
        int v = (JQ_LUMA[i]   * scale + 50) / 100;
        ql[i] = (uint8_t)(v < 1 ? 1 : v > 255 ? 255 : v);
        v = (JQ_CHROMA[i] * scale + 50) / 100;
        qc[i] = (uint8_t)(v < 1 ? 1 : v > 255 ? 255 : v);
    }
    jenc_code_t dcl[256], dcc[256], acl[256], acc[256];
    memset(dcl, 0, sizeof(dcl)); memset(dcc, 0, sizeof(dcc));
    memset(acl, 0, sizeof(acl)); memset(acc, 0, sizeof(acc));
    jenc_build(HB_DCL, HV_DCL, dcl);
    jenc_build(HB_DCC, HV_DCC, dcc);
    jenc_build(HB_ACL, HV_ACL, acl);
    jenc_build(HB_ACC, HV_ACC, acc);

    jw_t j;
    memset(&j, 0, sizeof(j));
    uint8_t b[300];
    if (bw_byte(&j.w, 0xFF) < 0 || bw_byte(&j.w, 0xD8) < 0) goto fail;
    /* DQT x2 */
    b[0] = 0x00; memcpy(b + 1, ql, 64);
    if (seg(&j.w, 0xDB, b, 65) < 0) goto fail;
    b[0] = 0x01; memcpy(b + 1, qc, 64);
    if (seg(&j.w, 0xDB, b, 65) < 0) goto fail;
    /* SOF0: 3 componenti 1x1 (4:4:4) */
    b[0] = 8;
    b[1] = (uint8_t)(h >> 8); b[2] = (uint8_t)h;
    b[3] = (uint8_t)(w >> 8); b[4] = (uint8_t)w;
    b[5] = 3;
    b[6] = 1; b[7] = 0x11; b[8]  = 0;
    b[9] = 2; b[10] = 0x11; b[11] = 1;
    b[12] = 3; b[13] = 0x11; b[14] = 1;
    if (seg(&j.w, 0xC0, b, 15) < 0) goto fail;
    /* DHT x4 */
    b[0] = 0x00; memcpy(b+1, HB_DCL, 16); memcpy(b+17, HV_DCL, 12);
    if (seg(&j.w, 0xC4, b, 29) < 0) goto fail;
    b[0] = 0x10; memcpy(b+1, HB_ACL, 16); memcpy(b+17, HV_ACL, 162);
    if (seg(&j.w, 0xC4, b, 179) < 0) goto fail;
    b[0] = 0x01; memcpy(b+1, HB_DCC, 16); memcpy(b+17, HV_DCC, 12);
    if (seg(&j.w, 0xC4, b, 29) < 0) goto fail;
    b[0] = 0x11; memcpy(b+1, HB_ACC, 16); memcpy(b+17, HV_ACC, 162);
    if (seg(&j.w, 0xC4, b, 179) < 0) goto fail;
    /* SOS */
    b[0] = 3;
    b[1] = 1; b[2] = 0x00;
    b[3] = 2; b[4] = 0x11;
    b[5] = 3; b[6] = 0x11;
    b[7] = 0; b[8] = 63; b[9] = 0;
    if (seg(&j.w, 0xDA, b, 10) < 0) goto fail;

    int dpY = 0, dpCb = 0, dpCr = 0;
    for (int my = 0; my < (h + 7) / 8; my++)
    for (int mx = 0; mx < (w + 7) / 8; mx++)
    {
        int Y[64], Cb[64], Cr[64];
        for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
        {
            int sx = mx * 8 + x, sy = my * 8 + y;
            if (sx >= w) sx = w - 1;
            if (sy >= h) sy = h - 1;
            uint32_t c = px[sy * w + sx];
            int r = (int)((c >> 16) & 0xFF);
            int g = (int)((c >> 8)  & 0xFF);
            int bl = (int)(c & 0xFF);
            int i = y * 8 + x;
            Y[i]  = ((19595*r + 38470*g + 7471*bl)  >> 16) - 128;
            Cb[i] = ((-11059*r - 21709*g + 32768*bl) >> 16);
            Cr[i] = ((32768*r - 27439*g - 5329*bl)   >> 16);
        }
        if (jenc_block(&j, Y,  ql, dcl, acl, &dpY)  < 0 ||
            jenc_block(&j, Cb, qc, dcc, acc, &dpCb) < 0 ||
            jenc_block(&j, Cr, qc, dcc, acc, &dpCr) < 0)
            goto fail;
    }
    if (j.bc > 0 && jw_bits(&j, 0x7F, 8 - j.bc) < 0) goto fail;
    if (bw_byte(&j.w, 0xFF) < 0 || bw_byte(&j.w, 0xD9) < 0) goto fail;
    *out   = j.w.d;
    *out_n = j.w.n;
    return 0;
fail:
    free(j.w.d);
    return -1;
}

/* ===================================================================
 * ORCHESTRATORE — dispatcher
 * =================================================================== */

int img_probe(const uint8_t *head, int n)
{
    if (n >= 4 && head[0] == 137 && head[1] == 'P' &&
        head[2] == 'N' && head[3] == 'G')
        return 1;
    if (n >= 2 && head[0] == 0xFF && head[1] == 0xD8)
        return 2;
    return 0;
}

int img_decode(const uint8_t *data, uint32_t size,
               uint32_t **out_px, int *out_w, int *out_h,
               int max_w, int max_h)
{
    switch (img_probe(data, (int)size))
    {
    case 1:  return png_decode(data, size, out_px, out_w, out_h,
                               max_w, max_h);
    case 2:  return jpeg_decode(data, size, out_px, out_w, out_h,
                                max_w, max_h);
    default: return -1;
    }
}

/* ===================================================================
 * EXPORT del .mem
 * =================================================================== */

imgcodec_api_t __mem_exports = {
    IMGCODEC_API_VERSION,
    set_allocator,
    img_probe,
    img_decode,
    png_encode,
    jpeg_encode,
};

