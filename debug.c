#include <stdarg.h>
#include <stdio.h>
int dlog(const char* func, const char* file, int line, const char* fmt, ...)
{
    va_list ap;
    char buf[1024];

    // va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    // va_end(ap);

    fprintf(stderr, "%s(): %s: %d: %s\n", func, file, line, buf);
    return 0;
}
