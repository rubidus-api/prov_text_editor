#include "lineedit.h"
#include "unicode.h"   /* prov_utf8_decode, prov_char_width — display-width scroll */

static bool is_cont(unsigned char b) { return (b & 0xC0) == 0x80; }   /* UTF-8 continuation */
static bool is_word(unsigned char b) {   /* word run: alnum, '_', or any multibyte (CJK) byte */
    return b == '_' || (b >= '0' && b <= '9') || (b >= 'a' && b <= 'z')
        || (b >= 'A' && b <= 'Z') || b >= 0x80;
}

/* previous / next code-point boundary from byte offset `o`. */
static proven_size_t cp_prev(const prov_lineedit_t *le, proven_size_t o) {
    if (o == 0) return 0;
    o--;
    while (o > 0 && is_cont((unsigned char)le->buf[o])) o--;
    return o;
}
static proven_size_t cp_next(const prov_lineedit_t *le, proven_size_t o) {
    if (o >= le->len) return le->len;
    o++;
    while (o < le->len && is_cont((unsigned char)le->buf[o])) o++;
    return o;
}

void prov_le_clear(prov_lineedit_t *le) {
    le->len = le->cur = le->anchor = 0;
    le->buf[0] = '\0';
}

void prov_le_set(prov_lineedit_t *le, const char *text) {
    proven_size_t n = 0;
    while (text[n] && n + 1 < PROV_LE_CAP) { le->buf[n] = text[n]; n++; }
    le->buf[n] = '\0';
    le->len = le->cur = le->anchor = n;
}

bool prov_le_has_sel(const prov_lineedit_t *le) { return le->anchor != le->cur; }

void prov_le_sel_range(const prov_lineedit_t *le, proven_size_t *start, proven_size_t *end) {
    if (le->anchor <= le->cur) { *start = le->anchor; *end = le->cur; }
    else                       { *start = le->cur;    *end = le->anchor; }
}

void prov_le_delete_sel(prov_lineedit_t *le) {
    if (le->anchor == le->cur) return;
    proven_size_t a, b; prov_le_sel_range(le, &a, &b);
    proven_size_t gap = b - a;
    for (proven_size_t i = b; i < le->len; i++) le->buf[i - gap] = le->buf[i];
    le->len -= gap;
    le->buf[le->len] = '\0';
    le->cur = le->anchor = a;
}

void prov_le_insert(prov_lineedit_t *le, const char *bytes, proven_size_t n) {
    if (le->anchor != le->cur) prov_le_delete_sel(le);
    if (le->len + n >= PROV_LE_CAP) n = (PROV_LE_CAP - 1) - le->len;   /* clamp to capacity */
    for (proven_size_t i = le->len; i > le->cur; ) { i--; le->buf[i + n] = le->buf[i]; }  /* open a gap */
    for (proven_size_t i = 0; i < n; i++) le->buf[le->cur + i] = bytes[i];
    le->len += n; le->cur += n; le->anchor = le->cur;
    le->buf[le->len] = '\0';
}

void prov_le_backspace(prov_lineedit_t *le) {
    if (le->anchor != le->cur) { prov_le_delete_sel(le); return; }
    if (le->cur == 0) return;
    proven_size_t p = cp_prev(le, le->cur);
    proven_size_t gap = le->cur - p;
    for (proven_size_t i = le->cur; i < le->len; i++) le->buf[i - gap] = le->buf[i];
    le->len -= gap; le->cur = le->anchor = p;
    le->buf[le->len] = '\0';
}

void prov_le_delete(prov_lineedit_t *le) {
    if (le->anchor != le->cur) { prov_le_delete_sel(le); return; }
    if (le->cur >= le->len) return;
    proven_size_t nx = cp_next(le, le->cur);
    proven_size_t gap = nx - le->cur;
    for (proven_size_t i = nx; i < le->len; i++) le->buf[i - gap] = le->buf[i];
    le->len -= gap;
    le->buf[le->len] = '\0';
}

static proven_size_t word_left(const prov_lineedit_t *le, proven_size_t o) {
    while (o > 0 && !is_word((unsigned char)le->buf[o - 1])) o--;   /* skip separators */
    while (o > 0 &&  is_word((unsigned char)le->buf[o - 1])) o--;   /* skip the word */
    return o;
}
static proven_size_t word_right(const prov_lineedit_t *le, proven_size_t o) {
    while (o < le->len &&  is_word((unsigned char)le->buf[o])) o++;
    while (o < le->len && !is_word((unsigned char)le->buf[o])) o++;
    return o;
}

void prov_le_move(prov_lineedit_t *le, prov_le_dir_t dir, bool extend) {
    switch (dir) {
        case PROV_LE_LEFT:       le->cur = cp_prev(le, le->cur); break;
        case PROV_LE_RIGHT:      le->cur = cp_next(le, le->cur); break;
        case PROV_LE_HOME:       le->cur = 0; break;
        case PROV_LE_END:        le->cur = le->len; break;
        case PROV_LE_WORD_LEFT:  le->cur = word_left(le, le->cur); break;
        case PROV_LE_WORD_RIGHT: le->cur = word_right(le, le->cur); break;
    }
    if (!extend) le->anchor = le->cur;
}

/* ---- history (ring) ---- */

static int hist_entries(const prov_lehist_t *h) {
    return h->count < PROV_LE_HIST_MAX ? h->count : PROV_LE_HIST_MAX;
}
static void copy_cap(char *dst, const char *src) {
    proven_size_t i = 0;
    for (; src[i] && i + 1 < PROV_LE_CAP; i++) dst[i] = src[i];
    dst[i] = '\0';
}
static bool streq(const char *a, const char *b) {
    for (proven_size_t i = 0;; i++) { if (a[i] != b[i]) return false; if (!a[i]) return true; }
}

void prov_lehist_push(prov_lehist_t *h, const char *text) {
    if (!text || !text[0]) return;
    if (hist_entries(h) > 0 && streq(h->buf[(h->count - 1) % PROV_LE_HIST_MAX], text)) {
        h->pos = -1; return;                              /* skip a repeat of the newest */
    }
    copy_cap(h->buf[h->count % PROV_LE_HIST_MAX], text);   /* ring: overwrite the oldest, no shift */
    h->count++; h->pos = -1;
}

int prov_lehist_len(const prov_lehist_t *h) { return hist_entries(h); }

const char *prov_lehist_get(const prov_lehist_t *h, int i) {
    int e = hist_entries(h);
    if (i < 0 || i >= e) return NULL;
    /* oldest sits at count-e, newest at count-1 (modulo the ring) */
    return h->buf[(h->count - e + i) % PROV_LE_HIST_MAX];
}

void prov_le_history(prov_lineedit_t *le, prov_lehist_t *h, bool up) {
    int e = hist_entries(h);
    if (e == 0) return;
    if (h->pos == -1) copy_cap(h->draft, le->buf);        /* save the draft on first recall */
    if (up) { if (h->pos < e - 1) h->pos++; }
    else    { if (h->pos >= 0) h->pos--; }
    if (h->pos < 0) prov_le_set(le, h->draft);
    else            prov_le_set(le, h->buf[(h->count - 1 - h->pos) % PROV_LE_HIST_MAX]);
}

/* ---- rendering (horizontal scroll, display-width accurate) ---- */

static int cpwidth_at(const prov_lineedit_t *le, proven_size_t o, proven_size_t *clen) {
    prov_decode_t d = prov_utf8_decode((const proven_u8 *)le->buf + o, le->len - o);
    *clen = d.len ? d.len : 1;
    int w = prov_char_width(d.cp);
    return w < 1 ? 1 : w;           /* a control/zero-width byte still takes a cell here */
}
static int col_at(const prov_lineedit_t *le, proven_size_t off) {   /* display col of byte offset */
    int col = 0; proven_size_t o = 0, cl;
    while (o < off && o < le->len) { col += cpwidth_at(le, o, &cl); o += cl; }
    return col;
}

void prov_le_render(const prov_lineedit_t *le, int width, char *out, proven_size_t cap,
                    int *curcol, int *sel_a, int *sel_b) {
    if (width < 1) width = 1;
    int cfull = col_at(le, le->cur);
    int start = (cfull >= width) ? cfull - width + 1 : 0;   /* scroll so the cursor stays visible */
    proven_size_t sA = le->cur, sB = le->cur;
    if (prov_le_has_sel(le)) prov_le_sel_range(le, &sA, &sB);
    int saFull = col_at(le, sA), sbFull = col_at(le, sB);
    proven_size_t o = 0, w = 0; int col = 0;
    while (o < le->len) {
        proven_size_t cl; int cw = cpwidth_at(le, o, &cl);
        if (col >= start && col + cw <= start + width) {     /* fully within the window */
            for (proven_size_t k = 0; k < cl && w + 1 < cap; k++) out[w++] = le->buf[o + k];
        } else if (col >= start + width) break;
        col += cw; o += cl;
    }
    if (cap) out[w] = '\0';
    #define CLAMP(v) ((v) < 0 ? 0 : (v) > width ? width : (v))
    *curcol = CLAMP(cfull - start);
    *sel_a  = CLAMP(saFull - start);
    *sel_b  = CLAMP(sbFull - start);
    #undef CLAMP
}
