#include "textbox.h"
#include "unicode.h"

/* ---- text mode: a single pass that counts rows and optionally captures one ----
 *
 * Wrapping mirrors the editor's soft-wrap: advance by code points, break before a
 * glyph that would exceed `width`, and start a new row on '\n'. Invalid / partial
 * UTF-8 is skipped (one byte) and contributes nothing — the no-tofu rule. When
 * `want >= 0` and `out != NULL`, the bytes of row `want` are copied to `out`
 * (NUL-terminated, <= cap-1 bytes); `*disp` (if non-NULL) gets its display width.
 * Returns the total row count. */
static proven_size_t text_walk(const prov_textbox_t *tb, int width,
                               long want, char *out, proven_size_t cap, int *disp) {
    if (width < 1) width = 1;
    proven_size_t rows = 0, pos = 0, o = 0;
    int col = 0, wrote = 0;
    bool any = false;                 /* any glyph on the current row */
    bool cap_row = (want == 0);       /* capturing the current (row == want)? */
    if (out && cap) out[0] = '\0';

    while (pos < tb->len) {
        prov_decode_t d = prov_utf8_decode(tb->bytes + pos, tb->len - pos);
        if (!d.valid) { pos += d.len ? d.len : 1; continue; }   /* skip, no tofu */
        proven_size_t nb = d.len;
        if (d.cp == '\n' && !tb->flow) {           /* normal mode: a newline ends the row */
            rows++;
            if (cap_row) { if (out && o < cap) out[o] = '\0'; if (disp) *disp = wrote; return rows; }
            col = 0; any = false; cap_row = ((long)rows == want);
            pos += nb; continue;
        }
        /* flow mode: line breaks / tabs become a space so the text char-wraps as one
         * continuous block (no ragged right edge from short lines). */
        bool subspace = tb->flow && (d.cp == '\n' || d.cp == '\r' || d.cp == '\t');
        proven_u32 cp = subspace ? ' ' : d.cp;
        int w = prov_char_width(cp);
        if (any && col + w > width) {              /* wrap before this glyph */
            rows++;
            if (cap_row) { if (out && o < cap) out[o] = '\0'; if (disp) *disp = wrote; return rows; }
            col = 0; any = false; cap_row = ((long)rows == want);
        }
        if (cap_row && out) {                       /* capture this glyph's bytes */
            if (subspace) { if (o + 1 < cap) { out[o++] = ' '; wrote += w; } }
            else if (o + nb < cap) { for (proven_size_t k = 0; k < nb; k++) out[o++] = (char)tb->bytes[pos + k]; wrote += w; }
        }
        col += w; any = true; pos += nb;
    }
    if (any) {                                      /* trailing partial row */
        rows++;
        if (cap_row) { if (out && o < cap) out[o] = '\0'; if (disp) *disp = wrote; return rows; }
    }
    if (out && o < cap) out[o] = '\0';              /* want past the end: empty */
    if (disp && want >= 0 && (proven_size_t)want >= rows) *disp = 0;
    return rows;
}

/* ---- hex mode ---- */

static char hexd(int v) { return (char)(v < 10 ? '0' + v : 'a' + (v - 10)); }

static int hex_render(const prov_textbox_t *tb, int width, proven_size_t row,
                      char *out, proven_size_t cap) {
    if (width < 1) width = 1;
    proven_size_t off = row * PROV_TEXTBOX_HEX_BPR;
    if (off >= tb->len) { if (cap) out[0] = '\0'; return 0; }
    char line[128];
    int n = 0;
    unsigned long o = (unsigned long)off;
    for (int sh = 28; sh >= 0; sh -= 4) line[n++] = hexd((int)((o >> sh) & 0xF));  /* 8-hex offset */
    line[n++] = ' '; line[n++] = ' ';
    for (int j = 0; j < PROV_TEXTBOX_HEX_BPR; j++) {                               /* HH HH … */
        if (off + (proven_size_t)j < tb->len) {
            proven_u8 b = tb->bytes[off + j];
            line[n++] = hexd(b >> 4); line[n++] = hexd(b & 0xF);
        } else { line[n++] = ' '; line[n++] = ' '; }
        line[n++] = ' ';
    }
    line[n++] = '|';
    for (int j = 0; j < PROV_TEXTBOX_HEX_BPR; j++) {                               /* |ascii| */
        if (off + (proven_size_t)j < tb->len) {
            proven_u8 b = tb->bytes[off + j];
            line[n++] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        } else line[n++] = ' ';
    }
    line[n++] = '|';
    /* clamp to width (hex is pure ASCII so display width == byte count) and cap */
    int lim = n;
    if (lim > width) lim = width;
    proven_size_t o2 = 0;
    for (int i = 0; i < lim && o2 + 1 < cap; i++) out[o2++] = line[i];
    if (cap) out[o2] = '\0';
    return (int)o2;
}

proven_size_t prov_textbox_rows(const prov_textbox_t *tb, int width) {
    if (!tb || tb->len == 0) return 0;
    if (tb->hex) return (tb->len + PROV_TEXTBOX_HEX_BPR - 1) / PROV_TEXTBOX_HEX_BPR;
    return text_walk(tb, width, -1, NULL, 0, NULL);
}

int prov_textbox_render(const prov_textbox_t *tb, int width, proven_size_t row,
                        char *out, proven_size_t cap) {
    if (cap) out[0] = '\0';
    if (!tb || !out || cap == 0) return 0;
    if (tb->hex) return hex_render(tb, width, row, out, cap);
    int disp = 0;
    text_walk(tb, width, (long)row, out, cap, &disp);
    return disp;
}
