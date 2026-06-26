/* RFC-0010 S1 — panel core model tests (filter / select / goto). */

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"
#include "panel.h"

static int g_fail;
#define EXPECT(c, m) do { if (!(c)) { printf("FAIL %s\n", m); g_fail++; } } while (0)

/* ---- a synthetic virtual source: the integers [0, n) as decimal strings,
 * filtered by decimal-substring. No filtered array is materialized (each query
 * scans), so it exercises the heavy path the way the browser will. ---- */
typedef struct { proven_size_t n; char filter[64]; } dyn_ctx;
static void dyn_itoa(proven_size_t v, char *b) {
    char t[24]; int j = 0;
    do { t[j++] = (char)('0' + v % 10); v /= 10; } while (v);
    int k = 0; while (j) b[k++] = t[--j]; b[k] = 0;
}
static bool dyn_match(const dyn_ctx *c, proven_size_t v) {
    if (!c->filter[0]) return true;
    char b[24]; dyn_itoa(v, b); return strstr(b, c->filter) != NULL;
}
static proven_size_t dyn_nth(const dyn_ctx *c, proven_size_t idx) {
    proven_size_t k = 0;
    for (proven_size_t i = 0; i < c->n; i++) if (dyn_match(c, i)) { if (k == idx) return i; k++; }
    return 0;
}
static proven_size_t dyn_count(void *ctx) {
    dyn_ctx *c = ctx; proven_size_t k = 0;
    for (proven_size_t i = 0; i < c->n; i++) if (dyn_match(c, i)) k++;
    return k;
}
static void dyn_row(void *ctx, proven_size_t i, char *buf, proven_size_t cap) { (void)cap; dyn_itoa(dyn_nth(ctx, i), buf); }
static int  dyn_id(void *ctx, proven_size_t i) { return (int)dyn_nth(ctx, i); }
static void dyn_filter(void *ctx, const char *f) {
    dyn_ctx *c = ctx; proven_size_t j = 0;
    for (; f[j] && j < sizeof c->filter - 1; j++) c->filter[j] = f[j];
    c->filter[j] = 0;
}

static void test_dynamic(proven_allocator_t A) {
    dyn_ctx ctx = { .n = 100000, .filter = {0} };
    prov_panel_vsource_t src = { .ctx = &ctx, .count = dyn_count, .row = dyn_row, .id = dyn_id, .filter = dyn_filter };
    prov_panel_t p;
    prov_panel_init_dynamic(&p, A, "nums", &src, NULL);
    EXPECT(p.nview == 100000, "dyn: all 100k in view");
    EXPECT(prov_panel_selected_id(&p) == 0, "dyn: sel 0 = item 0");
    prov_panel_goto(&p, 0);
    EXPECT(prov_panel_selected_id(&p) == 99999, "dyn: 0g = last (99999)");
    prov_panel_move(&p, NAV_UP, 1, 10);
    EXPECT(prov_panel_selected_id(&p) == 99998, "dyn: up 1 = 99998");
    prov_panel_filter_push(&p, '9'); prov_panel_filter_push(&p, '9'); prov_panel_filter_push(&p, '9');
    proven_size_t cnt = p.nview;
    EXPECT(cnt > 0 && cnt < 100000, "dyn: filter '999' shrinks the view");
    EXPECT(p.sel < cnt, "dyn: selection clamped into filtered view");
    char rb[32]; src.row(&ctx, 0, rb, sizeof rb);
    EXPECT(strstr(rb, "999") != NULL, "dyn: first filtered row contains 999");
    prov_panel_filter_clear(&p);
    EXPECT(p.nview == 100000, "dyn: clear restores all");
    prov_panel_free(&p);
}

int main(void) {
    proven_allocator_t A = proven_heap_allocator();
    static const prov_panel_row_t rows[] = {
        { .text = "apple",  .id = 10 },
        { .text = "banana", .id = 20 },
        { .text = "cherry", .id = 30 },
        { .text = "apricot",.id = 40 },
        { .text = "date",   .id = 50 },
    };
    prov_panel_t p;
    prov_panel_init(&p, A, "fruit", rows, 5, NULL);

    EXPECT(p.nview == 5, "all rows in view");
    EXPECT(p.sel == 0, "sel starts at 0");
    EXPECT(prov_panel_selected_id(&p) == 10, "selected = apple");

    /* move */
    prov_panel_move(&p, NAV_DOWN, 2, 4);
    EXPECT(p.sel == 2 && prov_panel_selected_id(&p) == 30, "down 2 = cherry");
    prov_panel_move(&p, NAV_DOWN, 100, 4);
    EXPECT(p.sel == 4, "down clamps to last");
    prov_panel_move(&p, NAV_UP, 1, 4);
    EXPECT(p.sel == 3, "up 1");
    prov_panel_move(&p, NAV_HOME, 1, 4);
    EXPECT(p.sel == 0, "home");
    prov_panel_move(&p, NAV_END, 1, 4);
    EXPECT(p.sel == 4, "end");
    prov_panel_move(&p, NAV_LEFT, 1, 4);
    EXPECT(p.sel == 4, "left = no-op in model");

    /* goto: 0=last, N=item N */
    prov_panel_goto(&p, 2);
    EXPECT(p.sel == 1 && prov_panel_selected_id(&p) == 20, "2g = banana");
    prov_panel_goto(&p, 0);
    EXPECT(p.sel == 4 && prov_panel_selected_id(&p) == 50, "0g = last (date)");
    prov_panel_goto(&p, 99);
    EXPECT(p.sel == 4, "Ng clamps");

    /* filter: case-insensitive substring */
    prov_panel_filter_push(&p, 'A');
    prov_panel_filter_push(&p, 'P');
    EXPECT(p.nview == 2, "filter 'ap' -> apple, apricot");
    EXPECT(prov_panel_selected_id(&p) == 10, "filter sel = first match (apple)");
    prov_panel_move(&p, NAV_DOWN, 1, 4);
    EXPECT(prov_panel_selected_id(&p) == 40, "down in filtered = apricot");
    prov_panel_filter_pop(&p);   /* "ap" -> "a": apple, banana, apricot, date (cherry has no 'a') */
    EXPECT(p.nview == 4, "pop -> filter 'a' = 4 matches");
    prov_panel_filter_clear(&p);
    EXPECT(p.nview == 5 && p.flen == 0, "clear -> all");

    /* reload (set_rows) */
    static const prov_panel_row_t rows2[] = { { .text = "x", .id = 1 }, { .text = "y", .id = 2 } };
    prov_panel_set_rows(&p, rows2, 2);
    EXPECT(p.nview == 2 && prov_panel_selected_id(&p) == 1, "reload to 2 rows");

    /* empty view */
    prov_panel_filter_push(&p, 'z');
    EXPECT(p.nview == 0 && prov_panel_selected_id(&p) == -1, "no match -> empty, id -1");
    prov_panel_move(&p, NAV_DOWN, 1, 4);
    EXPECT(p.sel == 0, "move on empty is safe");

    prov_panel_free(&p);

    /* ---- heavy/virtual mode over a synthetic 100k-item source ---- */
    test_dynamic(A);

    if (g_fail) { printf("test_panel: %d FAILED\n", g_fail); return 1; }
    printf("test_panel: OK\n");
    return 0;
}
