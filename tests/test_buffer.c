/*
 * Unit tests for the prov document buffer (piece table + line index + undo).
 * One main(), exit 0 == pass. Driven by `./nob test`.
 */

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"
#include "proven/allocator.h"
#include "buffer.h"

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

/* True if the whole document equals the C string `expect`. */
static int content_is(const prov_buffer_t *b, const char *expect) {
    proven_u8 tmp[256];
    proven_size_t n =
        prov_buffer_copy_range(b, 0, prov_buffer_byte_len(b), tmp, sizeof tmp);
    return n == strlen(expect) && memcmp(tmp, expect, n) == 0;
}

static const proven_u8 *U(const char *s) { return (const proven_u8 *)s; }

int main(void) {
    proven_allocator_t a = proven_heap_allocator();

    /* ---- empty buffer ---- */
    prov_result_buffer_t r = prov_buffer_create(a);
    CHECK(r.err == PROVEN_OK && r.value != NULL, "create empty");
    prov_buffer_t *b = r.value;
    CHECK(prov_buffer_byte_len(b) == 0, "empty len 0");
    CHECK(prov_buffer_line_count(b) == 1, "empty has 1 line");
    CHECK(prov_buffer_line_start(b, 0) == 0, "line 0 starts at 0");

    /* ---- insert at end, middle, with newline ---- */
    CHECK(prov_buffer_insert(b, 0, U("hello"), 5) == PROVEN_OK, "insert hello");
    CHECK(prov_buffer_byte_len(b) == 5, "len 5");
    CHECK(content_is(b, "hello"), "content == hello");
    CHECK(prov_buffer_line_count(b) == 1, "still 1 line");

    CHECK(prov_buffer_insert(b, 3, U("XY"), 2) == PROVEN_OK, "insert mid (split)");
    CHECK(content_is(b, "helXYlo"), "content == helXYlo");

    CHECK(prov_buffer_insert(b, 7, U("\nworld"), 6) == PROVEN_OK, "insert newline");
    CHECK(content_is(b, "helXYlo\nworld"), "content two lines");
    CHECK(prov_buffer_line_count(b) == 2, "now 2 lines");
    CHECK(prov_buffer_line_start(b, 1) == 8, "line 1 starts at 8");

    /* ---- copy a sub-range ---- */
    proven_u8 tmp[16];
    proven_size_t n = prov_buffer_copy_range(b, 8, 5, tmp, sizeof tmp);
    CHECK(n == 5 && memcmp(tmp, "world", 5) == 0, "copy [8,13) == world");

    /* ---- out-of-bounds edits ---- */
    CHECK(prov_buffer_insert(b, 1000, U("x"), 1) == PROVEN_ERR_OUT_OF_BOUNDS,
          "insert past end is OOB");
    CHECK(prov_buffer_delete(b, prov_buffer_byte_len(b), 1) ==
              PROVEN_ERR_OUT_OF_BOUNDS,
          "delete past end is OOB");

    /* ---- delete (the inserted XY) ---- */
    CHECK(prov_buffer_delete(b, 3, 2) == PROVEN_OK, "delete XY");
    CHECK(content_is(b, "hello\nworld"), "content after delete");
    CHECK(prov_buffer_line_count(b) == 2, "delete kept 2 lines");

    /* ---- undo / redo the delete ---- */
    prov_edit_info_t u = prov_buffer_undo(b);
    CHECK(u.applied && !u.was_insert && u.at == 3 && u.len == 2, "undo delete info");
    CHECK(content_is(b, "helXYlo\nworld"), "undo restored XY");
    prov_edit_info_t rd = prov_buffer_redo(b);
    CHECK(rd.applied && !rd.was_insert && rd.at == 3 && rd.len == 2, "redo delete info");
    CHECK(content_is(b, "hello\nworld"), "redo re-applied delete");

    /* a new edit clears redo */
    CHECK(prov_buffer_undo(b).applied, "undo again");
    CHECK(content_is(b, "helXYlo\nworld"), "undo restored again");
    CHECK(prov_buffer_insert(b, 0, U("Z"), 1) == PROVEN_OK, "edit clears redo");
    CHECK(prov_buffer_redo(b).applied == false, "redo empty after new edit");
    CHECK(content_is(b, "ZhelXYlo\nworld"), "content after Z insert");

    /* ---- create_from_bytes + multi-line index ---- */
    prov_result_buffer_t r2 = prov_buffer_create_from_bytes(a, U("a\nb\nc"), 5);
    CHECK(r2.err == PROVEN_OK && r2.value != NULL, "from_bytes");
    prov_buffer_t *b2 = r2.value;
    CHECK(prov_buffer_byte_len(b2) == 5, "fb len 5");
    CHECK(prov_buffer_line_count(b2) == 3, "fb 3 lines");
    CHECK(prov_buffer_line_start(b2, 0) == 0, "fb line0 @0");
    CHECK(prov_buffer_line_start(b2, 1) == 2, "fb line1 @2");
    CHECK(prov_buffer_line_start(b2, 2) == 4, "fb line2 @4");
    CHECK(content_is(b2, "a\nb\nc"), "fb content");

    /* delete across a newline, then undo */
    CHECK(prov_buffer_delete(b2, 1, 2) == PROVEN_OK, "fb delete \\nb");
    CHECK(content_is(b2, "a\nc"), "fb after cross-newline delete");
    CHECK(prov_buffer_line_count(b2) == 2, "fb 2 lines after delete");
    CHECK(prov_buffer_undo(b2).applied, "fb undo");
    CHECK(content_is(b2, "a\nb\nc"), "fb undo restored");
    CHECK(prov_buffer_line_count(b2) == 3, "fb 3 lines after undo");

    /* ---- undo on empty stack ---- */
    prov_result_buffer_t r3 = prov_buffer_create(a);
    CHECK(prov_buffer_undo(r3.value).applied == false, "undo empty stack -> false");

    /* ---- bounded undo history (undo_limit) ---- */
    prov_result_buffer_t r4 = prov_buffer_create(a);
    prov_buffer_t *bl = r4.value;
    prov_buffer_set_undo_limit(bl, 2);
    for (int i = 0; i < 5; i++) prov_buffer_insert(bl, prov_buffer_byte_len(bl), U("x"), 1);
    CHECK(prov_buffer_byte_len(bl) == 5, "5 inserts -> len 5");
    int undos = 0;
    while (prov_buffer_undo(bl).applied) undos++;
    CHECK(undos == 2, "only the 2 most recent edits are undoable");
    CHECK(prov_buffer_byte_len(bl) == 3, "older 3 edits forgotten (len stays 3)");
    /* raising the limit afterwards does not resurrect forgotten history */
    prov_buffer_set_undo_limit(bl, 100);
    CHECK(prov_buffer_undo(bl).applied == false, "no more history after trim");
    /* limit 0 = unbounded */
    prov_buffer_set_undo_limit(bl, 0);
    for (int i = 0; i < 4; i++) prov_buffer_insert(bl, prov_buffer_byte_len(bl), U("y"), 1);
    undos = 0;
    while (prov_buffer_undo(bl).applied) undos++;
    CHECK(undos == 4, "unbounded (0) keeps all 4 edits");

    /* ---- randomized incremental line-index cross-check (RFC-0005 Phase C) ----
     * Many random inserts/deletes (with newlines) against a byte-array model;
     * after each edit the buffer's line count and every line start must match a
     * brute-force scan of the model. */
    {
        prov_result_buffer_t rr = prov_buffer_create(a);
        prov_buffer_t *rb = rr.value;
        static char model[5000];
        size_t mlen = 0;
        unsigned seed = 0x1234567u;
        #define RND() (seed = seed * 1103515245u + 12345u, (seed >> 16) & 0x7fff)
        int ok = 1;
        for (int iter = 0; iter < 4000 && ok; iter++) {
            if (mlen + 8 < sizeof model && (mlen == 0 || (RND() % 3) != 0)) {
                size_t at = mlen ? RND() % (mlen + 1) : 0;
                char ins[8]; int il = 1 + (int)(RND() % 5);
                for (int k = 0; k < il; k++) ins[k] = (RND() % 8 == 0) ? '\n' : (char)('a' + RND() % 26);
                if (PROVEN_IS_OK(prov_buffer_insert(rb, at, (const proven_u8 *)ins, (proven_size_t)il))) {
                    memmove(model + at + il, model + at, mlen - at);
                    memcpy(model + at, ins, (size_t)il);
                    mlen += (size_t)il;
                }
            } else if (mlen > 0) {
                size_t at = RND() % mlen;
                size_t maxd = mlen - at; if (maxd > 5) maxd = 5;
                size_t dl = 1 + RND() % maxd;
                if (PROVEN_IS_OK(prov_buffer_delete(rb, at, dl))) {
                    memmove(model + at, model + at + dl, mlen - at - dl);
                    mlen -= dl;
                }
            }
            size_t exp = 1;
            for (size_t k = 0; k < mlen; k++) if (model[k] == '\n') exp++;
            if (prov_buffer_line_count(rb) != exp) { ok = 0; break; }
            if (prov_buffer_line_start(rb, 0) != 0) { ok = 0; break; }
            size_t li = 1;
            for (size_t k = 0; k < mlen && ok; k++)
                if (model[k] == '\n') { if (prov_buffer_line_start(rb, li++) != k + 1) ok = 0; }
        }
        #undef RND
        CHECK(ok, "randomized incremental line index matches the reference model");
        prov_buffer_destroy(rb);
    }

    /* ---- compaction reclaims orphaned add-store bytes (save-time GC) ---- */
    {
        prov_result_buffer_t cr = prov_buffer_create(a);
        prov_buffer_t *cb = cr.value;
        for (int i = 0; i < 500; i++) prov_buffer_insert(cb, prov_buffer_byte_len(cb), U("a"), 1);
        for (int i = 0; i < 400; i++) prov_buffer_delete(cb, 0, 1);      /* orphan ~400 add bytes */
        proven_size_t total_before = prov_buffer_byte_len(cb);
        CHECK(total_before == 100, "100 bytes remain after churn");
        CHECK(prov_buffer_store_bytes(cb) > total_before, "add store holds orphaned bytes before compaction");

        proven_u8 snap[256];
        proven_size_t sn = prov_buffer_copy_range(cb, 0, total_before, snap, sizeof snap);

        CHECK(PROVEN_IS_OK(prov_buffer_compact(cb)), "compact ok");
        CHECK(prov_buffer_byte_len(cb) == total_before, "content length unchanged by compaction");
        proven_u8 snap2[256];
        proven_size_t sn2 = prov_buffer_copy_range(cb, 0, total_before, snap2, sizeof snap2);
        CHECK(sn == sn2 && memcmp(snap, snap2, sn) == 0, "content unchanged by compaction");
        CHECK(prov_buffer_store_bytes(cb) == total_before, "compaction reclaims to exactly the content size");

        /* undo still works across a compaction (the last edit was a delete) */
        CHECK(prov_buffer_undo(cb).applied, "undo works after compaction");
        CHECK(prov_buffer_byte_len(cb) == total_before + 1, "undo restored the deleted byte");
        /* editing keeps working: append and re-check content integrity */
        prov_buffer_insert(cb, prov_buffer_byte_len(cb), U("Z"), 1);
        CHECK(prov_buffer_byte_len(cb) == total_before + 2, "edits resume after compaction");
        prov_buffer_destroy(cb);
    }

    /* ---- scoped undo (field mode, S1) ---- */
    {
        prov_result_buffer_t rs = prov_buffer_create(a);
        prov_buffer_t *sb = rs.value;
        prov_buffer_insert(sb, 0, U("A"), 1);              /* global edit, content "A" */

        prov_buffer_undo_scope_begin(sb);
        prov_buffer_insert(sb, 1, U("B"), 1);              /* scope edits */
        prov_buffer_insert(sb, 2, U("C"), 1);
        CHECK(content_is(sb, "ABC"), "scope edits applied");
        /* in-scope undo/redo act only on the scope */
        CHECK(prov_buffer_undo(sb).applied, "in-scope undo C");
        CHECK(content_is(sb, "AB"), "scope undo -> AB");
        CHECK(prov_buffer_redo(sb).applied, "in-scope redo C");
        CHECK(content_is(sb, "ABC"), "scope redo -> ABC");
        /* a nested begin is ignored (no crash, still one scope) */
        prov_buffer_undo_scope_begin(sb);
        /* revert the whole scope, then end -> global stack must be intact */
        while (prov_buffer_undo(sb).applied) { }
        CHECK(content_is(sb, "A"), "scope fully reverted to pre-scope state");
        prov_buffer_undo_scope_end(sb);
        /* the global "A" insert survived the scope and is still undoable */
        CHECK(prov_buffer_undo(sb).applied, "global edit still undoable after scope");
        CHECK(content_is(sb, ""), "global undo removes A");
        CHECK(prov_buffer_undo(sb).applied == false, "no history below the scope");
        prov_buffer_destroy(sb);
    }
    /* ---- prov_buffer_replace = ONE undoable action (field-mode c commit) ---- */
    {
        prov_result_buffer_t rr = prov_buffer_create_from_bytes(a, U("hello world"), 11);
        prov_buffer_t *rb = rr.value;
        CHECK(prov_buffer_replace(rb, 0, 5, U("hej"), 3) == PROVEN_OK, "replace ok");
        CHECK(content_is(rb, "hej world"), "replace hello->hej");
        CHECK(prov_buffer_line_count(rb) == 1, "replace keeps line count");
        CHECK(prov_buffer_undo(rb).applied, "replace undo (one step)");
        CHECK(content_is(rb, "hello world"), "undo restores original in one step");
        CHECK(prov_buffer_redo(rb).applied, "replace redo");
        CHECK(content_is(rb, "hej world"), "redo re-applies");
        /* degenerate sides fall back to insert / delete */
        CHECK(prov_buffer_replace(rb, 0, 0, U("X"), 1) == PROVEN_OK, "replace ins-only");
        CHECK(content_is(rb, "Xhej world"), "ins-only side");
        CHECK(prov_buffer_replace(rb, 0, 1, NULL, 0) == PROVEN_OK, "replace del-only");
        CHECK(content_is(rb, "hej world"), "del-only side");
        prov_buffer_destroy(rb);
    }

    /* ---- buffer-local bookmarks (M4.3): set + auto-shift on edits ---- */
    {
        prov_result_buffer_t rr = prov_buffer_create_from_bytes(a, U("0123456789"), 10);
        prov_buffer_t *mb = rr.value;
        proven_size_t p;
        CHECK(prov_buffer_get_mark(mb, 0, &p) == false, "unset mark -> false");
        prov_buffer_set_mark(mb, 0, 5);
        CHECK(prov_buffer_get_mark(mb, 0, &p) && p == 5, "mark set at 5");
        prov_buffer_insert(mb, 2, U("XYZ"), 3);
        CHECK(prov_buffer_get_mark(mb, 0, &p) && p == 8, "insert before mark -> +3");
        prov_buffer_insert(mb, 10, U("!"), 1);
        CHECK(prov_buffer_get_mark(mb, 0, &p) && p == 8, "insert after mark -> no shift");
        prov_buffer_delete(mb, 0, 2);
        CHECK(prov_buffer_get_mark(mb, 0, &p) && p == 6, "delete before mark -> -2");
        prov_buffer_set_mark(mb, 1, 4);
        prov_buffer_delete(mb, 3, 5);                 /* [3,8) contains mark b @4 */
        CHECK(prov_buffer_get_mark(mb, 1, &p) && p == 3, "mark inside deletion clamps to cut");
        CHECK(prov_buffer_get_mark(mb, 25, &p) == false, "slot z still unset");
        prov_buffer_destroy(mb);
    }

    /* defensive: destroy with a scope left open must not leak (ASan) */
    {
        prov_result_buffer_t rs = prov_buffer_create(a);
        prov_buffer_undo_scope_begin(rs.value);
        prov_buffer_insert(rs.value, 0, U("leak?"), 5);
        prov_buffer_destroy(rs.value);   /* frees both scope and saved stacks */
    }

    prov_buffer_destroy(b);
    prov_buffer_destroy(b2);
    prov_buffer_destroy(r3.value);
    prov_buffer_destroy(bl);

    if (failures) {
        fprintf(stderr, "buffer: %d checks failed\n", failures);
        return 1;
    }
    printf("ok: buffer tests passed\n");
    return 0;
}
