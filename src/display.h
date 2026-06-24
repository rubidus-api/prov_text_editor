#ifndef PROV_DISPLAY_H
#define PROV_DISPLAY_H

#include "proven/types.h"

#include "editor.h"

/*
 * Rendering (SPEC.md §12). The renderer is a pure function from editor state to
 * an in-memory grid of terminal cells; it performs no I/O. The terminal
 * backend (platform layer) paints the grid and reads input. Keeping rendering
 * pure makes it unit-testable without a real terminal (TEST.md strategy).
 *
 * Layout is visual: tabs expand to the next multiple of `tabstop`, East Asian
 * wide code points occupy two cells (a head cell plus a continuation cell), and
 * control characters render as a single blank. Zero-width / combining code
 * points are dropped in this milestone (best-effort, SPEC §5.1).
 */

/* Per-cell display attributes (OR-able). Selection reverse is kept as its own
 * `selected` flag for clarity; panes use these for label/focus styling. */
enum {
    PROV_ATTR_REVERSE   = 1u << 0,   /* render reverse-video (pane label rows) */
    PROV_ATTR_DIM       = 1u << 1,   /* render dim (unfocused pane content/labels) */
    PROV_ATTR_UNDERLINE = 1u << 2,   /* render underlined (field-mode region, RFC-0007) */
    PROV_ATTR_MATCH     = 1u << 3,   /* render highlighted (search matches, M4.5) */
    PROV_ATTR_BOLD      = 1u << 4    /* render bold (splash title) */
};

typedef struct {
    proven_u32 cp;        /* code point to draw; 0x20 (space) for blanks */
    bool       cont;      /* true for the trailing cell of a wide code point (cp 0) */
    bool       selected;  /* true when the cell falls within the active selection */
    proven_u8  attr;      /* PROV_ATTR_* style bits */
} prov_cell_t;

typedef struct {
    proven_size_t row;   /* viewport row of the cursor (line - top_line)        */
    proven_size_t col;   /* visual column of the cursor on its line             */
} prov_screen_pos_t;

/* Render the viewport starting at logical line `top_line` into `grid`, which
 * must hold rows*cols cells. `tabstop` must be > 0. Cells outside the document
 * and unused trailing cells are blanks (cp 0x20). */
void prov_render_into(const prov_editor_t *ed, proven_size_t top_line,
                      proven_size_t rows, proven_size_t cols,
                      proven_size_t tabstop, prov_cell_t *grid);

/* As prov_render_into, but cells whose byte falls in [ul_start, ul_end) are
 * marked PROV_ATTR_UNDERLINE (the field-mode region). ul_start >= ul_end = none. */
void prov_render_into_ul(const prov_editor_t *ed, proven_size_t top_line,
                         proven_size_t rows, proven_size_t cols,
                         proven_size_t tabstop, prov_cell_t *grid,
                         proven_size_t ul_start, proven_size_t ul_end);

/* As prov_render_into_ul, plus search highlight (PROV_ATTR_MATCH): when `rx` is
 * non-NULL, every regex match in the visible text (computed per visible line) is
 * highlighted; otherwise every occurrence of needle[0..needlelen). Both NULL =
 * no highlight. */
struct prov_regex;

/* Rectangular (visual-block) highlight: inclusive logical rows [r0,r1], code-point
 * columns [c0,c1). Pass NULL for none; c0 >= c1 also means none. */
typedef struct { proven_size_t r0, r1, c0, c1; } prov_block_sel_t;

/* `wrap_off`: when false, long lines soft-wrap (char wrap) and `leftcol` is 0.
 * When true, each logical line occupies exactly one screen row and the viewport
 * shows visual columns [leftcol, leftcol+cols) of it (horizontal scroll); columns
 * outside that window are clipped. */
void prov_render_into_full(const prov_editor_t *ed, proven_size_t top_line,
                           proven_size_t rows, proven_size_t cols,
                           proven_size_t tabstop, prov_cell_t *grid,
                           proven_size_t ul_start, proven_size_t ul_end,
                           const proven_u8 *needle, proven_size_t needlelen, bool fold,
                           struct prov_regex *rx, const prov_block_sel_t *blk,
                           bool wrap_off, proven_size_t leftcol, bool word_wrap);

/* Visual position of the cursor for a given scroll origin. `row` is
 * line_of(cursor) - top_line (0 if the cursor is above top_line); `col` is the
 * cursor's visual column on its line. The caller clamps to the viewport. */
prov_screen_pos_t prov_cursor_screen_pos(const prov_editor_t *ed,
                                         proven_size_t top_line,
                                         proven_size_t tabstop);

/* Number of screen rows logical line `line` occupies when soft-wrapped to a
 * `cols`-wide viewport (>= 1). */
proven_size_t prov_wrap_rows(const prov_editor_t *ed, proven_size_t line,
                             proven_size_t cols, proven_size_t tabstop, bool word_wrap);

/* Cursor position in a `cols`-wide viewport whose first line is `top_line`.
 * Soft-wrap (wrap_off=false): `row` counts wrapped rows from the top, `col` in
 * [0,cols). Horizontal scroll (wrap_off=true): `row` = cursor line - top_line,
 * `col` = cursor visual column - leftcol (0 if left of the window). */
prov_screen_pos_t prov_cursor_wrap_pos(const prov_editor_t *ed, proven_size_t top_line,
                                       proven_size_t cols, proven_size_t tabstop,
                                       bool wrap_off, proven_size_t leftcol, bool word_wrap);

/* Keep the cursor's visual column visible in a `width`-wide horizontal viewport
 * (wrap=off). Given the current `left` origin and the cursor column `col`, return
 * a new origin so `col` sits within [left+so, left+width-so) for a margin `so`
 * bounded by the width. Pure. */
proven_size_t prov_hscroll_left(proven_size_t left, proven_size_t col,
                                proven_size_t width, proven_size_t scrolloff);

/* Visual width of logical `line` (tabs expanded, wide chars = 2). */
proven_size_t prov_line_visual_width(const prov_editor_t *ed, proven_size_t line,
                                     proven_size_t tabstop);

/* Code-point counts: whole buffer, and from the start up to the cursor. */
proven_size_t prov_buffer_char_count(const prov_buffer_t *b);
proven_size_t prov_cursor_char_offset(const prov_editor_t *ed);

/* New first-visible line that keeps the cursor (on logical `line`) inside a
 * `vis`-row viewport whose current origin is `top`, preserving `scrolloff` rows
 * of margin above and below when the buffer is tall enough. `line_count` bounds
 * the bottom so the margin never scrolls blank space into view. With
 * scrolloff == 0 this is the plain "snap just enough to keep the cursor visible"
 * rule. Pure; no I/O. */
proven_size_t prov_scroll_top(proven_size_t top, proven_size_t line,
                              proven_size_t vis, proven_size_t scrolloff,
                              proven_size_t line_count);

/* Abbreviate `name` to fit `maxw` display columns for a status line: if it does
 * not fit, keep a head fragment and the trailing extension joined by "..."
 * (e.g. "verylongname.txt" -> "veryl....txt"). Width-aware (wide glyphs = 2) and
 * never splits a multibyte character. NUL-terminates `out`. */
void prov_abbreviate_filename(const char *name, proven_size_t maxw,
                              char *out, proven_size_t outcap);

/* Display width of `s` in terminal cells (wide glyphs = 2; invalid bytes = 1). */
proven_size_t prov_str_disp_width(const char *s);

/* Columns a line-number gutter needs for a buffer of `line_count` lines: the
 * decimal digits of the largest line number (min 3 for a stable narrow gutter)
 * plus one trailing separator space. Same width serves absolute and relative
 * modes (relative still shows the cursor line's absolute number). Pure. */
int prov_gutter_width(proven_size_t line_count);

/* Render `src` into `dst` (cap bytes, NUL-terminated) as a field exactly `cells`
 * display columns wide: whole code points only (never split a multibyte char or
 * straddle a wide glyph past the edge), space-padded; `left` = left-align else
 * right-align. Robust to invalid UTF-8 (1 cell each). Returns bytes written. */
proven_size_t prov_fit_field(char *dst, proven_size_t cap, const char *src,
                             int cells, bool left);

/* The final path component (after the last '/'), or the whole string when there
 * is no '/'. Returns a pointer into `path` (a suffix of a NUL-terminated string
 * is itself a valid cstr). */
const char *prov_basename(const char *path);

#endif /* PROV_DISPLAY_H */
