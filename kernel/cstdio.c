#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

struct fmt_out {
    char* dst;
    size_t size;
    size_t pos;
    size_t total;
};

static void fmt_putc(struct fmt_out* out, char c) {
    if (out->dst && out->size && out->pos + 1 < out->size) {
        out->dst[out->pos] = c;
    }
    out->pos++;
    out->total++;
}

static void fmt_puts(struct fmt_out* out, const char* s) {
    if (!s) s = "(null)";
    while (*s) {
        fmt_putc(out, *s);
        s++;
    }
}

static void fmt_put_uint(struct fmt_out* out, uint64_t value, unsigned base, int uppercase) {
    char tmp[32];
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    size_t n = 0;

    if (base < 2 || base > 16) return;
    if (value == 0) {
        fmt_putc(out, '0');
        return;
    }

    while (value && n < sizeof(tmp)) {
        tmp[n++] = digits[value % base];
        value /= base;
    }
    while (n) {
        fmt_putc(out, tmp[--n]);
    }
}

static void fmt_put_int(struct fmt_out* out, int64_t value) {
    uint64_t magnitude;
    if (value < 0) {
        fmt_putc(out, '-');
        magnitude = (uint64_t)(-(value + 1)) + 1U;
    } else {
        magnitude = (uint64_t)value;
    }
    fmt_put_uint(out, magnitude, 10, 0);
}

static void fmt_finish(struct fmt_out* out) {
    if (!out->dst || out->size == 0) return;
    if (out->pos < out->size) {
        out->dst[out->pos] = '\0';
    } else {
        out->dst[out->size - 1] = '\0';
    }
}

int vsnprintf(char* dst, size_t size, const char* fmt, va_list ap) {
    struct fmt_out out = { dst, size, 0, 0 };

    if (!fmt) {
        fmt_finish(&out);
        return 0;
    }

    while (*fmt) {
        int long_count = 0;
        char spec;

        if (*fmt != '%') {
            fmt_putc(&out, *fmt++);
            continue;
        }

        fmt++;
        if (*fmt == '%') {
            fmt_putc(&out, '%');
            fmt++;
            continue;
        }

        while (*fmt == 'l' && long_count < 2) {
            long_count++;
            fmt++;
        }

        spec = *fmt;
        if (!spec) {
            fmt_putc(&out, '%');
            break;
        }
        fmt++;

        switch (spec) {
            case 'c':
                fmt_putc(&out, (char)va_arg(ap, int));
                break;
            case 's':
                fmt_puts(&out, va_arg(ap, const char*));
                break;
            case 'd':
            case 'i':
                if (long_count >= 2) {
                    fmt_put_int(&out, va_arg(ap, long long));
                } else if (long_count == 1) {
                    fmt_put_int(&out, va_arg(ap, long));
                } else {
                    fmt_put_int(&out, va_arg(ap, int));
                }
                break;
            case 'u':
                if (long_count >= 2) {
                    fmt_put_uint(&out, va_arg(ap, unsigned long long), 10, 0);
                } else if (long_count == 1) {
                    fmt_put_uint(&out, va_arg(ap, unsigned long), 10, 0);
                } else {
                    fmt_put_uint(&out, va_arg(ap, unsigned int), 10, 0);
                }
                break;
            case 'x':
            case 'X':
                if (long_count >= 2) {
                    fmt_put_uint(&out, va_arg(ap, unsigned long long), 16, spec == 'X');
                } else if (long_count == 1) {
                    fmt_put_uint(&out, va_arg(ap, unsigned long), 16, spec == 'X');
                } else {
                    fmt_put_uint(&out, va_arg(ap, unsigned int), 16, spec == 'X');
                }
                break;
            case 'p':
                fmt_puts(&out, "0x");
                fmt_put_uint(&out, (uint64_t)(uintptr_t)va_arg(ap, void*), 16, 0);
                break;
            default:
                fmt_putc(&out, '%');
                for (int i = 0; i < long_count; i++) fmt_putc(&out, 'l');
                fmt_putc(&out, spec);
                break;
        }
    }

    fmt_finish(&out);
    return (int)out.total;
}

int snprintf(char* dst, size_t size, const char* fmt, ...) {
    va_list ap;
    int rc;
    va_start(ap, fmt);
    rc = vsnprintf(dst, size, fmt, ap);
    va_end(ap);
    return rc;
}
