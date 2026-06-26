#include "display.h"

#include "buffer.h"
#include "search.h"
#include "regex.h"
#include "unicode.h"
#include "proven/u8str.h"

/* End of line L: just before its trailing '\n', or the document end. */
static proven_size_t line_end_of(const prov_buffer_t *b, proven_size_t L) {
    proven_size_t lc = prov_buffer_line_count(b);
    if (L + 1 < lc) return prov_buffer_line_start(b, L + 1) - 1;
    return prov_buffer_byte_len(b);
}

/* Decode the code point at byte `pos`, reading up to 4 bytes from the buffer. */
static prov_decode_t decode_at(const prov_buffer_t *b, proven_size_t pos) {
    proven_u8 tmp[4];
    proven_size_t got = prov_buffer_copy_range(b, pos, 4, tmp, 4);
    return prov_utf8_decode(tmp, got);
}

/* Display code point for rendering: control characters collapse to a blank. */
static proven_u32 display_cp(proven_u32 cp) {
    if (cp < 0x20 || cp == 0x7F) return 0x20;
    return cp;
}

/* Total visual width of logical line `L` (tabs expanded, wide chars = 2), with
 * no wrapping — used for the cursor column / line-length readout. */
static proven_size_t line_visual_width(const prov_buffer_t *b, proven_size_t L,
                                       proven_size_t tabstop) {
    proven_size_t pos = prov_buffer_line_start(b, L);
    proven_size_t end = line_end_of(b, L);
    proven_size_t visx = 0;
    while (pos < end) {
        prov_decode_t d = decode_at(b, pos);
        proven_size_t adv = d.len ? d.len : 1;
        if (d.cp == 0x09) visx = ((visx / tabstop) + 1) * tabstop;
        else { int w = prov_char_width(display_cp(d.cp)); visx += (w == 2) ? 2 : (w == 0 ? 0 : 1); }
        pos += adv;
    }
    return visx;
}

/* Wrapped visual position of byte `stop` (or the line end) in a `cols`-wide
 * viewport: like line_visual_width, but a wide char that would straddle the
 * right edge skips the last cell (a 1-cell pad), so it never splits across rows.
 * The renderer applies the identical rule, so geometry stays consistent. */
static proven_size_t wrapped_visx(const prov_buffer_t *b, proven_size_t L,
                                  proven_size_t cols, proven_size_t tabstop,
                                  proven_size_t stop) {
    proven_size_t pos = prov_buffer_line_start(b, L);
    proven_size_t end = line_end_of(b, L);
    if (end > stop) end = stop;
    proven_size_t visx = 0;
    while (pos < end) {
        prov_decode_t d = decode_at(b, pos);
        proven_size_t adv = d.len ? d.len : 1;
        if (d.cp == 0x09) { visx = ((visx / tabstop) + 1) * tabstop; pos += adv; continue; }
        int w = prov_char_width(display_cp(d.cp));
        if (w == 0) { pos += adv; continue; }
        if (w == 2 && cols > 1 && (visx % cols) == cols - 1) visx++;   /* pad before the edge */
        visx += (w == 2) ? 2 : 1;
        pos += adv;
    }
    return visx;
}

static int wordwrap_breaks(const prov_buffer_t *b, proven_size_t L, proven_size_t W,
                           proven_size_t tabstop, proven_size_t *breaks, int cap);

proven_size_t prov_wrap_rows(const prov_editor_t *ed, proven_size_t line,
                             proven_size_t cols, proven_size_t tabstop, bool word_wrap) {
    if (cols == 0) return 1;
    if (tabstop == 0) tabstop = 1;
    const prov_buffer_t *b = prov_editor_buffer(ed);
    if (line >= prov_buffer_line_count(b)) return 1;
    if (word_wrap) {
        proven_size_t W = cols >= 2 ? cols - 1 : cols;
        proven_size_t wb[256];
        return (proven_size_t)wordwrap_breaks(b, line, W, tabstop, wb, 256);
    }
    proven_size_t w = wrapped_visx(b, line, cols, tabstop, (proven_size_t)-1);
    return w == 0 ? 1 : (w + cols - 1) / cols;
}

/* Word-wrap break points: fill `breaks[r]` with the line-relative visual column
 * at which screen row r begins when line L is word-wrapped to width W. breaks[0]
 * is 0. Greedy: break before a word that would overflow the row (a word longer
 * than W char-breaks; a wide glyph that would straddle the edge breaks too); a
 * space that overflows is consumed at the wrap. Breaks fall on code-point
 * boundaries, so a multibyte character is never split. Returns the row count
 * (>= 1), capped at `cap`. This is the single source of truth for word geometry. */
static int wordwrap_breaks(const prov_buffer_t *b, proven_size_t L, proven_size_t W,
                           proven_size_t tabstop, proven_size_t *breaks, int cap) {
    if (W == 0) W = 1;
    proven_size_t pos = prov_buffer_line_start(b, L);
    proven_size_t end = line_end_of(b, L);
    breaks[0] = 0; int n = 1;
    proven_size_t vx = 0;          /* line-relative visual column */
    proven_size_t rs = 0;          /* visx at which the current row started */
    #define WW_BREAK(at) do { rs = (at); if (n < cap) { breaks[n] = rs; n++; } } while (0)
    while (pos < end) {
        prov_decode_t d = decode_at(b, pos);
        proven_size_t adv = d.len ? d.len : 1;
        if (d.cp == 0x09 || d.cp == 0x20) {            /* whitespace: a break opportunity */
            proven_size_t cw = (d.cp == 0x09) ? (((vx / tabstop) + 1) * tabstop - vx) : 1;
            if (vx - rs + cw > W) WW_BREAK(vx + cw);   /* overflows: wrap, consume the space */
            vx += cw; pos += adv; continue;
        }
        proven_size_t wp = pos, ww = 0;                /* measure the word's visual width */
        while (wp < end) {
            prov_decode_t wd = decode_at(b, wp);
            if (wd.cp == 0x09 || wd.cp == 0x20) break;
            int cw0 = prov_char_width(display_cp(wd.cp));
            ww += (cw0 == 2) ? 2 : (cw0 == 0 ? 0 : 1);
            wp += wd.len ? wd.len : 1;
        }
        if (vx > rs && (vx - rs) + ww > W && ww <= W) WW_BREAK(vx);   /* wrap before the word */
        while (pos < end) {                            /* place the word; char-break if over-long */
            prov_decode_t cd = decode_at(b, pos);
            if (cd.cp == 0x09 || cd.cp == 0x20) break;
            int cw0 = prov_char_width(display_cp(cd.cp));
            proven_size_t cw = (cw0 == 2) ? 2 : (cw0 == 0 ? 0 : 1);
            if (cw > 0 && vx - rs + cw > W) WW_BREAK(vx);
            vx += cw; pos += cd.len ? cd.len : 1;
        }
    }
    #undef WW_BREAK
    return n;
}

/* Screen (row, col) for a char at line-relative visual column `vx`, per mode.
 * Returns false when the cell is outside the horizontal window (clipped). The
 * caller advances `wr` (the word-wrap row index) monotonically as `vx` grows. */
static bool cell_pos(bool wrap_off, bool word_wrap, proven_size_t sr, proven_size_t cols,
                     proven_size_t leftcol, const proven_size_t *wb, int wr,
                     proven_size_t vx, proven_size_t *out_rr, proven_size_t *out_sc) {
    if (word_wrap) {
        if (vx < wb[wr]) return false;
        proven_size_t sc = vx - wb[wr];
        if (sc >= cols) return false;
        *out_rr = sr + (proven_size_t)wr; *out_sc = sc; return true;
    }
    if (wrap_off) {
        if (vx < leftcol) return false;
        proven_size_t sc = vx - leftcol;
        if (sc >= cols) return false;
        *out_rr = sr; *out_sc = sc; return true;
    }
    *out_rr = sr + vx / cols; *out_sc = vx % cols; return true;
}

/* Write one cell at an explicit screen (rr, sc); drops it when past the grid.
 * `fg` is a biased cell foreground (0 = leave default; see display.h). */
static void put_cell_rc(prov_cell_t *grid, proven_size_t rows, proven_size_t cols,
                        proven_size_t rr, proven_size_t sc, proven_u32 cp, bool cont,
                        bool here, bool ul, bool mt, proven_u8 fg) {
    if (rr >= rows || sc >= cols) return;
    prov_cell_t *cell = &grid[rr * cols + sc];
    cell->cp = cont ? 0 : cp;
    cell->cont = cont;
    cell->selected = here;
    if (fg) cell->fg = fg;
    if (ul) cell->attr |= PROV_ATTR_UNDERLINE;
    if (mt) cell->attr |= PROV_ATTR_MATCH;
}

void prov_render_into(const prov_editor_t *ed, proven_size_t top_line,
                      proven_size_t rows, proven_size_t cols,
                      proven_size_t tabstop, prov_cell_t *grid) {
    prov_render_into_ul(ed, top_line, rows, cols, tabstop, grid, 0, 0);
}

void prov_render_into_ul(const prov_editor_t *ed, proven_size_t top_line,
                         proven_size_t rows, proven_size_t cols,
                         proven_size_t tabstop, prov_cell_t *grid,
                         proven_size_t ul_start, proven_size_t ul_end) {
    prov_render_into_full(ed, top_line, rows, cols, tabstop, grid, ul_start, ul_end, NULL, 0, false, NULL, NULL, false, 0, false);
}

void prov_render_into_full(const prov_editor_t *ed, proven_size_t top_line,
                           proven_size_t rows, proven_size_t cols,
                           proven_size_t tabstop, prov_cell_t *grid,
                           proven_size_t ul_start, proven_size_t ul_end,
                           const proven_u8 *needle, proven_size_t needlelen, bool fold,
                           struct prov_regex *rx, const prov_block_sel_t *blk,
                           bool wrap_off, proven_size_t leftcol, bool word_wrap) {
    prov_render_into_hl(ed, top_line, rows, cols, tabstop, grid, ul_start, ul_end,
                        needle, needlelen, fold, rx, blk, wrap_off, leftcol, word_wrap,
                        NULL, 0, 0);
}

void prov_render_into_hl(const prov_editor_t *ed, proven_size_t top_line,
                         proven_size_t rows, proven_size_t cols,
                         proven_size_t tabstop, prov_cell_t *grid,
                         proven_size_t ul_start, proven_size_t ul_end,
                         const proven_u8 *needle, proven_size_t needlelen, bool fold,
                         struct prov_regex *rx, const prov_block_sel_t *blk,
                         bool wrap_off, proven_size_t leftcol, bool word_wrap,
                         const proven_u8 *fgmap, proven_size_t fg_base, proven_size_t fg_len) {
    const prov_buffer_t *b = prov_editor_buffer(ed);
    if (wrap_off) word_wrap = false;       /* off and word are mutually exclusive */
    if (!wrap_off) leftcol = 0;
    proven_size_t Wword = (word_wrap && cols >= 2) ? cols - 1 : cols;  /* reserve marker col */
    bool ww_mark = word_wrap && cols >= 2;
    proven_size_t doc_total = prov_buffer_byte_len(b);
    proven_size_t match_end = 0;          /* bytes < match_end are inside a search match */
    proven_size_t lc = prov_buffer_line_count(b);
    prov_selection_t sel = prov_editor_selection(ed);
    if (tabstop == 0) tabstop = 1;
    if (cols == 0) return;

    for (proven_size_t i = 0; i < rows * cols; i++)
        grid[i] = (prov_cell_t){ .cp = 0x20 };

    /* Lines wrap onto further screen rows instead of being truncated: a char at
     * visual column `visx` of its line lands at screen (sr + visx/cols, visx%cols). */
    proven_size_t sr = 0;                  /* current screen row */
    for (proven_size_t line = top_line; sr < rows; line++) {
        if (line >= lc) {                  /* past EOF: a vi-style ~ marker, one row */
            grid[sr * cols].cp = '~';
            sr++;
            continue;
        }
        proven_size_t pos = prov_buffer_line_start(b, line);
        proven_size_t end = line_end_of(b, line);
        proven_size_t visx = 0;
        bool blk_row = blk && blk->c0 < blk->c1 && line >= blk->r0 && line <= blk->r1;

        /* word-wrap row breaks (line-relative visx of each row start); single row
         * otherwise. `wr` tracks the current row as visx grows. */
        proven_size_t wb[256]; wb[0] = 0; int nwb = 1, wr = 0;
        if (word_wrap) nwb = wordwrap_breaks(b, line, Wword, tabstop, wb, 256);

        /* regex highlight: match ranges for this visible line (doc-relative) */
        proven_size_t mr[128]; int nmr = 0;
        if (rx) {
            proven_u8 lb[2048];
            proven_size_t llen = end - pos;
            if (llen > sizeof lb) llen = sizeof lb;
            prov_buffer_copy_range(b, pos, llen, lb, llen);
            proven_size_t off = 0;
            while (nmr < 64 && off <= llen) {
                prov_regex_match_t mm;
                if (!prov_regex_search(rx, lb, llen, off, &mm)) break;
                mr[2 * nmr] = pos + mm.start; mr[2 * nmr + 1] = pos + mm.end; nmr++;
                off = mm.end > mm.start ? mm.end : mm.start + 1;
            }
        }

        /* place visual column VX (advancing `wr` first), clipped per mode.
         * `fgb` is the biased highlight fg for the current document byte `pos`. */
        #define FG_AT(BYTE) ((fgmap && (BYTE) >= fg_base && (BYTE) - fg_base < fg_len) \
                             ? fgmap[(BYTE) - fg_base] : (proven_u8)0)
        #define PLACE(VX, CP, CONT) do { \
            if (word_wrap) while (wr + 1 < nwb && (VX) >= wb[wr + 1]) wr++; \
            proven_size_t rr_, sc_; \
            if (cell_pos(wrap_off, word_wrap, sr, cols, leftcol, wb, wr, (VX), &rr_, &sc_)) \
                put_cell_rc(grid, rows, cols, rr_, sc_, (CP), (CONT), here, ul, mt, FG_AT(pos)); \
        } while (0)
        /* current screen-row offset within this line (for the off-the-bottom break) */
        #define CROW() (word_wrap ? (proven_size_t)wr : (wrap_off ? (proven_size_t)0 : visx / cols))

        while (pos < end) {
            prov_decode_t d = decode_at(b, pos);
            proven_u32 cp = d.cp;
            proven_size_t adv = d.len ? d.len : 1;
            bool here = (sel.active && pos >= sel.start && pos < sel.end)
                        || (blk_row && visx >= blk->c0 && visx < blk->c1);   /* block: visual columns */
            bool ul = ul_start < ul_end && pos >= ul_start && pos < ul_end;
            if (needle && needlelen && needlelen <= 256 && pos >= match_end
                && pos + needlelen <= doc_total) {
                proven_u8 nb[256];
                prov_buffer_copy_range(b, pos, needlelen, nb, needlelen);
                if (prov_match_at(nb, needle, needlelen, 0, fold)) match_end = pos + needlelen;
            }
            bool mt = needle && pos < match_end;
            if (rx) { for (int q = 0; q < nmr; q++) if (pos >= mr[2 * q] && pos < mr[2 * q + 1]) { mt = true; break; } }
            if (word_wrap) while (wr + 1 < nwb && visx >= wb[wr + 1]) wr++;
            if (!wrap_off && sr + CROW() >= rows) break;        /* ran off the bottom */

            if (cp == 0x09) {                          /* tab: pad to the next stop */
                proven_size_t next = ((visx / tabstop) + 1) * tabstop;
                while (visx < next) {
                    if (word_wrap) while (wr + 1 < nwb && visx >= wb[wr + 1]) wr++;
                    if (!wrap_off && sr + CROW() >= rows) break;
                    PLACE(visx, 0x20, false);
                    visx++;
                }
                pos += adv;
                continue;
            }

            cp = display_cp(cp);
            int w = prov_char_width(cp);
            if (w == 0) { pos += adv; continue; }   /* drop zero-width */

            /* Char-wrap only: a wide char that would straddle the right edge gets a
             * 1-cell pad so it never spills a half-glyph; it then lands on the next
             * row. (Word-wrap breaks before it; horizontal scroll clips instead.) */
            if (!wrap_off && !word_wrap && w == 2 && cols > 1 && (visx % cols) == cols - 1) {
                PLACE(visx, 0x20, false);
                visx++;
            }

            PLACE(visx, cp, false);
            if (w == 2) PLACE(visx + 1, 0, true);
            visx += (w == 2) ? 2 : 1;
            pos += adv;
        }

        /* word-wrap continuation marker: a reverse `<` in the reserved last column
         * of every row that is followed by more of this logical line. */
        if (ww_mark)
            for (int r = 0; r + 1 < nwb; r++) {
                proven_size_t rr = sr + (proven_size_t)r;
                if (rr >= rows) break;
                prov_cell_t *m = &grid[rr * cols + (cols - 1)];
                m->cp = '<'; m->cont = false; m->attr |= PROV_ATTR_REVERSE;
            }

        #undef PLACE
        #undef CROW
        sr += wrap_off ? 1                               /* one row per line when not wrapping */
                       : (word_wrap ? (proven_size_t)nwb            /* word-wrapped rows */
                                    : (visx == 0 ? 1 : (visx + cols - 1) / cols));  /* char-wrapped */
    }
}

prov_screen_pos_t prov_cursor_screen_pos(const prov_editor_t *ed,
                                         proven_size_t top_line,
                                         proven_size_t tabstop) {
    const prov_buffer_t *b = prov_editor_buffer(ed);
    if (tabstop == 0) tabstop = 1;

    proven_size_t line = prov_editor_cursor_line(ed);
    proven_size_t cur = prov_editor_cursor_byte(ed);
    proven_size_t pos = prov_buffer_line_start(b, line);
    proven_size_t visx = 0;

    while (pos < cur) {
        prov_decode_t d = decode_at(b, pos);
        proven_u32 cp = d.cp;
        proven_size_t adv = d.len ? d.len : 1;
        if (cp == 0x09) {
            visx = ((visx / tabstop) + 1) * tabstop;
        } else {
            int w = prov_char_width(display_cp(cp));
            visx += (w == 2) ? 2 : (w == 0 ? 0 : 1);
        }
        pos += adv;
    }

    prov_screen_pos_t p;
    p.row = (line >= top_line) ? line - top_line : 0;
    p.col = visx;
    return p;
}

prov_screen_pos_t prov_cursor_wrap_pos(const prov_editor_t *ed, proven_size_t top_line,
                                       proven_size_t cols, proven_size_t tabstop,
                                       bool wrap_off, proven_size_t leftcol, bool word_wrap) {
    prov_screen_pos_t p = { 0, 0 };
    if (cols == 0) return p;
    if (tabstop == 0) tabstop = 1;
    if (wrap_off) word_wrap = false;
    proven_size_t cline = prov_editor_cursor_line(ed);
    if (cline < top_line) return p;
    if (wrap_off) {                                  /* one row per line; clip columns */
        p.row = cline - top_line;
        proven_size_t cvx = prov_cursor_screen_pos(ed, top_line, tabstop).col;
        p.col = cvx >= leftcol ? cvx - leftcol : 0;
        return p;
    }
    const prov_buffer_t *b = prov_editor_buffer(ed);
    if (word_wrap) {                                 /* word-wrap: row/col from the break table */
        proven_size_t row = 0;
        for (proven_size_t L = top_line; L < cline; L++)
            row += prov_wrap_rows(ed, L, cols, tabstop, true);
        proven_size_t W = cols >= 2 ? cols - 1 : cols;
        proven_size_t wb[256];
        int nwb = wordwrap_breaks(b, cline, W, tabstop, wb, 256);
        proven_size_t cvx = prov_cursor_screen_pos(ed, top_line, tabstop).col;  /* line-relative */
        int wr = 0; while (wr + 1 < nwb && cvx >= wb[wr + 1]) wr++;
        p.row = row + (proven_size_t)wr;
        p.col = cvx - wb[wr];
        return p;
    }
    proven_size_t row = 0;
    for (proven_size_t L = top_line; L < cline; L++)
        row += prov_wrap_rows(ed, L, cols, tabstop, false);
    proven_size_t visx = wrapped_visx(b, cline, cols, tabstop, prov_editor_cursor_byte(ed));
    p.row = row + visx / cols;
    p.col = visx % cols;
    return p;
}

proven_size_t prov_hscroll_left(proven_size_t left, proven_size_t col,
                                proven_size_t width, proven_size_t scrolloff) {
    if (width == 0) return left;
    proven_size_t so = scrolloff;
    if (so * 2 >= width) so = width > 1 ? (width - 1) / 2 : 0;
    if (col < left + so) left = col > so ? col - so : 0;
    if (col + so >= left + width)
        left = (col + so + 1 >= width) ? col + so + 1 - width : 0;
    if (col < left) left = col;                      /* keep the cursor column itself visible */
    else if (col >= left + width) left = col - width + 1;
    return left;
}

proven_size_t prov_line_visual_width(const prov_editor_t *ed, proven_size_t line,
                                     proven_size_t tabstop) {
    if (tabstop == 0) tabstop = 1;
    const prov_buffer_t *b = prov_editor_buffer(ed);
    if (line >= prov_buffer_line_count(b)) return 0;
    return line_visual_width(b, line, tabstop);
}

proven_size_t prov_buffer_char_count(const prov_buffer_t *b) {
    return prov_buffer_char_total(b);   /* O(1) cached; was an O(n) decode walk */
}

proven_size_t prov_cursor_char_offset(const prov_editor_t *ed) {
    const prov_buffer_t *b = prov_editor_buffer(ed);
    proven_size_t cur = prov_editor_cursor_byte(ed), pos = 0, cnt = 0;
    while (pos < cur) { prov_decode_t d = decode_at(b, pos); pos += d.len ? d.len : 1; cnt++; }
    return cnt;
}

proven_size_t prov_scroll_top(proven_size_t top, proven_size_t line,
                              proven_size_t vis, proven_size_t scrolloff,
                              proven_size_t line_count) {
    if (vis == 0) return top;

    /* A margin only fits when the viewport has room for it on both sides. */
    proven_size_t so = scrolloff;
    if (so * 2 >= vis) so = vis > 1 ? (vis - 1) / 2 : 0;

    if (line < top + so)                         /* too close to the top edge */
        top = line > so ? line - so : 0;
    if (line + so >= top + vis) {                /* too close to the bottom edge */
        proven_size_t want = (line + so + 1 >= vis) ? line + so + 1 - vis : 0;
        proven_size_t cap = line_count > vis ? line_count - vis : 0;  /* last line at bottom */
        top = want < cap ? want : cap;
    }

    /* Belt and suspenders: keep the cursor itself on screen regardless of so. */
    if (line < top) top = line;
    else if (line >= top + vis) top = line - vis + 1;
    return top;
}

/* Display width of `s` (wide glyphs = 2; invalid bytes = 1 each). */
proven_size_t prov_str_disp_width(const char *s) {
    proven_size_t i = 0, len = 0; while (s[len]) len++;
    proven_size_t w = 0;
    while (s[i]) {
        prov_decode_t d = prov_utf8_decode((const proven_u8 *)s + i, len - i);
        proven_u32 cp = d.valid ? d.cp : (proven_u8)s[i];
        int cw = prov_char_width(cp); if (cw < 1) cw = 1; if (cw > 2) cw = 2;
        w += (proven_size_t)cw;
        i += d.len ? d.len : 1;
    }
    return w;
}

int prov_gutter_width(proven_size_t line_count) {
    int digits = 1;
    for (proven_size_t n = line_count; n >= 10; n /= 10) digits++;
    if (digits < 3) digits = 3;        /* stable minimum for small files */
    return digits + 1;                 /* + trailing separator space */
}

/* Bytes of `s` whose glyphs together occupy at most `cells` display columns
 * (whole code points only — a glyph is taken in full or not at all, so a
 * multibyte char or a wide glyph is never split at the boundary). Writes the
 * columns actually used to *used. `slen` = strlen(s). */
static proven_size_t fit_bytes(const char *s, proven_size_t slen, int cells, int *used) {
    proven_size_t i = 0; int u = 0;
    while (i < slen) {
        prov_decode_t d = prov_utf8_decode((const proven_u8 *)s + i, slen - i);
        proven_u32 cp = d.valid ? d.cp : (proven_u8)s[i];
        int cw = prov_char_width(cp); if (cw < 1) cw = 1; if (cw > 2) cw = 2;
        if (u + cw > cells) break;
        u += cw;
        i += d.len ? d.len : 1;
    }
    if (used) *used = u;
    return i;
}

proven_size_t prov_fit_field(char *dst, proven_size_t cap, const char *src,
                             int cells, bool left) {
    if (cap == 0) return 0;
    if (cells < 0) cells = 0;
    proven_size_t slen = 0; while (src[slen]) slen++;
    int used = 0;
    proven_size_t bytes = fit_bytes(src, slen, cells, &used);
    int pad = cells - used; if (pad < 0) pad = 0;
    proven_size_t o = 0;
    #define PUT(ch) do { if (o + 1 < cap) dst[o] = (char)(ch); o++; } while (0)
    if (!left) for (int k = 0; k < pad; k++) PUT(' ');
    for (proven_size_t k = 0; k < bytes; k++) PUT(src[k]);
    if (left)  for (int k = 0; k < pad; k++) PUT(' ');
    #undef PUT
    if (o >= cap) o = cap - 1;
    dst[o] = '\0';
    return o;
}

void prov_abbreviate_filename(const char *name, proven_size_t maxw,
                              char *out, proven_size_t outcap) {
    if (outcap == 0) return;
    if (maxw == 0) { out[0] = '\0'; return; }
    proven_size_t len = 0; while (name[len]) len++;
    proven_size_t o = 0;
    #define PUT(ch) do { if (o + 1 < outcap) out[o++] = (char)(ch); } while (0)

    if (prov_str_disp_width(name) <= maxw) {              /* fits as-is */
        for (proven_size_t i = 0; i < len; i++) PUT(name[i]);
        out[o] = '\0'; return;
    }

    /* extension = the last '.' that is not the leading character (byte index). */
    proven_size_t dot = len;
    for (proven_size_t i = 1; i < len; i++)
        if (name[i] == '.') dot = i;
    proven_size_t extw = (dot < len) ? prov_str_disp_width(name + dot) : 0;

    if (maxw < 4 || extw + 3 >= maxw) {                   /* no room for "...ext": head + "..." */
        int hw = (maxw <= 3) ? (int)maxw : (int)maxw - 3, used = 0;
        proven_size_t hb = fit_bytes(name, len, hw, &used);
        for (proven_size_t i = 0; i < hb; i++) PUT(name[i]);
        if (maxw > 3) { PUT('.'); PUT('.'); PUT('.'); }
    } else {                                              /* head + "..." + ext */
        int hw = (int)maxw - 3 - (int)extw, used = 0;
        proven_size_t hb = fit_bytes(name, dot, hw, &used);
        for (proven_size_t i = 0; i < hb; i++) PUT(name[i]);
        PUT('.'); PUT('.'); PUT('.');
        for (proven_size_t i = dot; i < len; i++) PUT(name[i]);
    }
    out[o] = '\0';
    #undef PUT
}

const char *prov_basename(const char *path) {
    if (!path) return "";
    proven_u8str_view_t v = proven_u8str_view_from_cstr(path);
    proven_size_t start = 0;
    for (proven_size_t i = 0; i < v.size; i++)
        if (v.ptr[i] == '/') start = i + 1;
    return path + start;
}
