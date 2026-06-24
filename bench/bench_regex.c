/*
 * Micro-benchmark for the regex engine (RFC-0009). The headline is the
 * anti-ReDoS proof: `(a+)+$` against "a"*n + "X" forces the engine to explore
 * the whole ambiguous structure. A backtracking engine (PCRE-style) takes
 * EXPONENTIAL time here and hangs by n≈30; the Pike VM is linear.
 *
 * Build (from repo root):
 *   cc -O2 -std=c2x -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
 *      -Iinclude -Iplatform -Isrc -o /tmp/bench_regex bench/bench_regex.c \
 *      src/regex.c src/unicode.c src/proven/*.c platform/proven_sys_*.c -lm -lpthread
 */
#include <stdio.h>
#include <time.h>
#include "proven/heap.h"
#include "regex.h"

static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e9 + (double)t.tv_nsec; }

int main(void) {
    proven_allocator_t A = proven_heap_allocator();

    printf("anti-ReDoS: (a+)+$  on  a^n X   (a backtracker is exponential here)\n");
    printf("  %8s  %12s  %10s\n", "n", "total ns", "ns/byte");
    const proven_size_t ns[] = { 10, 20, 30, 50, 100, 1000, 10000, 100000 };
    for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) {
        proven_size_t n = ns[i];
        proven_u8 *hay = (proven_u8 *)A.alloc_fn(A.ctx, n + 1, 16).value.ptr;
        for (proven_size_t k = 0; k < n; k++) hay[k] = 'a';
        hay[n] = 'X';                                   /* the trailing X forces a full no-match */
        prov_result_regex_t r = prov_regex_compile(A, PROVEN_LIT("(a+)+$"), 0);
        prov_regex_match_t m;
        double t0 = now_ns();
        bool found = prov_regex_search(r.re, hay, n + 1, 0, &m);
        double t1 = now_ns();
        printf("  %8zu  %12.0f  %10.2f  (match=%s)\n", (size_t)n, t1 - t0, (t1 - t0) / (double)(n + 1), found ? "y" : "n");
        prov_regex_destroy(A, r.re);
        A.free_fn(A.ctx, hay);
    }

    /* throughput: a realistic pattern scanned over a large buffer */
    printf("\nthroughput: search  [a-z]+[0-9]  over a %d MB buffer\n", 8);
    proven_size_t big = 8u * 1024 * 1024;
    proven_u8 *buf = (proven_u8 *)A.alloc_fn(A.ctx, big, 16).value.ptr;
    for (proven_size_t k = 0; k < big; k++) buf[k] = (proven_u8)("abcd1 efgh2 ijkl3 mnop4 "[k % 24]);
    prov_result_regex_t r = prov_regex_compile(A, PROVEN_LIT("[a-z]+[0-9]"), 0);
    prov_regex_match_t m;
    double t0 = now_ns();
    proven_size_t from = 0, hits = 0;
    while (prov_regex_search(r.re, buf, big, from, &m)) { hits++; from = m.end > m.start ? m.end : m.start + 1; }
    double t1 = now_ns();
    double mbps = (double)big / ((t1 - t0) / 1e9) / 1e6;
    printf("  %zu hits in %.1f ms  =  %.0f MB/s\n", (size_t)hits, (t1 - t0) / 1e6, mbps);
    prov_regex_destroy(A, r.re);
    A.free_fn(A.ctx, buf);
    return 0;
}
