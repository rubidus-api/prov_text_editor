/*
 * Unit tests for the pluggable charset backends (hosted). Exercises the registry
 * probe/selection and a real CP949 <-> UTF-8 conversion through the active
 * backend. One main(), exit 0 == pass.
 */

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"
#include "proven/allocator.h"
#include "platform_charset.h"

static int failures = 0;
#define CHECK(cond, msg)                                                      \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } } while (0)

/* Convert with `backend` (auto/libc/command) and assert CP949 round-trips. */
static void try_backend(proven_allocator_t a, const char *want) {
    prov_charset_configure(want);
    const char *act = prov_charset_active();
    if (!act) { fprintf(stderr, "note: no charset backend for '%s'\n", want ? want : "auto"); return; }
    if (want && strcmp(want, "auto") != 0)
        CHECK(strcmp(act, want) == 0, "requested backend is the active one");

    /* CP949/EUC-KR '가' = 0xB0 0xA1 ; UTF-8 '가' (U+AC00) = EA B0 80 */
    proven_size_t on = 0;
    proven_u8 *u = prov_charset_to_utf8(a, "CP949", (const proven_u8 *)"\xB0\xA1", 2, &on);
    CHECK(u && on == 3 && memcmp(u, "\xEA\xB0\x80", 3) == 0, "CP949 B0A1 -> UTF-8 가");
    if (u) a.free_fn(a.ctx, u);

    proven_u8 *k = prov_charset_from_utf8(a, "CP949", (const proven_u8 *)"\xEA\xB0\x80", 3, &on);
    CHECK(k && on == 2 && memcmp(k, "\xB0\xA1", 2) == 0, "UTF-8 가 -> CP949 B0A1");
    if (k) a.free_fn(a.ctx, k);

    CHECK(prov_charset_supports("CP949"), "active backend reports CP949 supported");
}

int main(void) {
    proven_allocator_t a = proven_heap_allocator();

    prov_charset_configure("auto");
    CHECK(prov_charset_active() != NULL, "a charset backend probed OK (auto)");

    try_backend(a, "auto");
    try_backend(a, "libc");        /* the linked iconv */
    try_backend(a, "command");     /* the external `iconv` tool, if present */

    /* config value wins, but an unknown/failed one falls back to auto-detection */
    prov_charset_configure("does-not-exist");
    CHECK(prov_charset_active() != NULL, "unknown backend falls back to auto");

    /* per-backend code-page support query */
    prov_charset_configure("auto");
    CHECK(prov_charset_supports("CP949"), "active backend supports CP949");
    CHECK(!prov_charset_supports("NO-SUCH-ENCODING-XYZ"), "unknown encoding unsupported");

    if (failures) { fprintf(stderr, "charset: %d checks failed\n", failures); return 1; }
    printf("ok: charset tests passed (backend: ");
    prov_charset_configure("auto");
    printf("%s)\n", prov_charset_active() ? prov_charset_active() : "none");
    return 0;
}
