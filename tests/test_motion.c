/*
 * Unit tests for text motions and objects (pure, over a buffer).
 * One main(), exit 0 == pass.
 */

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"
#include "proven/allocator.h"
#include "buffer.h"
#include "motion.h"

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static const proven_u8 *U(const char *s) { return (const proven_u8 *)s; }

int main(void) {
    proven_allocator_t a = proven_heap_allocator();
    prov_buffer_t *b;

    /* ---- word motions on "foo bar baz" ---- */
    b = prov_buffer_create_from_bytes(a, U("foo bar baz"), 11).value;
    CHECK(prov_motion_word_next(b, 0) == 4, "word_next 0 -> 4");
    CHECK(prov_motion_word_next(b, 4) == 8, "word_next 4 -> 8");
    CHECK(prov_motion_word_next(b, 8) == 11, "word_next 8 -> end");
    CHECK(prov_motion_word_prev(b, 11) == 8, "word_prev end -> 8");
    CHECK(prov_motion_word_prev(b, 8) == 4, "word_prev 8 -> 4");
    CHECK(prov_motion_word_prev(b, 4) == 0, "word_prev 4 -> 0");
    CHECK(prov_motion_word_end(b, 0) == 3, "word_end 0 -> 3");
    CHECK(prov_motion_word_end(b, 4) == 7, "word_end 4 -> 7");
    prov_buffer_destroy(b);

    /* ---- find / till on "abc,def" ---- */
    b = prov_buffer_create_from_bytes(a, U("abc,def"), 7).value;
    CHECK(prov_motion_find(b, 0, ',', false) == 4, "find ',' includes -> 4");
    CHECK(prov_motion_find(b, 0, ',', true) == 3, "till ',' excludes -> 3");
    CHECK(prov_motion_find(b, 0, 'z', false) == 0, "find missing -> no-op");
    prov_buffer_destroy(b);

    /* ---- match on "(ab)" ---- */
    b = prov_buffer_create_from_bytes(a, U("(ab)"), 4).value;
    CHECK(prov_motion_match(b, 0) == 3, "match '(' -> ')'");
    CHECK(prov_motion_match(b, 3) == 0, "match ')' -> '('");
    CHECK(prov_motion_match(b, 1) == 1, "no match on non-bracket");
    prov_buffer_destroy(b);

    /* nested match "((x))" */
    b = prov_buffer_create_from_bytes(a, U("((x))"), 5).value;
    CHECK(prov_motion_match(b, 0) == 4, "nested outer ( -> )");
    CHECK(prov_motion_match(b, 1) == 3, "nested inner ( -> )");
    prov_buffer_destroy(b);

    /* ---- text object: word ---- */
    b = prov_buffer_create_from_bytes(a, U("foo bar"), 7).value;
    prov_range_t r = prov_motion_textobj(b, 1, PROV_TOBJ_WORD, true);
    CHECK(r.ok && r.start == 0 && r.end == 3, "iw -> foo [0,3)");
    r = prov_motion_textobj(b, 1, PROV_TOBJ_WORD, false);
    CHECK(r.ok && r.start == 0 && r.end == 4, "aw -> foo+ws [0,4)");
    prov_buffer_destroy(b);

    /* ---- text object: parens "x(ab)y" ---- */
    b = prov_buffer_create_from_bytes(a, U("x(ab)y"), 6).value;
    r = prov_motion_textobj(b, 3, PROV_TOBJ_PAREN, true);
    CHECK(r.ok && r.start == 2 && r.end == 4, "i( -> ab [2,4)");
    r = prov_motion_textobj(b, 3, PROV_TOBJ_PAREN, false);
    CHECK(r.ok && r.start == 1 && r.end == 5, "a( -> (ab) [1,5)");
    prov_buffer_destroy(b);

    /* ---- text object: dquote 'a"hi"b' ---- */
    b = prov_buffer_create_from_bytes(a, U("a\"hi\"b"), 6).value;
    r = prov_motion_textobj(b, 3, PROV_TOBJ_DQUOTE, true);
    CHECK(r.ok && r.start == 2 && r.end == 4, "i\" -> hi [2,4)");
    r = prov_motion_textobj(b, 3, PROV_TOBJ_DQUOTE, false);
    CHECK(r.ok && r.start == 1 && r.end == 5, "a\" -> \"hi\" [1,5)");
    prov_buffer_destroy(b);

    /* ---- text object: paragraph "p1a\np1b\n\np2a\n" ----
       lines: L0 p1a[0,4) L1 p1b[4,8) L2 (blank)[8,9) L3 p2a[9,13) */
    b = prov_buffer_create_from_bytes(a, U("p1a\np1b\n\np2a\n"), 13).value;
    r = prov_motion_textobj(b, 1, PROV_TOBJ_PARAGRAPH, true);
    CHECK(r.ok && r.start == 0 && r.end == 8, "ip -> first para [0,8)");
    r = prov_motion_textobj(b, 1, PROV_TOBJ_PARAGRAPH, false);
    CHECK(r.ok && r.start == 0 && r.end == 9, "ap -> para+blank [0,9)");
    r = prov_motion_textobj(b, 9, PROV_TOBJ_PARAGRAPH, true);
    CHECK(r.ok && r.start == 9 && r.end == 13, "ip in 2nd para [9,13)");
    r = prov_motion_textobj(b, 8, PROV_TOBJ_PARAGRAPH, true);
    CHECK(r.ok && r.start == 8 && r.end == 9, "ip on blank line [8,9)");
    prov_buffer_destroy(b);

    /* ---- text object: tag "<a>hi</a>" (<a>[0,3) hi[3,5) </a>[5,9)) ---- */
    b = prov_buffer_create_from_bytes(a, U("<a>hi</a>"), 9).value;
    r = prov_motion_textobj(b, 4, PROV_TOBJ_TAG, true);
    CHECK(r.ok && r.start == 3 && r.end == 5, "it -> hi [3,5)");
    r = prov_motion_textobj(b, 4, PROV_TOBJ_TAG, false);
    CHECK(r.ok && r.start == 0 && r.end == 9, "at -> <a>hi</a> [0,9)");
    r = prov_motion_textobj(b, 1, PROV_TOBJ_TAG, true);
    CHECK(r.ok && r.start == 3 && r.end == 5, "it from inside the open tag");
    prov_buffer_destroy(b);

    /* ---- nested tags "<p><b>x</b></p>" (innermost wins) ---- */
    b = prov_buffer_create_from_bytes(a, U("<p><b>x</b></p>"), 15).value;
    r = prov_motion_textobj(b, 6, PROV_TOBJ_TAG, true);
    CHECK(r.ok && r.start == 6 && r.end == 7, "it innermost -> x [6,7)");
    r = prov_motion_textobj(b, 6, PROV_TOBJ_TAG, false);
    CHECK(r.ok && r.start == 3 && r.end == 11, "at innermost -> <b>x</b> [3,11)");
    prov_buffer_destroy(b);

    /* ---- findc cursor motion "abc,def,gh" (commas at 3 and 7) ---- */
    b = prov_buffer_create_from_bytes(a, U("abc,def,gh"), 10).value;
    CHECK(prov_motion_findc(b, 0, ',', false, false) == 3, "f, -> 3");
    CHECK(prov_motion_findc(b, 0, ',', true,  false) == 2, "t, -> 2");
    CHECK(prov_motion_findc(b, 3, ',', false, false) == 7, "f, again (;) -> 7");
    CHECK(prov_motion_findc(b, 7, ',', false, true)  == 3, "F, back (,) -> 3");
    CHECK(prov_motion_findc(b, 9, ',', true,  true)  == 8, "T, back till -> 8");
    CHECK(prov_motion_findc(b, 0, 'z', false, false) == 0, "f missing -> no move");
    prov_buffer_destroy(b);

    if (failures) {
        fprintf(stderr, "motion: %d checks failed\n", failures);
        return 1;
    }
    printf("ok: motion tests passed\n");
    return 0;
}
