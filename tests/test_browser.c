/* Tests for the file-open browser directory model (goal 4, S1). Runs from the
 * repo root (nob executes test binaries there), so it browses the real "src"
 * and "tests" directories. */

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"
#include "proven/allocator.h"
#include "browser.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static long find(const prov_browser_t *b, const char *name) {
    for (proven_size_t i = 0; i < b->count; i++)
        if (strcmp(b->entries[i].name, name) == 0) return (long)i;
    return -1;
}

int main(void) {
    proven_allocator_t a = proven_heap_allocator();
    prov_browser_t b = {0};

    /* --- load src/ --- */
    CHECK(prov_browser_load(&b, a, "src"));
    CHECK(b.count >= 3);
    CHECK(strcmp(b.entries[0].name, "..") == 0);   /* parent always first */
    CHECK(b.entries[0].is_dir);

    long ib = find(&b, "browser.c");
    long im = find(&b, "main.c");
    long ip = find(&b, "proven");                  /* a subdirectory */
    CHECK(ib >= 0);
    CHECK(im >= 0);
    CHECK(ip >= 0);
    if (ib >= 0) { CHECK(!b.entries[ib].is_dir); CHECK(b.entries[ib].size > 0); }
    if (ip >= 0) CHECK(b.entries[ip].is_dir);

    /* dirs sort before files: "proven" (dir) precedes "main.c" (file) */
    if (ip >= 0 && im >= 0) CHECK(ip < im);

    /* no "." or duplicate ".." beyond the synthetic leading one */
    int dots = 0;
    for (proven_size_t i = 0; i < b.count; i++) {
        if (strcmp(b.entries[i].name, ".") == 0) dots++;
        if (strcmp(b.entries[i].name, "..") == 0 && i != 0) dots++;
    }
    CHECK(dots == 0);

    /* perms/mtime are filled lazily, on demand, for a regular file */
    if (ib >= 0) {
        CHECK(!b.entries[ib].stat_done);                  /* not stat'd until requested */
        prov_browser_ensure_stat(&b, a, (proven_size_t)ib);
        CHECK(b.entries[ib].stat_done);
        CHECK(b.entries[ib].stat_ok);
        CHECK(b.entries[ib].mtime > 0);
        prov_browser_ensure_stat(&b, a, (proven_size_t)ib);  /* idempotent */
        CHECK(b.entries[ib].stat_ok);
    }

    /* --- parent path resolution --- */
    char path[1024];
    prov_browser_path_at(&b, 0, path, sizeof path);   /* ".." from "src" -> "." or repo root */
    CHECK(path[0] != '\0');
    CHECK(strcmp(path, "src") != 0);

    if (im >= 0) {
        prov_browser_path_at(&b, (proven_size_t)im, path, sizeof path);
        CHECK(strcmp(path, "src/main.c") == 0);
    }

    /* --- reload a different dir reuses the struct cleanly --- */
    CHECK(prov_browser_load(&b, a, "tests"));
    CHECK(find(&b, "test_browser.c") >= 0);
    CHECK(strcmp(b.entries[0].name, "..") == 0);

    /* --- nonexistent dir fails gracefully --- */
    CHECK(!prov_browser_load(&b, a, "no_such_dir_zzz"));

    prov_browser_free(&b, a);

    /* --- path resolver (RFC-0015) --- */
    {
        char out[1024];
        #define RP(base, in, want) do { prov_browser_resolve_path((base), (in), out, sizeof out); \
            CHECK(strcmp(out, (want)) == 0); if (strcmp(out,(want))) printf("  got '%s' want '%s'\n", out, (want)); } while (0)
        RP("/home/u", "docs", "/home/u/docs");          /* relative join */
        RP("/home/u", "docs/", "/home/u/docs");          /* trailing slash dropped */
        RP("/home/u", "./downloads/new/", "/home/u/downloads/new");
        RP("/home/u", "../other", "/home/other");        /* .. ascends */
        RP("/home/u", "./../document", "/home/document");
        RP("/home/u", "a//b/./c", "/home/u/a/b/c");       /* dup sep + . collapse */
        RP("/home/u", "/etc/passwd", "/etc/passwd");      /* absolute ignores base */
        RP("/home/u", "..", "/home");
        RP("/", "..", "/");                               /* can't ascend past root */
        RP("/home/u", "", "/home/u");                     /* empty -> base */
        RP("/a/b/c", "../../x", "/a/x");
        #undef RP
    }

    if (g_fail) { printf("test_browser: %d FAILED\n", g_fail); return 1; }
    printf("test_browser: OK\n");
    return 0;
}
