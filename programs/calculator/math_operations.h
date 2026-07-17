#ifndef MATH_OPERATIONS_H
#define MATH_OPERATIONS_H

/* Math operations for the calculator.
 *
 * Integer-only implementations (no FPU), linked directly into the
 * calculator binary. */

int math_square(int x);        /* x²                              */
int math_cube(int x);          /* x³                              */
int math_sqrt(int x);          /* √x (integer floor, 0 if x <= 0) */
int math_cos_fp(int degrees);  /* cos(x°) * 1000                  */
int math_sin_fp(int degrees);  /* sin(x°) * 1000                  */

#endif
