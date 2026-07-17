/* MainDOB Solitario — Klondike Solitaire
 *
 * Classic single-deck solitaire. 7 tableau columns, 4 foundations,
 * stock + waste. Click to move cards. Auto-foundation on double click.
 *
 * Controls:
 *   Left click    = select card / place card / deal from stock
 *   Panel: "Nuova partita" = restart
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <app.h>
#include <DobInterface.h>
#include <dob/types.h>

/* Window */
static uint32_t win_id;
static int win_w = 580, win_h = 500;

/* Colors */
#define COL_BG      0x00006622  /* Dark green baize */
#define COL_CARD    0x00FFFFFF  /* White card face */
#define COL_BACK    0x00882200  /* Brown card back */
#define COL_EMPTY   0x00004411  /* Empty slot outline */
#define COL_RED     0x000000CC  /* Hearts/Diamonds (BGR) */
#define COL_BLACK   0x00111111  /* Clubs/Spades */
#define COL_SEL     0x0000FFFF  /* Yellow selection highlight */
#define COL_TEXT    0x00FFFFFF
#define COL_HDR     0x00003311
#define COL_WIN     0x0000DDFF

/* Card dimensions */
#define CW          58  /* Card width */
#define CH          78  /* Card height */
#define GAP         8   /* Gap between piles */
#define TOP_Y       8   /* Top row Y */
#define TAB_Y       100 /* Tableau Y start */
#define FACE_DOWN_DY 4  /* Vertical offset for face-down cards */
#define FACE_UP_DY   18 /* Vertical offset for face-up cards */
#define HEADER_H     0  /* No header bar — use panel */

/* Card encoding: 0-51. suit = card/13, rank = card%13 (0=A,1=2,...12=K).
 * NONE = 255. */
#define NONE        255
#define SUIT(c)     ((c) / 13)
#define RANK(c)     ((c) % 13)
#define IS_RED(c)   (SUIT(c) == 0 || SUIT(c) == 1)  /* Hearts=0, Diamonds=1 */

static const char *suit_ch[] = { "\x03", "\x04", "\x05", "\x06" }; /* ♥♦♣♠ as chars */
static const char *rank_ch[] = { "A","2","3","4","5","6","7","8","9","10","J","Q","K" };

/* Game state */
#define MAX_TAB     20  /* Max cards in a tableau column */

static uint8_t tableau[7][MAX_TAB]; /* Cards in each column */
static int     tab_count[7];        /* Total cards per column */
static int     tab_face_up[7];      /* Index where face-up starts */

static uint8_t foundation[4];       /* Top card of each foundation (NONE=empty) */

static uint8_t stock[24];           /* Remaining deck */
static int     stock_count;
static uint8_t waste[24];           /* Dealt cards */
static int     waste_count;

/* Selection */
static int sel_pile = -1;   /* -1=none, 0-6=tableau, 7=waste, 8-11=foundation */
static int sel_index = -1;  /* Card index within pile */

/* RNG */
static uint32_t rng_state;
static uint32_t rng(void)
{
    rng_state = rng_state * 1103515245 + 12345;
    return (rng_state >> 16) & 0x7FFF;
}

static bool game_won = false;

/* Game logic */

static void shuffle_deck(uint8_t *deck)
{
    for (int i = 51; i > 0; i--)
    {
        int j = (int)(rng() % (uint32_t)(i + 1));
        uint8_t tmp = deck[i];
        deck[i] = deck[j];
        deck[j] = tmp;
    }
}

static void new_game(void)
{
    get_random(&rng_state, sizeof(rng_state));

    uint8_t deck[52];
    for (int i = 0; i < 52; i++) deck[i] = (uint8_t)i;
    shuffle_deck(deck);

    int di = 0;

    /* Deal tableau: column i gets i+1 cards, last one face-up */
    for (int c = 0; c < 7; c++)
    {
        tab_count[c] = c + 1;
        tab_face_up[c] = c;
        for (int r = 0; r <= c; r++)
            tableau[c][r] = deck[di++];
    }

    /* Rest goes to stock */
    stock_count = 0;
    for (int i = di; i < 52; i++)
        stock[stock_count++] = deck[i];

    waste_count = 0;
    for (int i = 0; i < 4; i++) foundation[i] = NONE;

    sel_pile = -1;
    game_won = false;
}

static bool can_place_foundation(uint8_t card, int fi)
{
    if (card == NONE) return false;
    if (SUIT(card) != (uint8_t)fi) return false;
    if (foundation[fi] == NONE) return RANK(card) == 0;  /* Only Ace */
    return RANK(card) == RANK(foundation[fi]) + 1;
}

static bool can_place_tableau(uint8_t card, int col)
{
    if (tab_count[col] == 0) return RANK(card) == 12;  /* Only King on empty */
    uint8_t top = tableau[col][tab_count[col] - 1];
    /* Must alternate color and descend */
    return IS_RED(card) != IS_RED(top) && RANK(card) == RANK(top) - 1;
}

static void deal_from_stock(void)
{
    if (stock_count > 0)
    {
        waste[waste_count++] = stock[--stock_count];
    }
    else
    {
        /* Flip waste back to stock */
        while (waste_count > 0)
            stock[stock_count++] = waste[--waste_count];
    }
}

static void check_win(void)
{
    for (int fi = 0; fi < 4; fi++)
        if (foundation[fi] == NONE || RANK(foundation[fi]) != 12) return;
    game_won = true;
}

/* Drawing */

static int pile_x(int col)
{
    return GAP + col * (CW + GAP);
}

static void draw_card_face(int x, int y, uint8_t card, bool selected)
{
    dobui_FillRect(win_id, x, y, CW, CH, COL_CARD);
    if (selected)
        dobui_DrawRect(win_id, x, y, CW, CH, COL_SEL);
    else
        dobui_DrawRect(win_id, x, y, CW, CH, COL_BLACK);

    uint32_t col = IS_RED(card) ? COL_RED : COL_BLACK;
    char label[4];
    sprintf(label, "%s", rank_ch[RANK(card)]);
    dobui_DrawText(win_id, x + 3, y + 3, label, col, COL_CARD);

    /* Suit symbol at center */
    const char *s = suit_ch[SUIT(card)];
    dobui_DrawText(win_id, x + CW/2 - 4, y + CH/2 - 8, s, col, COL_CARD);

    /* Bottom-right rank */
    dobui_DrawText(win_id, x + CW - 18, y + CH - 16, label, col, COL_CARD);
}

static void draw_card_back(int x, int y)
{
    dobui_FillRect(win_id, x, y, CW, CH, COL_BACK);
    dobui_DrawRect(win_id, x, y, CW, CH, COL_BLACK);
    /* Cross-hatch pattern via inner rect */
    dobui_DrawRect(win_id, x + 4, y + 4, CW - 8, CH - 8, 0x00AA4400);
}

static void draw_empty_slot(int x, int y)
{
    dobui_DrawRect(win_id, x, y, CW, CH, COL_EMPTY);
    dobui_DrawRect(win_id, x + 1, y + 1, CW - 2, CH - 2, COL_EMPTY);
}

static void draw_all(void)
{
    dobui_FillRect(win_id, 0, 0, win_w, win_h, COL_BG);

    /* Top row: Stock, Waste, [gap], Foundation 0-3 */
    int sx = pile_x(0);

    /* Stock */
    if (stock_count > 0)
        draw_card_back(sx, TOP_Y);
    else
        draw_empty_slot(sx, TOP_Y);

    /* Waste */
    int wx = pile_x(1);
    if (waste_count > 0)
    {
        bool ws = (sel_pile == 7);
        draw_card_face(wx, TOP_Y, waste[waste_count - 1], ws);
    }
    else
        draw_empty_slot(wx, TOP_Y);

    /* Foundations */
    for (int fi = 0; fi < 4; fi++)
    {
        int fx = pile_x(3 + fi);
        if (foundation[fi] != NONE)
        {
            bool fs = (sel_pile == 8 + fi);
            draw_card_face(fx, TOP_Y, foundation[fi], fs);
        }
        else
        {
            draw_empty_slot(fx, TOP_Y);
            /* Suit hint */
            dobui_DrawText(win_id, fx + CW/2 - 4, TOP_Y + CH/2 - 8,
                           suit_ch[fi], COL_EMPTY, COL_BG);
        }
    }

    /* Tableau */
    for (int c = 0; c < 7; c++)
    {
        int x = pile_x(c);
        if (tab_count[c] == 0)
        {
            draw_empty_slot(x, TAB_Y);
            continue;
        }

        for (int r = 0; r < tab_count[c]; r++)
        {
            int dy = (r < tab_face_up[c]) ? FACE_DOWN_DY : FACE_UP_DY;
            int y = TAB_Y + r * dy;
            /* Adjust: first card has no offset predecessor */
            if (r == 0) y = TAB_Y;
            else
            {
                y = TAB_Y;
                for (int k = 0; k < r; k++)
                    y += (k < tab_face_up[c]) ? FACE_DOWN_DY : FACE_UP_DY;
            }

            if (r < tab_face_up[c])
                draw_card_back(x, y);
            else
            {
                bool s = (sel_pile == c && sel_index <= r);
                draw_card_face(x, y, tableau[c][r], s);
            }
        }
    }

    /* Win overlay */
    if (game_won)
    {
        int bw = 240, bh = 40;
        int bx = (win_w - bw) / 2, by = (win_h - bh) / 2;
        dobui_FillRect(win_id, bx, by, bw, bh, 0x00005500);
        dobui_DrawRect(win_id, bx, by, bw, bh, COL_WIN);
        dobui_DrawText(win_id, bx + 24, by + 12, "HAI VINTO! Complimenti!", COL_WIN, 0x00005500);
    }

    dobui_Invalidate(win_id);
}

/* Hit testing */

/* Returns: pile (0-6=tab, 7=waste, 8-11=found, 12=stock, -1=none) and card index */
static void hit_test(int mx, int my, int *out_pile, int *out_idx)
{
    *out_pile = -1;
    *out_idx = -1;

    /* Stock */
    int sx = pile_x(0);
    if (mx >= sx && mx < sx + CW && my >= TOP_Y && my < TOP_Y + CH)
    { *out_pile = 12; return; }

    /* Waste */
    int wx = pile_x(1);
    if (mx >= wx && mx < wx + CW && my >= TOP_Y && my < TOP_Y + CH && waste_count > 0)
    { *out_pile = 7; *out_idx = waste_count - 1; return; }

    /* Foundations */
    for (int fi = 0; fi < 4; fi++)
    {
        int fx = pile_x(3 + fi);
        if (mx >= fx && mx < fx + CW && my >= TOP_Y && my < TOP_Y + CH)
        { *out_pile = 8 + fi; return; }
    }

    /* Tableau — find which column and which card */
    for (int c = 0; c < 7; c++)
    {
        int x = pile_x(c);
        if (mx < x || mx >= x + CW) continue;
        if (tab_count[c] == 0)
        {
            if (my >= TAB_Y && my < TAB_Y + CH)
            { *out_pile = c; *out_idx = -1; return; }
            continue;
        }

        /* Calculate Y of each card */
        int best_r = -1;
        for (int r = 0; r < tab_count[c]; r++)
        {
            int y = TAB_Y;
            for (int k = 0; k < r; k++)
                y += (k < tab_face_up[c]) ? FACE_DOWN_DY : FACE_UP_DY;

            int card_h = (r == tab_count[c] - 1) ? CH :
                         ((r < tab_face_up[c]) ? FACE_DOWN_DY : FACE_UP_DY);
            if (my >= y && my < y + card_h)
                best_r = r;
        }
        /* Last card extends full CH */
        if (best_r < 0)
        {
            int last_y = TAB_Y;
            for (int k = 0; k < tab_count[c] - 1; k++)
                last_y += (k < tab_face_up[c]) ? FACE_DOWN_DY : FACE_UP_DY;
            if (my >= last_y && my < last_y + CH)
                best_r = tab_count[c] - 1;
        }

        if (best_r >= 0)
        {
            /* Can only pick face-up cards */
            if (best_r < tab_face_up[c]) best_r = tab_face_up[c];
            *out_pile = c;
            *out_idx = best_r;
            return;
        }
    }
}

/* Move logic */

static void handle_click(int mx, int my)
{
    int pile, idx;
    hit_test(mx, my, &pile, &idx);

    if (pile < 0) { sel_pile = -1; return; }

    /* Stock clicked: deal */
    if (pile == 12)
    {
        deal_from_stock();
        sel_pile = -1;
        draw_all();
        return;
    }

    /* Nothing selected yet — select this card */
    if (sel_pile < 0)
    {
        if (pile == 7 && waste_count > 0)
        {
            /* Try auto-foundation first */
            uint8_t card = waste[waste_count - 1];
            for (int fi = 0; fi < 4; fi++)
            {
                if (can_place_foundation(card, fi))
                {
                    foundation[fi] = card;
                    waste_count--;
                    check_win();
                    draw_all();
                    return;
                }
            }
            sel_pile = 7;
            sel_index = waste_count - 1;
        }
        else if (pile >= 0 && pile <= 6 && idx >= 0 && idx >= tab_face_up[pile])
        {
            /* Single face-up card at top? Try auto-foundation */
            if (idx == tab_count[pile] - 1)
            {
                uint8_t card = tableau[pile][idx];
                for (int fi = 0; fi < 4; fi++)
                {
                    if (can_place_foundation(card, fi))
                    {
                        foundation[fi] = card;
                        tab_count[pile]--;
                        if (tab_face_up[pile] > tab_count[pile] && tab_count[pile] > 0)
                            tab_face_up[pile] = tab_count[pile] - 1;
                        check_win();
                        draw_all();
                        return;
                    }
                }
            }
            sel_pile = pile;
            sel_index = idx;
        }
        else if (pile >= 8 && pile <= 11)
        {
            /* Foundation → select for moving back to tableau */
            if (foundation[pile - 8] != NONE)
            {
                sel_pile = pile;
                sel_index = 0;
            }
        }
        draw_all();
        return;
    }

    /* Something selected — try to place */

    /* Target is a foundation */
    if (pile >= 8 && pile <= 11)
    {
        int fi = pile - 8;
        uint8_t card = NONE;

        if (sel_pile == 7 && waste_count > 0)
            card = waste[waste_count - 1];
        else if (sel_pile >= 0 && sel_pile <= 6 && sel_index == tab_count[sel_pile] - 1)
            card = tableau[sel_pile][sel_index];

        if (card != NONE && can_place_foundation(card, fi))
        {
            foundation[fi] = card;
            if (sel_pile == 7)
                waste_count--;
            else
            {
                tab_count[sel_pile]--;
                if (tab_face_up[sel_pile] > tab_count[sel_pile] && tab_count[sel_pile] > 0)
                    tab_face_up[sel_pile] = tab_count[sel_pile] - 1;
            }
            sel_pile = -1;
            check_win();
            draw_all();
            return;
        }
    }

    /* Target is a tableau column */
    if (pile >= 0 && pile <= 6)
    {
        uint8_t moving_card = NONE;

        if (sel_pile == 7 && waste_count > 0)
            moving_card = waste[waste_count - 1];
        else if (sel_pile >= 0 && sel_pile <= 6)
            moving_card = tableau[sel_pile][sel_index];
        else if (sel_pile >= 8 && sel_pile <= 11)
            moving_card = foundation[sel_pile - 8];

        if (moving_card != NONE && can_place_tableau(moving_card, pile))
        {
            if (sel_pile == 7)
            {
                tableau[pile][tab_count[pile]++] = waste[--waste_count];
            }
            else if (sel_pile >= 8 && sel_pile <= 11)
            {
                int fi = sel_pile - 8;
                tableau[pile][tab_count[pile]++] = foundation[fi];
                /* Set foundation to card below, or NONE */
                if (RANK(foundation[fi]) > 0)
                    foundation[fi]--;
                else
                    foundation[fi] = NONE;
            }
            else if (sel_pile >= 0 && sel_pile <= 6)
            {
                /* Move stack of cards from sel_index to end */
                int n = tab_count[sel_pile] - sel_index;
                for (int i = 0; i < n; i++)
                    tableau[pile][tab_count[pile]++] = tableau[sel_pile][sel_index + i];
                tab_count[sel_pile] = sel_index;

                /* Flip new top card face-up */
                if (tab_face_up[sel_pile] > tab_count[sel_pile] && tab_count[sel_pile] > 0)
                    tab_face_up[sel_pile] = tab_count[sel_pile] - 1;
            }

            sel_pile = -1;
            draw_all();
            return;
        }
    }

    /* Clicked same card = deselect. Clicked different = reselect. */
    if (pile == sel_pile && idx == sel_index)
        sel_pile = -1;
    else
    {
        sel_pile = pile;
        sel_index = idx;
    }
    draw_all();
}

/* Event handlers */

void event_mouseclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    handle_click(x, y);
}

void event_panel(int cmd_idx)
{
    if (cmd_idx == 0)
    {
        new_game();
        draw_all();
    }
}

void event_resize(int w, int h)
{
    win_w = w;
    win_h = h;
    draw_all();
}

void event_start(void)
{
    win_id = dobui_window();
    draw_all();
}

int main(void)
{
    new_game();
    dobui_set_panel("Nuova partita");
    dobui_run("Solitario", 580, 500);
    return 0;
}
