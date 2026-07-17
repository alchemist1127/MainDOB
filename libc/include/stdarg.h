#ifndef MAINDOB_LIBC_STDARG_H
#define MAINDOB_LIBC_STDARG_H

/* Thin wrapper around GCC built-in variadic argument handling.
 * The compiler provides these intrinsics for all targets. */

typedef __builtin_va_list va_list;

#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)
#define va_end(ap)          __builtin_va_end(ap)
#define va_copy(dest, src)  __builtin_va_copy(dest, src)

#endif /* MAINDOB_LIBC_STDARG_H */
