/* RFC-0021 policy guard + RFC-0022 fg-map wiring.
 *   1. prov_render_into (no highlight) leaves every cell fg = 0 — i.e. plain
 *      buffer rendering (and, by the same code path, all chrome) never colors.
 *   2. prov_render_into_hl with a per-byte fg map applies that fg to the matching
 *      cells, and only those. */
#include <stdio.h>
#include <string.h>

#include "proven/heap.h"
#include "editor.h"
#include "display.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } } while (0)

#define ROWS 4
#define COLS 16

int main(void) {
    proven_allocator_t a = proven_heap_allocator();
    prov_cell_t grid[ROWS * COLS];

    prov_result_editor_t r = prov_editor_create(a);
    CHECK(PROVEN_IS_OK(r.err), "editor create");
    prov_editor_t *ed = r.value;
    prov_editor_insert(ed, (const proven_u8 *)"int x;", 6);

    /* 1) plain render: no cell may carry a foreground color (the RFC-0021 policy:
     *    chrome / unhighlighted buffers stay monochrome). */
    prov_render_into(ed, 0, ROWS, COLS, 4, grid);
    int colored = 0;
    for (int i = 0; i < ROWS * COLS; i++) if (grid[i].fg != 0) colored++;
    CHECK(colored == 0, "plain render leaves all cells fg=0 (monochrome policy)");

    /* 2) highlighted render: a fg map over the document bytes colors exactly the
     *    mapped cells. Color bytes [0,3) ("int") with ANSI 12 (brightblue). */
    proven_u8 fgmap[6];
    for (int i = 0; i < 6; i++) fgmap[i] = 0;
    fgmap[0] = fgmap[1] = fgmap[2] = PROV_CELL_FG(12);   /* "int" */
    prov_render_into_hl(ed, 0, ROWS, COLS, 4, grid, 0, 0, NULL, 0, false,
                        NULL, NULL, false, 0, false, fgmap, 0, 6);
    /* row 0: cells 0..2 = 'i','n','t' should be brightblue; cell 3 = ' ' default */
    CHECK(grid[0].cp == 'i' && grid[0].fg == PROV_CELL_FG(12), "cell 'i' colored");
    CHECK(grid[1].cp == 'n' && grid[1].fg == PROV_CELL_FG(12), "cell 'n' colored");
    CHECK(grid[2].cp == 't' && grid[2].fg == PROV_CELL_FG(12), "cell 't' colored");
    CHECK(grid[3].fg == 0, "cell after 'int' uncolored");
    CHECK(grid[4].cp == 'x' && grid[4].fg == 0, "cell 'x' uncolored");

    /* PROV_CELL_FG bias: -1 (default) -> 0, n -> n+1 */
    CHECK(PROV_CELL_FG(-1) == 0, "fg bias: default -> 0");
    CHECK(PROV_CELL_FG(0) == 1, "fg bias: ansi 0 -> 1");
    CHECK(PROV_CELL_FG(15) == 16, "fg bias: ansi 15 -> 16");

    prov_editor_destroy(ed);
    if (failures) { fprintf(stderr, "render_hl: %d checks failed\n", failures); return 1; }
    printf("ok: render_hl tests passed\n");
    return 0;
}
