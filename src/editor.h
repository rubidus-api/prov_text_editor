#ifndef PROV_EDITOR_H
#define PROV_EDITOR_H

#include "proven/types.h"
#include "proven/allocator.h"

#include "buffer.h"
#include "encoding.h"   /* prov_fileinfo_t */

/*
 * Editor state on top of a document buffer (SPEC.md §20). This layer is
 * backend-agnostic: it owns the cursor and edit/movement operations but knows
 * nothing about terminals or rendering. The display and input layers drive it.
 *
 * The cursor is a byte offset into the document; logical line/column are
 * derived. Vertical movement preserves a "goal column" (in code points) so
 * moving across short lines keeps the intended horizontal position.
 *
 * Cursor movement and columns operate on Unicode code points, not grapheme
 * clusters (SPEC §5.1).
 */

typedef struct prov_editor prov_editor_t;

typedef struct {
    prov_editor_t *value;
    proven_err_t   err;
} prov_result_editor_t;

/* Create an editor over a fresh empty buffer. */
prov_result_editor_t prov_editor_create(proven_allocator_t alloc);

/* Create an editor by loading `path` (see prov_load_file): BOM stripped, CRLF/CR
 * normalized to LF, and `out_info` (when non-NULL) filled with the original
 * encoding / EOL / BOM for a faithful save. When `sanitized` is non-NULL the load
 * is lossy (invalid bytes dropped, *sanitized set if any were); NULL = strict. */
prov_result_editor_t prov_editor_open(proven_allocator_t alloc, const char *path,
                                      bool *sanitized, prov_fileinfo_t *out_info,
                                      const char *want_enc, const char *fallback_enc);

/* Open `path` as raw bytes (RFC-0019 binary mode, see prov_load_binary): verbatim
 * load, no decoding/normalization; `out_info.binary = true` with `interp_enc` as
 * the interpretation charset ("" = UTF-8). Save reproduces the bytes exactly. */
prov_result_editor_t prov_editor_open_binary(proven_allocator_t alloc, const char *path,
                                             prov_fileinfo_t *out_info, const char *interp_enc);

void prov_editor_destroy(prov_editor_t *ed);

/* ---- accessors ---- */

const prov_buffer_t *prov_editor_buffer(const prov_editor_t *ed);
/* Reclaim the buffer's orphaned add-store memory (see prov_buffer_compact).
 * Content and cursor are unchanged. Intended to run after a successful save. */
proven_err_t prov_editor_compact(prov_editor_t *ed);
/* Bound this editor's buffer undo history (0 = unbounded). See buffer.h. */
void prov_editor_set_undo_limit(prov_editor_t *ed, proven_size_t limit);
proven_size_t prov_editor_cursor_byte(const prov_editor_t *ed);
/* Code point at the cursor, or 0 at end of document. */
proven_u32 prov_editor_char_at_cursor(const prov_editor_t *ed);
proven_size_t prov_editor_cursor_line(const prov_editor_t *ed);  /* 0-based */
proven_size_t prov_editor_cursor_col(const prov_editor_t *ed);   /* 0-based code points */
proven_size_t prov_editor_cursor_line_len(const prov_editor_t *ed); /* code points in the cursor's line (excl. newline) */

/* Byte offset of code-point column `col` on line `line` (clamped to line end);
 * for rectangular (visual-block) column ranges. */
proven_size_t prov_editor_line_col_byte(prov_editor_t *ed, proven_size_t line, proven_size_t col);

/* Visual column (tabs expanded to `tabstop`, wide chars counted as 2) at which
 * the char at byte `pos` begins, within its own `line`. For visual-block ops. */
proven_size_t prov_editor_vcol_at(prov_editor_t *ed, proven_size_t line,
                                  proven_size_t pos, proven_size_t tabstop);

/* Byte offset of the first char on `line` whose cell range reaches visual column
 * `vcol` (rounded to a char boundary; a wide char / tab straddling `vcol` stops
 * before it). When the line is shorter than `vcol`, returns the line-end byte and
 * sets *reached (if non-NULL) to the line's visual width. */
proven_size_t prov_editor_byte_at_vcol(prov_editor_t *ed, proven_size_t line,
                                       proven_size_t vcol, proven_size_t tabstop,
                                       proven_size_t *reached);

/* Visual width (cells) of the char at byte `pos` on `line`: 2 for a wide glyph,
 * the distance to the next tab stop for a tab, else 1 (also 1 at/after line end).
 * Lets a visual-block edge span the full glyph under a corner. */
proven_size_t prov_editor_vwidth_at(prov_editor_t *ed, proven_size_t line,
                                    proven_size_t pos, proven_size_t tabstop);

/* ---- editing at the cursor ---- */

/* Insert bytes at the cursor and advance the cursor past them. */
proven_err_t prov_editor_insert(prov_editor_t *ed, const proven_u8 *bytes,
                                proven_size_t len);

/* Delete the code point before the cursor (Backspace). No-op at start. */
proven_err_t prov_editor_backspace(prov_editor_t *ed);

/* Delete the code point at the cursor (Delete). No-op at end. */
proven_err_t prov_editor_delete(prov_editor_t *ed);

/* ---- cursor movement ---- */

void prov_editor_move_left(prov_editor_t *ed);   /* by one code point */
void prov_editor_move_right(prov_editor_t *ed);
void prov_editor_move_up(prov_editor_t *ed);     /* by line, keeping goal column */
void prov_editor_move_down(prov_editor_t *ed);
void prov_editor_move_home(prov_editor_t *ed);   /* to start of line */
void prov_editor_move_end(prov_editor_t *ed);    /* to end of line   */
void prov_editor_move_doc_start(prov_editor_t *ed);  /* to byte 0 */
void prov_editor_move_doc_end(prov_editor_t *ed);    /* to end of document */
void prov_editor_move_to(prov_editor_t *ed, proven_size_t byte);  /* to an absolute byte */

/* Set the selection to an explicit byte range [start, end) (clamped). Used to
 * realize linewise operators (e.g. select a line range then cut). */
void prov_editor_select_range(prov_editor_t *ed, proven_size_t start,
                              proven_size_t end);

/* ---- selection (Ed-mode block selection, SPEC §11) ---- */

typedef struct {
    bool          active;   /* true when a non-empty selection exists */
    proven_size_t start;    /* byte offset of selection start (inclusive) */
    proven_size_t end;      /* byte offset of selection end (exclusive) */
} prov_selection_t;

/* Current selection as an ordered byte range. When inactive, start == end ==
 * the cursor. */
prov_selection_t prov_editor_selection(const prov_editor_t *ed);
bool prov_editor_has_selection(const prov_editor_t *ed);
void prov_editor_clear_selection(prov_editor_t *ed);
void prov_editor_select_all(prov_editor_t *ed);

/* When extending is on, movement keeps the selection anchor (Shift+move);
 * when off, the next movement collapses any selection. */
void prov_editor_set_extending(prov_editor_t *ed, bool on);

/* Delete / copy / cut the current selection (into the internal register), and
 * paste the register at the cursor. Edits that run with an active selection
 * (insert/backspace/delete/paste) replace the selection first. No-ops when
 * there is no selection (except paste, which still inserts). */
proven_err_t prov_editor_delete_selection(prov_editor_t *ed);
proven_err_t prov_editor_copy_selection(prov_editor_t *ed);

/* Register shape (RFC-0006): how the yanked bytes should paste. copy/cut set
 * CHAR; reg_ensure_trailing_newline (linewise yy/dd) sets LINE. BLOCK is future
 * (visual-block). */
typedef enum { PROV_REG_CHAR = 0, PROV_REG_LINE, PROV_REG_BLOCK } prov_reg_shape_t;
prov_reg_shape_t prov_editor_reg_shape(const prov_editor_t *ed);

/* Read/write the unnamed register (M4.2 named-register snapshot/restore). */
proven_size_t prov_editor_reg_len(const prov_editor_t *ed);
proven_size_t prov_editor_reg_copy(const prov_editor_t *ed, proven_u8 *dst, proven_size_t cap);
proven_err_t prov_editor_reg_set(prov_editor_t *ed, const proven_u8 *bytes,
                                 proven_size_t len, prov_reg_shape_t shape);

/* Append a newline to the yank register unless it already ends with one — used
 * for linewise yank/delete so a last line without a trailing newline still
 * pastes as a whole line. Also tags the register LINE. */
void prov_editor_reg_ensure_trailing_newline(prov_editor_t *ed);
proven_err_t prov_editor_cut_selection(prov_editor_t *ed);
proven_err_t prov_editor_paste(prov_editor_t *ed);

/* Paste the register as whole line(s): `below` true = below the current line
 * (`p`), false = above (`P`); `count` copies. For a CHAR/BLOCK register this is
 * the same as prov_editor_paste (insert at cursor). */
proven_err_t prov_editor_paste_lines(prov_editor_t *ed, bool below, proven_u32 count);

/* ---- undo / redo (restores the cursor to the edit site) ---- */

bool prov_editor_undo(prov_editor_t *ed);
bool prov_editor_redo(prov_editor_t *ed);

/* Pin bookmark slot `idx` (0..25 = a..z) at the cursor. Jump via
 * prov_buffer_get_mark(prov_editor_buffer(ed), idx, &pos) + prov_editor_move_to. */
void prov_editor_set_mark(prov_editor_t *ed, int idx);
/* Clear bookmark slot `idx` (0..25). */
void prov_editor_clear_mark(prov_editor_t *ed, int idx);

/* field mode (RFC-0007): an isolated, temporary undo scope for a fragment-input
 * session, plus a single-action replace for the commit. */
void prov_editor_undo_scope_begin(prov_editor_t *ed);
void prov_editor_undo_scope_end(prov_editor_t *ed);
proven_err_t prov_editor_replace_range(prov_editor_t *ed, proven_size_t pos,
                                       proven_size_t del_len,
                                       const proven_u8 *bytes, proven_size_t ins_len);

#endif /* PROV_EDITOR_H */
