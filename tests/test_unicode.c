/*
 * Unit tests for UTF-8 decoding and East Asian width.
 * One main(), exit 0 == pass. Driven by `./nob test`.
 */

#include <stdio.h>
#include <string.h>

#include "unicode.h"
#include "unicode_width_table.h"   /* the curated width ranges, verified below */

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static const proven_u8 *U(const char *s) { return (const proven_u8 *)s; }

static prov_decode_t dec(const char *s) {
    return prov_utf8_decode(U(s), strlen(s));
}

int main(void) {
    /* ---- decoding: well-formed 1..4 byte sequences ---- */
    prov_decode_t d;

    d = dec("A");                       /* U+0041 */
    CHECK(d.valid && d.cp == 0x41 && d.len == 1, "ascii A");

    d = dec("\xC3\xA9");                /* U+00E9 é */
    CHECK(d.valid && d.cp == 0xE9 && d.len == 2, "2-byte é");

    d = dec("\xED\x95\x9C");            /* U+D55C 한 (Hangul, wide) */
    CHECK(d.valid && d.cp == 0xD55C && d.len == 3, "3-byte 한");

    d = dec("\xF0\x9F\x98\x80");        /* U+1F600 😀 */
    CHECK(d.valid && d.cp == 0x1F600 && d.len == 4, "4-byte emoji");

    d = dec("\xCC\x81");                /* U+0301 combining acute */
    CHECK(d.valid && d.cp == 0x301 && d.len == 2, "combining acute");

    /* ---- decoding: malformed input resyncs by one byte ---- */
    d = dec("\xFF");                    /* invalid lead byte */
    CHECK(!d.valid && d.cp == PROV_CP_REPLACEMENT && d.len == 1, "bad lead 0xFF");

    d = prov_utf8_decode(U("\xC3"), 1); /* truncated 2-byte sequence */
    CHECK(!d.valid && d.len == 1, "truncated 2-byte");

    d = dec("\xC0\x80");                /* overlong encoding of NUL */
    CHECK(!d.valid && d.len == 1, "overlong rejected");

    d = dec("\xED\xA0\x80");            /* U+D800 surrogate (ill-formed) */
    CHECK(!d.valid && d.len == 1, "surrogate rejected");

    d = dec("\xC3\x28");               /* valid lead, bad continuation */
    CHECK(!d.valid && d.len == 1, "bad continuation");

    d = prov_utf8_decode(U(""), 0);    /* empty */
    CHECK(!d.valid && d.len == 0, "empty input");

    /* ---- validation over whole ranges ---- */
    CHECK(prov_utf8_validate(U(""), 0), "empty is valid");
    CHECK(prov_utf8_validate(U("hello"), 5), "ascii valid");
    CHECK(prov_utf8_validate(U("h\xC3\xA9llo \xED\x95\x9C"), 10), "mixed valid");
    CHECK(!prov_utf8_validate(U("a\xFF"), 2), "trailing bad byte invalid");
    CHECK(!prov_utf8_validate(U("a\xC3"), 2), "trailing truncated invalid");

    /* ---- widths ---- */
    CHECK(prov_char_width(0x41) == 1, "ascii width 1");
    CHECK(prov_char_width(0xE9) == 1, "latin-1 width 1");
    CHECK(prov_char_width(0x301) == 0, "combining width 0");
    CHECK(prov_char_width(0x200B) == 0, "ZWSP width 0");
    CHECK(prov_char_width(0xD55C) == 2, "hangul width 2");
    CHECK(prov_char_width(0x3042) == 2, "hiragana width 2");
    CHECK(prov_char_width(0x4E00) == 2, "CJK ideograph width 2");
    CHECK(prov_char_width(0xFF21) == 2, "fullwidth A width 2");
    CHECK(prov_char_width(0x1F600) == 2, "emoji width 2");
    CHECK(prov_char_width(0x20AC) == 1, "euro sign width 1");

    /* ---- width-table verification (M6): the table is binary-searched, so it
     *      MUST be sorted ascending and non-overlapping. A hand-edit that breaks
     *      ordering would silently corrupt width queries; pin it here, and check
     *      prov_char_width honors every range's two endpoints. ---- */
    for (size_t i = 0; i < PROV_WIDTH_ZERO_COUNT; i++) {
        CHECK(prov_width_zero[i][0] <= prov_width_zero[i][1], "zero range lo <= hi");
        if (i + 1 < PROV_WIDTH_ZERO_COUNT)
            CHECK(prov_width_zero[i][1] < prov_width_zero[i + 1][0],
                  "zero ranges sorted + disjoint");
        CHECK(prov_char_width(prov_width_zero[i][0]) == 0, "zero range lo -> width 0");
        CHECK(prov_char_width(prov_width_zero[i][1]) == 0, "zero range hi -> width 0");
    }
    for (size_t i = 0; i < PROV_WIDTH_WIDE_COUNT; i++) {
        CHECK(prov_width_wide[i][0] <= prov_width_wide[i][1], "wide range lo <= hi");
        if (i + 1 < PROV_WIDTH_WIDE_COUNT)
            CHECK(prov_width_wide[i][1] < prov_width_wide[i + 1][0],
                  "wide ranges sorted + disjoint");
        CHECK(prov_char_width(prov_width_wide[i][0]) == 2, "wide range lo -> width 2");
        CHECK(prov_char_width(prov_width_wide[i][1]) == 2, "wide range hi -> width 2");
    }
    /* the zero and wide sets must be mutually disjoint (a cp is 0, 2, or default 1,
     * never two of them) — a merge walk over both sorted lists. */
    for (size_t z = 0, w = 0; z < PROV_WIDTH_ZERO_COUNT && w < PROV_WIDTH_WIDE_COUNT; ) {
        proven_u32 zhi = prov_width_zero[z][1], whi = prov_width_wide[w][1];
        bool overlap = prov_width_zero[z][0] <= whi && prov_width_wide[w][0] <= zhi;
        CHECK(!overlap, "zero and wide ranges disjoint");
        if (zhi < whi) z++; else w++;
    }

    if (failures) {
        fprintf(stderr, "unicode: %d checks failed\n", failures);
        return 1;
    }
    printf("ok: unicode tests passed\n");
    return 0;
}
