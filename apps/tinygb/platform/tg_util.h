/*
 * tg_util.h — the six string functions a freestanding build does not have.
 *
 * RetailOS links no libc: no snprintf, no strcasecmp, no strlcpy. Rather
 * than sprinkle "#ifdef RETAILOS" through the modules that want them, every
 * target uses these. They are small enough that carrying them on Linux too
 * costs nothing and keeps one behaviour everywhere.
 */

#ifndef TINYGB_UTIL_H
#define TINYGB_UTIL_H

#include <stddef.h>

/* Copy / append with a bound, always NUL-terminated when cap > 0. Return the
   length the result would have had with no bound, as the BSD ones do. */
size_t tg_strlcpy(char *dst, const char *src, size_t cap);
size_t tg_strlcat(char *dst, const char *src, size_t cap);

/* Decimal. Returns `buf`; an empty string when cap is 0. */
char *tg_utoa(unsigned long v, char *buf, size_t cap);
char *tg_itoa(long v, char *buf, size_t cap);

/* ASCII case-insensitive compare, and "does `s` end with `suffix`". */
int tg_stricmp(const char *a, const char *b);
int tg_ends_with_nocase(const char *s, const char *suffix);

/* Case-insensitive substring search; NULL when absent. */
const char *tg_strcasestr(const char *hay, const char *needle);

/* strrchr, which the freestanding string.h does not have. */
char *tg_strrchr(const char *s, int c);

#endif /* TINYGB_UTIL_H */
