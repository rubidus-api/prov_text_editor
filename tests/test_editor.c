/*
 * Unit tests for the editor layer: cursor, movement, edits, undo/redo.
 * Backend-agnostic; no terminal involved. One main(), exit 0 == pass.
 */

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"
#include "proven/allocator.h"
#include "editor.h"
#include "buffer.h"

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static const proven_u8 *U(const char *s) { return (const proven_u8 *)s; }

static int content_is(const prov_editor_t *ed, const char *expect) {
    const prov_buffer_t *b = prov_editor_buffer(ed);
    proven_u8 tmp[256];
    proven_size_t n =
        prov_buffer_copy_range(b, 0, prov_buffer_byte_len(b), tmp, sizeof tmp);
    return n == strlen(expect) && memcmp(tmp, expect, n) == 0;
}

/* shorthand: assert cursor byte/line/col */
static void check_pos(const prov_editor_t *ed, proven_size_t byte,
                      proven_size_t line, proven_size_t col, const char *msg) {
    if (prov_editor_cursor_byte(ed) != byte ||
        prov_editor_cursor_line(ed) != line ||
        prov_editor_cursor_col(ed) != col) {
        fprintf(stderr, "FAIL: %s -> byte=%zu line=%zu col=%zu "
                        "(want %zu/%zu/%zu)\n",
                msg, (size_t)prov_editor_cursor_byte(ed),
                (size_t)prov_editor_cursor_line(ed),
                (size_t)prov_editor_cursor_col(ed),
                (size_t)byte, (size_t)line, (size_t)col);
        failures++;
    }
}

int main(void) {
    proven_allocator_t a = proven_heap_allocator();

    /* ---- empty editor ---- */
    prov_result_editor_t r = prov_editor_create(a);
    CHECK(r.err == PROVEN_OK && r.value != NULL, "create editor");
    prov_editor_t *ed = r.value;
    check_pos(ed, 0, 0, 0, "empty cursor");

    /* ---- insert advances cursor ---- */
    CHECK(prov_editor_insert(ed, U("abc\nde\nf"), 8) == PROVEN_OK, "insert");
    CHECK(content_is(ed, "abc\nde\nf"), "content after insert");
    check_pos(ed, 8, 2, 1, "cursor at end");

    /* ---- home / left / vertical movement with goal column ---- */
    prov_editor_move_home(ed);
    check_pos(ed, 7, 2, 0, "home on line 2");

    prov_editor_move_left(ed);                 /* across line break */
    check_pos(ed, 6, 1, 2, "left to end of line 1");

    prov_editor_move_up(ed);                    /* goal col 2 -> line 0 col 2 */
    check_pos(ed, 2, 0, 2, "up keeps goal col 2");

    prov_editor_move_down(ed);                  /* back to line 1 col 2 */
    check_pos(ed, 6, 1, 2, "down keeps goal col 2");

    prov_editor_move_down(ed);                  /* line 2 only has col 0..1 */
    check_pos(ed, 8, 2, 1, "down clamps to short line");

    /* end / left clamp at start */
    prov_editor_move_home(ed);
    prov_editor_move_up(ed);
    prov_editor_move_up(ed);
    check_pos(ed, 0, 0, 0, "up clamps at top");
    prov_editor_move_left(ed);
    check_pos(ed, 0, 0, 0, "left clamps at start");
    prov_editor_move_end(ed);
    check_pos(ed, 3, 0, 3, "end of line 0");

    /* cursor_line_len: code points in the cursor's line, excluding newline */
    CHECK(prov_editor_cursor_line_len(ed) == 3, "line 0 len = 3 (abc)");
    prov_editor_move_down(ed);
    CHECK(prov_editor_cursor_line_len(ed) == 2, "line 1 len = 2 (de)");
    prov_editor_move_down(ed);
    CHECK(prov_editor_cursor_line_len(ed) == 1, "line 2 len = 1 (f)");

    prov_editor_destroy(ed);

    /* ---- multibyte movement (é is 2 bytes) ---- */
    r = prov_editor_create(a);
    ed = r.value;
    CHECK(prov_editor_insert(ed, U("a\xC3\xA9" "b"), 4) == PROVEN_OK, "insert aéb");
    prov_editor_move_home(ed);
    check_pos(ed, 0, 0, 0, "aéb home");
    prov_editor_move_right(ed);
    check_pos(ed, 1, 0, 1, "right over a");
    prov_editor_move_right(ed);                 /* over é (2 bytes, 1 col) */
    check_pos(ed, 3, 0, 2, "right over é");
    prov_editor_move_left(ed);                  /* back over é */
    check_pos(ed, 1, 0, 1, "left over é");

    /* backspace removes a whole code point */
    prov_editor_move_end(ed);                   /* byte 4 */
    prov_editor_backspace(ed);                  /* removes 'b' */
    CHECK(content_is(ed, "a\xC3\xA9"), "backspace b");
    prov_editor_backspace(ed);                  /* removes é (2 bytes) */
    CHECK(content_is(ed, "a"), "backspace é");
    check_pos(ed, 1, 0, 1, "cursor after backspace é");

    /* forward delete */
    prov_editor_move_home(ed);
    prov_editor_delete(ed);                     /* removes 'a' */
    CHECK(content_is(ed, ""), "forward delete a");
    check_pos(ed, 0, 0, 0, "cursor after delete");
    prov_editor_destroy(ed);

    /* ---- undo / redo with cursor restoration ---- */
    r = prov_editor_create(a);
    ed = r.value;
    prov_editor_insert(ed, U("hello"), 5);      /* "hello" cur5 */
    prov_editor_insert(ed, U("!"), 1);          /* "hello!" cur6 */

    CHECK(prov_editor_undo(ed), "undo !");
    CHECK(content_is(ed, "hello"), "undo ! content");
    check_pos(ed, 5, 0, 5, "undo ! cursor");

    CHECK(prov_editor_undo(ed), "undo hello");
    CHECK(content_is(ed, ""), "undo hello content");
    check_pos(ed, 0, 0, 0, "undo hello cursor");

    CHECK(prov_editor_redo(ed), "redo hello");
    CHECK(content_is(ed, "hello"), "redo hello content");
    check_pos(ed, 5, 0, 5, "redo hello cursor after text");

    CHECK(prov_editor_undo(ed), "undo again");  /* back to empty */
    CHECK(prov_editor_undo(ed) == false, "undo empty stack");

    /* delete undo restores text and cursor to the edit site */
    prov_editor_redo(ed);                        /* "hello" cur5 */
    prov_editor_move_home(ed);                   /* cur0 */
    prov_editor_delete(ed);                      /* remove 'h' -> "ello" */
    CHECK(content_is(ed, "ello"), "delete h");
    CHECK(prov_editor_undo(ed), "undo delete h");
    CHECK(content_is(ed, "hello"), "undo delete content");
    check_pos(ed, 0, 0, 0, "undo delete cursor at site");
    prov_editor_destroy(ed);

    /* ---- selection ---- */
    r = prov_editor_create(a);
    ed = r.value;
    prov_editor_insert(ed, U("hello world"), 11);   /* cur 11 */
    CHECK(!prov_editor_has_selection(ed), "no selection initially");

    prov_editor_move_home(ed);                       /* cur 0 */
    prov_editor_set_extending(ed, true);
    for (int n = 0; n < 5; n++) prov_editor_move_right(ed);   /* select [0,5) */
    CHECK(prov_editor_has_selection(ed), "shift-right selects");
    prov_selection_t sel = prov_editor_selection(ed);
    CHECK(sel.active && sel.start == 0 && sel.end == 5, "selection [0,5)");
    check_pos(ed, 5, 0, 5, "cursor at selection end");

    prov_editor_copy_selection(ed);                  /* register = "hello" */
    prov_editor_set_extending(ed, false);
    prov_editor_move_right(ed);                       /* plain move collapses */
    CHECK(!prov_editor_has_selection(ed), "plain move clears selection");

    /* select all + cut */
    prov_editor_select_all(ed);
    CHECK(prov_editor_has_selection(ed), "select all");
    prov_editor_cut_selection(ed);
    CHECK(content_is(ed, ""), "cut empties doc");
    check_pos(ed, 0, 0, 0, "cursor 0 after cut");

    /* paste the cut text back */
    prov_editor_paste(ed);
    CHECK(content_is(ed, "hello world"), "paste restores text");
    check_pos(ed, 11, 0, 11, "cursor after paste");

    /* typing replaces the selection */
    prov_editor_move_home(ed);
    prov_editor_set_extending(ed, true);
    for (int n = 0; n < 5; n++) prov_editor_move_right(ed);   /* select "hello" */
    prov_editor_set_extending(ed, false);
    prov_editor_insert(ed, U("Hi"), 2);
    CHECK(content_is(ed, "Hi world"), "typing replaces selection");
    check_pos(ed, 2, 0, 2, "cursor after replace");

    /* backspace deletes the selection */
    prov_editor_select_all(ed);
    prov_editor_backspace(ed);
    CHECK(content_is(ed, ""), "backspace deletes selection");
    prov_editor_destroy(ed);

    /* ---- register shape + linewise paste (RFC-0006 / M4.1) ---- */
    {
        prov_result_editor_t rr = prov_editor_create(a);
        prov_editor_t *e = rr.value;
        prov_editor_insert(e, U("a\nb"), 3);
        prov_editor_select_range(e, 0, 1); prov_editor_copy_selection(e);
        CHECK(prov_editor_reg_shape(e) == PROV_REG_CHAR, "copy => CHAR shape");
        prov_editor_reg_ensure_trailing_newline(e);
        CHECK(prov_editor_reg_shape(e) == PROV_REG_LINE, "ensure-newline => LINE shape");
        prov_editor_clear_selection(e);
        prov_editor_move_to(e, 0);
        prov_editor_paste_lines(e, true, 1);                 /* p below line 0 */
        CHECK(content_is(e, "a\na\nb"), "linewise p below");
        prov_editor_move_to(e, 0);
        prov_editor_paste_lines(e, false, 1);                /* P above line 0 */
        CHECK(content_is(e, "a\na\na\nb"), "linewise P above");
        prov_editor_destroy(e);
    }
    {
        prov_result_editor_t rr = prov_editor_create(a);
        prov_editor_t *e = rr.value;
        prov_editor_insert(e, U("x\ny"), 3);                 /* last line "y", no trailing \n */
        prov_editor_select_range(e, 0, 1); prov_editor_copy_selection(e);
        prov_editor_reg_ensure_trailing_newline(e); prov_editor_clear_selection(e);
        prov_editor_move_to(e, 2);                           /* on "y" (last line) */
        prov_editor_paste_lines(e, true, 1);
        CHECK(content_is(e, "x\ny\nx"), "p below last line: no trailing blank");
        prov_editor_move_to(e, 0);
        prov_editor_paste_lines(e, true, 3);                 /* [N]p = 3 copies */
        CHECK(content_is(e, "x\nx\nx\nx\ny\nx"), "3p below = 3 line copies");
        prov_editor_destroy(e);
    }
    {   /* register snapshot/restore (M4.2): copy out, overwrite, restore, paste */
        prov_result_editor_t rr = prov_editor_create(a);
        prov_editor_t *e = rr.value;
        prov_editor_insert(e, U("hello"), 5);
        prov_editor_select_range(e, 0, 3); prov_editor_copy_selection(e);   /* "hel", CHAR */
        CHECK(prov_editor_reg_len(e) == 3, "reg_len 3");
        proven_u8 snap[8];
        proven_size_t n = prov_editor_reg_copy(e, snap, sizeof snap);
        CHECK(n == 3 && snap[0] == 'h' && snap[2] == 'l', "reg_copy = hel");
        prov_reg_shape_t sh = prov_editor_reg_shape(e);
        prov_editor_reg_set(e, U("ZZ"), 2, PROV_REG_CHAR);
        CHECK(prov_editor_reg_len(e) == 2, "reg overwritten to 2");
        prov_editor_reg_set(e, snap, 3, sh);                               /* restore */
        prov_editor_clear_selection(e); prov_editor_move_to(e, 5);
        prov_editor_paste_lines(e, true, 1);
        CHECK(content_is(e, "hellohel"), "restore + paste = hellohel");
        prov_editor_destroy(e);
    }
    {   /* char shape pastes at the cursor (P == p), with count */
        prov_result_editor_t rr = prov_editor_create(a);
        prov_editor_t *e = rr.value;
        prov_editor_insert(e, U("XY"), 2);
        prov_editor_select_range(e, 0, 1); prov_editor_copy_selection(e);  /* reg "X", CHAR */
        prov_editor_clear_selection(e); prov_editor_move_to(e, 2);
        prov_editor_paste_lines(e, true, 2);                 /* 2p char => insert "XX" at cursor */
        CHECK(content_is(e, "XYXX"), "charwise [N]p inserts N copies at cursor");
        prov_editor_destroy(e);
    }

    if (failures) {
        fprintf(stderr, "editor: %d checks failed\n", failures);
        return 1;
    }
    printf("ok: editor tests passed\n");
    return 0;
}
