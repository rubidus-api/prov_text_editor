#ifndef PROV_DRAW_H
#define PROV_DRAW_H

/*
 * Low-level presentation helpers: pure, stateless functions that format values
 * into text buffers or blit glyphs / widgets into a cell grid. They hold no
 * session state, so they are reusable and trivially testable. The stateful
 * layout (which window/panel goes where, what to show) stays in the event loop;
 * everything here just renders what it is handed.
 */

#include "proven/types.h"
#include "proven/fs.h"
#include "display.h"   /* prov_cell_t, PROV_ATTR_* */

#define PROV_VERSION "0.4"   /* shown on the splash */

/* ---- value formatting (into a NUL-terminated char buffer) ---- */

/* Format `v` as a decimal with thousands separators (e.g. 1,234,567). */
void prov_fmt_count(char *buf, proven_size_t cap, proven_size_t v);

/* Human-readable file size: "-" for a directory, a raw byte count below 1 KiB,
 * else a one-decimal value with a B/K/M/G/T unit suffix. */
void prov_fmt_size(char *b, proven_size_t cap, proven_size_t sz, bool is_dir);

/* A 10-char "drwxr-xr-x"-style permission string (always NUL-terminated). */
void prov_fmt_perms(char *b, proven_fs_perms_t p, bool is_dir);

/* "YYYY-MM-DD HH:MM" from a Unix-seconds timestamp ("-" when non-positive). */
void prov_fmt_mtime(char *b, proven_size_t cap, proven_i64 secs);

/* ---- glyph blitting into a cell grid ---- */

/* Place UTF-8 `str` at (row, col), one codepoint per cell, until the right edge
 * (`cols`). Returns the column just past the last cell written. */
int prov_blit_utf8(prov_cell_t *grid, int cols, int row, int col, proven_u8 attr,
                   const char *str);

/* Like prov_blit_utf8 but clipped to `maxw` display columns and width-aware: a
 * wide (CJK/emoji) glyph occupies two cells (lead + continuation), so box
 * borders stay aligned. */
void prov_blit_utf8_clip(prov_cell_t *grid, int cols, int row, int col,
                         int maxw, proven_u8 attr, const char *str);

/* The centered welcome screen over an empty start buffer. */
void prov_draw_splash(prov_cell_t *grid, int cols, int rows);

/* ---- scrollbar widgets (positioned from a viewport) ---- */

/* Shared vertical scrollbar for both editing windows and panels: a `█` thumb on
 * a `│` track over rows [row, row+h), with `▲`/`▼` buttons on the bottom two
 * rows when there is room (h >= 4). When the content fits (total <= h) no
 * scrollbar is drawn — just the plain `│` box border. Every cell is stamped with
 * `attr` (windows pass PROV_ATTR_DIM when unfocused; panels pass 0). The box
 * corners around it are drawn by the caller and are left intact. */
void prov_draw_vscroll(prov_cell_t *grid, int cols, int row, int col, int h,
                       proven_size_t top, proven_size_t total, proven_u8 attr);

/* Shared horizontal scrollbar: a `█` thumb on a `─` track over cols [col, col+w),
 * so it doubles as a box bottom line when nothing overflows (total <= w draws a
 * plain `─` track). Every cell is stamped with `attr`. Corners are the caller's. */
void prov_draw_hscroll(prov_cell_t *grid, int cols, int row, int col, int w,
                       proven_size_t left, proven_size_t total, proven_u8 attr);

/* ---- panel help text ---- */

/* Fill `out` (capacity 16) with the intent/help lines for a panel of `kind`
 * (PANEL_K_*), returning the line count. Carries only what the shared keymap
 * legend cannot: what the panel is *for*. */
int prov_panel_help_lines(int kind, const char *out[16]);

#endif /* PROV_DRAW_H */
