/*
 * tg_util.c — see tg_util.h.
 */

#include "tg_util.h"

#include <string.h>

size_t tg_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);

    if (cap) {
        size_t k = n < cap - 1 ? n : cap - 1;
        memcpy(dst, src, k);
        dst[k] = 0;
    }
    return n;
}

size_t tg_strlcat(char *dst, const char *src, size_t cap)
{
    size_t d = 0;

    while (d < cap && dst[d]) d++;
    if (d == cap) return cap + strlen(src);
    return d + tg_strlcpy(dst + d, src, cap - d);
}

char *tg_utoa(unsigned long v, char *buf, size_t cap)
{
    char tmp[24];
    size_t n = 0, keep;

    if (!cap) return buf;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < sizeof tmp);

    /* A number that does not fit keeps its leading digits: wrong, but
       wrong in the direction a reader notices. */
    keep = n > cap - 1 ? cap - 1 : n;
    for (size_t i = 0; i < keep; i++) buf[i] = tmp[n - 1 - i];
    buf[keep] = 0;
    return buf;
}

char *tg_strrchr(const char *s, int c)
{
    const char *last = NULL;

    for (; ; s++) {
        if (*s == (char)c) last = s;
        if (!*s) break;
    }
    return (char *)last;
}

char *tg_itoa(long v, char *buf, size_t cap)
{
    if (!cap) return buf;
    if (v < 0) {
        buf[0] = '-';
        tg_utoa((unsigned long)(-(v + 1)) + 1u, buf + 1, cap - 1);
    } else {
        tg_utoa((unsigned long)v, buf, cap);
    }
    return buf;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

int tg_stricmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int d = lower((unsigned char)*a) - lower((unsigned char)*b);

        if (d || !*a) return d;
    }
}

int tg_ends_with_nocase(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);

    return n >= m && tg_stricmp(s + n - m, suffix) == 0;
}

const char *tg_strcasestr(const char *hay, const char *needle)
{
    size_t m = strlen(needle);

    if (!m) return hay;
    for (; *hay; hay++) {
        size_t i;

        for (i = 0; i < m; i++)
            if (lower((unsigned char)hay[i]) != lower((unsigned char)needle[i]))
                break;
        if (i == m) return hay;
    }
    return NULL;
}
