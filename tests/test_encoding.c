/*
 * Unit tests for the UTF-8 file loader. One main(), exit 0 == pass.
 * These tests write temporary files (hosted environment) and load them back.
 */

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"
#include "proven/allocator.h"
#include "encoding.h"
#include "buffer.h"

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

#define TMP "build/test/_encoding_case.bin"

static void write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (f) {
        if (n) fwrite(data, 1, n, f);
        fclose(f);
    }
}

static int content_is(const prov_buffer_t *b, const char *expect) {
    proven_u8 tmp[256];
    proven_size_t n =
        prov_buffer_copy_range(b, 0, prov_buffer_byte_len(b), tmp, sizeof tmp);
    return n == strlen(expect) && memcmp(tmp, expect, n) == 0;
}

int main(void) {
    proven_allocator_t a = proven_heap_allocator();

    /* ---- valid multi-line ASCII ---- */
    write_file(TMP, "a\nb\nc", 5);
    prov_result_buffer_t r = prov_load_utf8_file(a, TMP, NULL);
    CHECK(r.err == PROVEN_OK && r.value != NULL, "load valid file");
    if (r.value) {
        CHECK(prov_buffer_byte_len(r.value) == 5, "loaded len 5");
        CHECK(prov_buffer_line_count(r.value) == 3, "loaded 3 lines");
        CHECK(content_is(r.value, "a\nb\nc"), "loaded content");
        prov_buffer_destroy(r.value);
    }

    /* ---- empty file -> empty buffer ---- */
    write_file(TMP, "", 0);
    r = prov_load_utf8_file(a, TMP, NULL);
    CHECK(r.err == PROVEN_OK && r.value != NULL, "load empty file");
    if (r.value) {
        CHECK(prov_buffer_byte_len(r.value) == 0, "empty len 0");
        CHECK(prov_buffer_line_count(r.value) == 1, "empty 1 line");
        prov_buffer_destroy(r.value);
    }

    /* ---- valid UTF-8 multibyte ---- */
    write_file(TMP, "h\xC3\xA9llo \xED\x95\x9C", 10);   /* "héllo 한" */
    r = prov_load_utf8_file(a, TMP, NULL);
    CHECK(r.err == PROVEN_OK && r.value != NULL, "load utf8 multibyte");
    if (r.value) {
        CHECK(content_is(r.value, "h\xC3\xA9llo \xED\x95\x9C"), "utf8 content");
        prov_buffer_destroy(r.value);
    }

    /* ---- invalid UTF-8 -> INVALID_ENCODING, no buffer (strict, sanitized=NULL) ---- */
    write_file(TMP, "ok\xFF" "bad", 6);
    r = prov_load_utf8_file(a, TMP, NULL);
    CHECK(r.err == PROVEN_ERR_INVALID_ENCODING, "invalid utf8 rejected (strict)");
    CHECK(r.value == NULL, "invalid utf8 yields no buffer (strict)");

    /* ---- lossy mode: invalid bytes dropped, file still loads, flag set ---- */
    write_file(TMP, "ok\xFF" "bad", 6);                 /* the 0xFF is invalid */
    bool san = false;
    r = prov_load_utf8_file(a, TMP, &san);
    CHECK(r.err == PROVEN_OK && r.value != NULL, "invalid utf8 loads lossily");
    CHECK(san == true, "lossy load reports sanitization");
    if (r.value) { CHECK(content_is(r.value, "okbad"), "lossy drops the invalid byte"); prov_buffer_destroy(r.value); }
    /* a clean file in lossy mode leaves the flag false */
    write_file(TMP, "clean", 5);
    san = true;
    r = prov_load_utf8_file(a, TMP, &san);
    CHECK(r.err == PROVEN_OK && san == false, "clean file: no sanitization flag");
    if (r.value) prov_buffer_destroy(r.value);

    /* ---- missing file -> error, not INVALID_ENCODING ---- */
    remove(TMP);
    r = prov_load_utf8_file(a, "build/test/_does_not_exist_xyz.bin", NULL);
    CHECK(r.err != PROVEN_OK && r.err != PROVEN_ERR_INVALID_ENCODING,
          "missing file errors");
    CHECK(r.value == NULL, "missing file yields no buffer");

    /* ---- invalid args ---- */
    r = prov_load_utf8_file(a, NULL, NULL);
    CHECK(r.err == PROVEN_ERR_INVALID_ARG, "NULL path rejected");

    /* ---- file metadata detection (encoding / EOL / BOM) ---- */
    struct { const char *bytes; size_t n; prov_eol_t eol; bool bom; } cases[] = {
        { "abc\ndef\n",            8, PROV_EOL_LF,    false },
        { "abc\r\ndef\r\n",       10, PROV_EOL_CRLF,  false },
        { "abc\rdef\r",            8, PROV_EOL_CR,     false },
        { "a\nb\r\nc",             6, PROV_EOL_MIXED,  false },
        { "no newline at all",    17, PROV_EOL_LF,     false },  /* default LF */
        { "\xEF\xBB\xBF" "x\ny",   6, PROV_EOL_LF,     true  },  /* UTF-8 BOM */
        { "\xEF\xBB\xBF" "x\r\n",  6, PROV_EOL_CRLF,   true  },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        prov_result_buffer_t rb = prov_buffer_create_from_bytes(
            a, (const proven_u8 *)cases[i].bytes, cases[i].n);
        CHECK(rb.err == PROVEN_OK, "fileinfo: buffer created");
        prov_fileinfo_t fi = prov_detect_fileinfo(rb.value);
        CHECK(fi.eol == cases[i].eol, "fileinfo: eol detected");
        CHECK(fi.bom == cases[i].bom, "fileinfo: bom detected");
        CHECK(strcmp(fi.encoding, "UTF-8") == 0, "fileinfo: encoding UTF-8");
        prov_buffer_destroy(rb.value);
    }
    CHECK(strcmp(prov_eol_name(PROV_EOL_CRLF), "CR/LF") == 0, "eol name CR/LF");
    CHECK(prov_fileinfo_default().eol == PROV_EOL_LF, "default eol is LF");

    if (failures) {
        fprintf(stderr, "encoding: %d checks failed\n", failures);
        return 1;
    }
    printf("ok: encoding tests passed\n");
    return 0;
}
