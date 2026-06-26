/*
 * Smoke test for the proven string-system APIs that RFC-0004 (Special
 * Milestone S) builds on: u8str owning strings, read-only views, scan, and
 * structured fmt. Pins the vendored build wiring and documents the idioms the
 * conversion steps will use. One main(), exit 0 == pass.
 */

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"
#include "proven/allocator.h"
#include "proven/u8str.h"
#include "proven/scan.h"
#include "proven/fmt.h"

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static int view_is(proven_u8str_view_t v, const char *s) {
    return v.size == strlen(s) && memcmp(v.ptr, s, v.size) == 0;
}

int main(void) {
    proven_allocator_t a = proven_heap_allocator();

    /* ---- views: literals, cstr, eq, starts/ends, slice ---- */
    proven_u8str_view_t hello = PROVEN_LIT("hello world");
    CHECK(hello.size == 11, "PROVEN_LIT size");
    CHECK(proven_u8str_view_eq(hello, proven_u8str_view_from_cstr("hello world")),
          "view_eq cstr");
    CHECK(proven_u8str_view_starts_with(hello, PROVEN_LIT("hello")),
          "starts_with");
    CHECK(proven_u8str_view_ends_with(hello, PROVEN_LIT("world")),
          "ends_with");
    CHECK(view_is(proven_u8str_view_slice(hello, 6, 5), "world"),
          "slice tail");
    CHECK(proven_u8str_view_find(hello, 0, PROVEN_LIT("world")) == 6,
          "find substring");
    CHECK(proven_u8str_view_find(hello, 0, PROVEN_LIT("xyz")) == PROVEN_INDEX_NOT_FOUND,
          "find missing -> NOT_FOUND");

    /* ---- owning string: create / append_grow / byte / remove / as_view ---- */
    proven_result_u8str_t cr = proven_u8str_create(a, 4);
    CHECK(PROVEN_IS_OK(cr.err), "u8str_create");
    proven_u8str_t s = cr.value;

    CHECK(PROVEN_IS_OK(proven_u8str_append_grow(a, &s, PROVEN_LIT("abc"))),
          "append_grow grows past cap");
    CHECK(PROVEN_IS_OK(proven_u8str_append_byte(a, &s, 'd')), "append_byte");
    CHECK(view_is(proven_u8str_as_view(&s), "abcd"), "content after append");
    CHECK(strcmp(proven_u8str_as_cstr(&s), "abcd") == 0, "as_cstr NUL-terminated");

    /* backspace-style trim (prompt editing idiom for S4) */
    CHECK(PROVEN_IS_OK(proven_u8str_remove(&s, proven_u8str_as_view(&s).size - 1, 1)),
          "remove last byte");
    CHECK(view_is(proven_u8str_as_view(&s), "abc"), "content after remove");

    proven_u8str_destroy(a, &s);

    /* ---- scan: integer + bounds (config parser idiom for S1) ---- */
    proven_scan_t sc = proven_scan_init(PROVEN_LIT("42 rest"));
    proven_result_i64_t iv = proven_scan_i64(&sc);
    CHECK(PROVEN_IS_OK(iv.err) && iv.val == 42, "scan_i64 = 42");

    proven_scan_t scn = proven_scan_init(PROVEN_LIT("-7"));
    proven_result_i64_t neg = proven_scan_i64(&scn);
    CHECK(PROVEN_IS_OK(neg.err) && neg.val == -7, "scan_i64 negative");

    /* ---- fmt: type-safe structured formatting into an owning string (S5) ---- */
    proven_result_u8str_t fr = proven_u8str_create(a, 8);
    CHECK(PROVEN_IS_OK(fr.err), "u8str_create for fmt");
    proven_u8str_t f = fr.value;
    proven_fmt_result_t fres =
        proven_u8str_append_fmt_grow(a, &f, "L{}/{} {}", PROVEN_ARG(3), PROVEN_ARG(9),
                                     PROVEN_ARG(proven_u8str_view_from_cstr("ok")));
    CHECK(PROVEN_FMT_IS_OK(fres), "append_fmt_grow ok");
    CHECK(view_is(proven_u8str_as_view(&f), "L3/9 ok"), "fmt output");
    proven_u8str_destroy(a, &f);

    if (failures) {
        fprintf(stderr, "pstr: %d checks failed\n", failures);
        return 1;
    }
    printf("ok: pstr smoke tests passed\n");
    return 0;
}
