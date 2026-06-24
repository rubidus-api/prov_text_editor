/*
 * Unit tests for the hex view geometry + row rendering (RFC-0018): row counts,
 * the dump layout, column helpers, byte<->cell location, and the alignment nudge.
 * One main(), exit 0 == pass.
 */
#include <stdio.h>
#include <string.h>

#include "hexview.h"

static int failures = 0;
#define CHECK(cond, msg)                                                      \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } } while (0)

static prov_hexview_t H(const char *s, int align) {
    return (prov_hexview_t){ .bytes = (const proven_u8 *)s, .len = strlen(s), .align = align };
}

int main(void) {
    char buf[256];

    /* ---- row counts ---- */
    CHECK(prov_hexview_rows(0, 0) == 1, "empty -> 1 row (cursor home)");
    CHECK(prov_hexview_rows(3, 0) == 1, "3 bytes -> 1 row");
    CHECK(prov_hexview_rows(16, 0) == 1, "16 bytes -> 1 row");
    CHECK(prov_hexview_rows(17, 0) == 2, "17 bytes -> 2 rows");
    CHECK(prov_hexview_rows(16, 1) == 2, "16 bytes + align 1 -> 2 rows (span 17)");

    /* ---- layout of a basic row ---- */
    prov_hexview_t a = H("ABC", 0);
    prov_hexview_render(&a, 0, buf, sizeof buf);
    CHECK(strncmp(buf, "00000000  41 42 43 ", 19) == 0, "offset + hex bytes");
    CHECK(strstr(buf, "|ABC") != NULL, "ascii pane");
    CHECK((int)strlen(buf) == PROV_HEX_ROW_W, "row width = 76");

    /* control bytes show as '.' in the ascii pane */
    prov_hexview_t ctl = { .bytes = (const proven_u8 *)"\x01\x7f", .len = 2, .align = 0 };
    prov_hexview_render(&ctl, 0, buf, sizeof buf);
    CHECK(strncmp(buf, "00000000  01 7f ", 16) == 0, "control hex");
    CHECK(buf[59] == '.' && buf[60] == '.', "control bytes -> '.' ascii");

    /* second row offset for a 17-byte buffer */
    prov_hexview_t big = { .bytes = (const proven_u8 *)"0123456789abcdefg", .len = 17, .align = 0 };
    prov_hexview_render(&big, 1, buf, sizeof buf);
    CHECK(strncmp(buf, "00000010  67 ", 13) == 0, "row 1 offset 0x10 + byte 16");

    /* ---- column helpers ---- */
    CHECK(prov_hexview_hexcol(0) == 10, "hexcol slot 0");
    CHECK(prov_hexview_hexcol(1) == 13, "hexcol slot 1");
    CHECK(prov_hexview_hexcol(15) == 55, "hexcol slot 15");
    CHECK(prov_hexview_asciicol(0) == 59, "asciicol slot 0");
    CHECK(prov_hexview_asciicol(15) == 74, "asciicol slot 15");

    /* ---- locate (byte -> row, slot) ---- */
    proven_size_t row; int slot;
    prov_hexview_locate(0, 0, &row, &slot);   CHECK(row == 0 && slot == 0,  "byte 0 @ (0,0)");
    prov_hexview_locate(16, 0, &row, &slot);  CHECK(row == 1 && slot == 0,  "byte 16 @ (1,0)");
    prov_hexview_locate(17, 0, &row, &slot);  CHECK(row == 1 && slot == 1,  "byte 17 @ (1,1)");

    /* ---- alignment nudge ---- */
    prov_hexview_locate(0, 3, &row, &slot);   CHECK(row == 0 && slot == 3,  "align 3: byte 0 @ slot 3");
    prov_hexview_locate(13, 3, &row, &slot);  CHECK(row == 1 && slot == 0,  "align 3: byte 13 @ (1,0)");
    /* row 0 with align 3: slots 0..2 are a blank lead-in (3 cols each = 9 spaces),
     * byte 0 ('A'=0x41) at slot 3, byte 1 ('B'=0x42) at slot 4 */
    prov_hexview_t al = H("ABCD", 3);
    prov_hexview_render(&al, 0, buf, sizeof buf);
    CHECK(strncmp(buf, "00000000", 8) == 0, "align 3: row 0 offset 0");
    CHECK(strncmp(buf + 10, "         41 42 ", 15) == 0, "align 3: blank lead-in then 41 42");
    /* offset label of row 1 = 16 - 3 = 13 = 0x0d */
    prov_hexview_render(&al, 1, buf, sizeof buf);
    CHECK(strncmp(buf, "0000000d ", 9) == 0, "align 3: row 1 offset 0x0d");

    if (failures) { fprintf(stderr, "hexview: %d checks failed\n", failures); return 1; }
    printf("ok: hexview tests passed\n");
    return 0;
}
