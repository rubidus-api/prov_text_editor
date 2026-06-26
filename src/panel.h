#ifndef PROV_PANEL_H
#define PROV_PANEL_H

/* Common modal panel (RFC-0010). S1 = the pure model: caller-owned rows, a
 * filtered view, selection, and goto/move/filter operations (generalized from
 * the file browser). Rendering (the layer composite) and editor wiring come in
 * later stages; this header/module stays render-free and testable. */

#include "keymap.h"
#include "proven/types.h"
#include "proven/allocator.h"

typedef struct {
    const char  *text;       /* primary label */
    const char **cols;       /* optional extra columns (NULL/0 = none) */
    int          ncols;
    int          id;         /* caller's meaning (slot letter, buffer idx, …) */
} prov_panel_row_t;

typedef enum { PANEL_FULL, PANEL_TOP, PANEL_BOTTOM, PANEL_LEFT, PANEL_RIGHT } prov_panel_pos_t;

/* Which consumer a modal panel is serving (the event loop dispatches on this;
 * the renderer uses it for the per-panel help text). */
enum {
    PANEL_K_WINDOWS = 1, PANEL_K_TABS, PANEL_K_REGS, PANEL_K_MACROS,
    PANEL_K_BOOKMARKS, PANEL_K_BROWSER, PANEL_K_SEARCH, PANEL_K_CMDS,
    PANEL_K_MOVES, PANEL_K_UNDO, PANEL_K_HELP, PANEL_K_SAVEAS, PANEL_K_FIND,
    PANEL_K_HEXEDIT   /* RFC-0019: multi-line ed-mode edit of a decoded byte range */
};

/* Virtual ("heavy") row source: for huge, dynamically-produced lists (the file
 * browser) that must not materialize every row. The provider owns its data; the
 * panel keeps only a pointer + the current selection index. All indices are in
 * the *filtered* order. Allocated by the consumer only while such a panel is
 * open. When `src` is NULL the panel uses the light caller-owned `rows[]` path. */
typedef struct prov_panel_vsource {
    void          *ctx;
    proven_size_t (*count)(void *ctx);                                   /* filtered item count */
    void          (*row)(void *ctx, proven_size_t i, char *buf, proven_size_t cap);  /* render item i */
    int           (*id)(void *ctx, proven_size_t i);                     /* caller id for item i */
    void          (*filter)(void *ctx, const char *f);                   /* apply the filter string */
} prov_panel_vsource_t;

typedef struct {
    const char           *title;
    const prov_panel_row_t *rows;   /* caller-owned (lifetime > panel); light mode */
    proven_size_t         nrows;
    const prov_panel_vsource_t *src;   /* non-NULL = heavy/virtual mode (rows fetched on demand) */
    prov_panel_pos_t      pos;
    const prov_keymap_t  *keys;

    /* filtered view + selection (owned state) */
    proven_size_t        *view;     /* indices into rows that pass the filter */
    proven_size_t         nview, view_cap;
    proven_size_t         sel;      /* index into view */
    char                  filter[128];
    proven_size_t         flen;
    prov_keymap_parser_t  parser;
    char                  legend[192];   /* footer key legend, built once at init (not per frame) */
    proven_allocator_t    a;
} prov_panel_t;

/* Initialize over `rows` (caller-owned). Filter is empty (view = all). */
void prov_panel_init(prov_panel_t *p, proven_allocator_t a, const char *title,
                     const prov_panel_row_t *rows, proven_size_t nrows, const prov_keymap_t *keys);
/* Initialize a heavy/virtual panel over `src` (caller-owned, lifetime > panel). */
void prov_panel_init_dynamic(prov_panel_t *p, proven_allocator_t a, const char *title,
                             const prov_panel_vsource_t *src, const prov_keymap_t *keys);
void prov_panel_free(prov_panel_t *p);

/* Re-point at new rows (a verb mutated the data); re-applies the filter, clamps. */
void prov_panel_set_rows(prov_panel_t *p, const prov_panel_row_t *rows, proven_size_t nrows);

/* Rebuild the view from the current filter (case-insensitive substring on text). */
void prov_panel_refilter(prov_panel_t *p);

/* Filter editing (the `ss` sub-mode): append a byte / backspace / clear, each
 * re-filters and clamps the selection. */
void prov_panel_filter_push(prov_panel_t *p, char c);
void prov_panel_filter_pop(prov_panel_t *p);
void prov_panel_filter_clear(prov_panel_t *p);

/* Move the selection. UP/DOWN/HOME/END and PGUP/PGDN (by `page`); LEFT/RIGHT are
 * surface-defined and ignored here. */
void prov_panel_move(prov_panel_t *p, prov_nav_t dir, proven_u32 count, proven_size_t page);

/* Go to item: 0 = last, N>=1 = the N-th visible item (1-based). */
void prov_panel_goto(prov_panel_t *p, proven_u32 index);

/* The caller `id` of the selected row, or -1 if the view is empty. */
int prov_panel_selected_id(const prov_panel_t *p);

#endif /* PROV_PANEL_H */
