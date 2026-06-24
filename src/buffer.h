#ifndef PROV_BUFFER_H
#define PROV_BUFFER_H

#include "proven/types.h"
#include "proven/allocator.h"

/*
 * prov document buffer.
 *
 * Opaque, array-backed piece table (SPEC.md §6) with a buffer-owned
 * line-offset index and per-edit undo/redo (SPEC.md §8, §20). The array
 * backend is a deliberate Milestone-1 choice; the public interface hides it so
 * a balanced-tree backend can replace it without touching callers.
 *
 * All storage is internal UTF-8 bytes. The buffer does not validate encoding;
 * that is the loader's responsibility (encoding module).
 */

typedef struct prov_buffer prov_buffer_t;

typedef struct {
    prov_buffer_t *value;
    proven_err_t   err;
} prov_result_buffer_t;

/* Create an empty buffer.
 * err = PROVEN_ERR_INVALID_ARG if `alloc` is invalid, PROVEN_ERR_NOMEM on
 * allocation failure. */
prov_result_buffer_t prov_buffer_create(proven_allocator_t alloc);

/* Create a buffer initialized from `len` bytes, copied into the original
 * store. `bytes` may be NULL iff `len` is 0. */
prov_result_buffer_t prov_buffer_create_from_bytes(proven_allocator_t alloc,
                                                   const proven_u8 *bytes,
                                                   proven_size_t len);

/* Destroy a buffer and release all owned memory. Safe to call on NULL. */
void prov_buffer_destroy(prov_buffer_t *buf);

/* Reclaim orphaned add-store bytes and collapse the piece list by
 * re-materializing the current content as a single original piece. Logical
 * content, length, line index, and undo/redo are unchanged; the memory the
 * buffer holds shrinks. Failure-atomic. Intended to run on save. */
proven_err_t prov_buffer_compact(prov_buffer_t *buf);

/* Bytes held by the original + add content stores. Grows with every insert and
 * is only reclaimed by prov_buffer_compact, so it can exceed the document
 * length after heavy edit churn (introspection / tests). */
proven_size_t prov_buffer_store_bytes(const prov_buffer_t *buf);

/* ---- queries ---- */

/* Total document length in bytes. */
proven_size_t prov_buffer_byte_len(const prov_buffer_t *buf);

/* Total document length in Unicode code points (maintained incrementally, O(1));
 * `prov_buffer_char_count` in display.h is the public name and delegates here. */
proven_size_t prov_buffer_char_total(const prov_buffer_t *buf);

/* Number of logical lines: (count of '\n') + 1. An empty buffer has 1 line. */
proven_size_t prov_buffer_line_count(const prov_buffer_t *buf);

/* Byte offset where logical line `line` (0-based) begins. Line 0 starts at 0.
 * For `line` >= line_count, returns the document byte length. */
proven_size_t prov_buffer_line_start(const prov_buffer_t *buf,
                                     proven_size_t line);

/* Copy the document range [at, at+len) into `out`, clamped to the document and
 * to `out_cap`. Returns the number of bytes actually copied. */
proven_size_t prov_buffer_copy_range(const prov_buffer_t *buf,
                                     proven_size_t at, proven_size_t len,
                                     proven_u8 *out, proven_size_t out_cap);

/* ---- edits (each call is one undoable action) ---- */

/* Insert `len` bytes at byte offset `at`.
 * err = PROVEN_ERR_OUT_OF_BOUNDS if at > byte_len, PROVEN_ERR_NOMEM on
 * allocation failure. A zero-length insert is a successful no-op. */
proven_err_t prov_buffer_insert(prov_buffer_t *buf, proven_size_t at,
                                const proven_u8 *bytes, proven_size_t len);

/* Delete the byte range [at, at+len).
 * err = PROVEN_ERR_OUT_OF_BOUNDS if the range exceeds the document. A
 * zero-length delete is a successful no-op. */
proven_err_t prov_buffer_delete(prov_buffer_t *buf, proven_size_t at,
                                proven_size_t len);

/* Information about the action an undo/redo applied, so callers (the editor)
 * can restore the cursor (SPEC §8). `applied` is false when the stack was
 * empty, in which case the other fields are 0. `at`/`len` describe the byte
 * span of the original edit; `was_insert` is true if that original action was
 * an insertion. */
typedef struct {
    bool          applied;
    bool          was_insert;
    proven_size_t at;
    proven_size_t len;
} prov_edit_info_t;

/* ---- undo / redo ----
 *
 * Each insert/delete pushes exactly one action. Undo applies the inverse and
 * moves the action onto the redo stack; any new edit clears the redo stack.
 * Group-merging of continuous typing (SPEC §8.2) is an editor-layer concern
 * and is intentionally not done here. */
prov_edit_info_t prov_buffer_undo(prov_buffer_t *buf);
prov_edit_info_t prov_buffer_redo(prov_buffer_t *buf);

/* Read-only introspection of the undo stack (for a `0u` undo browser). `depth` is
 * how many undoable actions are retained; `peek` reads the i-th (i = 0 is the most
 * recent), filling a view of the action and a pointer to its text (owned by the
 * buffer — valid only until the next edit). */
typedef struct {
    bool              is_insert;   /* false + !is_replace = a deletion */
    bool              is_replace;
    proven_size_t     at, len;
    const proven_u8  *bytes;       /* inserted text (insert/replace) or deleted text (delete) */
    proven_size_t     bytes_len;
} prov_undo_view_t;
proven_size_t prov_buffer_undo_depth(const prov_buffer_t *buf);
bool prov_buffer_undo_peek(const prov_buffer_t *buf, proven_size_t i, prov_undo_view_t *out);

/* Bound the retained undo history to `limit` most-recent actions (0 = unbounded,
 * the default). Applied as new edits are recorded; lowering it past the current
 * depth trims immediately on the next edit. The redo stack is unaffected. */
void prov_buffer_set_undo_limit(prov_buffer_t *buf, proven_size_t limit);

/* ---- scoped undo (field mode) ----
 * begin swaps the live undo/redo stacks out and installs fresh empty ones, so
 * edits + prov_buffer_undo/redo operate on an isolated, temporary scope. end
 * frees the scope stacks and restores the saved ones. The caller must leave the
 * content consistent with the restored stacks (e.g. undo the whole scope first).
 * Not nestable. */
void prov_buffer_undo_scope_begin(prov_buffer_t *buf);
void prov_buffer_undo_scope_end(prov_buffer_t *buf);

/* Replace [pos, pos+del_len) with `ins_len` bytes as a single undoable action
 * (used by field-mode `c` commit). Degrades to insert/delete when a side is 0. */
proven_err_t prov_buffer_replace(prov_buffer_t *buf, proven_size_t pos,
                                 proven_size_t del_len,
                                 const proven_u8 *bytes, proven_size_t ins_len);

/* ---- buffer-local bookmarks (M4.3) ----
 * 26 named slots (idx 0..25 = a..z), byte offsets auto-shifted by edits. set
 * clamps to the document length; get returns false for an unset slot. */
void prov_buffer_set_mark(prov_buffer_t *buf, int idx, proven_size_t pos);
bool prov_buffer_get_mark(const prov_buffer_t *buf, int idx, proven_size_t *pos);
void prov_buffer_clear_mark(prov_buffer_t *buf, int idx);

#endif /* PROV_BUFFER_H */
