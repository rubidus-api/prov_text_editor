/*
 * Micro-benchmark for the document buffer / piece table (RFC-0005).
 * Standalone; compile -O2 and link the core + proven. Measures the edit
 * operations whose cost the container/arena work targets:
 *   - typing at end / front (front insert is the pieces-shift worst case)
 *   - editing inside a large document (rebuild_lines dominance)
 *   - delete churn, undo/redo churn (undo trim)
 * Prints wall-clock ns/op so before/after numbers are comparable.
 *
 * Build (from repo root):
 *   cc -O2 -std=c2x -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
 *      -Iinclude -Iplatform -Isrc -o /tmp/bench_buffer bench/bench_buffer.c \
 *      src/buffer.c src/editor.c src/motion.c src/unicode.c \
 *      src/proven/*.c platform/proven_sys_*.c -lm -lpthread
 */
#include <stdio.h>
#include <time.h>

#include "proven/heap.h"
#include "proven/allocator.h"
#include "editor.h"
#include "buffer.h"

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static const proven_u8 *U(const char *s) { return (const proven_u8 *)s; }

/* Build a document of `lines` lines ("lorem ipsum dolor sit amet\n"). */
static prov_editor_t *make_doc(proven_allocator_t a, int lines) {
    prov_result_editor_t r = prov_editor_create(a);
    prov_editor_t *ed = r.value;
    const char *L = "lorem ipsum dolor sit amet\n";
    for (int i = 0; i < lines; i++) prov_editor_insert(ed, U(L), 27);
    return ed;
}

int main(void) {
    proven_allocator_t a = proven_heap_allocator();
    printf("op                          ns/op     iters\n");
    printf("--------------------------------------------\n");

    /* 1. type N chars at the END (cursor follows). */
    {
        int N = 20000;
        prov_result_editor_t r = prov_editor_create(a);
        prov_editor_t *ed = r.value;
        double t0 = now_ns();
        for (int i = 0; i < N; i++) prov_editor_insert(ed, U("x"), 1);
        double dt = now_ns() - t0;
        printf("%-26s %8.1f %8d\n", "insert@end", dt / N, N);
        prov_editor_destroy(ed);
    }

    /* 2. type N chars at the FRONT (worst case: pieces shift + rebuild). */
    {
        int N = 8000;
        prov_result_editor_t r = prov_editor_create(a);
        prov_editor_t *ed = r.value;
        double t0 = now_ns();
        for (int i = 0; i < N; i++) { prov_editor_move_to(ed, 0); prov_editor_insert(ed, U("x"), 1); }
        double dt = now_ns() - t0;
        printf("%-26s %8.1f %8d\n", "insert@front", dt / N, N);
        prov_editor_destroy(ed);
    }

    /* 3. insert in the MIDDLE of a large document (rebuild_lines dominance). */
    {
        int N = 4000, lines = 4000;
        prov_editor_t *ed = make_doc(a, lines);   /* ~108 KB, 4000 pieces collapse */
        proven_size_t total = prov_buffer_byte_len(prov_editor_buffer(ed));
        double t0 = now_ns();
        for (int i = 0; i < N; i++) { prov_editor_move_to(ed, total / 2); prov_editor_insert(ed, U("z"), 1); }
        double dt = now_ns() - t0;
        printf("%-26s %8.1f %8d\n", "insert@mid(large doc)", dt / N, N);
        prov_editor_destroy(ed);
    }

    /* 4. delete churn at the front of a large document. */
    {
        int N = 4000;
        prov_editor_t *ed = make_doc(a, 4000);
        double t0 = now_ns();
        for (int i = 0; i < N; i++) { prov_editor_move_to(ed, 0); prov_editor_delete(ed); }
        double dt = now_ns() - t0;
        printf("%-26s %8.1f %8d\n", "delete@front(large doc)", dt / N, N);
        prov_editor_destroy(ed);
    }

    /* 5. undo/redo churn (exercises the undo trim past undo_limit). */
    {
        int N = 20000;
        prov_result_editor_t r = prov_editor_create(a);
        prov_editor_t *ed = r.value;
        for (int i = 0; i < N; i++) prov_editor_insert(ed, U("y"), 1);
        double t0 = now_ns();
        int u = 0;
        for (int i = 0; i < N; i++) if (prov_editor_undo(ed)) u++;
        for (int i = 0; i < N; i++) prov_editor_redo(ed);
        double dt = now_ns() - t0;
        printf("%-26s %8.1f %8d\n", "undo+redo", dt / (2.0 * N), 2 * N);
        prov_editor_destroy(ed);
    }

    /* 6. copy_range: read the whole large document repeatedly. */
    {
        int N = 2000;
        prov_editor_t *ed = make_doc(a, 4000);
        const prov_buffer_t *b = prov_editor_buffer(ed);
        proven_size_t total = prov_buffer_byte_len(b);
        static proven_u8 out[200000];
        double t0 = now_ns();
        volatile proven_size_t sink = 0;
        for (int i = 0; i < N; i++) sink += prov_buffer_copy_range(b, 0, total, out, sizeof out);
        double dt = now_ns() - t0;
        printf("%-26s %8.1f %8d\n", "copy_range(whole large)", dt / N, N);
        (void)sink;
        prov_editor_destroy(ed);
    }

    return 0;
}
