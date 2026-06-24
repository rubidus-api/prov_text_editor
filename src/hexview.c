#include "hexview.h"

static char hexd(int v) { return (char)(v < 10 ? '0' + v : 'a' + (v - 10)); }

int prov_hexview_hexcol(int slot)   { return 10 + slot * 3; }   /* offset(8) + "  " + slot*"HH " */
int prov_hexview_asciicol(int slot) { return 59 + slot; }       /* "|" at 58, ascii at 59.. */

static int clamp_align(int a) { return a < 0 ? 0 : a > 15 ? 15 : a; }

proven_size_t prov_hexview_rows(proven_size_t len, int align) {
    align = clamp_align(align);
    proven_size_t span = (proven_size_t)align + len;
    proven_size_t rows = (span + PROV_HEX_BPR - 1) / PROV_HEX_BPR;
    return rows ? rows : 1;   /* always at least one row (cursor home / empty buffer) */
}

void prov_hexview_locate(proven_size_t off, int align, proven_size_t *row, int *slot) {
    align = clamp_align(align);
    proven_size_t pos = off + (proven_size_t)align;
    if (row)  *row  = pos / PROV_HEX_BPR;
    if (slot) *slot = (int)(pos % PROV_HEX_BPR);
}

int prov_hexview_render(const prov_hexview_t *hv, proven_size_t row,
                        char *out, proven_size_t cap) {
    if (cap) out[0] = '\0';
    if (!hv || !out || cap == 0) return 0;
    int align = clamp_align(hv->align);

    /* byte index of this row's slot 0 (may be negative on row 0 with a nudge) */
    long base = (long)row * PROV_HEX_BPR - align;
    char line[PROV_HEX_ROW_W + 1];
    int n = 0;

    unsigned long lbl = base < 0 ? 0UL : (unsigned long)base;        /* leftmost shown offset */
    for (int sh = 28; sh >= 0; sh -= 4) line[n++] = hexd((int)((lbl >> sh) & 0xF));
    line[n++] = ' '; line[n++] = ' ';

    for (int s = 0; s < PROV_HEX_BPR; s++) {                          /* hex pane */
        long bi = base + s;
        if (bi >= 0 && (proven_size_t)bi < hv->len) {
            proven_u8 b = hv->bytes[bi];
            line[n++] = hexd(b >> 4); line[n++] = hexd(b & 0xF);
        } else { line[n++] = ' '; line[n++] = ' '; }
        line[n++] = ' ';
    }
    line[n++] = '|';
    for (int s = 0; s < PROV_HEX_BPR; s++) {                          /* ascii pane */
        long bi = base + s;
        if (bi >= 0 && (proven_size_t)bi < hv->len) {
            proven_u8 b = hv->bytes[bi];
            line[n++] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        } else line[n++] = ' ';
    }
    line[n++] = '|';

    proven_size_t o = 0;
    for (int i = 0; i < n && o + 1 < cap; i++) out[o++] = line[i];
    out[o] = '\0';
    return (int)o;                                                   /* ASCII: width == bytes */
}
