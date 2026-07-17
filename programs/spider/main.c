/* MainDOB Spider — Spider Solitaire
 *
 * Tech demo: LAYERIZZAZIONE.  Il modello logico e' sempre completo (104
 * carte-oggetto esistono sempre: rango, seme, coperta/scoperta, posizione).
 * La presentazione materializza SOLO cio' che e' esposto: una carta sepolta
 * possiede la sola surface della sua banda-indice; una carta di coda possiede
 * la surface piena (indice + grande seme al centro); una carta totalmente
 * occlusa non possiede NESSUNA surface.  Ogni carta e' un oggetto a cui gli
 * sprite di seme e di numero vengono compositati DENTRO la sua surface RAM,
 * poi blittata (la VRAM la tocca solo l'ultimo passaggio).
 *
 * Il drag e' promozione/retrocessione di layer: afferrare una run la promuove
 * a un layer vivo che segue il mouse; sganciarla la retrocede a puro stato
 * logico nella pila di destinazione.
 *
 * Struttura del file (vincolo del C: i chiamati precedono i chiamanti):
 *   - in cima  : i VERBI esecutivi  (surface, sprite, primitive di pila)
 *   - in mezzo : le MATRIOSKE       (compose carta, materializza layer,
 *                                     semina mazzo, solleva run, scopri)
 *   - in fondo : le FUNZIONI LOGICHE (render della scena, handler d'evento,
 *                                     il flusso di gioco) e main()
 *
 * Controlli:
 *   Tieni premuto e trascina  = solleva una run scoperta e sganciala
 *   Click sul tallone (stock)  = sfoglia una carta sugli scarti;
 *                                a tallone vuoto, ricicla gli scarti da capo
 *   Trascina dalla carta scarti = gioca la carta sfogliata sul tableau
 *   Pannello                   = 1/2/4 semi (difficolta') + Nuova partita
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <app.h>
#include <DobInterface.h>
#include <dob/types.h>

/* ================================================================= *
 *  COSTANTI E TIPI
 * ================================================================= */

/* Due famiglie di colori, DUE convenzioni diverse (verificate sul codice
 * esistente).  Il blit delle surface (sw_blit_pixel_alpha) disegna un pixel
 * SOLO se il suo alpha byte != 0; il disegno diretto nella finestra
 * (FillRect/DrawText) usa invece la convenzione opaca del compositore. */

/* (A) Colori per il disegno DIRETTO nella finestra (solo il riquadro
 * vittoria, disegnato sopra la scena con FillRect/DrawText). */
#define COL_WIN     0x0000DDFFu  /* testo vittoria (DrawText)        */
#define COL_OVER    0x00005500u  /* sfondo riquadro vittoria         */

/* (B) Colori scritti DENTRO le surface e nello scene-buffer (percorso
 * BlitBuffer): opaco => alpha 0xFF, formato 0xAARRGGBB come
 * libdobpage/DobWrite; trasparente => alpha 0 (il pixel viene saltato). */
#define RGB(r,g,b)  (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define KEY_TRANSP  0x00000000u        /* alpha 0 => saltato          */
#define COL_BG      RGB(  0,102, 40)   /* verde tavolo                */
#define COL_EMPTY   RGB( 30, 74, 44)   /* contorno slot vuoto         */
#define COL_CARD    RGB(255,255,255)   /* faccia carta                */
#define COL_BACK    RGB(139, 90, 43)   /* dorso (marrone)             */
#define COL_BACK_IN RGB(176,112, 48)   /* motivo interno del dorso    */
#define COL_RED     RGB(200,  0,  0)   /* cuori / quadri              */
#define COL_BLACK   RGB( 20, 20, 20)   /* fiori / picche + bordi      */
#define COL_SEL     RGB(255,215,  0)   /* evidenza selezione          */

/* Geometria carta e tavolo. */
#define CW          58   /* larghezza carta                       */
#define CH          80   /* altezza carta                         */
#define MARGIN      10   /* margine sinistro                      */
#define GAP         11   /* spazio fra colonne                    */
#define TOP_Y       8    /* riga superiore (foundation + stock)   */
#define TAB_Y       118  /* inizio tableau                        */
#define DY_DOWN     7    /* offset verticale carta coperta        */
#define DY_UP       19   /* offset verticale carta scoperta       */

#define NCOL        10   /* colonne tableau                        */
#define STOCK       10   /* indice pila "tallone" (coperto)        */
#define WASTE       11   /* indice pila degli scarti (sfogliati)   */
#define NPILE       12   /* 0..9 tableau, 10 = stock, 11 = waste    */
#define MAXC        64   /* max carte per pila (margine ampio)     */
#define NCARDS      104  /* due mazzi                              */
#define NFOUND      8    /* run complete necessarie a vincere      */

/* Codifica carta: valore 0..51.  rango = val%13 (0=A..12=K), seme = val/13
 * (0=cuori,1=quadri,2=fiori,3=picche).  Cuori/quadri = rossi. */
#define R(v)        ((v) % 13)
#define S(v)        ((v) / 13)
#define IS_RED(v)   (S(v) < 2)

/* Firma della surface materializzata: se non cambia, non si ricompone.
 * OCCLUSA = nessuna surface (la carta esiste solo logicamente). */
#define SIG_OCCLUDED 0xFFFF

/* La carta-oggetto.  val/face_up sono il modello (sempre presenti); surf/sig
 * sono la presentazione (esistono solo quando la carta e' esposta). */
typedef struct
{
    uint8_t   val;      /* 0..51                                    */
    uint8_t   face_up;  /* 1 = scoperta                             */
    uint32_t *surf;     /* surface RAM propria, o NULL se occlusa   */
    uint16_t  sig;      /* cosa raffigura ora surf                  */
} Card;

/* Uno sprite: piccola surface RAM con color-key. */
typedef struct { int w, h; uint32_t *px; } Sprite;

/* Sessione di drag: la run promossa a layer vivo. */
typedef struct
{
    bool      active;
    int       src;      /* colonna d'origine                        */
    int       from;     /* indice della prima carta sollevata       */
    int       n;        /* quante carte                             */
    uint32_t *surf;     /* surface fluttuante (la run impilata)     */
    int       sw, sh;   /* sue dimensioni                           */
    int       grab_dx;  /* offset del punto di presa                */
    int       grab_dy;
    int       x, y;     /* posizione corrente a schermo             */
} Drag;

/* ================================================================= *
 *  STATO GLOBALE
 * ================================================================= */

static uint32_t win_id;
static int      win_w = 700, win_h = 560;

/* Scene-buffer: l'intera finestra composta in RAM e blittata UNA volta.
 * Il pool di texture del server ha 16 slot: 50+ blit separati collidono,
 * quindi si appiattiscono tutti i layer visibili qui e si blitta una volta. */
static uint32_t *g_scene;
static int       g_scene_w, g_scene_h;

static Card     deck[NCARDS];        /* i 104 oggetti-carta          */
static int      pile[NPILE][MAXC];   /* pile -> indici in deck[]      */
static int      pile_n[NPILE];
static uint8_t  found_suit[NFOUND];  /* seme delle run completate     */
static int      found_n;             /* run completate                */

static int      n_suits = 4;         /* difficolta': 1, 2 o 4         */
static int      n_cards = NCARDS;    /* carte attive (26 / 52 / 104)  */
static int      win_runs = NFOUND;   /* run da completare per vincere */
static bool     game_won;

static Drag     drag;

/* Atlante sprite, costruito una volta a init. */
static Sprite   spr_suit_big[4];     /* seme grande (centro)          */
static Sprite   spr_suit_sml[4];     /* seme piccolo (indice)         */
static Sprite   spr_glyph[14];       /* 0-9, A, J, Q, K               */

static uint32_t rng_state;

/* ================================================================= *
 *  VERBI ESECUTIVI — surface e pixel
 * ================================================================= */

static uint32_t rng(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7FFF;
}

static uint32_t *surf_alloc(int w, int h)
{
    return (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
}

static void surf_fill(uint32_t *s, int w, int h, uint32_t color)
{
    int n = w * h;
    for (int i = 0; i < n; i++) s[i] = color;
}

static void px_set(uint32_t *s, int w, int h, int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    s[y * w + x] = c;
}

/* Cornice rettangolare di 1px dentro la surface. */
static void surf_border(uint32_t *s, int w, int h, uint32_t c)
{
    for (int x = 0; x < w; x++) { px_set(s, w, h, x, 0, c); px_set(s, w, h, x, h - 1, c); }
    for (int y = 0; y < h; y++) { px_set(s, w, h, 0, y, c); px_set(s, w, h, w - 1, y, c); }
}

/* Angoli arrotondati: rende trasparenti (alpha 0) i 4 pixel d'angolo. */
static void surf_round_corners(uint32_t *s, int w, int h)
{
    px_set(s, w, h, 0, 0, KEY_TRANSP);
    px_set(s, w, h, w - 1, 0, KEY_TRANSP);
    px_set(s, w, h, 0, h - 1, KEY_TRANSP);
    px_set(s, w, h, w - 1, h - 1, KEY_TRANSP);
}

/* Blit di uno sprite (color-key) dentro una surface, con opzioni scala intera
 * e rotazione di 180 gradi.  Ricolora i pixel opachi dello sprite in `color`. */
static void surf_stamp(uint32_t *dst, int dw, int dh, int dx, int dy,
                       const Sprite *sp, uint32_t color, int scale, bool rot180)
{
    for (int sy = 0; sy < sp->h; sy++)
        for (int sx = 0; sx < sp->w; sx++)
        {
            uint32_t p = sp->px[sy * sp->w + sx];
            if (p == KEY_TRANSP) continue;
            int ux = rot180 ? (sp->w - 1 - sx) : sx;
            int uy = rot180 ? (sp->h - 1 - sy) : sy;
            for (int ky = 0; ky < scale; ky++)
                for (int kx = 0; kx < scale; kx++)
                    px_set(dst, dw, dh, dx + ux * scale + kx, dy + uy * scale + ky, color);
        }
}

/* Compositing surface -> scene-buffer: copia [0,w)x[0,h) di `s` in (dx,dy)
 * dello scene, saltando i pixel trasparenti (alpha 0) e ritagliando ai bordi.
 * E' il cuore dell'appiattimento: i layer-carta finiscono qui, non a video. */
static void scene_stamp(int dx, int dy, const uint32_t *s, int w, int h)
{
    for (int y = 0; y < h; y++)
    {
        int ty = dy + y;
        if (ty < 0 || ty >= g_scene_h) continue;
        for (int x = 0; x < w; x++)
        {
            int tx = dx + x;
            if (tx < 0 || tx >= g_scene_w) continue;
            uint32_t p = s[y * w + x];
            if (p == KEY_TRANSP) continue;
            g_scene[ty * g_scene_w + tx] = p;
        }
    }
}

/* Un pixel opaco nello scene, con clip. */
static void scene_px(int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= g_scene_w || y >= g_scene_h) return;
    g_scene[y * g_scene_w + x] = c;
}

/* Contorno rettangolare (2px) di uno slot vuoto direttamente nello scene. */
static void scene_slot(int x, int y)
{
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < CW; i++)
        {
            scene_px(x + i, y + t,          COL_EMPTY);
            scene_px(x + i, y + CH - 1 - t, COL_EMPTY);
        }
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < CH; i++)
        {
            scene_px(x + t,          y + i, COL_EMPTY);
            scene_px(x + CW - 1 - t, y + i, COL_EMPTY);
        }
}

/* Manda l'INTERO scene-buffer a schermo in un solo blit (dinamico: lo scene
 * muta in place ogni frame, il blit statico lo congelerebbe). */
static void scene_present(void)
{
    dobui_BlitBufferDynamic(win_id, 0, 0, g_scene, g_scene_w, g_scene_h);
}

/* ================================================================= *
 *  VERBI ESECUTIVI — costruzione dell'atlante sprite
 * ================================================================= */

/* Trasforma una bitmap testuale ('#'=pieno) in uno Sprite con color-key. */
static Sprite sprite_from_rows(const char *const *rows, int w, int h)
{
    Sprite sp; sp.w = w; sp.h = h;
    sp.px = surf_alloc(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            sp.px[y * w + x] = (rows[y][x] == '#') ? 0xFFFFFFFFu : KEY_TRANSP;
    return sp;
}

/* Riduce uno sprite a meta' risoluzione (per l'indice d'angolo). */
static Sprite sprite_half(const Sprite *big)
{
    int w = big->w / 2, h = big->h / 2;
    Sprite sp; sp.w = w; sp.h = h; sp.px = surf_alloc(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            /* opaco se almeno un pixel del blocco 2x2 e' pieno */
            int any = 0;
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++)
                    if (big->px[(y * 2 + j) * big->w + (x * 2 + i)] != KEY_TRANSP) any = 1;
            sp.px[y * w + x] = any ? 0xFFFFFFFFu : KEY_TRANSP;
        }
    return sp;
}

static void atlas_build_suits(void)
{
    static const char *heart[] = {
        "..##...##..", ".####.####.", "###########", "###########",
        ".#########.", "..#######..", "...#####...", "....###....",
        ".....#.....",
    };
    static const char *diamond[] = {
        ".....#.....", "....###....", "...#####...", "..#######..",
        ".#########.", "###########", ".#########.", "..#######..",
        "...#####...", "....###....", ".....#.....",
    };
    static const char *club[] = {
        "....###....", "...#####...", "...#####...", "....###....",
        ".##.###.##.", "###########", "###########", ".##.###.##.",
        "....###....", "...#####...",
    };
    static const char *spade[] = {
        ".....#.....", "....###....", "...#####...", "..#######..",
        ".#########.", "###########", "###########", ".###.#.###.",
        "....###....", "...#####...",
    };
    const char *const *rows[4] = { heart, diamond, club, spade };
    int hgt[4] = { 9, 11, 10, 10 };
    for (int s = 0; s < 4; s++)
    {
        spr_suit_big[s] = sprite_from_rows(rows[s], 11, hgt[s]);
        spr_suit_sml[s] = sprite_half(&spr_suit_big[s]);
    }
}

static void atlas_build_glyphs(void)
{
    /* font 5x7 per: 0..9, A, J, Q, K */
    static const char *G[14][7] = {
        {"01110","10001","10011","10101","11001","10001","01110"}, /* 0 */
        {"00100","01100","00100","00100","00100","00100","01110"}, /* 1 */
        {"01110","10001","00001","00010","00100","01000","11111"}, /* 2 */
        {"11111","00010","00100","00010","00001","10001","01110"}, /* 3 */
        {"00010","00110","01010","10010","11111","00010","00010"}, /* 4 */
        {"11111","10000","11110","00001","00001","10001","01110"}, /* 5 */
        {"00110","01000","10000","11110","10001","10001","01110"}, /* 6 */
        {"11111","00001","00010","00100","01000","01000","01000"}, /* 7 */
        {"01110","10001","10001","01110","10001","10001","01110"}, /* 8 */
        {"01110","10001","10001","01111","00001","00010","01100"}, /* 9 */
        {"01110","10001","10001","11111","10001","10001","10001"}, /* A */
        {"00111","00010","00010","00010","00010","10010","01100"}, /* J */
        {"01110","10001","10001","10001","10101","10010","01101"}, /* Q */
        {"10001","10010","10100","11000","10100","10010","10001"}, /* K */
    };
    for (int g = 0; g < 14; g++)
    {
        Sprite sp; sp.w = 5; sp.h = 7; sp.px = surf_alloc(5, 7);
        for (int y = 0; y < 7; y++)
            for (int x = 0; x < 5; x++)
                sp.px[y * 5 + x] = (G[g][y][x] == '1') ? 0xFFFFFFFFu : KEY_TRANSP;
        spr_glyph[g] = sp;
    }
}

/* Mappa un rango 0..12 alla sequenza di glifi che lo scrive ("10" = due). */
static int rank_glyphs(int rank, int *out /* max 2 */)
{
    switch (rank)
    {
        case 0:  out[0] = 10; return 1;                   /* A */
        case 9:  out[0] = 1; out[1] = 0; return 2;        /* 10 */
        case 10: out[0] = 11; return 1;                   /* J */
        case 11: out[0] = 12; return 1;                   /* Q */
        case 12: out[0] = 13; return 1;                   /* K */
        default: out[0] = rank + 1; return 1;             /* 2..9 */
    }
}

/* ================================================================= *
 *  VERBI ESECUTIVI — pile e geometria
 * ================================================================= */

static int  pile_top(int p)          { return pile_n[p] ? pile[p][pile_n[p] - 1] : -1; }
static void pile_push(int p, int ci) { pile[p][pile_n[p]++] = ci; }

static int col_x(int c) { return MARGIN + c * (CW + GAP); }

/* Y a schermo della carta in posizione `pos` della colonna `c`. */
static int card_y(int c, int pos)
{
    int y = TAB_Y;
    for (int k = 0; k < pos; k++)
        y += deck[pile[c][k]].face_up ? DY_UP : DY_DOWN;
    return y;
}

/* Altezza esposta di una carta: la banda visibile (o CH se e' la coda). */
static int card_band(int c, int pos)
{
    if (pos == pile_n[c] - 1) return CH;
    return deck[pile[c][pos]].face_up ? DY_UP : DY_DOWN;
}

/* ================================================================= *
 *  MATRIOSKE — compositing di una carta nella sua surface
 * ================================================================= */

/* Disegna l'indice d'angolo (rango sopra, semino sotto) a (ox,oy). */
static void stamp_index(uint32_t *s, int w, int h, int ox, int oy,
                        int val, uint32_t col, bool rot180)
{
    int gl[2]; int ng = rank_glyphs(R(val), gl);
    if (!rot180)
    {
        int gx = ox;
        for (int i = 0; i < ng; i++) { surf_stamp(s, w, h, gx, oy, &spr_glyph[gl[i]], col, 1, false); gx += 6; }
        surf_stamp(s, w, h, ox, oy + 8, &spr_suit_sml[S(val)], col, 1, false);
    }
    else
    {
        int gx = ox;
        surf_stamp(s, w, h, ox, oy, &spr_suit_sml[S(val)], col, 1, true);
        for (int i = 0; i < ng; i++) { surf_stamp(s, w, h, gx, oy + 8, &spr_glyph[gl[ng - 1 - i]], col, 1, true); gx += 6; }
    }
}

/* Compone la surface di una carta scoperta.  band_h < CH => solo la banda
 * indice (il grande seme centrale NON viene rasterizzato: e' occluso). */
static uint32_t *compose_face(int val, int band_h, bool selected)
{
    uint32_t *s = surf_alloc(CW, band_h);
    surf_fill(s, CW, band_h, COL_CARD);
    surf_border(s, CW, band_h, selected ? COL_SEL : COL_BLACK);
    if (band_h >= 8) surf_round_corners(s, CW, band_h);

    uint32_t col = IS_RED(val) ? COL_RED : COL_BLACK;
    stamp_index(s, CW, band_h, 4, 4, val, col, false);

    if (band_h == CH)   /* carta di coda: indice opposto + grande seme */
    {
        stamp_index(s, CW, CH, CW - 15, CH - 19, val, col, true);
        const Sprite *big = &spr_suit_big[S(val)];
        surf_stamp(s, CW, CH, CW / 2 - big->w, CH / 2 - big->h,
                   big, col, 2, false);
    }
    return s;
}

/* Compone la surface (o banda) di una carta coperta: dorso. */
static uint32_t *compose_back(int band_h)
{
    uint32_t *s = surf_alloc(CW, band_h);
    surf_fill(s, CW, band_h, COL_BACK);
    surf_border(s, CW, band_h, COL_BLACK);
    if (band_h >= 8) surf_round_corners(s, CW, band_h);
    for (int y = 3; y < band_h - 3; y += 3)
        for (int x = 4; x < CW - 4; x += 3)
            px_set(s, CW, band_h, x, y, COL_BACK_IN);
    return s;
}

/* ================================================================= *
 *  MATRIOSKE — materializzazione del layer visivo
 * ================================================================= */

/* Libera la surface di un oggetto-carta e lo riporta a puro stato logico. */
static void card_release(int ci)
{
    if (deck[ci].surf) { free(deck[ci].surf); deck[ci].surf = NULL; }
    deck[ci].sig = SIG_OCCLUDED;
}

/* Firma di cio' che una carta deve raffigurare, dati banda/scoperta/selezione. */
static uint16_t face_sig(int band_h, bool face_up, bool selected)
{
    return (uint16_t)((band_h << 2) | (face_up ? 2 : 0) | (selected ? 1 : 0));
}

/* Garantisce che la surface della carta combaci con cio' che deve mostrare.
 * Ricompone (nuovo buffer => la cache di blit ricarica) solo se cambia. */
static void card_materialize(int ci, int band_h, bool selected)
{
    Card *c = &deck[ci];
    uint16_t want = face_sig(band_h, c->face_up, selected);
    if (c->surf && c->sig == want) return;
    if (c->surf) free(c->surf);
    c->surf = c->face_up ? compose_face(c->val, band_h, selected)
                         : compose_back(band_h);
    c->sig  = want;
}

/* Banda effettiva di una carta tenendo conto del drag: la carta appena sopra
 * la run sollevata (hide-1) diventa la nuova coda e va mostrata piena (CH). */
static int eff_band(int c, int pos, int hide)
{
    if (hide >= 0 && pos == hide - 1) return CH;
    if (pos == pile_n[c] - 1)         return CH;
    return deck[pile[c][pos]].face_up ? DY_UP : DY_DOWN;
}

/* Cammina una colonna e materializza SOLO le carte con banda esposta;
 * le carte totalmente occluse restano senza surface (solo logiche).
 * hide_from: prima carta sollevata dal drag (da non disegnare in colonna). */
static void layer_materialize_col(int c, int hide_from)
{
    for (int pos = 0; pos < pile_n[c]; pos++)
    {
        int ci = pile[c][pos];
        if (hide_from >= 0 && pos >= hide_from) { card_release(ci); continue; }
        card_materialize(ci, eff_band(c, pos, hide_from), false);
    }
}

/* ================================================================= *
 *  MATRIOSKE — semina del mazzo, sollevamento run, scoperta
 * ================================================================= */

/* Costruisce il mazzo secondo la difficolta': DUE copie (i due mazzi) dei
 * soli semi in gioco.  Meno semi = meno carte (26/52/104), NON cloni per
 * riempire 104.  Le run da completare scalano: 2 per seme in gioco. */
static void deck_seed(int suits)
{
    /* semi scelti per contrasto: 1=picche, 2=picche+cuori, 4=tutti */
    static const int pick1[] = { 3 };
    static const int pick2[] = { 3, 0 };
    static const int pick4[] = { 0, 1, 2, 3 };
    const int *pick = (suits == 1) ? pick1 : (suits == 2) ? pick2 : pick4;

    uint8_t vals[NCARDS];
    int n = 0;
    for (int si = 0; si < suits; si++)
        for (int cpy = 0; cpy < 2; cpy++)       /* due mazzi, sempre */
            for (int r = 0; r < 13; r++)
                vals[n++] = (uint8_t)(pick[si] * 13 + r);

    for (int i = n - 1; i > 0; i--)             /* Fisher-Yates */
    {
        int j = (int)(rng() % (uint32_t)(i + 1));
        uint8_t t = vals[i]; vals[i] = vals[j]; vals[j] = t;
    }
    for (int i = 0; i < n; i++)
    {
        deck[i].val = vals[i];
        deck[i].face_up = 0;
    }
    n_cards  = n;
    win_runs = 2 * suits;                        /* 2 / 4 / 8 run */
}

/* Una run [from..fine] della colonna e' sollevabile se e' tutta scoperta e
 * discendente per rango.  Il SEME e' libero: si puo' sollevare una sequenza
 * discendente anche a semi misti (le cataste distribuite dal mazzo). */
static bool run_is_movable(int c, int from)
{
    if (from < 0 || from >= pile_n[c]) return false;
    if (!deck[pile[c][from]].face_up) return false;
    for (int p = from; p < pile_n[c] - 1; p++)
    {
        int a = deck[pile[c][p]].val, b = deck[pile[c][p + 1]].val;
        if (R(a) != R(b) + 1) return false;
    }
    return true;
}

/* Si puo' appoggiare la carta `val` in cima alla colonna `t`?  Colonna vuota
 * accetta qualunque carta; altrimenti serve STESSO SEME e rango esattamente
 * +1 (niente impilamento fra semi diversi). */
static bool can_drop(int val, int t)
{
    if (pile_n[t] == 0) return true;
    int top = deck[pile_top(t)].val;
    return S(top) == S(val) && R(top) == R(val) + 1;
}

/* Se la nuova coda della colonna e' coperta, scoprila. */
static void col_reveal(int c)
{
    if (pile_n[c] > 0 && !deck[pile[c][pile_n[c] - 1]].face_up)
        deck[pile[c][pile_n[c] - 1]].face_up = 1;
}

/* Rimuove una run completa K..A dello stesso seme dal fondo della colonna. */
static void col_collect_complete(int c)
{
    if (pile_n[c] < 13) return;
    int base = pile_n[c] - 13;
    for (int i = 0; i < 13; i++)
    {
        int v = deck[pile[c][base + i]].val;
        if (!deck[pile[c][base + i]].face_up) return;
        if (R(v) != 12 - i) return;                 /* K,Q,...,A */
        if (S(v) != S(deck[pile[c][base]].val)) return;
    }
    for (int i = 0; i < 13; i++) card_release(pile[c][base + i]);
    found_suit[found_n++] = (uint8_t)S(deck[pile[c][base]].val);
    pile_n[c] = base;
    col_reveal(c);
}

/* Costruisce la surface fluttuante del drag: la run impilata verticalmente. */
static void drag_build_surface(int c, int from)
{
    int n = pile_n[c] - from;
    int h = CH + (n - 1) * DY_UP;
    drag.surf = surf_alloc(CW, h);
    surf_fill(drag.surf, CW, h, KEY_TRANSP);
    for (int i = 0; i < n; i++)
    {
        int val = deck[pile[c][from + i]].val;
        int bh  = (i == n - 1) ? CH : DY_UP;
        uint32_t *face = compose_face(val, bh, false);
        for (int y = 0; y < bh; y++)
            memcpy(&drag.surf[(i * DY_UP + y) * CW], &face[y * CW],
                   (size_t)CW * sizeof(uint32_t));
        free(face);
    }
    drag.sw = CW; drag.sh = h; drag.n = n;
}

/* ================================================================= *
 *  FUNZIONI LOGICHE — render della scena
 * ================================================================= */

/* Garantisce lo scene-buffer della dimensione della finestra. */
static void scene_ensure(void)
{
    if (g_scene && g_scene_w == win_w && g_scene_h == win_h) return;
    if (g_scene) free(g_scene);
    g_scene_w = win_w; g_scene_h = win_h;
    g_scene = surf_alloc(g_scene_w, g_scene_h);
}

/* Compone una colonna nello scene: materializza il layer visibile e stampa
 * le bande esposte (le carte occluse non producono pixel). */
static void draw_column(int c)
{
    int hide = (drag.active && drag.src == c) ? drag.from : -1;
    layer_materialize_col(c, hide);

    int visible_n = (hide >= 0) ? hide : pile_n[c];
    if (visible_n == 0) { scene_slot(col_x(c), TAB_Y); return; }

    for (int pos = 0; pos < visible_n; pos++)
    {
        int ci = pile[c][pos];
        scene_stamp(col_x(c), card_y(c, pos), deck[ci].surf, CW, eff_band(c, pos, hide));
    }
}

static void draw_top_row(void)
{
    for (int i = 0; i < win_runs; i++)   /* run completate (2/4/8) */
    {
        int x = col_x(i);
        if (i < found_n)
        {
            uint32_t *f = compose_face(found_suit[i] * 13 + 12, CH, false); /* K */
            scene_stamp(x, TOP_Y, f, CW, CH);
            free(f);
        }
        else scene_slot(x, TOP_Y);
    }

    int wx = col_x(8);                 /* scarti: carta sfogliata scoperta */
    int wtop = pile_n[WASTE] - 1;
    if (drag.active && drag.src == WASTE) wtop--;   /* la cima sollevata */
    if (wtop >= 0)
    {
        uint32_t *f = compose_face(deck[pile[WASTE][wtop]].val, CH, false);
        scene_stamp(wx, TOP_Y, f, CW, CH);
        free(f);
    }
    else scene_slot(wx, TOP_Y);

    int sx = col_x(9);                 /* tallone coperto (o slot = ricicla) */
    if (pile_n[STOCK] > 0)
    {
        int depth = 1 + pile_n[STOCK] / 13;
        if (depth > 4) depth = 4;
        uint32_t *b = compose_back(CH);
        for (int d = depth - 1; d >= 0; d--) scene_stamp(sx - d * 3, TOP_Y, b, CW, CH);
        free(b);
    }
    else scene_slot(sx, TOP_Y);
}

static void draw_scene(void)
{
    scene_ensure();
    surf_fill(g_scene, g_scene_w, g_scene_h, COL_BG);   /* verde tavolo */

    draw_top_row();
    for (int c = 0; c < NCOL; c++) draw_column(c);

    if (drag.active)                                    /* layer vivo in cima */
        scene_stamp(drag.x, drag.y, drag.surf, drag.sw, drag.sh);

    scene_present();                                    /* un solo blit */

    if (game_won)                                       /* overlay sopra la scena */
    {
        int bw = 260, bh = 44, bx = (win_w - bw) / 2, by = (win_h - bh) / 2;
        dobui_FillRect(win_id, bx, by, bw, bh, COL_OVER);
        dobui_DrawRect(win_id, bx, by, bw, bh, COL_WIN);
        dobui_DrawText(win_id, bx + 28, by + 15, "HAI VINTO! Complimenti!", COL_WIN, COL_OVER);
    }
    dobui_Invalidate(win_id);
}

/* ================================================================= *
 *  FUNZIONI LOGICHE — hit-test e flusso di gioco
 * ================================================================= */

/* Sotto il cursore c'e' il tallone? */
static bool hit_stock(int mx, int my)
{
    int sx = col_x(9);
    return mx >= sx - 12 && mx < sx + CW && my >= TOP_Y && my < TOP_Y + CH;
}

/* Sotto il cursore c'e' la carta in cima agli scarti? */
static bool hit_waste(int mx, int my)
{
    int wx = col_x(8);
    return pile_n[WASTE] > 0 && mx >= wx && mx < wx + CW
        && my >= TOP_Y && my < TOP_Y + CH;
}

/* Colonna e carta sotto il cursore (idx = prima carta scoperta afferrabile). */
static void hit_column(int mx, int my, int *out_c, int *out_pos)
{
    *out_c = -1; *out_pos = -1;
    for (int c = 0; c < NCOL; c++)
    {
        int x = col_x(c);
        if (mx < x || mx >= x + CW) continue;
        if (pile_n[c] == 0) return;
        for (int pos = pile_n[c] - 1; pos >= 0; pos--)
        {
            int y = card_y(c, pos);
            int bh = card_band(c, pos);
            if (my >= y && my < y + bh) { *out_c = c; *out_pos = pos; return; }
        }
        return;
    }
}

/* Sfoglia: scopre la carta in cima al tallone e la posa sugli scarti. */
static void stock_flip(void)
{
    if (pile_n[STOCK] == 0) return;
    int ci = pile[STOCK][--pile_n[STOCK]];
    deck[ci].face_up = 1;
    pile_push(WASTE, ci);
}

/* Ricicla: tallone esaurito -> gli scarti tornano coperti nel tallone,
 * nello stesso ordine, cosi' si puo' risfogliare da capo. */
static void stock_recycle(void)
{
    while (pile_n[WASTE] > 0)
    {
        int ci = pile[WASTE][--pile_n[WASTE]];
        deck[ci].face_up = 0;
        pile_push(STOCK, ci);
    }
}

/* Nuova partita con la difficolta' corrente. */
static void new_game(void)
{
    get_random(&rng_state, sizeof(rng_state));
    for (int i = 0; i < NCARDS; i++) card_release(i);
    for (int p = 0; p < NPILE; p++) pile_n[p] = 0;
    found_n = 0;
    drag.active = false;
    game_won = false;

    deck_seed(n_suits);                     /* imposta n_cards e win_runs */

    /* Distribuzione proporzionale al mazzo: ~52% sul tableau (per 104 =>
     * 54 => 4 colonne da 6 e 6 da 5, identico a prima), il resto al tallone.
     * Le colonne 0..extra-1 ricevono una carta in piu'; la coda e' scoperta. */
    int tab  = n_cards * 52 / 100;
    if (tab < NCOL) tab = (n_cards >= NCOL) ? NCOL : n_cards;
    int base = tab / NCOL, extra = tab % NCOL;

    int di = 0;
    for (int c = 0; c < NCOL; c++)
    {
        int cnt = base + (c < extra ? 1 : 0);
        for (int r = 0; r < cnt; r++) pile_push(c, di++);
        if (cnt > 0) deck[pile[c][pile_n[c] - 1]].face_up = 1;
    }
    while (di < n_cards) pile_push(STOCK, di++);  /* resto nel tallone */
}

/* Vittoria: tutte le run del mazzo completate (2 / 4 / 8). */
static void check_win(void) { if (found_n >= win_runs) game_won = true; }

/* --- Il drag come promozione/retrocessione di layer --- */

/* Ancora a schermo di una carta, tableau o scarti. */
static void pile_card_xy(int p, int pos, int *ox, int *oy)
{
    if (p == WASTE) { *ox = col_x(8); *oy = TOP_Y; }
    else            { *ox = col_x(p); *oy = card_y(p, pos); }
}

static void drag_begin(int p, int pos, int mx, int my)
{
    int ox, oy; pile_card_xy(p, pos, &ox, &oy);
    drag.active  = true;
    drag.src     = p;
    drag.from    = pos;
    drag.grab_dx = mx - ox;
    drag.grab_dy = my - oy;
    drag.x = ox;
    drag.y = oy;
    drag_build_surface(p, pos);
    dobui_SetCursor(win_id, CURSOR_DEFAULT);
}

static void drag_cancel(void)
{
    if (drag.surf) { free(drag.surf); drag.surf = NULL; }
    drag.active = false;
}

static void drag_drop(int mx, int my)
{
    int tc, tp; hit_column(mx, my, &tc, &tp);
    if (tc < 0)                                  /* fuori: verifica per colonna vuota */
        for (int c = 0; c < NCOL; c++)
        {
            int x = col_x(c);
            if (mx >= x && mx < x + CW) { tc = c; break; }
        }

    int top_val = deck[pile[drag.src][drag.from]].val;
    if (tc >= 0 && tc != drag.src && can_drop(top_val, tc))
    {
        int n = pile_n[drag.src] - drag.from;
        for (int i = 0; i < n; i++) pile_push(tc, pile[drag.src][drag.from + i]);
        pile_n[drag.src] = drag.from;
        if (drag.src < NCOL) col_reveal(drag.src);
        col_collect_complete(tc);
        check_win();
    }
    drag_cancel();
}

/* ================================================================= *
 *  FUNZIONI LOGICHE — handler d'evento
 * ================================================================= */

void event_mouseclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (game_won) return;

    if (hit_stock(x, y))                      /* sfoglia una carta, o ricicla */
    {
        if (pile_n[STOCK] > 0) stock_flip();
        else                   stock_recycle();
        draw_scene();
        return;
    }

    if (hit_waste(x, y))                       /* prendi la carta sfogliata */
    {
        drag_begin(WASTE, pile_n[WASTE] - 1, x, y);
        draw_scene();
        return;
    }

    int c, pos; hit_column(x, y, &c, &pos);    /* solleva una run dal tableau */
    if (c >= 0 && deck[pile[c][pos]].face_up && run_is_movable(c, pos))
    {
        drag_begin(c, pos, x, y);
        draw_scene();
    }
}

void event_mousemove(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (!drag.active) return;
    drag.x = x - drag.grab_dx;
    drag.y = y - drag.grab_dy;
    draw_scene();
}

void event_mouserelease(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (!drag.active) return;
    drag_drop(x, y);
    draw_scene();
}

void event_panel(int cmd_idx)
{
    switch (cmd_idx)
    {
        case 0: n_suits = 1; new_game(); break;
        case 1: n_suits = 2; new_game(); break;
        case 2: n_suits = 4; new_game(); break;
        case 3: new_game(); break;
        default: return;
    }
    draw_scene();
}

void event_resize(int w, int h) { win_w = w; win_h = h; draw_scene(); }

void event_start(void)
{
    win_id = dobui_window();
    draw_scene();
}

/* ================================================================= *
 *  main — orchestrazione (ultima, per il vincolo del C)
 * ================================================================= */

int main(void)
{
    atlas_build_suits();
    atlas_build_glyphs();
    new_game();
    dobui_set_panel("1 seme\n2 semi\n4 semi\nNuova partita");
    dobui_run("Spider", win_w, win_h);
    return 0;
}
