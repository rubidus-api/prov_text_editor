/*
 * Unit tests for atomic save. Writes real files (hosted) and reads them back.
 * One main(), exit 0 == pass.
 */

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"
#include "proven/allocator.h"
#include "save.h"
#include "buffer.h"

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

#define TMP "build/test/_save_case.txt"

static const proven_u8 *U(const char *s) { return (const proven_u8 *)s; }

/* Read a whole file; returns bytes read into out (cap), or -1 if missing. */
static long read_file(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(out, 1, cap, f);
    fclose(f);
    return (long)n;
}

static int file_is(const char *path, const char *expect) {
    char buf[256];
    long n = read_file(path, buf, sizeof buf);
    return n >= 0 && (size_t)n == strlen(expect) && memcmp(buf, expect, n) == 0;
}

int main(void) {
    proven_allocator_t a = proven_heap_allocator();

    /* ---- save new file ---- */
    prov_result_buffer_t r = prov_buffer_create_from_bytes(a, U("hello\nworld"), 11);
    CHECK(r.err == PROVEN_OK, "make buffer");
    remove(TMP);
    CHECK(prov_save_buffer(a, r.value, TMP, NULL) == PROVEN_OK, "save new");
    CHECK(file_is(TMP, "hello\nworld"), "file content matches");
    prov_buffer_destroy(r.value);

    /* ---- overwrite existing file atomically ---- */
    FILE *f = fopen(TMP, "wb");
    if (f) { fputs("OLD CONTENT", f); fclose(f); }
    r = prov_buffer_create_from_bytes(a, U("NEW!"), 4);
    CHECK(prov_save_buffer(a, r.value, TMP, NULL) == PROVEN_OK, "save overwrite");
    CHECK(file_is(TMP, "NEW!"), "overwritten content");
    prov_buffer_destroy(r.value);

    /* ---- the temp file is not left behind ---- */
    CHECK(read_file(TMP ".prov-tmp", (char[16]){0}, 16) == -1,
          "temp file removed");

    /* ---- empty buffer saves an empty file ---- */
    r = prov_buffer_create(a);
    CHECK(prov_save_buffer(a, r.value, TMP, NULL) == PROVEN_OK, "save empty");
    CHECK(file_is(TMP, ""), "empty file");
    prov_buffer_destroy(r.value);

    /* ---- invalid args ---- */
    r = prov_buffer_create(a);
    CHECK(prov_save_buffer(a, r.value, NULL, NULL) == PROVEN_ERR_INVALID_ARG,
          "NULL path rejected");
    prov_buffer_destroy(r.value);

    /* ---- Phase 3a: BOM + CRLF normalized on load, reproduced on save ---- */
    {
        FILE *g = fopen(TMP, "wb");
        if (g) { fwrite("\xEF\xBB\xBF" "a\r\nb\r\n", 1, 9, g); fclose(g); }  /* BOM + CRLF */
        prov_fileinfo_t info;
        prov_result_buffer_t lr = prov_load_file(a, TMP, NULL, &info, NULL, NULL);
        CHECK(lr.err == PROVEN_OK, "load BOM+CRLF file");
        CHECK(info.bom && info.eol == PROV_EOL_CRLF, "detected BOM + CRLF");
        proven_size_t bn = prov_buffer_byte_len(lr.value);
        char bb[16];
        proven_size_t got = prov_buffer_copy_range(lr.value, 0, bn, (proven_u8 *)bb, sizeof bb);
        CHECK(got == 4 && memcmp(bb, "a\nb\n", 4) == 0, "load normalized to LF, BOM stripped");
        CHECK(prov_save_buffer(a, lr.value, TMP, &info) == PROVEN_OK, "save with info");
        CHECK(file_is(TMP, "\xEF\xBB\xBF" "a\r\nb\r\n"), "BOM + CRLF reproduced on save");
        prov_buffer_destroy(lr.value);
    }

    /* ---- Phase 3a: UTF-16LE (BOM) decodes on load, re-encodes on save ---- */
    {
        FILE *g = fopen(TMP, "wb");
        if (g) { fwrite("\xFF\xFE" "H\0i\0\n\0", 1, 8, g); fclose(g); }   /* "Hi\n" UTF-16LE+BOM */
        prov_fileinfo_t info;
        prov_result_buffer_t lr = prov_load_file(a, TMP, NULL, &info, NULL, NULL);
        CHECK(lr.err == PROVEN_OK, "load UTF-16LE");
        CHECK(info.bom && info.codepage == 1200, "detected UTF-16LE + BOM");
        proven_size_t bn = prov_buffer_byte_len(lr.value);
        char bb[16];
        proven_size_t got = prov_buffer_copy_range(lr.value, 0, bn, (proven_u8 *)bb, sizeof bb);
        CHECK(got == 3 && memcmp(bb, "Hi\n", 3) == 0, "UTF-16LE decoded to internal UTF-8/LF");
        CHECK(prov_save_buffer(a, lr.value, TMP, &info) == PROVEN_OK, "save UTF-16LE");
        char rb[32]; long rn = read_file(TMP, rb, sizeof rb);
        CHECK(rn == 8 && memcmp(rb, "\xFF\xFE" "H\0i\0\n\0", 8) == 0, "UTF-16LE round-trip bytes");
        prov_buffer_destroy(lr.value);
    }

    /* ---- Phase 3a: a non-UTF-8 (no-BOM) file falls back to Windows-1252 ---- */
    {
        FILE *g = fopen(TMP, "wb");
        if (g) { fwrite("caf\xE9\n", 1, 5, g); fclose(g); }   /* 0xE9 = é in CP1252, invalid UTF-8 */
        prov_fileinfo_t info;
        prov_result_buffer_t lr = prov_load_file(a, TMP, NULL, &info, NULL, NULL);
        CHECK(lr.err == PROVEN_OK && info.codepage == 1252, "invalid UTF-8 -> Windows-1252");
        proven_size_t bn = prov_buffer_byte_len(lr.value);
        char bb[16];
        proven_size_t got = prov_buffer_copy_range(lr.value, 0, bn, (proven_u8 *)bb, sizeof bb);
        CHECK(got == 6 && memcmp(bb, "caf\xC3\xA9\n", 6) == 0, "CP1252 0xE9 decoded to UTF-8 é");
        CHECK(prov_save_buffer(a, lr.value, TMP, &info) == PROVEN_OK, "save Windows-1252");
        CHECK(file_is(TMP, "caf\xE9\n"), "Windows-1252 round-trip");
        prov_buffer_destroy(lr.value);
    }

    /* ---- Phase 3a: an explicit encoding (CP949) decodes/saves via the charset PAL ---- */
    {
        FILE *g = fopen(TMP, "wb");
        if (g) { fwrite("\xB0\xA1\n", 1, 3, g); fclose(g); }   /* 가\n in CP949 (B0 A1) */
        prov_fileinfo_t info;
        prov_result_buffer_t lr = prov_load_file(a, TMP, NULL, &info, "CP949", NULL);
        CHECK(lr.err == PROVEN_OK && strcmp(info.enc_name, "CP949") == 0, "load forced CP949 via PAL");
        proven_size_t bn = prov_buffer_byte_len(lr.value);
        char bb[16];
        proven_size_t got = prov_buffer_copy_range(lr.value, 0, bn, (proven_u8 *)bb, sizeof bb);
        CHECK(got == 4 && memcmp(bb, "\xEA\xB0\x80\n", 4) == 0, "CP949 B0A1 -> UTF-8 가");
        CHECK(prov_save_buffer(a, lr.value, TMP, &info) == PROVEN_OK, "save CP949 via PAL");
        CHECK(file_is(TMP, "\xB0\xA1\n"), "CP949 round-trip via PAL");
        prov_buffer_destroy(lr.value);
    }

    /* ---- RFC-0019: binary mode loads and saves bytes verbatim (no conversion) ---- */
    {
        /* bytes a text load would mangle: a CR, a lone 0xFF, a NUL, a CRLF */
        const char raw[] = { 'A', '\r', '\n', (char)0xFF, 0x00, 'B', '\r', '\n' };
        FILE *g = fopen(TMP, "wb");
        if (g) { fwrite(raw, 1, sizeof raw, g); fclose(g); }
        prov_fileinfo_t info;
        prov_result_buffer_t lr = prov_load_binary(a, TMP, &info, "CP949");
        CHECK(lr.err == PROVEN_OK && info.binary, "binary load ok + flagged binary");
        CHECK(strcmp(info.enc_name, "CP949") == 0, "interpretation charset recorded");
        proven_size_t bn = prov_buffer_byte_len(lr.value);
        char bb[16];
        proven_size_t got = prov_buffer_copy_range(lr.value, 0, bn, (proven_u8 *)bb, sizeof bb);
        CHECK(got == sizeof raw && memcmp(bb, raw, sizeof raw) == 0, "raw bytes verbatim (CR/0xFF/NUL kept)");
        CHECK(prov_save_buffer(a, lr.value, TMP, &info) == PROVEN_OK, "binary save ok");
        /* read back: must equal the original bytes exactly */
        FILE *h = fopen(TMP, "rb");
        char rb[16]; size_t rn = h ? fread(rb, 1, sizeof rb, h) : 0; if (h) fclose(h);
        CHECK(rn == sizeof raw && memcmp(rb, raw, sizeof raw) == 0, "binary round-trip is byte-exact");
        prov_buffer_destroy(lr.value);
    }

    remove(TMP);

    if (failures) {
        fprintf(stderr, "save: %d checks failed\n", failures);
        return 1;
    }
    printf("ok: save tests passed\n");
    return 0;
}
