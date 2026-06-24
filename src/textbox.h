#ifndef PROV_TEXTBOX_H
#define PROV_TEXTBOX_H

#include "proven/types.h"

/*
 * Reusable fixed-width, vertically-scrollable text-box widget (RFC-0013).
 *
 * Pure and render-free: it turns a borrowed byte span into width-wrapped visible
 * rows, in one of two modes — UTF-8 text (code-point-safe wrap; invalid /
 * incomplete sequences are SKIPPED, never rendered as the replacement glyph) or a
 * hex dump (`OFFSET  HH … HH  |ascii|`). The first consumer is the file-browser
 * preview pane; the widget itself knows nothing about panels or terminals.
 */

typedef struct {
    const proven_u8 *bytes;     /* borrowed content (UTF-8 when !hex) */
    proven_size_t    len;
    bool             hex;       /* hex dump vs. wrapped text */
    bool             flow;      /* text mode: ignore line breaks — one continuous code-point
                                 * char-wrap (newline/CR/tab render as a space), so the text
                                 * fills the box as a clean rectangle (file-open preview). */
    bool             editable;  /* reserved (RFC-0013 Phase B); inert today */
} prov_textbox_t;

/* Total number of visible rows at `width` display columns (width clamped to >=1).
 * Text: sum of wrapped rows per logical line (a trailing newline yields a final
 * empty row only if content ends with '\n'? no — see .c). Hex: ceil(len/16). */
proven_size_t prov_textbox_rows(const prov_textbox_t *tb, int width);

/* Render visible row `row` as a NUL-terminated UTF-8 string into `out` (cap),
 * clamped to `width` display columns. Returns the display width written (0 for an
 * out-of-range row, which yields an empty string). */
int prov_textbox_render(const prov_textbox_t *tb, int width, proven_size_t row,
                        char *out, proven_size_t cap);

/* Bytes per row in hex mode (fixed). Exposed for tests / geometry. */
#define PROV_TEXTBOX_HEX_BPR 16

#endif /* PROV_TEXTBOX_H */
