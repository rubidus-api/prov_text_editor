#include "layout.h"

void prov_layout_init(prov_layout_t *L, int buf) {
    L->count = 1;
    L->root = 0;
    L->focus = 0;
    L->freecount = 0;
    L->nodes[0] = (prov_pane_node_t){ .kind = PROV_PANE_LEAF, .parent = -1,
                                      .buf = buf, .top = 0, .child0 = -1, .child1 = -1 };
}

/* Allocate a node slot, reusing one reclaimed by a previous close before
 * growing the high-water mark; -1 when the tree is full. */
static int alloc_node(prov_layout_t *L) {
    if (L->freecount > 0) return L->freelist[--L->freecount];
    if (L->count < PROV_MAX_PANE_NODES) return L->count++;
    return -1;
}
static void free_node(prov_layout_t *L, int i) {
    if (L->freecount < PROV_MAX_PANE_NODES) L->freelist[L->freecount++] = i;
}

void prov_layout_split(prov_layout_t *L, bool vertical) {
    int f = L->focus;
    if (L->nodes[f].kind != PROV_PANE_LEAF) return;

    int n0 = alloc_node(L);
    int n1 = alloc_node(L);
    if (n0 < 0 || n1 < 0) {                  /* not enough room: roll back */
        if (n1 >= 0) free_node(L, n1);
        if (n0 >= 0) free_node(L, n0);
        return;
    }
    prov_pane_node_t leaf = L->nodes[f];     /* copy original leaf content */

    L->nodes[n0] = (prov_pane_node_t){ .kind = PROV_PANE_LEAF, .parent = f,
                                       .buf = leaf.buf, .top = leaf.top, .leftcol = leaf.leftcol,
                                       .child0 = -1, .child1 = -1, .readonly = leaf.readonly,
                                       .hex_align = leaf.hex_align };
    L->nodes[n1] = (prov_pane_node_t){ .kind = PROV_PANE_LEAF, .parent = f,
                                       .buf = leaf.buf, .top = leaf.top, .leftcol = leaf.leftcol,
                                       .child0 = -1, .child1 = -1, .readonly = leaf.readonly,
                                       .hex_align = leaf.hex_align };
    L->nodes[f].kind = vertical ? PROV_PANE_VSPLIT : PROV_PANE_HSPLIT;
    L->nodes[f].child0 = n0;
    L->nodes[f].child1 = n1;
    L->nodes[f].ratio = 50;
    L->focus = n1;
}

bool prov_layout_resize(prov_layout_t *L, int delta) {
    int f = L->focus;
    int p = L->nodes[f].parent;
    if (p < 0) return false;                      /* root leaf: nothing to resize */
    int r = L->nodes[p].ratio;
    r += (L->nodes[p].child0 == f) ? delta : -delta;   /* grow the focused child */
    if (r < 10) r = 10;
    if (r > 90) r = 90;
    if (r == L->nodes[p].ratio) return false;
    L->nodes[p].ratio = r;
    return true;
}

bool prov_layout_resize_axis(prov_layout_t *L, bool vertical, int delta) {
    prov_pane_kind_t want = vertical ? PROV_PANE_HSPLIT : PROV_PANE_VSPLIT;
    int node = L->focus;
    int p = L->nodes[node].parent;
    while (p >= 0) {                              /* climb to the nearest such split */
        if (L->nodes[p].kind == want) {
            int r = L->nodes[p].ratio;
            r += (L->nodes[p].child0 == node) ? delta : -delta;
            if (r < 10) r = 10;
            if (r > 90) r = 90;
            if (r == L->nodes[p].ratio) return false;
            L->nodes[p].ratio = r;
            return true;
        }
        node = p;
        p = L->nodes[node].parent;
    }
    return false;                                 /* no split of this orientation */
}

static int first_leaf(const prov_layout_t *L, int node) {
    while (L->nodes[node].kind != PROV_PANE_LEAF) node = L->nodes[node].child0;
    return node;
}

bool prov_layout_close(prov_layout_t *L) {
    int f = L->focus;
    int p = L->nodes[f].parent;
    if (p < 0) return false;                  /* the root leaf: cannot close */

    int sib = (L->nodes[p].child0 == f) ? L->nodes[p].child1 : L->nodes[p].child0;
    int gp = L->nodes[p].parent;

    /* Move the sibling into the parent's slot, keeping `p`'s parent link. */
    L->nodes[p] = L->nodes[sib];
    L->nodes[p].parent = gp;
    if (L->nodes[p].kind != PROV_PANE_LEAF) {
        L->nodes[L->nodes[p].child0].parent = p;
        L->nodes[L->nodes[p].child1].parent = p;
    }
    free_node(L, f);                 /* the closed leaf and the now-empty */
    free_node(L, sib);               /* sibling slot are reclaimed */
    L->focus = first_leaf(L, p);
    return true;
}

/* In-order leaf collection. */
static void collect_leaves(const prov_layout_t *L, int node, int *leaves, int *n) {
    if (L->nodes[node].kind == PROV_PANE_LEAF) { leaves[(*n)++] = node; return; }
    collect_leaves(L, L->nodes[node].child0, leaves, n);
    collect_leaves(L, L->nodes[node].child1, leaves, n);
}

void prov_layout_focus_next(prov_layout_t *L) {
    int leaves[PROV_MAX_PANE_NODES];
    int n = 0;
    collect_leaves(L, L->root, leaves, &n);
    if (n <= 1) return;
    for (int i = 0; i < n; i++)
        if (leaves[i] == L->focus) { L->focus = leaves[(i + 1) % n]; return; }
    L->focus = leaves[0];
}

int prov_layout_leaf_count(const prov_layout_t *L) {
    int leaves[PROV_MAX_PANE_NODES];
    int n = 0;
    collect_leaves(L, L->root, leaves, &n);
    return n;
}

int prov_layout_leaves(const prov_layout_t *L, int *out, int cap) {
    int leaves[PROV_MAX_PANE_NODES];
    int n = 0;
    collect_leaves(L, L->root, leaves, &n);
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) out[i] = leaves[i];
    return n;
}

/* child0's span along a `dim`-cell axis for the given split ratio (%). Siblings
 * are adjacent (no border cell) — each window's own status row / scrollbar acts
 * as its edge. Each side keeps at least 1 cell when there is room. Shared by
 * rect assignment and the renderer so they never disagree. */
int prov_layout_split_span(int dim, int ratio) {
    if (dim <= 1) return dim;
    int span = dim * ratio / 100;
    if (span < 1) span = 1;
    if (span > dim - 1) span = dim - 1;
    return span;
}

static void rects_rec(const prov_layout_t *L, int node, prov_rect_t a, prov_rect_t *out) {
    const prov_pane_node_t *nd = &L->nodes[node];
    if (nd->kind == PROV_PANE_LEAF) { out[node] = a; return; }

    if (nd->kind == PROV_PANE_HSPLIT) {        /* stacked: top / bottom, adjacent */
        int h0 = prov_layout_split_span(a.h, nd->ratio);
        prov_rect_t top = { a.row, a.col, h0, a.w };
        prov_rect_t bot = { a.row + h0, a.col, a.h - h0, a.w };
        rects_rec(L, nd->child0, top, out);
        rects_rec(L, nd->child1, bot, out);
    } else {                                   /* vsplit: left / right, adjacent */
        int w0 = prov_layout_split_span(a.w, nd->ratio);
        prov_rect_t left = { a.row, a.col, a.h, w0 };
        prov_rect_t right = { a.row, a.col + w0, a.h, a.w - w0 };
        rects_rec(L, nd->child0, left, out);
        rects_rec(L, nd->child1, right, out);
    }
}

void prov_layout_rects(const prov_layout_t *L, prov_rect_t area, prov_rect_t *out) {
    rects_rec(L, L->root, area, out);
}

bool prov_layout_move_focus(prov_layout_t *L, prov_rect_t area, prov_dir_t dir) {
    prov_rect_t rects[PROV_MAX_PANE_NODES];
    rects_rec(L, L->root, area, rects);
    prov_rect_t f = rects[L->focus];
    int fcr = f.row + f.h / 2, fcc = f.col + f.w / 2;   /* focused centre */

    int leaves[PROV_MAX_PANE_NODES], n = 0;
    collect_leaves(L, L->root, leaves, &n);

    int best = -1;
    long best_primary = 0, best_perp = 0;
    for (int i = 0; i < n; i++) {
        int g = leaves[i];
        if (g == L->focus) continue;
        prov_rect_t r = rects[g];
        long primary, perp;
        switch (dir) {
            case PROV_DIR_RIGHT:
                if (r.col < f.col + f.w) continue;            /* must be to the right */
                if (!(r.row < f.row + f.h && r.row + r.h > f.row)) continue;  /* row overlap */
                primary = r.col - (f.col + f.w);
                perp = (r.row + r.h / 2) - fcr;
                break;
            case PROV_DIR_LEFT:
                if (r.col + r.w > f.col) continue;
                if (!(r.row < f.row + f.h && r.row + r.h > f.row)) continue;
                primary = f.col - (r.col + r.w);
                perp = (r.row + r.h / 2) - fcr;
                break;
            case PROV_DIR_DOWN:
                if (r.row < f.row + f.h) continue;
                if (!(r.col < f.col + f.w && r.col + r.w > f.col)) continue;  /* col overlap */
                primary = r.row - (f.row + f.h);
                perp = (r.col + r.w / 2) - fcc;
                break;
            default: /* PROV_DIR_UP */
                if (r.row + r.h > f.row) continue;
                if (!(r.col < f.col + f.w && r.col + r.w > f.col)) continue;
                primary = f.row - (r.row + r.h);
                perp = (r.col + r.w / 2) - fcc;
                break;
        }
        if (perp < 0) perp = -perp;
        if (best < 0 || primary < best_primary ||
            (primary == best_primary && perp < best_perp)) {
            best = g; best_primary = primary; best_perp = perp;
        }
    }
    if (best < 0) return false;
    L->focus = best;
    return true;
}
