/*
 * Unit tests for the reusable text-box widget (RFC-0013): text wrapping with
 * invalid-byte skipping, and the hex dump. One main(), exit 0 == pass.
 */
#include <stdio.h>
#include <string.h>

#include "textbox.h"

static int failures = 0;
#define CHECK(cond, msg)                                                      \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } } while (0)

static prov_textbox_t T(const char *s, bool hex) {
    return (prov_textbox_t){ .bytes = (const proven_u8 *)s, .len = strlen(s), .hex = hex };
}

/* render row `r` at `w` into a static buffer and compare to `want`. */
static int row_is(const prov_textbox_t *tb, int w, proven_size_t r, const char *want) {
    char buf[256];
    prov_textbox_render(tb, w, r, buf, sizeof buf);
    return strcmp(buf, want) == 0;
}

int main(void) {
    /* ---- text row counting ---- */
    prov_textbox_t a = T("abc", false);
    CHECK(prov_textbox_rows(&a, 80) == 1, "abc -> 1 row");
    prov_textbox_t b = T("abc\n", false);
    CHECK(prov_textbox_rows(&b, 80) == 1, "abc\\n -> 1 row (no phantom)");
    prov_textbox_t c = T("abc\n\ndef", false);
    CHECK(prov_textbox_rows(&c, 80) == 3, "abc/blank/def -> 3 rows");
    CHECK(row_is(&c, 80, 0, "abc") && row_is(&c, 80, 1, "") && row_is(&c, 80, 2, "def"),
          "abc/blank/def rows render");
    prov_textbox_t e = T("", false);
    CHECK(prov_textbox_rows(&e, 80) == 0, "empty -> 0 rows");

    /* ---- text wrap (code-point safe) ---- */
    prov_textbox_t w = T("abcdef", false);
    CHECK(prov_textbox_rows(&w, 3) == 2, "abcdef@3 -> 2 rows");
    CHECK(row_is(&w, 3, 0, "abc") && row_is(&w, 3, 1, "def"), "abcdef@3 wraps abc/def");

    /* wide glyphs occupy 2 columns: 你好 at width 3 -> one per row */
    prov_textbox_t cjk = T("\xe4\xbd\xa0\xe5\xa5\xbd", false);   /* 你好 */
    CHECK(prov_textbox_rows(&cjk, 3) == 2, "你好@3 -> 2 rows (wide=2 cols)");
    CHECK(prov_textbox_rows(&cjk, 4) == 1, "你好@4 -> 1 row");

    /* invalid / partial UTF-8 is skipped, never rendered (no tofu) */
    prov_textbox_t bad = T("a\xff" "b", false);   /* split escape: 0xFF then 'b' */
    CHECK(prov_textbox_rows(&bad, 80) == 1, "a<bad>b -> 1 row");
    CHECK(row_is(&bad, 80, 0, "ab"), "invalid byte skipped (no replacement glyph)");

    /* ---- hex mode ---- */
    prov_textbox_t h = T("ABC", true);
    CHECK(prov_textbox_rows(&h, 80) == 1, "hex ABC -> 1 row");
    char hb[256];
    prov_textbox_render(&h, 80, 0, hb, sizeof hb);
    CHECK(strncmp(hb, "00000000  41 42 43 ", 19) == 0, "hex offset + bytes");
    CHECK(strstr(hb, "|ABC") != NULL, "hex ascii gutter");

    /* 17 bytes -> 2 hex rows (16 per row) */
    prov_textbox_t h2 = { .bytes = (const proven_u8 *)"0123456789abcdefg", .len = 17, .hex = true };
    CHECK(prov_textbox_rows(&h2, 80) == 2, "17 bytes -> 2 hex rows");
    prov_textbox_render(&h2, 80, 1, hb, sizeof hb);
    CHECK(strncmp(hb, "00000010  67 ", 13) == 0, "second hex row offset 0x10 + last byte");

    /* ---- flow mode: ignore newlines, one continuous char-wrap (RFC-0019 preview) ---- */
    {
        prov_textbox_t f = { .bytes = (const proven_u8 *)"ab\ncd\nef", .len = 8, .flow = true };
        /* with newlines as spaces: "ab cd ef" (8 cols) -> 1 row at width 80 */
        CHECK(prov_textbox_rows(&f, 80) == 1, "flow: newlines don't break rows");
        CHECK(row_is(&f, 80, 0, "ab cd ef"), "flow: newline renders as space");
        /* char-wrap fills each row to the width: "ab cd ef" @4 -> "ab c" / "d ef" */
        CHECK(prov_textbox_rows(&f, 4) == 2, "flow: char-wraps to width");
        CHECK(row_is(&f, 4, 0, "ab c") && row_is(&f, 4, 1, "d ef"), "flow: rectangular wrap (no ragged edge)");
    }

    if (failures) { fprintf(stderr, "textbox: %d checks failed\n", failures); return 1; }
    printf("ok: textbox tests passed\n");
    return 0;
}
