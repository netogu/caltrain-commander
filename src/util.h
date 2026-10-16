#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    return p;
}

static inline char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    return (char *)memcpy(xrealloc(NULL, n), s, n);
}

/* Double a dynamic array's capacity (starting at 64 elements). */
static inline void *xgrow(void *p, int *cap, size_t elem) {
    *cap = *cap ? *cap * 2 : 64;
    return xrealloc(p, (size_t)*cap * elem);
}

/* Make room for one more element: PUSH(arr, n, cap); arr[n++] = ...; */
#define PUSH(arr, n, cap)                                   \
    do {                                                    \
        if ((n) == (cap)) (arr) = xgrow((arr), &(cap), sizeof *(arr)); \
    } while (0)

#endif
