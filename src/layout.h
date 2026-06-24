#ifndef PROV_LAYOUT_H
#define PROV_LAYOUT_H

#include "proven/types.h"

/*
 * Pane layout (SPEC §10.7 `w` namespace, §12): a binary split tree over the
 * text area. Each leaf shows a buffer (by index into the buffer set) with its
 * own viewport top; internal nodes split horizontally (stacked) or vertically
 * (side by side). A split starts equal (50/50) and can be resized via `ratio`.
 * The model is pure — rect assignment is unit-tested without a terminal.
 */

#define PROV_MAX_PANE_NODES 64
#define PROV_MAX_TABS       16   /* a tab is a whole layout; this caps open tabs */

typedef enum { PROV_PANE_LEAF, PROV_PANE_HSPLIT, PROV_PANE_VSPLIT } prov_pane_kind_t;

typedef struct {
    prov_pane_kind_t kind;
    int              parent;   /* parent node index, -1 for the root */
    int              buf;      /* leaf: buffer index shown here */
    proven_size_t    top;      /* leaf: viewport (first visible line) */
    proven_size_t    leftcol;  /* leaf: horizontal origin (visual col) when wrap=off */
    int              child0;   /* split: children node indices */
    int              child1;
    int              ratio;    /* split: child0's share of the usable span, % (10..90) */
    bool             readonly; /* leaf: this window blocks edits (wr toggles) */
    int              hex_align;/* leaf: 0..15 byte-window nudge for the hex view (RFC-0019;
                                * hex-ness itself is a buffer property, prov_fileinfo_t.binary) */
} prov_pane_node_t;

typedef struct {
    prov_pane_node_t nodes[PROV_MAX_PANE_NODES];
    int              count;    /* high-water mark of node slots ever used */
    int              root;
    int              focus;    /* index of the focused leaf node */
    int              freelist[PROV_MAX_PANE_NODES]; /* reclaimed node slots */
    int              freecount;
} prov_layout_t;

typedef struct { int row, col, h, w; } prov_rect_t;

/* One leaf showing buffer `buf`. */
void prov_layout_init(prov_layout_t *L, int buf);

/* Split the focused leaf; the new pane shows the same buffer and gets focus.
 * `vertical` true = side-by-side (wv), false = stacked (wh). No-op if full. */
void prov_layout_split(prov_layout_t *L, bool vertical);

/* Close the focused leaf (its sibling takes the parent's place). No-op when the
 * root is the only leaf. Returns true if a pane was closed. */
bool prov_layout_close(prov_layout_t *L);

/* Move focus to the next leaf in left-to-right / top-to-bottom order (wraps). */
void prov_layout_focus_next(prov_layout_t *L);

int  prov_layout_leaf_count(const prov_layout_t *L);

/* Fill `out` with the leaf node indices in left-to-right / top-to-bottom order
 * (up to `cap`); returns the count. The focused leaf's position in this order is
 * what `wp`/`wn` and the window list use. */
int  prov_layout_leaves(const prov_layout_t *L, int *out, int cap);

/* Grow (delta > 0) or shrink (delta < 0) the focused pane by adjusting its
 * parent split's ratio, clamped to [10, 90] %. No-op for the root leaf.
 * Returns true if a split ratio changed. */
bool prov_layout_resize(prov_layout_t *L, int delta);

/* Resize the focused window along one axis: `vertical` true adjusts its height
 * (the nearest HSPLIT ancestor), false its width (nearest VSPLIT). delta > 0
 * grows the focused window. Returns true if a ratio changed. */
bool prov_layout_resize_axis(prov_layout_t *L, bool vertical, int delta);

typedef enum { PROV_DIR_UP, PROV_DIR_DOWN, PROV_DIR_LEFT, PROV_DIR_RIGHT } prov_dir_t;

/* Move focus to the nearest leaf adjacent to the focused pane in direction
 * `dir`, laying the tree out over `area` to compare geometry. Returns true and
 * updates focus if a pane exists in that direction; false (no change) otherwise. */
bool prov_layout_move_focus(prov_layout_t *L, prov_rect_t area, prov_dir_t dir);

/* Assign each LEAF node its rectangle within `area` (a 1-cell border sits
 * between siblings). `out` must hold PROV_MAX_PANE_NODES rects; only entries at
 * leaf node indices are written. */
void prov_layout_rects(const prov_layout_t *L, prov_rect_t area, prov_rect_t *out);

/* child0's span (cells) along a `dim`-cell axis for a split `ratio` (%); the
 * remaining `dim - 1 - span` cells go to child1 (1 cell is the border). Exposed
 * so the renderer and rect assignment agree exactly. */
int prov_layout_split_span(int dim, int ratio);

#endif /* PROV_LAYOUT_H */
