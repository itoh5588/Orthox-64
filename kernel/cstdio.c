#include <stddef.h>
#include <stdarg.h>

int vsnprintf(char* dst, size_t size, const char* fmt, va_list ap) {
    (void)fmt;
    (void)ap;
    if (dst && size) dst[0] = '\0';
    return 0;
}

int snprintf(char* dst, size_t size, const char* fmt, ...) {
    va_list ap;
    int rc;
    va_start(ap, fmt);
    rc = vsnprintf(dst, size, fmt, ap);
    va_end(ap);
    return rc;
}
