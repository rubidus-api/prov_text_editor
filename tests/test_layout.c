/* Unit tests for the pure pane-layout model (split / close / focus / rects). */
#include <stdio.h>

#include "layout.h"

static int failures = 0;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } \
    } while (0)

static bool req(prov_rect_t r, int row, int col, int h, int w) {
    return r.row == row && r.col == col && r.h == h && r.w == w;
}

int main(void) {
    prov_layout_t L;
    prov_rect_t out[PROV_MAX_PANE_NODES];
    prov_rect_t area = { 0, 0, 10, 40 };

    prov_layout_init(&L, 7);
    CHECK(prov_layout_leaf_count(&L) == 1, "init: 1 leaf");
    CHECK(L.nodes[0].buf == 7, "init: leaf buf 7");
    prov_layout_rects(&L, area, out);
    CHECK(req(out[0], 0, 0, 10, 40), "single pane fills the area");

    /* vertical split -> side by side with a 1-col border */
    prov_layout_split(&L, true);
    CHECK(prov_layout_leaf_count(&L) == 2, "vsplit: 2 leaves");
    CHECK(L.focus == 2, "vsplit: focus the new (right) leaf");
    prov_layout_rects(&L, area, out);
    CHECK(req(out[1], 0, 0, 10, 20), "left window (0,0,10,20)");
    CHECK(req(out[2], 0, 20, 10, 20), "right pane (0,20,10,20)");

    /* horizontal split of the focused (right) pane -> stacked */
    prov_layout_split(&L, false);
    CHECK(prov_layout_leaf_count(&L) == 3, "hsplit: 3 leaves");
    prov_layout_rects(&L, area, out);
    CHECK(req(out[1], 0, 0, 10, 20), "left window unchanged");
    CHECK(req(out[3], 0, 20, 5, 20), "right-top window (0,20,5,20)");
    CHECK(req(out[4], 5, 20, 5, 20), "right-bottom pane (5,20,5,20)");

    /* focus cycles in-order: leaves [1,3,4]; focus is 4 -> next wraps to 1 */
    prov_layout_focus_next(&L);
    CHECK(L.focus == 1, "focus_next wraps 4 -> 1");
    prov_layout_focus_next(&L);
    CHECK(L.focus == 3, "focus_next 1 -> 3");

    /* close the focused (3): its sibling (4) takes the parent's place */
    L.focus = 4;
    CHECK(prov_layout_close(&L), "close returns true");
    CHECK(prov_layout_leaf_count(&L) == 2, "close: back to 2 leaves");
    prov_layout_rects(&L, area, out);
    CHECK(req(out[1], 0, 0, 10, 20), "after close: left window");
    CHECK(req(out[2], 0, 20, 10, 20), "after close: right pane full height");

    /* close down to the root leaf, then closing is a no-op */
    CHECK(prov_layout_close(&L), "close to single");
    CHECK(prov_layout_leaf_count(&L) == 1, "single leaf");
    CHECK(!prov_layout_close(&L), "cannot close the last pane");

    /* ---- resize: ratio adjusts the focused pane's parent split ---- */
    prov_layout_t R;
    prov_rect_t ro[PROV_MAX_PANE_NODES];
    prov_layout_init(&R, 0);
    prov_layout_split(&R, true);                  /* vsplit; focus = right child (node 2) */
    prov_layout_rects(&R, area, ro);
    CHECK(req(ro[1], 0, 0, 10, 20), "default 50: left w20");
    CHECK(req(ro[2], 0, 20, 10, 20), "default 50: right w20");

    /* grow the focused (right) pane by 10%: child0 ratio 50 -> 40 */
    CHECK(prov_layout_resize(&R, 10), "resize returns true on change");
    prov_layout_rects(&R, area, ro);
    CHECK(ro[1].w == 16, "after grow-right: left w16 (ratio 40)");
    CHECK(ro[2].w == 24 && ro[2].col == 16, "after grow-right: right w24");

    /* repeated growth of the focused pane clamps child0 ratio at 10% */
    for (int i = 0; i < 50; i++) prov_layout_resize(&R, 10);
    prov_layout_rects(&R, area, ro);
    CHECK(ro[1].w == 4, "clamp: left window bottoms out at ratio 10 (w4)");
    CHECK(!prov_layout_resize(&R, 10), "no change once clamped -> returns false");

    /* a single root leaf cannot be resized */
    prov_layout_t S; prov_layout_init(&S, 0);
    CHECK(!prov_layout_resize(&S, 10), "root leaf: resize is a no-op");

    /* ---- directional focus move ---- */
    prov_layout_t M; prov_layout_init(&M, 0);
    prov_layout_split(&M, true);                  /* vsplit: 1=left, 2=right, focus=2 */
    CHECK(prov_layout_move_focus(&M, area, PROV_DIR_LEFT), "move left finds left pane");
    CHECK(M.focus == 1, "focus now left");
    CHECK(!prov_layout_move_focus(&M, area, PROV_DIR_LEFT), "nothing further left");
    CHECK(!prov_layout_move_focus(&M, area, PROV_DIR_UP), "single column: nothing above");
    CHECK(prov_layout_move_focus(&M, area, PROV_DIR_RIGHT), "move right");
    CHECK(M.focus == 2, "focus now right");

    /* split the left pane: leaves become 3=left-top, 4=left-bottom, 2=right */
    M.focus = 1;
    prov_layout_split(&M, false);                 /* hsplit left; focus = 4 (bottom) */
    CHECK(M.focus == 4, "focus left-bottom after hsplit");
    CHECK(prov_layout_move_focus(&M, area, PROV_DIR_UP), "move up within left column");
    CHECK(M.focus == 3, "focus left-top");
    M.focus = 4;
    CHECK(prov_layout_move_focus(&M, area, PROV_DIR_RIGHT), "move right to the right pane");
    CHECK(M.focus == 2, "focus right pane from left-bottom");
    CHECK(prov_layout_move_focus(&M, area, PROV_DIR_LEFT), "move back left");
    CHECK(M.focus == 4, "right -> nearest left pane is left-bottom (row overlap)");

    /* ---- directional (axis) resize: up/down = height, left/right = width ---- */
    prov_layout_t A; prov_layout_init(&A, 0);
    prov_layout_split(&A, true);                  /* vsplit: node0 = VSPLIT */
    prov_layout_split(&A, false);                 /* hsplit right: node2 = HSPLIT, focus 4 */
    CHECK(prov_layout_resize_axis(&A, true, 10), "axis: vertical resize hits the HSPLIT");
    CHECK(A.nodes[2].ratio == 40, "axis: bottom child -> HSPLIT 50->40");
    CHECK(prov_layout_resize_axis(&A, false, 10), "axis: horizontal resize hits the VSPLIT");
    CHECK(A.nodes[0].ratio == 40, "axis: right subtree -> VSPLIT 50->40");
    prov_layout_t B; prov_layout_init(&B, 0);
    prov_layout_split(&B, false);                 /* only an HSPLIT exists */
    CHECK(!prov_layout_resize_axis(&B, false, 10), "axis: no vsplit -> no-op");
    CHECK(prov_layout_resize_axis(&B, true, 10), "axis: hsplit present -> vertical works");

    /* ---- node slots are reclaimed: split/close cycles never exhaust them ---- */
    prov_layout_t F;
    prov_layout_init(&F, 0);
    for (int i = 0; i < 200; i++) {
        prov_layout_split(&F, i & 1);
        CHECK(prov_layout_leaf_count(&F) == 2, "free-list: split keeps succeeding");
        CHECK(prov_layout_close(&F), "free-list: close back to one");
    }
    CHECK(F.count <= 3, "free-list: node high-water stays small across cycles");
    /* deeper tree then unwind, still bounded */
    for (int i = 0; i < 10; i++) prov_layout_split(&F, i & 1);   /* 11 leaves */
    CHECK(prov_layout_leaf_count(&F) == 11, "free-list: deep split ok");
    while (prov_layout_close(&F)) { }
    CHECK(prov_layout_leaf_count(&F) == 1, "free-list: unwound to one leaf");

    if (failures) {
        fprintf(stderr, "layout: %d checks failed\n", failures);
        return 1;
    }
    printf("ok: layout tests passed\n");
    return 0;
}
