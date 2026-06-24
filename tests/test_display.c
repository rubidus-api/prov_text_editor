/*
 * Unit tests for the pure renderer (editor state -> cell grid). No terminal.
 * One main(), exit 0 == pass.
 */

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"
#include "proven/allocator.h"
#include "editor.h"
#include "display.h"

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static const proven_u8 *U(const char *s) { return (const proven_u8 *)s; }

/* assert a cell's code point */
#define CELL(grid, cols, r, c) ((grid)[(r) * (cols) + (c)].cp)

int main(void) {
    proven_allocator_t a = proven_heap_allocator();
    enum { ROWS = 3, COLS = 5 };
    prov_cell_t grid[ROWS * COLS];

    /* ---- ASCII + blank fill + nonexistent lines ---- */
    prov_result_editor_t r = prov_editor_create(a);
    prov_editor_t *ed = r.value;
    prov_editor_insert(ed, U("ab"), 2);
    prov_render_into(ed, 0, ROWS, COLS, 4, grid);
    CHECK(CELL(grid, COLS, 0, 0) == 'a', "row0 col0 a");
    CHECK(CELL(grid, COLS, 0, 1) == 'b', "row0 col1 b");
    CHECK(CELL(grid, COLS, 0, 2) == 0x20, "row0 col2 blank");
    /* rows past the last buffer line carry a '~' end-of-file marker (RFC 0002) */
    CHECK(CELL(grid, COLS, 1, 0) == '~', "row1 (past EOF) ~ marker");
    CHECK(CELL(grid, COLS, 1, 1) == 0x20, "row1 col1 blank after ~");
    CHECK(CELL(grid, COLS, 2, 0) == '~', "row2 (past EOF) ~ marker");
    CHECK(CELL(grid, COLS, 2, 4) == 0x20, "row2 col4 blank");
    prov_editor_destroy(ed);

    /* ---- wide char occupies two cells (head + continuation) ---- */
    r = prov_editor_create(a);
    ed = r.value;
    prov_editor_insert(ed, U("\xED\x95\x9C" "x"), 4);   /* "한x" */
    prov_render_into(ed, 0, ROWS, COLS, 4, grid);
    CHECK(CELL(grid, COLS, 0, 0) == 0xD55C, "wide head 한");
    CHECK(grid[1].cont == true && grid[1].cp == 0, "wide continuation cell");
    CHECK(CELL(grid, COLS, 0, 2) == 'x', "x after wide char");
    prov_editor_destroy(ed);

    /* ---- tab expands to next tabstop ---- */
    r = prov_editor_create(a);
    ed = r.value;
    prov_editor_insert(ed, U("a\tb"), 3);
    prov_render_into(ed, 0, ROWS, COLS, 4, grid);
    CHECK(CELL(grid, COLS, 0, 0) == 'a', "tab row a");
    CHECK(CELL(grid, COLS, 0, 1) == 0x20, "tab fill 1");
    CHECK(CELL(grid, COLS, 0, 2) == 0x20, "tab fill 2");
    CHECK(CELL(grid, COLS, 0, 3) == 0x20, "tab fill 3");
    CHECK(CELL(grid, COLS, 0, 4) == 'b', "b at column 4");

    /* cursor screen position with tab expansion */
    prov_editor_move_end(ed);                            /* after b, byte 3 */
    prov_screen_pos_t p = prov_cursor_screen_pos(ed, 0, 4);
    CHECK(p.row == 0 && p.col == 5, "cursor col after tab+b");
    prov_editor_destroy(ed);

    /* ---- vertical scroll via top_line ---- */
    r = prov_editor_create(a);
    ed = r.value;
    prov_editor_insert(ed, U("L0\nL1\nL2\nL3"), 11);
    prov_render_into(ed, 2, ROWS, COLS, 4, grid);        /* start at line 2 */
    CHECK(CELL(grid, COLS, 0, 0) == 'L' && CELL(grid, COLS, 0, 1) == '2',
          "scrolled row0 == L2");
    CHECK(CELL(grid, COLS, 1, 1) == '3', "scrolled row1 == L3");
    /* cursor is at end (line 3); with top_line 2 it is on viewport row 1 */
    p = prov_cursor_screen_pos(ed, 2, 4);
    CHECK(p.row == 1 && p.col == 2, "cursor row after scroll");
    prov_editor_destroy(ed);

    /* ---- selection highlight ---- */
    r = prov_editor_create(a);
    ed = r.value;
    prov_editor_insert(ed, U("abcde"), 5);
    prov_editor_move_home(ed);
    prov_editor_set_extending(ed, true);
    prov_editor_move_right(ed);
    prov_editor_move_right(ed);
    prov_editor_move_right(ed);                          /* select [0,3) = abc */
    prov_render_into(ed, 0, ROWS, COLS, 4, grid);
    CHECK(grid[0].selected && grid[1].selected && grid[2].selected,
          "abc cells selected");
    CHECK(!grid[3].selected && !grid[4].selected, "d,e not selected");
    CHECK(!grid[5].selected, "blank not selected");
    CHECK(CELL(grid, COLS, 0, 0) == 'a', "selected cell still shows glyph");
    prov_editor_destroy(ed);

    /* ---- prov_scroll_top: viewport follow + scrolloff ---- */
    /* No margin (scrolloff 0): snap just enough to keep the cursor visible. */
    CHECK(prov_scroll_top(0, 0, 10, 0, 100) == 0,  "so0: cursor in view -> unchanged");
    CHECK(prov_scroll_top(0, 9, 10, 0, 100) == 0,  "so0: last visible row -> unchanged");
    CHECK(prov_scroll_top(0, 10, 10, 0, 100) == 1, "so0: one past bottom -> scroll 1");
    CHECK(prov_scroll_top(20, 5, 10, 0, 100) == 5, "so0: cursor above top -> snap to cursor");
    CHECK(prov_scroll_top(0, 99, 10, 0, 100) == 90, "so0: last line sits at bottom");

    /* Margin (scrolloff 2): keep 2 rows above/below when possible. */
    CHECK(prov_scroll_top(0, 7, 10, 2, 100) == 0,  "so2: within margin -> unchanged");
    CHECK(prov_scroll_top(0, 8, 10, 2, 100) == 1,  "so2: enters bottom margin -> scroll");
    CHECK(prov_scroll_top(10, 11, 10, 2, 100) == 9, "so2: enters top margin -> scroll up");
    /* Margin never scrolls blank space past the last line into view. */
    CHECK(prov_scroll_top(80, 99, 10, 2, 100) == 90, "so2: clamps to last line at bottom");
    /* Oversized scrolloff is capped to fit the viewport (no oscillation). */
    CHECK(prov_scroll_top(0, 5, 4, 9, 100) <= 5 && prov_scroll_top(0, 5, 4, 9, 100) >= 2,
          "huge so capped, cursor still visible");
    CHECK(prov_scroll_top(7, 7, 0, 0, 100) == 7, "vis 0 -> unchanged");

    /* ---- prov_abbreviate_filename ---- */
    char ab[64];
    prov_abbreviate_filename("notes.txt", 20, ab, sizeof ab);
    CHECK(strcmp(ab, "notes.txt") == 0, "abbrev: fits unchanged");
    prov_abbreviate_filename("verylongname.txt", 12, ab, sizeof ab);
    CHECK(strlen(ab) == 12 && strstr(ab, "...") && strstr(ab, ".txt"),
          "abbrev: keeps ext + ... within width");
    prov_abbreviate_filename("noextensionverylong", 12, ab, sizeof ab);
    CHECK(strlen(ab) == 12 && ab[11] == '.' && ab[9] == '.', "abbrev: no-ext head + ...");
    prov_abbreviate_filename("anything", 3, ab, sizeof ab);
    CHECK(strlen(ab) == 3, "abbrev: tiny width hard-truncates");

    /* ---- prov_basename ---- */
    CHECK(strcmp(prov_basename("/a/b/c.txt"), "c.txt") == 0, "basename: nested path");
    CHECK(strcmp(prov_basename("file.c"), "file.c") == 0, "basename: no slash");
    CHECK(strcmp(prov_basename("/trailing/"), "") == 0, "basename: trailing slash -> empty");
    CHECK(strcmp(prov_basename("/root"), "root") == 0, "basename: leading slash only");
    CHECK(strcmp(prov_basename(""), "") == 0, "basename: empty string");

    /* ---- soft wrap: long lines continue on the next screen row ---- */
    r = prov_editor_create(a);
    ed = r.value;
    prov_editor_insert(ed, U("abcdefgh"), 8);          /* 8 chars in a 5-wide viewport */
    CHECK(prov_wrap_rows(ed, 0, COLS, 4, false) == 2, "wrap_rows: 8 cols / 5 wide = 2 rows");
    prov_render_into(ed, 0, ROWS, COLS, 4, grid);
    CHECK(CELL(grid, COLS, 0, 0) == 'a' && CELL(grid, COLS, 0, 4) == 'e', "wrap: row0 = abcde");
    CHECK(CELL(grid, COLS, 1, 0) == 'f' && CELL(grid, COLS, 1, 2) == 'h', "wrap: row1 = fgh");
    CHECK(CELL(grid, COLS, 2, 0) == '~', "wrap: EOF marker after the wrapped line");
    /* cursor at the end (byte 8) -> wrapped row 1, col 3 */
    prov_screen_pos_t wp = prov_cursor_wrap_pos(ed, 0, COLS, 4, false, 0, false);
    CHECK(wp.row == 1 && wp.col == 3, "wrap: cursor at col 8 -> row1 col3");
    CHECK(prov_wrap_rows(ed, 5, COLS, 4, false) == 1, "wrap_rows: nonexistent line = 1");
    prov_editor_destroy(ed);

    /* ---- a wide char that would straddle the right edge pads + wraps ---- */
    r = prov_editor_create(a);
    ed = r.value;
    prov_editor_insert(ed, U("abcd\xED\x95\x9C"), 7);    /* "abcd한", 한 lands at col 4 of 5 */
    prov_render_into(ed, 0, ROWS, COLS, 4, grid);
    CHECK(CELL(grid, COLS, 0, 4) == 0x20, "wide-at-edge: last cell left blank");
    CHECK(CELL(grid, COLS, 1, 0) == 0xD55C, "wide-at-edge: 한 head on the next row");
    CHECK(grid[1 * COLS + 1].cont, "wide-at-edge: continuation after the head");
    prov_editor_destroy(ed);

    /* ---- field-mode region underline (RFC-0007): [2,5) of "abcdef" ---- */
    r = prov_editor_create(a);
    ed = r.value;
    prov_editor_insert(ed, U("abcdef"), 6);
    prov_render_into_ul(ed, 0, ROWS, COLS, 4, grid, 2, 5);
    CHECK((grid[0].attr & PROV_ATTR_UNDERLINE) == 0, "col0 not underlined");
    CHECK((grid[1].attr & PROV_ATTR_UNDERLINE) == 0, "col1 not underlined");
    CHECK((grid[2].attr & PROV_ATTR_UNDERLINE) != 0, "col2 underlined (region start)");
    CHECK((grid[4].attr & PROV_ATTR_UNDERLINE) != 0, "col4 underlined (region)");
    CHECK((grid[5].attr & PROV_ATTR_UNDERLINE) == 0, "col5 not underlined (region end exclusive)");
    /* empty range underlines nothing */
    prov_render_into_ul(ed, 0, ROWS, COLS, 4, grid, 3, 3);
    CHECK((grid[3].attr & PROV_ATTR_UNDERLINE) == 0, "empty range: no underline");
    prov_editor_destroy(ed);

    /* ---- search match highlight (M4.5): "ab" in "xabyab" -> cols 1-2 and 4-5 ---- */
    r = prov_editor_create(a);
    ed = r.value;
    prov_editor_insert(ed, U("xabyab"), 6);
    prov_render_into_full(ed, 0, ROWS, COLS, 4, grid, 0, 0, U("ab"), 2, false, NULL, NULL, false, 0, false);
    CHECK((grid[0].attr & PROV_ATTR_MATCH) == 0, "col0 'x' not matched");
    CHECK((grid[1].attr & PROV_ATTR_MATCH) != 0, "col1 'a' matched");
    CHECK((grid[2].attr & PROV_ATTR_MATCH) != 0, "col2 'b' matched");
    CHECK((grid[3].attr & PROV_ATTR_MATCH) == 0, "col3 'y' not matched");
    CHECK((grid[4].attr & PROV_ATTR_MATCH) != 0, "col4 'a' (2nd match) matched");
    CHECK((grid[5].attr & PROV_ATTR_MATCH) != 0, "col5 'b' (2nd match) matched");
    /* NULL needle = no match attr */
    prov_render_into_full(ed, 0, ROWS, COLS, 4, grid, 0, 0, NULL, 0, false, NULL, NULL, false, 0, false);
    CHECK((grid[1].attr & PROV_ATTR_MATCH) == 0, "NULL needle: no highlight");
    prov_editor_destroy(ed);

    /* ---- horizontal scroll (wrap=off): one row per line, window [leftcol, +COLS) ---- */
    r = prov_editor_create(a);
    ed = r.value;
    prov_editor_insert(ed, U("0123456789"), 10);    /* one long line, no '\n' */
    /* leftcol=0: shows cols 0..4 of the single row; no wrap to row 1 */
    prov_render_into_full(ed, 0, ROWS, COLS, 4, grid, 0, 0, NULL, 0, false, NULL, NULL, true, 0, false);
    CHECK(CELL(grid, COLS, 0, 0) == '0' && CELL(grid, COLS, 0, 4) == '4', "hscroll left=0: row0 = 01234");
    CHECK(CELL(grid, COLS, 1, 0) == '~', "hscroll: no wrap — row1 is the EOF marker");
    /* leftcol=3: window starts at visual col 3 -> shows 3..7 */
    prov_render_into_full(ed, 0, ROWS, COLS, 4, grid, 0, 0, NULL, 0, false, NULL, NULL, true, 3, false);
    CHECK(CELL(grid, COLS, 0, 0) == '3' && CELL(grid, COLS, 0, 4) == '7', "hscroll left=3: row0 = 34567");
    /* cursor (byte 10, col 10) with leftcol=6 -> col 10-6 = 4 on row 0 */
    prov_screen_pos_t hp = prov_cursor_wrap_pos(ed, 0, COLS, 4, true, 6, false);
    CHECK(hp.row == 0 && hp.col == 4, "hscroll: cursor col10, left6 -> row0 col4");
    prov_editor_destroy(ed);

    {   /* prov_hscroll_left keeps the cursor column within [left, left+width) */
        CHECK(prov_hscroll_left(0, 3, 10, 0) == 0, "hscroll_left: col in view -> unchanged");
        CHECK(prov_hscroll_left(0, 12, 10, 0) == 3, "hscroll_left: col past right -> scroll");
        CHECK(prov_hscroll_left(8, 2, 10, 0) == 2, "hscroll_left: col left of origin -> scroll back");
    }

    /* ---- word-wrap (wrap=word): break at word boundaries + reverse `<` marker ---- */
    r = prov_editor_create(a);
    ed = r.value;
    prov_editor_insert(ed, U("aa bbbb cc"), 10);   /* COLS=5 -> W=4 (marker col reserved) */
    prov_render_into_full(ed, 0, ROWS, COLS, 4, grid, 0, 0, NULL, 0, false, NULL, NULL, false, 0, true);
    CHECK(CELL(grid, COLS, 0, 0) == 'a' && CELL(grid, COLS, 0, 1) == 'a', "wordwrap: row0 = aa");
    CHECK(CELL(grid, COLS, 1, 0) == 'b' && CELL(grid, COLS, 1, 3) == 'b', "wordwrap: 'bbbb' wrapped whole to row1");
    CHECK(CELL(grid, COLS, 2, 0) == 'c' && CELL(grid, COLS, 2, 1) == 'c', "wordwrap: 'cc' on row2");
    CHECK(CELL(grid, COLS, 0, 4) == '<' && (grid[0 * COLS + 4].attr & PROV_ATTR_REVERSE),
          "wordwrap: reverse '<' marker on continued row0");
    CHECK(CELL(grid, COLS, 1, 4) == '<', "wordwrap: marker on continued row1");
    CHECK(CELL(grid, COLS, 2, 4) != '<', "wordwrap: no marker on the last row");
    CHECK(prov_wrap_rows(ed, 0, COLS, 4, true) == 3, "wordwrap: 3 rows for 'aa bbbb cc'");
    {   /* cursor at end (byte 10, visual col 10) -> row2, col 10-8=2 */
        prov_screen_pos_t wwp = prov_cursor_wrap_pos(ed, 0, COLS, 4, false, 0, true);
        CHECK(wwp.row == 2 && wwp.col == 2, "wordwrap: cursor at end -> row2 col2");
    }
    prov_editor_destroy(ed);

    /* ---- width-aware, code-point-safe field helpers (browser rows + status) ---- */
    {
        char out[64];
        /* display width: ASCII = 1/char, CJK = 2/char, Korean = 2/char */
        CHECK(prov_str_disp_width("abc") == 3, "disp_width ascii");
        CHECK(prov_str_disp_width("\xed\x95\x9c\xea\xb8\x80") == 4, "disp_width korean (2 wide glyphs)");
        CHECK(prov_str_disp_width("a\xe6\x97\xa5""b") == 4, "disp_width mixed (CJK=2)");

        /* fit_field: pads ASCII to the cell width, left/right */
        proven_size_t n = prov_fit_field(out, sizeof out, "ab", 5, true);
        CHECK(n == 5 && memcmp(out, "ab   ", 5) == 0, "fit ascii left-pad");
        prov_fit_field(out, sizeof out, "ab", 5, false);
        CHECK(memcmp(out, "   ab", 5) == 0, "fit ascii right-pad");

        /* fit_field: a wide glyph is taken whole or not at all (never split). One
         * Korean syllable (3 bytes, 2 cells) in a 3-cell field -> glyph + 1 pad. */
        prov_fit_field(out, sizeof out, "\xed\x95\x9c", 3, true);
        CHECK(memcmp(out, "\xed\x95\x9c ", 4) == 0, "fit one wide glyph + pad");

        /* a 2-cell field can't hold a 2-cell glyph plus anything; exactly fits one */
        prov_fit_field(out, sizeof out, "\xed\x95\x9c\xea\xb8\x80", 2, true);
        CHECK(memcmp(out, "\xed\x95\x9c", 3) == 0 && out[3] == '\0', "fit clips to whole wide glyph");

        /* a 3-cell field holding two 2-cell glyphs: only the first fits (2 cells),
         * 1 cell padded — the second glyph is NOT split. */
        prov_fit_field(out, sizeof out, "\xed\x95\x9c\xea\xb8\x80", 3, true);
        CHECK(memcmp(out, "\xed\x95\x9c ", 4) == 0 && out[4] == '\0', "fit never splits a wide glyph");

        /* tiny cap is respected (no overflow), still NUL-terminated */
        char tiny[4];
        prov_fit_field(tiny, sizeof tiny, "abcdef", 6, true);
        CHECK(tiny[3] == '\0', "fit respects cap");

        /* abbreviate: short name unchanged; long name keeps head + ... + ext */
        prov_abbreviate_filename("short.txt", 20, out, sizeof out);
        CHECK(strcmp(out, "short.txt") == 0, "abbrev short unchanged");
        prov_abbreviate_filename("verylongfilename.txt", 12, out, sizeof out);
        CHECK(prov_str_disp_width(out) <= 12 && strstr(out, "...") && strstr(out, ".txt"), "abbrev head...ext fits");

        /* abbreviate never splits a multibyte char: a Korean name clipped stays valid UTF-8.
         * Re-measuring the width must not exceed maxw, and decoding must not see a stray byte. */
        prov_abbreviate_filename("\xed\x95\x9c\xea\xb8\x80\xed\x8c\x8c\xec\x9d\xbc.txt", 8, out, sizeof out);
        CHECK(prov_str_disp_width(out) <= 8, "abbrev korean within width");
        /* every byte of out is part of a complete code point (no lone continuation at the end) */
        CHECK((out[strlen(out) - 1] & 0xC0) != 0x80 || (unsigned char)out[strlen(out)-1] < 0x80,
              "abbrev ends on a code-point boundary");
    }

    {   /* line-number gutter width: digits of the max line + a separator space, min 3 digits */
        CHECK(prov_gutter_width(1) == 4, "gutter min width (1 line) = 3+1");
        CHECK(prov_gutter_width(99) == 4, "gutter 99 = 3+1");
        CHECK(prov_gutter_width(999) == 4, "gutter 999 = 3+1");
        CHECK(prov_gutter_width(1000) == 5, "gutter 1000 = 4+1");
        CHECK(prov_gutter_width(12345) == 6, "gutter 12345 = 5+1");
    }

    if (failures) {
        fprintf(stderr, "display: %d checks failed\n", failures);
        return 1;
    }
    printf("ok: display tests passed\n");
    return 0;
}
