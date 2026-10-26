/*
 * The four memory routines the compiler is allowed to emit calls to even in
 * freestanding mode, and that the loader uses to zero BSS and page tables.
 *
 * memset/memcpy use REP STOSB/MOVSB: one instruction, and on CPUs with ERMS
 * (Ivy Bridge+, Zen 3+) microcode moves whole cache lines, so it is as fast
 * as a hand-vectorised loop without touching SSE state. On older CPUs it is
 * still correct and no slower than a byte loop.
 *
 * -ffreestanding stops clang turning the remaining loops back into calls to
 * these functions (loop-idiom recognition), which would recurse forever.
 */
#include <stddef.h>
#include <stdint.h>
#include "lib/mem.h"

void *memset(void *dst, int c, size_t n)
{
    void *d = dst;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return dst;
}

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    void *d = dst;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d <= s || d >= s + n)
        return memcpy(dst, src, n);
    while (n--)
        d[n] = s[n];
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}
