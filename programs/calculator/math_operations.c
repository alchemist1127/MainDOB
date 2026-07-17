/* Math operations for the calculator.
 *
 *   math_square(x)  → x²
 *   math_cube(x)    → x³
 *   math_sqrt(x)    → √x (integer, floor)
 *   math_cos_fp(x)  → cos(x°) * 1000
 *   math_sin_fp(x)  → sin(x°) * 1000
 *
 * All functions use integer arithmetic only (no FPU).
 * Linked directly into the calculator binary.
 */
#include "math_operations.h"

/* [0] Square: returns x * x */
int math_square(int x)
{
    return x * x;
}

/* [1] Cube: returns x * x * x */
int math_cube(int x)
{
    return x * x * x;
}

/* [2] Integer square root (Newton's method, floor).
 * Returns the largest integer n such that n*n <= x.
 * Returns 0 for negative input. */
int math_sqrt(int x)
{
    if (x <= 0) return 0;
    if (x == 1) return 1;

    unsigned int ux = (unsigned int)x;
    unsigned int guess = ux / 2;

    while (guess > 0)
    {
        unsigned int next = (guess + ux / guess) / 2;
        if (next >= guess)
        {
            break;
        }
        guess = next;
    }

    /* Ensure floor: if guess*guess > x, back down by 1 */
    if (guess * guess > ux)
    {
        guess--;
    }

    return (int)guess;
}

/* [3] Cosine in fixed-point.
 * Input: degrees (integer, e.g. 45 = 45°)
 * Output: cos(x) * 1000 (e.g. cos(60°) → 500)
 *
 * Uses a lookup table for common angles, with linear
 * interpolation between entries. Handles negative angles
 * and angles > 360°. */
static const int cos_table[] =
{
    /* cos(0°..90°) * 1000, every 5 degrees */
    1000, 996, 985, 966, 940, 906,   /*  0  5 10 15 20 25 */
     866, 819, 766, 707, 643, 574,   /* 30 35 40 45 50 55 */
     500, 423, 342, 259, 174,  87,   /* 60 65 70 75 80 85 */
       0                              /* 90 */
};

int math_cos_fp(int degrees)
{
    /* Normalize to 0..359 */
    int d = degrees % 360;
    if (d < 0) d += 360;

    int sign = 1;
    int lookup;

    if (d <= 90)
        lookup = d;
    else if (d <= 180)
    {
        lookup = 180 - d;
        sign = -1;
    }
    else if (d <= 270)
    {
        lookup = d - 180;
        sign = -1;
    }
    else
        lookup = 360 - d;

    int idx = lookup / 5;
    int rem = lookup % 5;

    int val = cos_table[idx];
    if (rem > 0 && idx < 18)
    {
        int next = cos_table[idx + 1];
        val = val + (next - val) * rem / 5;
    }

    return sign * val;
}

/* [4] Sine in fixed-point.
 * Input: degrees (integer)
 * Output: sin(x) * 1000
 * sin(x) = cos(90 - x) */
int math_sin_fp(int degrees)
{
    return math_cos_fp(90 - degrees);
}
