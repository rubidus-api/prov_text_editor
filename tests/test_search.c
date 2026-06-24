/*
 * Unit tests for literal byte search (src/search.c). One main(), exit 0 == pass.
 */

#include <stdio.h>
#include <string.h>

#include "search.h"

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

#define U(s) ((const proven_u8 *)(s))

static proven_size_t find(const char *hay, const char *needle, proven_size_t from,
                          bool fwd, bool wrap) {
    bool f;
    return prov_search_bytes(U(hay), strlen(hay), U(needle), strlen(needle),
                             from, fwd, wrap, false, &f);
}
static proven_size_t findi(const char *hay, const char *needle) {  /* case-insensitive */
    bool f;
    return prov_search_bytes(U(hay), strlen(hay), U(needle), strlen(needle),
                             0, true, false, true, &f);
}

int main(void) {
    const char *h = "abXabYab";   /* "ab" at 0,3,6 ; X@2 Y@5 */

    /* forward */
    CHECK(find(h, "ab", 0, true, false) == 0, "fwd from 0 -> 0");
    CHECK(find(h, "ab", 1, true, false) == 3, "fwd from 1 -> 3");
    CHECK(find(h, "ab", 4, true, false) == 6, "fwd from 4 -> 6");
    CHECK(find(h, "ab", 7, true, false) == PROV_SEARCH_NPOS, "fwd from 7 -> none");
    CHECK(find(h, "ab", 7, true, true) == 0, "fwd from 7 wrap -> 0");

    /* backward (nearest start <= from) */
    CHECK(find(h, "ab", 8, false, false) == 6, "back from 8 -> 6");
    CHECK(find(h, "ab", 5, false, false) == 3, "back from 5 -> 3");
    CHECK(find(h, "ab", 2, false, false) == 0, "back from 2 -> 0");
    CHECK(find(h, "ab", 0, false, false) == 0, "back from 0 (start match) -> 0");

    /* misc / edges */
    CHECK(find(h, "Z", 0, true, true) == PROV_SEARCH_NPOS, "absent pattern -> none");
    CHECK(find(h, "", 0, true, true) == PROV_SEARCH_NPOS, "empty pattern -> none");
    CHECK(find("ab", "abc", 0, true, true) == PROV_SEARCH_NPOS, "needle longer than hay");
    CHECK(find(h, "abXabYab", 0, true, false) == 0, "whole string matches at 0");
    CHECK(find("aaaa", "aa", 0, true, false) == 0, "overlap: first at 0");
    CHECK(find("aaaa", "aa", 1, true, false) == 1, "overlap: from 1 -> 1");
    CHECK(find("aaaa", "aa", 9, false, false) == 2, "overlap back clamps to last (2)");

    /* case-insensitive (fold) */
    CHECK(find("Hello", "hello", 0, true, false) == PROV_SEARCH_NPOS, "case-sensitive: Hello != hello");
    CHECK(findi("Hello", "hello") == 0, "case-fold: hello matches Hello @0");
    CHECK(findi("xxFOObar", "foo") == 2, "case-fold: foo matches FOO @2");
    CHECK(prov_match_at(U("AbC"), U("abc"), 3, 0, true), "prov_match_at fold true");
    CHECK(!prov_match_at(U("AbC"), U("abc"), 3, 0, false), "prov_match_at no-fold false");

    if (failures) { fprintf(stderr, "search: %d checks failed\n", failures); return 1; }
    printf("ok: search tests passed\n");
    return 0;
}
