#ifndef PROV_HEXVIEW_H
#define PROV_HEXVIEW_H

#include "proven/types.h"

/*
 * Pure geometry + row rendering for the hex view/edit mode (RFC-0018).
 *
 * A buffer is laid out as a classic hex dump, a fixed 16 bytes per row:
 *
 *   OOOOOOOO  HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH |................|
 *   0      7  10                                            57 59             74 75
 *
 * `align` (0..15) nudges the byte window: byte 0 sits at slot `align`, so a
 * structure can be shifted to a row boundary for inspection. Render-free and
 * terminal-free, like the textbox widget — the panel/window code blits the
 * returned strings and uses the column helpers to place the cursor.
 */

#define PROV_HEX_BPR    16    /* bytes per row (fixed) */
#define PROV_HEX_ROW_W  76    /* full row display width (offset+hex+ascii) */

typedef struct {
    const proven_u8 *bytes;   /* borrowed buffer content */
    proven_size_t    len;
    int              align;   /* 0..15 byte-window nudge */
} prov_hexview_t;

/* Total rows the content occupies (>= 1 so an empty buffer still shows a row). */
proven_size_t prov_hexview_rows(proven_size_t len, int align);

/* Render row `row` as a NUL-terminated ASCII string into `out` (cap); returns
 * the display width written (0 for a row with no content and out of range). A
 * row past the end still renders its offset + blank slots (so the cursor can sit
 * at EOF / on a fresh insert). */
int prov_hexview_render(const prov_hexview_t *hv, proven_size_t row,
                        char *out, proven_size_t cap);

/* Column of slot `s`'s first hex nibble (hex pane) / of slot `s` (ascii pane). */
int prov_hexview_hexcol(int slot);
int prov_hexview_asciicol(int slot);

/* Map a byte offset to its (row, slot) given the alignment nudge. */
void prov_hexview_locate(proven_size_t off, int align, proven_size_t *row, int *slot);

#endif /* PROV_HEXVIEW_H */
