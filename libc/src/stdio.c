/* MainDOB libc stdio.c
 * Real printf/snprintf implementation + write/read via DobFileSystem IPC. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <dob/ipc.h>
#include <dob/registry.h>

/* ------------------------------------------------------------------ *
 * Helper di divisione intera a 64 bit.
 *
 * Su target a 32 bit (i686) il compilatore traduce `/` e `%` a 64 bit
 * in chiamate a libgcc (__udivmoddi4 e simili). MainDOB linka con
 * -nostdlib e non include libgcc, quindi forniamo noi questi helper: li
 * usano i percorsi a 64 bit e float di printf e qualunque altra
 * divisione a 64 bit del sistema.
 *
 * Dichiarati weak: se una build dovesse mai linkare libgcc, le sue
 * versioni prevalgono senza conflitto di simboli. La negazione dei
 * valori con segno passa dal dominio unsigned per evitare l'overflow
 * indefinito su INT64_MIN (il risultato e' corretto in complemento a due).
 * ------------------------------------------------------------------ */
typedef unsigned long long ull_t;
typedef signed long long   sll_t;

__attribute__((weak))
ull_t __udivmoddi4(ull_t n, ull_t d, ull_t *rem)
{
    ull_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1ull);
        if (r >= d) { r -= d; q |= (ull_t)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}

__attribute__((weak))
ull_t __udivdi3(ull_t n, ull_t d) { return __udivmoddi4(n, d, 0); }

__attribute__((weak))
ull_t __umoddi3(ull_t n, ull_t d) { ull_t r; __udivmoddi4(n, d, &r); return r; }

__attribute__((weak))
sll_t __divdi3(sll_t a, sll_t b)
{
    int neg = 0;
    ull_t ua, ub, q;
    if (a < 0) { ua = (ull_t)0 - (ull_t)a; neg ^= 1; } else ua = (ull_t)a;
    if (b < 0) { ub = (ull_t)0 - (ull_t)b; neg ^= 1; } else ub = (ull_t)b;
    q = __udivmoddi4(ua, ub, 0);
    return neg ? (sll_t)((ull_t)0 - q) : (sll_t)q;
}

__attribute__((weak))
sll_t __moddi3(sll_t a, sll_t b)
{
    int neg = (a < 0);
    ull_t ua, ub, r;
    ua = (a < 0) ? (ull_t)0 - (ull_t)a : (ull_t)a;
    ub = (b < 0) ? (ull_t)0 - (ull_t)b : (ull_t)b;
    __udivmoddi4(ua, ub, &r);
    return neg ? (sll_t)((ull_t)0 - r) : (sll_t)r;
}

/* divmod combinato con segno: quoziente troncato verso zero, il resto
 * prende il segno del dividendo (semantica C). */
__attribute__((weak))
sll_t __divmoddi4(sll_t a, sll_t b, sll_t *rem)
{
    int nq = 0, nr = (a < 0);
    ull_t ua, ub, q, r;
    if (a < 0) { ua = (ull_t)0 - (ull_t)a; nq ^= 1; } else ua = (ull_t)a;
    if (b < 0) { ub = (ull_t)0 - (ull_t)b; nq ^= 1; } else ub = (ull_t)b;
    q = __udivmoddi4(ua, ub, &r);
    if (rem) *rem = nr ? (sll_t)((ull_t)0 - r) : (sll_t)r;
    return nq ? (sll_t)((ull_t)0 - q) : (sll_t)q;
}

/*  * DobFileSystem IPC for write/read
 *
 * Port caching with automatic reconnect: if a server crashes and init
 * relaunches it, the cached port becomes dead. On IPC_ERR_DEAD, we
 * invalidate the cache and re-discover via the registry.
 */

static uint32_t dobfs_port = 0;
static uint32_t console_port = 0;

/* IPC call with reconnect on dead port */
static dob_status_t
_stdio_ipc(uint32_t *cached, const char *name, dob_msg_t *msg, dob_msg_t *reply)
{
    if (!*cached)
        *cached = dob_registry_find(name);
    if (!*cached)
        return DOB_ERR_NOT_FOUND;

    dob_status_t ret = dob_ipc_call(*cached, msg, reply);
    if (ret == DOB_ERR_DEAD || ret == DOB_ERR_NOT_FOUND)
    {
        *cached = 0;
        *cached = dob_registry_find(name);
        if (!*cached)
            return DOB_ERR_NOT_FOUND;
        memset(reply, 0, sizeof(dob_msg_t));
        ret = dob_ipc_call(*cached, msg, reply);
    }
    return ret;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO)
    {
        /* Try the console server first (proper IPC path).
         * Falls back to debug_print (direct VGA syscall) if console
         * is not registered yet (early boot) or IPC fails. */
        dob_msg_t msg = {0}, reply = {0};
        msg.code = 1;  /* CONSOLE_WRITE */
        msg.payload = (void *)buf;
        msg.payload_size = (uint32_t)count;
        if (_stdio_ipc(&console_port, "console", &msg, &reply) == DOB_OK)
            return (ssize_t)count;

        /* Fallback: direct kernel VGA output */
        char tmp[512];
        size_t n = count;
        if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        debug_print(tmp);
        return (ssize_t)n;
    }

    /* File write via DobFileSystem */
    dob_msg_t msg = {0}, reply = {0};
    msg.code = 3;  /* DOBFS WRITE */
    msg.arg0 = (uint32_t)fd;
    msg.payload = (void *)buf;
    msg.payload_size = (uint32_t)count;

    if (_stdio_ipc(&dobfs_port, "DobFileSystem", &msg, &reply) != DOB_OK)
        return -1;

    return (ssize_t)reply.arg0;
}

ssize_t read(int fd, void *buf, size_t count)
{
    /* stdin: not available (programs receive input via event handlers) */
    if (fd == STDIN_FILENO)
        return 0;

    /* File read via DobFileSystem */
    dob_msg_t msg = {0}, reply = {0};
    msg.code = 2;  /* READ */
    msg.arg0 = (uint32_t)fd;
    msg.arg1 = (uint32_t)count;

    if (_stdio_ipc(&dobfs_port, "DobFileSystem", &msg, &reply) != DOB_OK)
        return -1;

    if (reply.payload && reply.arg0 > 0)
    {
        size_t n = reply.arg0;
        if (n > count) n = count;
        memcpy(buf, reply.payload, n);
        return (ssize_t)n;
    }

    return 0;
}

int puts(const char *s)
{
    if (!s) return -1;
    write(STDOUT_FILENO, s, strlen(s));
    write(STDOUT_FILENO, "\n", 1);
    return 0;
}

/* printf engine - full format string support */

typedef struct
{
    char   *buf;
    size_t  pos;
    size_t  max;
    int     fd;     /* If >= 0, also write to fd */
} fmt_state_t;

static void fmt_putc(fmt_state_t *st, char c)
{
    if (st->buf && st->max > 0 && st->pos < st->max - 1)
        st->buf[st->pos] = c;
    st->pos++;
}

static void fmt_puts(fmt_state_t *st, const char *s)
{
    while (*s) fmt_putc(st, *s++);
}

static void fmt_putn(fmt_state_t *st, uint64_t val, int base, bool upper,
                      int width, char pad, bool neg)
{
    char digits[32];
    int  len = 0;
    const char *charset = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (val == 0)
    {
        digits[len++] = '0';
    }
    else
    {
        while (val > 0)
        {
            digits[len++] = charset[val % base];
            val /= base;
        }
    }

    /* Calculate total length including sign */
    int total = len + (neg ? 1 : 0);
    int padding = (width > total) ? width - total : 0;

    /* Left-pad with spaces or zeros */
    if (pad == '0' && neg) fmt_putc(st, '-');
    for (int i = 0; i < padding; i++) fmt_putc(st, pad);
    if (pad == ' ' && neg) fmt_putc(st, '-');

    /* Digits in reverse */
    for (int i = len - 1; i >= 0; i--)
        fmt_putc(st, digits[i]);
}

/*  * Floating-point formatting (no libm)
 *
 * Classification is done on the raw IEEE-754 bits, so nan/inf are
 * handled without any FP comparison. Finite values are formatted with
 * a scaled-integer method that rounds correctly across the range a
 * database or ordinary logging needs; a digit-by-digit fallback covers
 * very large magnitudes. Userspace has the FPU (see arch/x86/fpu.h),
 * so double arithmetic here is safe. */

static int flt_u64_digits(char *out, uint64_t v)
{
    char tmp[24];
    int  n = 0;
    if (v == 0) { out[0] = '0'; return 1; }
    while (v) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

static double flt_pow10d(int p)
{
    double r = 1.0;
    for (int i = 0; i < p; i++) r *= 10.0;
    return r;
}

static uint64_t flt_pow10u(int p)
{
    uint64_t r = 1;
    for (int i = 0; i < p && i < 19; i++) r *= 10u;
    return r;
}

/* Fixed notation of a non-negative finite magnitude, `prec` decimals.
 * Correctly rounded via scaled integer for the common range; the
 * digit-by-digit fallback handles very large magnitudes. Returns len. */
static int flt_fixed(char *out, double mag, int prec)
{
    int n = 0;

    if (prec <= 18)
    {
        uint64_t p10    = flt_pow10u(prec);
        double   scaled = mag * (double)p10;
        if (scaled < 9.2e18)                 /* fits uint64 after +0.5 */
        {
            uint64_t s  = (uint64_t)(scaled + 0.5);
            uint64_t ip = p10 ? s / p10 : s;
            uint64_t fp = p10 ? s % p10 : 0;
            n += flt_u64_digits(out + n, ip);
            if (prec > 0)
            {
                out[n++] = '.';
                char fb[24];
                int  fn = flt_u64_digits(fb, fp);
                for (int i = 0; i < prec - fn; i++) out[n++] = '0';
                for (int i = 0; i < fn; i++)        out[n++] = fb[i];
            }
            return n;
        }
    }

    /* Fallback (rare): large magnitude / precision. */
    uint64_t ip   = (mag < 1.8e19) ? (uint64_t)mag : 0;
    double   frac = mag - (double)ip;
    n += flt_u64_digits(out + n, ip);
    if (prec > 0)
    {
        out[n++] = '.';
        for (int i = 0; i < prec; i++)
        {
            frac *= 10.0;
            int d = (int)frac;
            if (d < 0) d = 0;
            if (d > 9) d = 9;
            out[n++] = (char)('0' + d);
            frac -= d;
        }
    }
    return n;
}

/* Scientific notation, `prec` mantissa decimals. Returns len. */
static int flt_sci(char *out, double mag, int prec, bool upper)
{
    int e = 0;
    if (mag > 0.0)
    {
        while (mag >= 10.0) { mag /= 10.0; e++; }
        while (mag <  1.0)  { mag *= 10.0; e--; }
    }
    /* Round mantissa to (prec+1) significant digits as an integer. */
    double   p   = flt_pow10d(prec);
    uint64_t dig = (uint64_t)(mag * p + 0.5);
    uint64_t top = flt_pow10u(prec + 1);
    if (prec + 1 <= 19 && dig >= top) { dig /= 10; e++; }

    char db[24];
    int  dn   = flt_u64_digits(db, dig);
    int  need = prec + 1;

    char full[48];
    int  fi = 0;
    for (int i = 0; i < need - dn; i++) full[fi++] = '0';
    for (int i = 0; i < dn; i++)        full[fi++] = db[i];

    int n = 0;
    out[n++] = full[0];
    if (prec > 0)
    {
        out[n++] = '.';
        for (int i = 1; i < need; i++) out[n++] = full[i];
    }
    out[n++] = upper ? 'E' : 'e';
    out[n++] = (e < 0) ? '-' : '+';

    int  ae = (e < 0) ? -e : e;
    char eb[8];
    int  en = flt_u64_digits(eb, (uint64_t)ae);
    for (int i = 0; i < 2 - en; i++) out[n++] = '0';   /* min 2 exp digits */
    for (int i = 0; i < en; i++)     out[n++] = eb[i];
    return n;
}

/* Strip trailing zeros (and a dangling '.') from a fixed string. */
static int flt_strip_fixed(char *s, int n)
{
    int dot = -1;
    for (int i = 0; i < n; i++) if (s[i] == '.') { dot = i; break; }
    if (dot < 0) return n;
    int end = n;
    while (end > dot + 1 && s[end - 1] == '0') end--;
    if (end == dot + 1) end = dot;
    return end;
}

/* General notation (%g): pick fixed vs scientific per the C rules,
 * `prec` significant digits, then strip trailing zeros. Returns len. */
static int flt_general(char *out, double mag, int prec, bool upper)
{
    if (prec < 1) prec = 1;

    int    e = 0;
    double m = mag;
    if (m > 0.0)
    {
        while (m >= 10.0) { m /= 10.0; e++; }
        while (m <  1.0)  { m *= 10.0; e--; }
    }

    int n;
    if (e < -4 || e >= prec)
    {
        n = flt_sci(out, mag, prec - 1, upper);
        /* strip trailing zeros inside the mantissa (before 'e'/'E') */
        int epos = -1;
        for (int i = 0; i < n; i++)
            if (out[i] == 'e' || out[i] == 'E') { epos = i; break; }
        if (epos > 0)
        {
            int dot = -1;
            for (int i = 0; i < epos; i++) if (out[i] == '.') { dot = i; break; }
            if (dot >= 0)
            {
                int end = epos;
                while (end > dot + 1 && out[end - 1] == '0') end--;
                if (end == dot + 1) end--;
                int shift = epos - end;
                if (shift > 0)
                {
                    for (int i = end; i < n - shift; i++) out[i] = out[i + shift];
                    n -= shift;
                }
            }
        }
    }
    else
    {
        int fprec = prec - 1 - e;
        if (fprec < 0) fprec = 0;
        n = flt_fixed(out, mag, fprec);
        n = flt_strip_fixed(out, n);
    }
    return n;
}

/* Emit a pre-built magnitude string with optional sign, honoring width.
 * '0' pad goes after the sign; ' ' pad before it. */
static void flt_emit(fmt_state_t *st, const char *mag, bool negative,
                     int width, char pad)
{
    int len   = (int)strlen(mag);
    int total = len + (negative ? 1 : 0);
    int padding = (width > total) ? width - total : 0;

    if (pad == '0')
    {
        if (negative) fmt_putc(st, '-');
        for (int i = 0; i < padding; i++) fmt_putc(st, '0');
    }
    else
    {
        for (int i = 0; i < padding; i++) fmt_putc(st, ' ');
        if (negative) fmt_putc(st, '-');
    }
    fmt_puts(st, mag);
}

static void fmt_putfloat(fmt_state_t *st, double val, char conv,
                         int prec, bool has_prec, int width, char pad)
{
    if (!has_prec) prec = 6;
    if (prec < 0)  prec = 0;
    if (prec > 18) prec = 18;               /* covers double's precision */

    bool upper = (conv >= 'A' && conv <= 'Z');
    char lo    = upper ? (char)(conv + 32) : conv;

    uint64_t bits;
    memcpy(&bits, &val, sizeof(bits));
    bool     neg  = (bits >> 63) != 0;
    uint64_t bexp = (bits >> 52) & 0x7FFu;
    uint64_t bman = bits & 0xFFFFFFFFFFFFFull;

    if (bexp == 0x7FF)                       /* inf / nan */
    {
        const char *w = bman ? (upper ? "NAN" : "nan")
                             : (upper ? "INF" : "inf");
        flt_emit(st, w, bman ? false : neg, width, ' ');
        return;
    }

    double mag = neg ? -val : val;
    char   nb[96];
    int    n;
    if (lo == 'f')      n = flt_fixed(nb, mag, prec);
    else if (lo == 'e') n = flt_sci(nb, mag, prec, upper);
    else                n = flt_general(nb, mag, prec, upper);   /* 'g' */
    nb[n] = '\0';

    flt_emit(st, nb, neg, width, pad);
}

static int fmt_core(fmt_state_t *st, const char *fmt, __builtin_va_list ap)
{
    while (*fmt)
    {
        if (*fmt != '%')
        {
            fmt_putc(st, *fmt++);
            continue;
        }
        fmt++;  /* Skip '%' */

        /* Flags */
        char pad = ' ';
        bool left_align = false;

        while (*fmt == '0' || *fmt == '-')
        {
            if (*fmt == '0') pad = '0';
            if (*fmt == '-') left_align = true;
            fmt++;
        }
        (void)left_align;  /* Not fully implemented yet */

        /* Width */
        int width = 0;
        if (*fmt == '*')
        {
            width = __builtin_va_arg(ap, int);
            fmt++;
        }
        else
        {
            while (*fmt >= '0' && *fmt <= '9')
            {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        /* Precision */
        bool has_prec = false;
        int  prec = 0;
        if (*fmt == '.')
        {
            has_prec = true;
            fmt++;
            if (*fmt == '*')
            {
                int p = __builtin_va_arg(ap, int);
                if (p < 0) has_prec = false;    /* negative => as if omitted */
                else       prec = p;
                fmt++;
            }
            else
            {
                while (*fmt >= '0' && *fmt <= '9')
                {
                    prec = prec * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        /* Length modifier. On i686 (ILP32): l = 32-bit long, ll/j =
         * 64-bit; h/hh promote to int; z/t are 32-bit. Only the 64-bit
         * case changes how the argument is read. */
        bool is64 = false;
        for (;;)
        {
            if (*fmt == 'l')
            {
                fmt++;
                if (*fmt == 'l') { is64 = true; fmt++; }
            }
            else if (*fmt == 'j') { is64 = true; fmt++; }
            else if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
            else if (*fmt == 'z' || *fmt == 't') { fmt++; }
            else break;
        }

        /* Specifier */
        switch (*fmt)
        {
            case 'd':
            case 'i':
            {
                int64_t val = is64 ? __builtin_va_arg(ap, int64_t)
                                   : (int64_t)__builtin_va_arg(ap, int32_t);
                bool neg = (val < 0);
                uint64_t uval = neg ? (0u - (uint64_t)val) : (uint64_t)val;
                fmt_putn(st, uval, 10, false, width, pad, neg);
                break;
            }
            case 'u':
            {
                uint64_t val = is64 ? __builtin_va_arg(ap, uint64_t)
                                    : (uint64_t)__builtin_va_arg(ap, uint32_t);
                fmt_putn(st, val, 10, false, width, pad, false);
                break;
            }
            case 'x':
            {
                uint64_t val = is64 ? __builtin_va_arg(ap, uint64_t)
                                    : (uint64_t)__builtin_va_arg(ap, uint32_t);
                fmt_putn(st, val, 16, false, width, pad, false);
                break;
            }
            case 'X':
            {
                uint64_t val = is64 ? __builtin_va_arg(ap, uint64_t)
                                    : (uint64_t)__builtin_va_arg(ap, uint32_t);
                fmt_putn(st, val, 16, true, width, pad, false);
                break;
            }
            case 'o':
            {
                uint64_t val = is64 ? __builtin_va_arg(ap, uint64_t)
                                    : (uint64_t)__builtin_va_arg(ap, uint32_t);
                fmt_putn(st, val, 8, false, width, pad, false);
                break;
            }
            case 'p':
            {
                uint32_t val = (uint32_t)__builtin_va_arg(ap, void *);
                fmt_puts(st, "0x");
                fmt_putn(st, val, 16, false, 8, '0', false);
                break;
            }
            case 'f': case 'F':
            case 'e': case 'E':
            case 'g': case 'G':
            {
                double val = __builtin_va_arg(ap, double);
                fmt_putfloat(st, val, *fmt, prec, has_prec, width, pad);
                break;
            }
            case 's':
            {
                const char *s = __builtin_va_arg(ap, const char *);
                if (!s) s = "(null)";
                int slen = (int)strlen(s);
                if (has_prec && prec < slen) slen = prec;   /* max chars */
                int spad = (width > slen) ? width - slen : 0;
                for (int i = 0; i < spad; i++) fmt_putc(st, ' ');
                for (int i = 0; i < slen; i++) fmt_putc(st, s[i]);
                break;
            }
            case 'c':
            {
                char c = (char)__builtin_va_arg(ap, int);
                fmt_putc(st, c);
                break;
            }
            case '%':
                fmt_putc(st, '%');
                break;
            case '\0':
                goto done;
            default:
                fmt_putc(st, '%');
                fmt_putc(st, *fmt);
                break;
        }
        fmt++;
    }
done:
    return (int)st->pos;
}

/* Public API */

int printf(const char *fmt, ...)
{
    char buf[1024];
    fmt_state_t st = { buf, 0, sizeof(buf), STDOUT_FILENO };

    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int ret = fmt_core(&st, fmt, ap);
    __builtin_va_end(ap);

    /* Null-terminate */
    size_t n = st.pos;
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    buf[n] = '\0';

    write(STDOUT_FILENO, buf, n);
    return ret;
}

int snprintf(char *buf, size_t max, const char *fmt, ...)
{
    fmt_state_t st = { buf, 0, max, -1 };

    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int ret = fmt_core(&st, fmt, ap);
    __builtin_va_end(ap);

    if (buf && max > 0)
    {
        size_t end = (st.pos < max) ? st.pos : max - 1;
        buf[end] = '\0';
    }
    return ret;
}

int sprintf(char *buf, const char *fmt, ...)
{
    fmt_state_t st = { buf, 0, 0xFFFFFFFF, -1 };

    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int ret = fmt_core(&st, fmt, ap);
    __builtin_va_end(ap);

    if (buf)
        buf[st.pos] = '\0';
    return ret;
}
