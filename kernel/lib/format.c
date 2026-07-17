#include "lib/format.h"

/* Semantica osservata dall'1.0 (lib/printf.c, format_output) e
 * replicata: stessi specificatori, stesso padding, stessa resa di
 * "(null)" e di %p (0x + 8 esadecimali zero-padded). L'unica modifica
 * e' strutturale: nessun riferimento a VGA o seriale qui dentro. */

/* === Verbi di emissione ================================================ */

static void emit_string(putchar_fn put, void *ctx, const char *s)
{
    if (s == NULL)
    {
        s = "(null)";
    }
    while (*s)
    {
        put(*s++, ctx);
    }
}

static void emit_uint(putchar_fn put, void *ctx, uint32_t val, uint32_t base,
                      bool upper, int min_width, char pad_char)
{
    char buf[32];
    int pos = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (val == 0)
    {
        buf[pos++] = '0';
    }
    else
    {
        while (val > 0)
        {
            buf[pos++] = digits[val % base];
            val /= base;
        }
    }

    while (pos < min_width)
    {
        buf[pos++] = pad_char;
    }

    for (int i = pos - 1; i >= 0; i--)
    {
        put(buf[i], ctx);
    }
}

static void emit_int(putchar_fn put, void *ctx, int32_t val,
                     int min_width, char pad_char)
{
    if (val < 0)
    {
        put('-', ctx);
        /* Cast a unsigned PRIMA della negazione: evita UB su INT32_MIN
         * (-2147483648 -> 2147483648 come uint32_t, magnitudine esatta). */
        emit_uint(put, ctx, -(uint32_t)val, 10, false,
                  min_width ? min_width - 1 : 0, pad_char);
    }
    else
    {
        emit_uint(put, ctx, (uint32_t)val, 10, false, min_width, pad_char);
    }
}

/* === Motore ============================================================ */

void format_output(putchar_fn put, void *ctx, const char *fmt, va_list args)
{
    while (*fmt)
    {
        if (*fmt != '%')
        {
            put(*fmt++, ctx);
            continue;
        }
        fmt++;                              /* salta '%' */

        char pad_char = ' ';
        int min_width = 0;

        if (*fmt == '0')
        {
            pad_char = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
        {
            min_width = min_width * 10 + (*fmt - '0');
            fmt++;
        }

        switch (*fmt)
        {
            case 'd':
            case 'i':
                emit_int(put, ctx, va_arg(args, int32_t), min_width, pad_char);
                break;
            case 'u':
                emit_uint(put, ctx, va_arg(args, uint32_t), 10, false,
                          min_width, pad_char);
                break;
            case 'x':
                emit_uint(put, ctx, va_arg(args, uint32_t), 16, false,
                          min_width, pad_char);
                break;
            case 'X':
                emit_uint(put, ctx, va_arg(args, uint32_t), 16, true,
                          min_width, pad_char);
                break;
            case 'p':
                emit_string(put, ctx, "0x");
                emit_uint(put, ctx, (uint32_t)va_arg(args, void *), 16,
                          false, 8, '0');
                break;
            case 's':
                emit_string(put, ctx, va_arg(args, const char *));
                break;
            case 'c':
                put((char)va_arg(args, int), ctx);
                break;
            case '%':
                put('%', ctx);
                break;
            case '\0':
                return;                     /* '%' a fine stringa: stop */
            default:
                put('%', ctx);              /* specificatore ignoto: */
                put(*fmt, ctx);             /* eco letterale (come 1.0) */
                break;
        }
        fmt++;
    }
}

/* === Sink su buffer ==================================================== */

typedef struct
{
    char   *buf;
    size_t  size;
    size_t  pos;
} buffer_sink_t;

static void buffer_put(char c, void *ctx)
{
    buffer_sink_t *s = (buffer_sink_t *)ctx;
    if (s->pos < s->size - 1)
    {
        s->buf[s->pos] = c;
    }
    s->pos++;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    if (size == 0 || buf == NULL)
    {
        return 0;
    }

    buffer_sink_t sink = { .buf = buf, .size = size, .pos = 0 };
    format_output(buffer_put, &sink, fmt, args);

    size_t end = (sink.pos < size - 1) ? sink.pos : size - 1;
    buf[end] = '\0';

    return (int)sink.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return ret;
}
