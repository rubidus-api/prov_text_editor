/*
 * Micro-benchmark for literal byte search (src/search.c, M4.5).
 * Measures the worst case (absent pattern => full O(n*m) scan) and a present
 * pattern, at a few needle lengths, on a multi-MB buffer — the cost a single
 * incremental-search keystroke pays after the document is materialized.
 *
 * Build (from repo root):
 *   cc -O2 -std=c2x -Iinclude -Isrc -o /tmp/bench_search \
 *      bench/bench_search.c src/search.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "search.h"

static double now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

int main(void) {
    const size_t N = 8u << 20;          /* 8 MiB haystack */
    unsigned char *hay = malloc(N);
    unsigned r = 2463534242u;
    for (size_t i = 0; i < N; i++) {     /* printable letters, no needle by luck */
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        hay[i] = (unsigned char)('a' + (r % 20));   /* a..t only */
    }

    printf("haystack = %zu MiB\n", N >> 20);
    printf("%-34s %10s %10s\n", "case", "ms", "MB/s");

    const char *absent[] = { "z", "zz", "zzzzz", "zzzzzzzzzzzzzzzz" };  /* never present (no 'z') */
    for (int k = 0; k < 4; k++) {
        size_t m = strlen(absent[k]);
        bool f; double t0 = now_ns();
        proven_size_t pos = prov_search_bytes(hay, N, (const unsigned char *)absent[k], m,
                                              0, true, false, &f);
        double dt = now_ns() - t0;
        char label[64]; snprintf(label, sizeof label, "absent needle len %zu (full scan)", m);
        printf("%-34s %10.2f %10.0f  %s\n", label, dt / 1e6, (double)N / (dt / 1e3),
               (pos == PROV_SEARCH_NPOS) ? "(not found ok)" : "??");
    }

    /* present once near the very end (forward scans almost the whole buffer) */
    const char *pat = "zzzz";
    memcpy(hay + N - 4, pat, 4);
    {
        bool f; double t0 = now_ns();
        proven_size_t pos = prov_search_bytes(hay, N, (const unsigned char *)pat, 4, 0, true, false, &f);
        double dt = now_ns() - t0;
        printf("%-34s %10.2f %10.0f  @%zu\n", "present at end (near-full scan)", dt / 1e6,
               (double)N / (dt / 1e3), (size_t)pos);
    }

    free(hay);
    return 0;
}
