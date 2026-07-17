/* math_helpers.c — Implementazione funzioni matematiche minime.
 *
 * Algoritmi:
 *  - sin/cos: riduzione argomento + polinomio minimax grado 9 su [-π/4, π/4]
 *  - sqrt: seed via bit-trick IEEE 754 + 3 iterazioni Newton-Raphson
 *  - pow43: cbrt via Newton + moltiplicazione
 */

#include "math_helpers.h"
#include <dob/types.h>

/* Union per bit-manipulation IEEE 754 */

typedef union
{
    float    f;
    uint32_t i;
} float_bits_t;

/*  * mh_sinf / mh_cosf — riduzione + polinomio
 *
 * Strategia ad alto livello:
 *   1. Riduci x a [0, 2π)
 *   2. Riduci a un quadrante [0, π/2] tenendo traccia di segno/complemento
 *   3. Valuta polinomio minimax su [0, π/2]
 */

/* Polinomio seno su [0, π/2]:
 * sin(x) ≈ x - x³/6 + x⁵/120 - x⁷/5040 + x⁹/362880
 * Implementato con Horner per stabilità numerica. */
static float
poly_sin_q1(float x)
{
    float x2 = x * x;
    float r;

    r = -2.7557313707e-06f;          /* -1/362880 */
    r = r * x2 + 1.9841269841e-04f;  /*  1/5040   */
    r = r * x2 - 8.3333333333e-03f;  /* -1/120    */
    r = r * x2 + 1.6666666667e-01f;  /*  1/6      */
    r = r * x2 - 1.0f;
    r = r * x2 * (-x) + x;           /* Riassemblaggio: x * (1 - ...) */
    return r;
}

/* Riduce x a [0, 2π) usando sottrazione ripetuta scalata.
 * Per |x| grande perderemmo precisione con modulo diretto. */
static float
reduce_2pi(float x)
{
    /* Gestione segno */
    int neg = 0;
    if (x < 0.0f)
    {
        x = -x;
        neg = 1;
    }

    /* Modulo 2π via moltiplicazione/troncamento */
    float k = x * (1.0f / MH_2PI);
    int   ik = (int)k;               /* troncamento verso zero */
    x = x - (float)ik * MH_2PI;

    if (neg) x = MH_2PI - x;
    return x;
}

float
mh_sinf(float x)
{
    /* Riduzione a [0, 2π) */
    x = reduce_2pi(x);

    /* Riduzione a [0, π/2] per quadrante */
    int sign = 0;
    if (x >= MH_PI)
    {
        x -= MH_PI;
        sign = 1;
    }
    if (x >= MH_PI_2)
    {
        x = MH_PI - x;
    }

    /* Valutazione */
    float r = poly_sin_q1(x);
    return sign ? -r : r;
}

float
mh_cosf(float x)
{
    /* cos(x) = sin(x + π/2) */
    return mh_sinf(x + MH_PI_2);
}

/*  * mh_sqrtf — bit trick + Newton
 *
 * Strategia ad alto livello:
 *   1. Seed magic number (rsqrt) per stima iniziale di 1/√x
 *   2. 3 iterazioni Newton-Raphson sulla funzione f(y) = 1/y² - x
 *   3. Moltiplica per x per ottenere √x
 */

float
mh_sqrtf(float x)
{
    if (x <= 0.0f) return 0.0f;

    /* Seed: formula di Quake III modificata */
    float_bits_t u;
    u.f = x;
    u.i = 0x5f3759dfu - (u.i >> 1);
    float y = u.f;                   /* ≈ 1/√x, precisione ~1e-2 */

    /* Newton per 1/√x: y_{n+1} = y_n * (1.5 - 0.5*x*y_n²) */
    float half_x = 0.5f * x;
    y = y * (1.5f - half_x * y * y);  /* ~1e-4 */
    y = y * (1.5f - half_x * y * y);  /* ~1e-8 */
    y = y * (1.5f - half_x * y * y);  /* limite macchina */

    return x * y;                    /* √x = x * (1/√x) */
}

/*  * mh_pow43f — x^(4/3) per requantization MP3
 *
 * Strategia ad alto livello:
 *   1. Calcola cbrt(x) via seed IEEE + Newton su f(y) = y³ - x
 *   2. Restituisci cbrt(x) * x (perché x^(4/3) = x^(1/3) * x)
 */

static float
cbrt_positive(float x)
{
    /* Seed: dividi esponente IEEE per 3.
     * Il bias è 127, l'esponente è nei bit 30..23.
     * exp_new = (exp_old - 127)/3 + 127 */
    float_bits_t u;
    u.f = x;

    uint32_t exp_bits = (u.i >> 23) & 0xFFu;
    int32_t  e = (int32_t)exp_bits - 127;
    int32_t  e3 = e / 3;
    uint32_t new_exp = (uint32_t)(e3 + 127) & 0xFFu;

    u.i = (u.i & 0x807FFFFFu) | (new_exp << 23);
    float y = u.f;

    /* Newton: y_{n+1} = y - (y³ - x)/(3y²) = (2y + x/y²)/3 */
    y = (2.0f * y + x / (y * y)) * (1.0f / 3.0f);
    y = (2.0f * y + x / (y * y)) * (1.0f / 3.0f);
    y = (2.0f * y + x / (y * y)) * (1.0f / 3.0f);
    y = (2.0f * y + x / (y * y)) * (1.0f / 3.0f);

    return y;
}

float
mh_pow43f(float x)
{
    if (x <= 0.0f) return 0.0f;
    return cbrt_positive(x) * x;
}
