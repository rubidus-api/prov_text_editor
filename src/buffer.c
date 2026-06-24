#include "buffer.h"

#include "proven/error.h"
#include "proven/u8str.h"
#include "proven/memory.h"

/*
 * Array-backed piece table (SPEC.md §6, §20).
 *
 *   - original : immutable owned copy of the bytes the buffer was loaded with.
 *   - add      : append-only store for inserted bytes.
 *   - pieces   : ordered descriptors {source, start, len} forming the document.
 *   - lines    : cached byte offset of each logical line start.
 *   - undo/redo: per-edit action stacks carrying the bytes needed to invert.
 *
 * Edit cost is O(n) in the number of pieces / document size (line index is
 * rebuilt by a full scan). That is an intentional Milestone-1 simplification;
 * the opaque interface lets a tree backend replace this later.
 */

typedef enum { SRC_ORIGINAL, SRC_ADD } piece_src_t;

typedef struct {
    piece_src_t   src;
    proven_size_t start;   /* byte offset within the source store */
    proven_size_t len;     /* byte length                         */
} piece_t;

typedef enum { ACT_INSERT, ACT_DELETE, ACT_REPLACE } act_kind_t;

typedef struct {
    act_kind_t    kind;
    proven_size_t at;        /* edit position                                     */
    proven_size_t len;       /* INSERT/DELETE: affected len; REPLACE: inserted len */
    proven_u8    *bytes;     /* owned copy of the inserted/affected text          */
    proven_size_t del_len;   /* REPLACE: deleted length (0 otherwise)             */
    proven_u8    *del_bytes; /* REPLACE: owned copy of the deleted text (else 0)  */
} action_t;

struct prov_buffer {
    proven_allocator_t alloc;

    proven_u8    *original;
    proven_size_t original_len;

    proven_u8str_t add;           /* append-only store for inserted bytes (proven string) */

    piece_t      *pieces;
    proven_size_t piece_len, piece_cap;

    proven_size_t total;          /* cached document byte length */
    proven_size_t total_chars;    /* cached document code-point count (incremental) */

    proven_size_t *lines;         /* line start offsets          */
    proven_size_t  line_len, line_cap;

    action_t     *undo;
    proven_size_t undo_len, undo_cap;
    action_t     *redo;
    proven_size_t redo_len, redo_cap;
    proven_size_t undo_limit;     /* max retained undo actions; 0 = unbounded */

    /* One-level scoped-undo save area (field mode): begin swaps the live stacks
     * out to s_*, installing fresh empty stacks; end frees the scope stacks and
     * restores s_*. So edit/undo/redo run unchanged on the isolated scope. */
    bool          scope_active;
    action_t     *s_undo; proven_size_t s_undo_len, s_undo_cap;
    action_t     *s_redo; proven_size_t s_redo_len, s_redo_cap;

    /* buffer-local a–z bookmarks (M4.3); byte offsets, auto-shifted by edits */
    proven_size_t marks[26];
    bool          mark_set[26];
};

#define PROV_ALIGN 16

/* ------------------------------------------------------------ allocation */

static void *xalloc(prov_buffer_t *b, proven_size_t n, proven_err_t *err) {
    proven_result_mem_mut_t r = b->alloc.alloc_fn(b->alloc.ctx, n, PROV_ALIGN);
    *err = r.err;
    return PROVEN_IS_OK(r.err) ? r.value.ptr : NULL;
}

static void xfree(prov_buffer_t *b, void *p) {
    if (p) b->alloc.free_fn(b->alloc.ctx, p);
}

/* Ensure `*cap` (in elements) can hold `need` elements of `elemsz`, growing the
 * block at `*data` geometrically. Returns PROVEN_OK or PROVEN_ERR_NOMEM. */
static proven_err_t ensure_cap(prov_buffer_t *b, void **data, proven_size_t *cap,
                               proven_size_t need, proven_size_t elemsz) {
    if (*cap >= need) return PROVEN_OK;
    proven_size_t newcap = *cap ? *cap * 2 : 8;
    while (newcap < need) newcap *= 2;

    proven_result_mem_mut_t r;
    if (*data == NULL) {
        r = b->alloc.alloc_fn(b->alloc.ctx, newcap * elemsz, PROV_ALIGN);
    } else {
        r = b->alloc.realloc_fn(b->alloc.ctx, *data, (*cap) * elemsz,
                                newcap * elemsz, PROV_ALIGN);
    }
    if (!PROVEN_IS_OK(r.err)) return r.err;
    *data = r.value.ptr;
    *cap = newcap;
    return PROVEN_OK;
}

static const proven_u8 *src_bytes(const prov_buffer_t *b, piece_src_t s) {
    return s == SRC_ORIGINAL ? b->original
                             : (const proven_u8 *)proven_u8str_as_view(&b->add).ptr;
}

/* ----------------------------------------------------------- char count */

/* UTF-8 code points in `[p, p+n)` = bytes that are not continuation bytes
 * (0b10xxxxxx). For valid UTF-8 this matches the decode-walk count; the buffer
 * is always valid UTF-8 (the encoding layer guarantees it), so the cheap
 * lead-byte count stays exact and lets `total_chars` be maintained per edit. */
static proven_size_t count_lead_bytes(const proven_u8 *p, proven_size_t n) {
    proven_size_t c = 0;
    for (proven_size_t i = 0; i < n; i++) if ((p[i] & 0xC0) != 0x80) c++;
    return c;
}

/* Code points in document byte range `[at, end)`, walking pieces without a copy
 * (used to decrement total_chars before a deletion removes the bytes). */
static proven_size_t chars_in_range(const prov_buffer_t *b, proven_size_t at,
                                    proven_size_t end) {
    proven_size_t pos = 0, total = 0;
    for (proven_size_t i = 0; i < b->piece_len && pos < end; i++) {
        const piece_t *p = &b->pieces[i];
        proven_size_t pstart = pos, pend = pos + p->len;
        if (pend > at && pstart < end) {
            proven_size_t a = at > pstart ? at : pstart;
            proven_size_t e = end < pend ? end : pend;
            total += count_lead_bytes(src_bytes(b, p->src) + p->start + (a - pstart),
                                      e - a);
        }
        pos = pend;
    }
    return total;
}

/* ------------------------------------------------------------ line index */

static proven_err_t rebuild_lines(prov_buffer_t *b) {
    b->line_len = 0;
    proven_err_t err = ensure_cap(b, (void **)&b->lines, &b->line_cap, 1,
                                  sizeof(proven_size_t));
    if (!PROVEN_IS_OK(err)) return err;
    b->lines[b->line_len++] = 0;

    proven_size_t pos = 0;
    for (proven_size_t i = 0; i < b->piece_len; i++) {
        const piece_t *p = &b->pieces[i];
        const proven_u8 *sp = src_bytes(b, p->src) + p->start;
        for (proven_size_t j = 0; j < p->len; j++) {
            if (sp[j] == '\n') {
                err = ensure_cap(b, (void **)&b->lines, &b->line_cap,
                                 b->line_len + 1, sizeof(proven_size_t));
                if (!PROVEN_IS_OK(err)) return err;
                b->lines[b->line_len++] = pos + j + 1;
            }
        }
        pos += p->len;
    }
    return PROVEN_OK;
}

/* Incremental line-index update for an insertion of `len` bytes at `at`
 * (RFC-0005 Phase C). Line starts strictly after `at` shift by +len; each '\n'
 * in the inserted bytes adds a new line start at `at + j + 1`. O(lines after
 * `at`) instead of rebuild_lines' O(total bytes). */
static proven_err_t lines_apply_insert(prov_buffer_t *b, proven_size_t at,
                                       const proven_u8 *bytes, proven_size_t len) {
    proven_size_t add = 0;
    for (proven_size_t j = 0; j < len; j++) if (bytes[j] == '\n') add++;

    proven_size_t s = b->line_len;                 /* first start strictly > at */
    for (proven_size_t i = 0; i < b->line_len; i++)
        if (b->lines[i] > at) { s = i; break; }

    if (add > 0) {
        proven_err_t err = ensure_cap(b, (void **)&b->lines, &b->line_cap,
                                      b->line_len + add, sizeof(proven_size_t));
        if (!PROVEN_IS_OK(err)) return err;
        proven_mem_move(&b->lines[s + add],
                        (b->line_cap - (s + add)) * sizeof(proven_size_t),
                        (proven_mem_view_t){ (const proven_byte_t *)&b->lines[s],
                                             (b->line_len - s) * sizeof(proven_size_t) });
        b->line_len += add;
    }
    for (proven_size_t i = s + add; i < b->line_len; i++) b->lines[i] += len;
    proven_size_t w = s;
    for (proven_size_t j = 0; j < len; j++)
        if (bytes[j] == '\n') b->lines[w++] = at + j + 1;
    return PROVEN_OK;
}

/* Incremental line-index update for a deletion of `len` bytes at `at`. Line
 * starts in (at, at+len] vanish (their '\n' was removed); starts after at+len
 * shift by -len. O(lines after `at`). */
static void lines_apply_delete(prov_buffer_t *b, proven_size_t at, proven_size_t len) {
    proven_size_t end = at + len;
    proven_size_t s = b->line_len;
    for (proven_size_t i = 0; i < b->line_len; i++)
        if (b->lines[i] > at) { s = i; break; }
    proven_size_t e = s;
    while (e < b->line_len && b->lines[e] <= end) e++;
    proven_size_t removed = e - s;
    if (removed > 0) {
        proven_mem_move(&b->lines[s],
                        (b->line_cap - s) * sizeof(proven_size_t),
                        (proven_mem_view_t){ (const proven_byte_t *)&b->lines[e],
                                             (b->line_len - e) * sizeof(proven_size_t) });
        b->line_len -= removed;
    }
    for (proven_size_t i = s; i < b->line_len; i++) b->lines[i] -= len;
}

/* Keep bookmarks anchored to their content across edits (M4.3). */
static void marks_apply_insert(prov_buffer_t *b, proven_size_t at, proven_size_t len) {
    for (int i = 0; i < 26; i++)
        if (b->mark_set[i] && b->marks[i] >= at) b->marks[i] += len;
}
static void marks_apply_delete(prov_buffer_t *b, proven_size_t at, proven_size_t len) {
    proven_size_t end = at + len;
    for (int i = 0; i < 26; i++) {
        if (!b->mark_set[i]) continue;
        if (b->marks[i] >= end)     b->marks[i] -= len;       /* after: shift left */
        else if (b->marks[i] > at)  b->marks[i] = at;         /* inside: clamp to the cut */
    }
}

/* Debug-only cross-check: the incremental line index must match a full rescan.
 * Compiled out unless -DPROV_DEBUG_LINES; the whole test suite then validates
 * every edit's line index against rebuild_lines' logic. */
#ifdef PROV_DEBUG_LINES
static void lines_verify(const prov_buffer_t *b) {
    if (b->line_len == 0 || b->lines[0] != 0) __builtin_trap();
    proven_size_t expect = 1, pos = 0;
    for (proven_size_t i = 0; i < b->piece_len; i++) {
        const piece_t *p = &b->pieces[i];
        const proven_u8 *sp = src_bytes(b, p->src) + p->start;
        for (proven_size_t j = 0; j < p->len; j++)
            if (sp[j] == '\n') {
                if (expect >= b->line_len || b->lines[expect] != pos + j + 1) __builtin_trap();
                expect++;
            }
        pos += p->len;
    }
    if (expect != b->line_len) __builtin_trap();
    if (b->total_chars != chars_in_range(b, 0, b->total)) __builtin_trap();
}
#define LINES_VERIFY(b) lines_verify(b)
#else
#define LINES_VERIFY(b) ((void)0)
#endif

/* ------------------------------------------------------------ piece ops */

static proven_err_t pieces_insert_at(prov_buffer_t *b, proven_size_t idx,
                                     piece_t pc) {
    proven_err_t err = ensure_cap(b, (void **)&b->pieces, &b->piece_cap,
                                  b->piece_len + 1, sizeof(piece_t));
    if (!PROVEN_IS_OK(err)) return err;
    proven_mem_move(&b->pieces[idx + 1],
                    (b->piece_cap - (idx + 1)) * sizeof(piece_t),
                    (proven_mem_view_t){ (const proven_byte_t *)&b->pieces[idx],
                                         (b->piece_len - idx) * sizeof(piece_t) });
    b->pieces[idx] = pc;
    b->piece_len++;
    return PROVEN_OK;
}

/* Apply an insertion without touching the undo/redo stacks. */
static proven_err_t insert_raw(prov_buffer_t *b, proven_size_t at,
                               const proven_u8 *bytes, proven_size_t len) {
    if (at > b->total) return PROVEN_ERR_OUT_OF_BOUNDS;
    if (len == 0) return PROVEN_OK;

    /* append bytes to the add store (the proven string grows as needed). On a
     * later piece-allocation failure the appended bytes are simply orphaned —
     * harmless wasted space; the document content stays correct. */
    proven_size_t addpos = proven_u8str_as_view(&b->add).size;
    proven_err_t err = proven_u8str_append_grow(b->alloc, &b->add,
                                                (proven_u8str_view_t){ bytes, len });
    if (!PROVEN_IS_OK(err)) return err;

    /* locate the piece/offset where `at` falls */
    proven_size_t pos = 0, idx = b->piece_len, off = 0;
    for (proven_size_t i = 0; i < b->piece_len; i++) {
        if (at <= pos + b->pieces[i].len) {
            idx = i;
            off = at - pos;
            break;
        }
        pos += b->pieces[i].len;
    }

    /* Coalesce sequential typing: if `at` sits at the end of an add-piece whose
     * bytes are contiguous with the freshly-appended add bytes, extend that
     * piece instead of allocating a new one. Keeps the piece count — and thus
     * this lookup — flat while typing (RFC-0005 Phase C.2). */
    piece_t *prev = NULL;
    if (idx == b->piece_len) { if (b->piece_len > 0) prev = &b->pieces[b->piece_len - 1]; }
    else if (off == b->pieces[idx].len) prev = &b->pieces[idx];
    if (prev && prev->src == SRC_ADD && prev->start + prev->len == addpos) {
        prev->len += len;
        b->total += len;
        b->total_chars += count_lead_bytes(bytes, len);
        err = lines_apply_insert(b, at, bytes, len);
        if (!PROVEN_IS_OK(err)) return err;
        marks_apply_insert(b, at, len);
        LINES_VERIFY(b);
        return PROVEN_OK;
    }

    piece_t newp = { SRC_ADD, addpos, len };

    if (idx == b->piece_len) {
        /* append at end */
        err = pieces_insert_at(b, b->piece_len, newp);
    } else if (off == 0) {
        err = pieces_insert_at(b, idx, newp);
    } else if (off == b->pieces[idx].len) {
        err = pieces_insert_at(b, idx + 1, newp);
    } else {
        /* split pieces[idx] at off: left keeps [0,off), right takes [off,len) */
        piece_t right = { b->pieces[idx].src, b->pieces[idx].start + off,
                          b->pieces[idx].len - off };
        b->pieces[idx].len = off;
        err = pieces_insert_at(b, idx + 1, right);
        if (PROVEN_IS_OK(err)) err = pieces_insert_at(b, idx + 1, newp);
    }
    if (!PROVEN_IS_OK(err)) return err;

    b->total += len;
    b->total_chars += count_lead_bytes(bytes, len);
    err = lines_apply_insert(b, at, bytes, len);
    if (!PROVEN_IS_OK(err)) return err;
    marks_apply_insert(b, at, len);
    LINES_VERIFY(b);
    return PROVEN_OK;
}

/* Apply a deletion without touching the undo/redo stacks. */
static proven_err_t delete_raw(prov_buffer_t *b, proven_size_t at,
                               proven_size_t len) {
    if (at > b->total || len > b->total - at) return PROVEN_ERR_OUT_OF_BOUNDS;
    if (len == 0) return PROVEN_OK;
    proven_size_t end = at + len;
    proven_size_t del_chars = chars_in_range(b, at, end);  /* count before pieces change */

    /* rebuild the piece list, keeping the spans outside [at, end) */
    piece_t      *np = NULL;
    proven_size_t nlen = 0, ncap = 0;
    proven_err_t  err = PROVEN_OK;
    proven_size_t pos = 0;

    for (proven_size_t i = 0; i < b->piece_len; i++) {
        const piece_t *p = &b->pieces[i];
        proven_size_t pstart = pos, pend = pos + p->len;

        if (pstart < at) {                       /* keep left part [pstart,min(at,pend)) */
            proven_size_t lend = at < pend ? at : pend;
            if (lend > pstart) {
                piece_t keep = { p->src, p->start, lend - pstart };
                err = ensure_cap(b, (void **)&np, &ncap, nlen + 1, sizeof(piece_t));
                if (!PROVEN_IS_OK(err)) goto fail;
                np[nlen++] = keep;
            }
        }
        if (pend > end) {                        /* keep right part [max(end,pstart),pend) */
            proven_size_t rstart = end > pstart ? end : pstart;
            if (rstart < pend) {
                piece_t keep = { p->src, p->start + (rstart - pstart), pend - rstart };
                err = ensure_cap(b, (void **)&np, &ncap, nlen + 1, sizeof(piece_t));
                if (!PROVEN_IS_OK(err)) goto fail;
                np[nlen++] = keep;
            }
        }
        pos = pend;
    }

    xfree(b, b->pieces);
    b->pieces = np;
    b->piece_len = nlen;
    b->piece_cap = ncap;
    b->total -= len;
    b->total_chars -= del_chars;
    lines_apply_delete(b, at, len);
    marks_apply_delete(b, at, len);
    LINES_VERIFY(b);
    return PROVEN_OK;

fail:
    xfree(b, np);
    return err;
}

/* ------------------------------------------------------------ undo stacks */

static void clear_stack(prov_buffer_t *b, action_t *stack, proven_size_t *len) {
    for (proven_size_t i = 0; i < *len; i++) {
        xfree(b, stack[i].bytes);
        xfree(b, stack[i].del_bytes);   /* NULL for INSERT/DELETE (xfree is NULL-safe) */
    }
    *len = 0;
}

static proven_err_t push_action(prov_buffer_t *b, action_t **stack,
                                proven_size_t *len, proven_size_t *cap,
                                action_t a) {
    proven_err_t err = ensure_cap(b, (void **)stack, cap, *len + 1, sizeof(action_t));
    if (!PROVEN_IS_OK(err)) return err;
    (*stack)[(*len)++] = a;
    return PROVEN_OK;
}

/* Drop the oldest undo actions so at most `undo_limit` remain (0 = unbounded).
 * Called after recording a new user edit; the freed actions are the least
 * recent, which is what a bounded history forgets first. */
static void trim_undo(prov_buffer_t *b) {
    if (b->undo_limit == 0 || b->undo_len <= b->undo_limit) return;
    proven_size_t drop = b->undo_len - b->undo_limit;
    for (proven_size_t i = 0; i < drop; i++) {
        xfree(b, b->undo[i].bytes);
        xfree(b, b->undo[i].del_bytes);
    }
    proven_mem_move(b->undo, b->undo_cap * sizeof(action_t),
                    (proven_mem_view_t){ (const proven_byte_t *)(b->undo + drop),
                                         (b->undo_len - drop) * sizeof(action_t) });
    b->undo_len -= drop;
}

void prov_buffer_set_undo_limit(prov_buffer_t *buf, proven_size_t limit) {
    buf->undo_limit = limit;
    trim_undo(buf);
}

/* Begin an isolated undo scope: swap the live stacks aside and install fresh,
 * empty ones. Subsequent edits and prov_buffer_undo/redo act only on the scope.
 * No nesting (a second begin is ignored). */
void prov_buffer_undo_scope_begin(prov_buffer_t *buf) {
    if (!buf || buf->scope_active) return;
    buf->s_undo = buf->undo; buf->s_undo_len = buf->undo_len; buf->s_undo_cap = buf->undo_cap;
    buf->s_redo = buf->redo; buf->s_redo_len = buf->redo_len; buf->s_redo_cap = buf->redo_cap;
    buf->undo = NULL; buf->undo_len = 0; buf->undo_cap = 0;
    buf->redo = NULL; buf->redo_len = 0; buf->redo_cap = 0;
    buf->scope_active = true;
}

/* End the scope: free the scope's stacks and restore the saved live stacks. The
 * caller is responsible for leaving the buffer content consistent with the
 * restored stacks (typically: undo the whole scope, end, then apply one edit). */
void prov_buffer_undo_scope_end(prov_buffer_t *buf) {
    if (!buf || !buf->scope_active) return;
    clear_stack(buf, buf->undo, &buf->undo_len); xfree(buf, buf->undo);
    clear_stack(buf, buf->redo, &buf->redo_len); xfree(buf, buf->redo);
    buf->undo = buf->s_undo; buf->undo_len = buf->s_undo_len; buf->undo_cap = buf->s_undo_cap;
    buf->redo = buf->s_redo; buf->redo_len = buf->s_redo_len; buf->redo_cap = buf->s_redo_cap;
    buf->s_undo = NULL; buf->s_undo_len = buf->s_undo_cap = 0;
    buf->s_redo = NULL; buf->s_redo_len = buf->s_redo_cap = 0;
    buf->scope_active = false;
}

/* ---- buffer-local bookmarks (M4.3) ---- */
void prov_buffer_set_mark(prov_buffer_t *buf, int idx, proven_size_t pos) {
    if (!buf || idx < 0 || idx >= 26) return;
    buf->marks[idx] = pos > buf->total ? buf->total : pos;
    buf->mark_set[idx] = true;
}
bool prov_buffer_get_mark(const prov_buffer_t *buf, int idx, proven_size_t *pos) {
    if (!buf || idx < 0 || idx >= 26 || !buf->mark_set[idx]) return false;
    if (pos) *pos = buf->marks[idx];
    return true;
}
void prov_buffer_clear_mark(prov_buffer_t *buf, int idx) {
    if (!buf || idx < 0 || idx >= 26) return;
    buf->mark_set[idx] = false;
}

/* Total bytes held by the two content stores (original + add). The add store is
 * append-only, so deleted text leaves orphaned bytes here until a compaction —
 * this lets callers/tests observe that reclaimable waste. */
proven_size_t prov_buffer_store_bytes(const prov_buffer_t *buf) {
    if (!buf) return 0;
    return buf->original_len + proven_u8str_as_view(&buf->add).size;
}

/* Reclaim orphaned add-store bytes (and collapse the piece list) by
 * re-materializing the current content as a single original piece with a fresh,
 * empty add store. The logical content, length, line index, cursor positions,
 * and undo/redo history are unchanged; only the internal representation and the
 * memory it holds shrink. Failure-atomic: on any allocation failure the buffer
 * is left exactly as it was. */
proven_err_t prov_buffer_compact(prov_buffer_t *b) {
    if (!b) return PROVEN_ERR_INVALID_ARG;
    proven_size_t n = b->total;
    proven_err_t err = PROVEN_OK;

    proven_u8 *fresh = NULL;                          /* the new contiguous original */
    if (n > 0) {
        fresh = (proven_u8 *)xalloc(b, n, &err);
        if (!PROVEN_IS_OK(err)) return err;
        prov_buffer_copy_range(b, 0, n, fresh, n);
    }
    proven_result_u8str_t ar = proven_u8str_create(b->alloc, 64);   /* fresh empty add */
    if (!PROVEN_IS_OK(ar.err)) { xfree(b, fresh); return ar.err; }
    if (n > 0) {
        err = ensure_cap(b, (void **)&b->pieces, &b->piece_cap, 1, sizeof(piece_t));
        if (!PROVEN_IS_OK(err)) { xfree(b, fresh); proven_u8str_destroy(b->alloc, &ar.value); return err; }
    }

    /* commit: swap in the fresh stores and drop the orphaned ones */
    xfree(b, b->original);
    b->original = fresh;
    b->original_len = n;
    proven_u8str_destroy(b->alloc, &b->add);
    b->add = ar.value;
    b->piece_len = 0;
    if (n > 0) { b->pieces[0] = (piece_t){ SRC_ORIGINAL, 0, n }; b->piece_len = 1; }
    /* content/total unchanged -> the line index stays valid */
    LINES_VERIFY(b);
    return PROVEN_OK;
}

/* ------------------------------------------------------------ lifecycle */

prov_result_buffer_t prov_buffer_create(proven_allocator_t alloc) {
    prov_result_buffer_t out = { NULL, PROVEN_OK };
    if (!proven_alloc_is_valid(alloc)) {
        out.err = PROVEN_ERR_INVALID_ARG;
        return out;
    }
    proven_result_mem_mut_t r =
        alloc.alloc_fn(alloc.ctx, sizeof(struct prov_buffer), PROV_ALIGN);
    if (!PROVEN_IS_OK(r.err)) {
        out.err = r.err;
        return out;
    }
    prov_buffer_t *b = (prov_buffer_t *)r.value.ptr;
    *b = (struct prov_buffer){0};
    b->alloc = alloc;

    proven_result_u8str_t ar = proven_u8str_create(alloc, 64);  /* the add store */
    if (!PROVEN_IS_OK(ar.err)) {
        alloc.free_fn(alloc.ctx, b);
        out.err = ar.err;
        return out;
    }
    b->add = ar.value;

    proven_err_t err = rebuild_lines(b);   /* establishes line 0 == offset 0 */
    if (!PROVEN_IS_OK(err)) {
        proven_u8str_destroy(alloc, &b->add);
        alloc.free_fn(alloc.ctx, b);
        out.err = err;
        return out;
    }
    out.value = b;
    return out;
}

prov_result_buffer_t prov_buffer_create_from_bytes(proven_allocator_t alloc,
                                                   const proven_u8 *bytes,
                                                   proven_size_t len) {
    prov_result_buffer_t out = prov_buffer_create(alloc);
    if (!PROVEN_IS_OK(out.err)) return out;
    prov_buffer_t *b = out.value;

    if (len > 0) {
        proven_err_t err;
        b->original = (proven_u8 *)xalloc(b, len, &err);
        if (!PROVEN_IS_OK(err)) goto fail;
        proven_mem_copy(b->original, len, (proven_mem_view_t){ bytes, len });
        b->original_len = len;

        piece_t p = { SRC_ORIGINAL, 0, len };
        err = pieces_insert_at(b, 0, p);
        if (!PROVEN_IS_OK(err)) goto fail;
        b->total = len;
        b->total_chars = count_lead_bytes(bytes, len);

        err = rebuild_lines(b);
        if (!PROVEN_IS_OK(err)) goto fail;
    }
    return out;

fail:
    prov_buffer_destroy(b);
    out.value = NULL;
    out.err = PROVEN_ERR_NOMEM;
    return out;
}

void prov_buffer_destroy(prov_buffer_t *buf) {
    if (!buf) return;
    if (buf->scope_active) {       /* defensive: a field scope left open */
        clear_stack(buf, buf->s_undo, &buf->s_undo_len);
        clear_stack(buf, buf->s_redo, &buf->s_redo_len);
        xfree(buf, buf->s_undo);
        xfree(buf, buf->s_redo);
    }
    clear_stack(buf, buf->undo, &buf->undo_len);
    clear_stack(buf, buf->redo, &buf->redo_len);
    xfree(buf, buf->undo);
    xfree(buf, buf->redo);
    xfree(buf, buf->lines);
    xfree(buf, buf->pieces);
    proven_u8str_destroy(buf->alloc, &buf->add);
    xfree(buf, buf->original);
    proven_allocator_t a = buf->alloc;
    a.free_fn(a.ctx, buf);
}

/* ------------------------------------------------------------ queries */

proven_size_t prov_buffer_byte_len(const prov_buffer_t *buf) {
    return buf->total;
}

proven_size_t prov_buffer_char_total(const prov_buffer_t *buf) {
    return buf->total_chars;
}

proven_size_t prov_buffer_line_count(const prov_buffer_t *buf) {
    return buf->line_len;
}

proven_size_t prov_buffer_line_start(const prov_buffer_t *buf,
                                     proven_size_t line) {
    if (line >= buf->line_len) return buf->total;
    return buf->lines[line];
}

proven_size_t prov_buffer_copy_range(const prov_buffer_t *buf,
                                     proven_size_t at, proven_size_t len,
                                     proven_u8 *out, proven_size_t out_cap) {
    if (at >= buf->total) return 0;
    if (len > buf->total - at) len = buf->total - at;
    if (len > out_cap) len = out_cap;
    proven_size_t end = at + len;

    proven_size_t pos = 0, copied = 0;
    for (proven_size_t i = 0; i < buf->piece_len && copied < len; i++) {
        const piece_t *p = &buf->pieces[i];
        proven_size_t pstart = pos, pend = pos + p->len;
        proven_size_t s = at > pstart ? at : pstart;
        proven_size_t e = end < pend ? end : pend;
        if (s < e) {
            const proven_u8 *sp = src_bytes(buf, p->src) + p->start + (s - pstart);
            proven_mem_copy(out + (s - at), out_cap - (s - at), (proven_mem_view_t){ sp, e - s });
            copied += e - s;
        }
        pos = pend;
    }
    return copied;
}

/* ------------------------------------------------------------ edits */

proven_err_t prov_buffer_insert(prov_buffer_t *buf, proven_size_t at,
                                const proven_u8 *bytes, proven_size_t len) {
    if (at > buf->total) return PROVEN_ERR_OUT_OF_BOUNDS;
    if (len == 0) return PROVEN_OK;

    proven_err_t err;
    proven_u8 *copy = (proven_u8 *)xalloc(buf, len, &err);
    if (!PROVEN_IS_OK(err)) return err;
    proven_mem_copy(copy, len, (proven_mem_view_t){ bytes, len });

    err = insert_raw(buf, at, bytes, len);
    if (!PROVEN_IS_OK(err)) { xfree(buf, copy); return err; }

    action_t a = { ACT_INSERT, at, len, copy, 0, NULL };
    err = push_action(buf, &buf->undo, &buf->undo_len, &buf->undo_cap, a);
    if (!PROVEN_IS_OK(err)) {
        /* keep the buffer state consistent: undo the raw edit we just did */
        delete_raw(buf, at, len);
        xfree(buf, copy);
        return err;
    }
    clear_stack(buf, buf->redo, &buf->redo_len);
    trim_undo(buf);
    return PROVEN_OK;
}

proven_err_t prov_buffer_delete(prov_buffer_t *buf, proven_size_t at,
                                proven_size_t len) {
    if (at > buf->total || len > buf->total - at) return PROVEN_ERR_OUT_OF_BOUNDS;
    if (len == 0) return PROVEN_OK;

    proven_err_t err;
    proven_u8 *copy = (proven_u8 *)xalloc(buf, len, &err);
    if (!PROVEN_IS_OK(err)) return err;
    prov_buffer_copy_range(buf, at, len, copy, len);   /* capture for undo */

    err = delete_raw(buf, at, len);
    if (!PROVEN_IS_OK(err)) { xfree(buf, copy); return err; }

    action_t a = { ACT_DELETE, at, len, copy, 0, NULL };
    err = push_action(buf, &buf->undo, &buf->undo_len, &buf->undo_cap, a);
    if (!PROVEN_IS_OK(err)) {
        insert_raw(buf, at, copy, len);   /* roll back to keep state consistent */
        xfree(buf, copy);
        return err;
    }
    clear_stack(buf, buf->redo, &buf->redo_len);
    trim_undo(buf);
    return PROVEN_OK;
}

/* Replace [pos, pos+del_len) with `ins_len` bytes as ONE undoable action. */
proven_err_t prov_buffer_replace(prov_buffer_t *buf, proven_size_t pos,
                                 proven_size_t del_len,
                                 const proven_u8 *bytes, proven_size_t ins_len) {
    if (pos > buf->total || del_len > buf->total - pos) return PROVEN_ERR_OUT_OF_BOUNDS;
    if (del_len == 0) return prov_buffer_insert(buf, pos, bytes, ins_len);  /* one side empty */
    if (ins_len == 0) return prov_buffer_delete(buf, pos, del_len);

    proven_err_t err;
    proven_u8 *del_copy = (proven_u8 *)xalloc(buf, del_len, &err);
    if (!PROVEN_IS_OK(err)) return err;
    prov_buffer_copy_range(buf, pos, del_len, del_copy, del_len);     /* capture for undo */
    proven_u8 *ins_copy = (proven_u8 *)xalloc(buf, ins_len, &err);
    if (!PROVEN_IS_OK(err)) { xfree(buf, del_copy); return err; }
    proven_mem_copy(ins_copy, ins_len, (proven_mem_view_t){ bytes, ins_len });

    err = delete_raw(buf, pos, del_len);
    if (!PROVEN_IS_OK(err)) goto fail;
    err = insert_raw(buf, pos, bytes, ins_len);
    if (!PROVEN_IS_OK(err)) { insert_raw(buf, pos, del_copy, del_len); goto fail; }

    action_t a = { ACT_REPLACE, pos, ins_len, ins_copy, del_len, del_copy };
    err = push_action(buf, &buf->undo, &buf->undo_len, &buf->undo_cap, a);
    if (!PROVEN_IS_OK(err)) {                 /* roll back to keep content consistent */
        delete_raw(buf, pos, ins_len);
        insert_raw(buf, pos, del_copy, del_len);
        goto fail;
    }
    clear_stack(buf, buf->redo, &buf->redo_len);
    trim_undo(buf);
    return PROVEN_OK;
fail:
    xfree(buf, del_copy);
    xfree(buf, ins_copy);
    return err;
}

/* ------------------------------------------------------------ undo / redo */

prov_edit_info_t prov_buffer_undo(prov_buffer_t *buf) {
    prov_edit_info_t info = { false, false, 0, 0 };
    if (buf->undo_len == 0) return info;
    action_t a = buf->undo[--buf->undo_len];

    if (a.kind == ACT_INSERT)        delete_raw(buf, a.at, a.len);
    else if (a.kind == ACT_DELETE)   insert_raw(buf, a.at, a.bytes, a.len);
    else {                           /* ACT_REPLACE: remove inserted, restore deleted */
        delete_raw(buf, a.at, a.len);
        if (a.del_len) insert_raw(buf, a.at, a.del_bytes, a.del_len);
    }

    info.applied = true;
    info.was_insert = (a.kind == ACT_INSERT);
    info.at = a.at;
    info.len = (a.kind == ACT_REPLACE) ? a.del_len : a.len;

    /* move the action (with its owned bytes) onto the redo stack */
    if (!PROVEN_IS_OK(push_action(buf, &buf->redo, &buf->redo_len,
                                  &buf->redo_cap, a))) {
        xfree(buf, a.bytes);   /* drop redo history rather than leak */
    }
    return info;
}

prov_edit_info_t prov_buffer_redo(prov_buffer_t *buf) {
    prov_edit_info_t info = { false, false, 0, 0 };
    if (buf->redo_len == 0) return info;
    action_t a = buf->redo[--buf->redo_len];

    if (a.kind == ACT_INSERT)        insert_raw(buf, a.at, a.bytes, a.len);
    else if (a.kind == ACT_DELETE)   delete_raw(buf, a.at, a.len);
    else {                           /* ACT_REPLACE: remove deleted, re-apply inserted */
        if (a.del_len) delete_raw(buf, a.at, a.del_len);
        insert_raw(buf, a.at, a.bytes, a.len);
    }

    info.applied = true;
    info.was_insert = (a.kind != ACT_DELETE);   /* place cursor after inserted text */
    info.at = a.at;
    info.len = a.len;

    if (!PROVEN_IS_OK(push_action(buf, &buf->undo, &buf->undo_len,
                                  &buf->undo_cap, a))) {
        xfree(buf, a.bytes);
    }
    return info;
}

proven_size_t prov_buffer_undo_depth(const prov_buffer_t *buf) {
    return buf ? buf->undo_len : 0;
}
bool prov_buffer_undo_peek(const prov_buffer_t *buf, proven_size_t i, prov_undo_view_t *out) {
    if (!buf || i >= buf->undo_len) return false;
    const action_t *a = &buf->undo[buf->undo_len - 1 - i];   /* i = 0 is the most recent */
    if (out) {
        out->is_insert  = (a->kind == ACT_INSERT);
        out->is_replace = (a->kind == ACT_REPLACE);
        out->at         = a->at;
        out->len        = a->len;       /* primary affected length */
        out->bytes      = a->bytes;     /* inserted (INSERT/REPLACE) or deleted (DELETE) text */
        out->bytes_len  = a->len;
    }
    return true;
}
