/* Unit tests for the color-theme model + *.theme.ini reading (RFC-0021).
 * No rendering is exercised — only the data model and the file loader. */
#include <stdio.h>
#include <string.h>

#include "proven/heap.h"
#include "proven/fs.h"
#include "proven/u8str.h"
#include "theme.h"

static int failures = 0;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } \
    } while (0)

static proven_u8str_view_t v(const char *s) { return proven_u8str_view_from_cstr(s); }

int main(void) {
    proven_allocator_t a = proven_heap_allocator();

    /* ---- color names ---- */
    CHECK(prov_theme_color_index("brightblue") == 12, "brightblue=12");
    CHECK(prov_theme_color_index("gray") == 8, "gray=8");
    CHECK(prov_theme_color_index("green") == 2, "green=2");
    CHECK(prov_theme_color_index("default") == -1, "default=-1");
    CHECK(prov_theme_color_index("BrightRed") == 9, "case-insensitive name");
    CHECK(prov_theme_color_index("nope") == -2, "unknown color=-2");

    /* ---- class keys (incl. alias) ---- */
    CHECK(prov_theme_class_from_key("comment") == PROV_TOK_COMMENT, "comment class");
    CHECK(prov_theme_class_from_key("preproc") == PROV_TOK_PREPROC, "preproc class");
    CHECK(prov_theme_class_from_key("preprocessor") == PROV_TOK_PREPROC, "preprocessor alias");
    CHECK(prov_theme_class_from_key("bogus") == PROV_TOK_COUNT, "unknown key");

    /* ---- color spec parsing ---- */
    prov_color_t c;
    CHECK(prov_theme_parse_color("green", &c) && c.fg == 2 && c.bg == -1 && c.attr == 0, "spec: green");
    CHECK(prov_theme_parse_color("white on red", &c) && c.fg == 7 && c.bg == 1, "spec: white on red");
    CHECK(prov_theme_parse_color("gray +dim", &c) && c.fg == 8 && (c.attr & PROV_THEME_DIM), "spec: gray +dim");
    CHECK(prov_theme_parse_color("brightblue +bold", &c) && c.fg == 12 && (c.attr & PROV_THEME_BOLD), "spec: bb+bold");
    CHECK(prov_theme_parse_color("+bold", &c) && c.fg == -1 && (c.attr & PROV_THEME_BOLD), "spec: attr-only");
    CHECK(!prov_theme_parse_color("", &c), "spec: empty fails");

    /* ---- built-ins ---- */
    prov_theme_t dark = prov_theme_builtin("prov_dark");
    CHECK(strcmp(dark.name, "prov_dark") == 0, "builtin name");
    CHECK(dark.cls[PROV_TOK_KEYWORD].fg == 12, "dark keyword=brightblue");
    CHECK(dark.cls[PROV_TOK_STRING].fg == 2, "dark string=green");
    CHECK((dark.cls[PROV_TOK_COMMENT].attr & PROV_THEME_DIM) != 0, "dark comment dim");
    CHECK(dark.cls[PROV_TOK_DEFAULT].fg == -1, "dark default uncolored");

    prov_theme_t mono = prov_theme_builtin("mono");
    CHECK(mono.cls[PROV_TOK_KEYWORD].fg == -1 && (mono.cls[PROV_TOK_KEYWORD].attr & PROV_THEME_BOLD),
          "mono keyword = default+bold");

    prov_theme_t unk = prov_theme_builtin("does-not-exist");
    CHECK(strcmp(unk.name, "prov_dark") == 0, "unknown builtin -> prov_dark");

    /* ---- resolution without files ---- */
    prov_theme_t r1 = prov_theme_resolve(a, NULL, "prov_light");
    CHECK(strcmp(r1.name, "prov_light") == 0 && r1.light, "resolve builtin prov_light");
    prov_theme_t r2 = prov_theme_resolve(a, NULL, "whatever");
    CHECK(strcmp(r2.name, "prov_dark") == 0, "resolve unknown -> prov_dark");

    /* ---- resolution from a *.theme.ini file ---- */
    const char *dir = "/tmp/prov_theme_test_dir";
    const char *file = "/tmp/prov_theme_test_dir/sample.theme.ini";
    (void)proven_fs_mkdir(a, v(dir));
    const char *body =
        "[theme_mytheme]\n"
        "extends = prov_dark\n"
        "keyword = red\n"
        "comment = brightgreen +dim\n"
        "background = light\n"
        "\n"
        "[theme_second]\n"
        "string = brightyellow\n";
    proven_result_file_t of = proven_fs_open(a, v(file),
        PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    CHECK(proven_is_ok(of.err), "create theme file");
    if (proven_is_ok(of.err)) {
        (void)proven_fs_write_all(of.value, (proven_mem_view_t){ (const proven_byte_t *)body, strlen(body) });
        proven_fs_close(of.value);

        prov_theme_t t = prov_theme_resolve(a, dir, "mytheme");
        CHECK(strcmp(t.name, "mytheme") == 0, "loaded theme name");
        CHECK(t.cls[PROV_TOK_KEYWORD].fg == 1, "file keyword overridden = red");
        CHECK(t.cls[PROV_TOK_COMMENT].fg == 10 && (t.cls[PROV_TOK_COMMENT].attr & PROV_THEME_DIM),
              "file comment = brightgreen +dim");
        CHECK(t.cls[PROV_TOK_STRING].fg == 2, "inherited string = prov_dark green");
        CHECK(t.light, "file background = light");

        prov_theme_t t2 = prov_theme_resolve(a, dir, "second");
        CHECK(t2.cls[PROV_TOK_STRING].fg == 11, "second theme string = brightyellow");

        prov_theme_t tm = prov_theme_resolve(a, dir, "missing-section");
        CHECK(strcmp(tm.name, "prov_dark") == 0, "missing section -> prov_dark");

        (void)proven_fs_remove(a, v(file));
    }
    (void)proven_fs_rmdir(a, v(dir));

    if (failures) { fprintf(stderr, "theme: %d checks failed\n", failures); return 1; }
    printf("ok: theme tests passed\n");
    return 0;
}
