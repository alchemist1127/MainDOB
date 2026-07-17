/* math_helpers.h — Funzioni matematiche minime per decoder audio
 *
 * Compilate direttamente dentro audioplayer.mdl. Nessuna chiamata di sistema,
 * nessuna allocazione. Usate solo in fase di init dei decoder per precomputare
 * tabelle (twiddle factor MDCT, requantization LUT, ecc.).
 */

#ifndef MAINDOB_MATH_HELPERS_H
#define MAINDOB_MATH_HELPERS_H

#define MH_PI     3.14159265358979323846f
#define MH_PI_2   1.57079632679489661923f
#define MH_2PI    6.28318530717958647692f

/* Trigonometriche, precisione ~1e-6 per |x| ragionevole. */
float mh_sinf(float x);
float mh_cosf(float x);

/* Radice quadrata, precisione ~1e-6. x <= 0 -> 0. */
float mh_sqrtf(float x);

/* x^(4/3) — specifica per requantization MP3 (necessaria in hot path). */
float mh_pow43f(float x);

#endif /* MAINDOB_MATH_HELPERS_H */
