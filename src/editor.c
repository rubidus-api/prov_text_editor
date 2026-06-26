#include "editor.h"

#include "encoding.h"
#include "unicode.h"

#include "proven/error.h"

struct prov_editor {
    proven_allocator_t alloc;
    prov_buffer_t     *buf;
    proven_size_t      cursor;     /* byte offset into the document */
    proven_size_t      goal_col;   /* code-point column for vertical moves */
    proven_u8         *scratch;    /* reusable slice buffer */
    proven_size_t      scratch_cap;

    bool               sel_active; /* a selection anchor is set */
    proven_size_t      sel_anchor; /* the fixed end of the selection */
    bool               extending;  /* movement extends the selection */

    proven_u8         *reg;        /* internal cut/copy register */
    proven_size_t      reg_len, reg_cap;
    prov_reg_shape_t   reg_shape;  /* RFC-0006: CHAR (default) / LINE / BLOCK */
};

#define IS_CONT(b) (((b) & 0xC0) == 0x80)

/* ------------------------------------------------------------ helpers */

static proven_size_t doc_len(const prov_editor_t *ed) {
    return prov_buffer_byte_len(ed->buf);
}

/* Ensure the scratch slice buffer holds at least `n` bytes. */
static bool ensure_scratch(prov_editor_t *ed, proven_size_t n) {
    if (ed->scratch_cap >= n) return true;
    proven_size_t cap = ed->scratch_cap ? ed->scratch_cap * 2 : 64;
    while (cap < n) cap *= 2;
    proven_result_mem_mut_t r;
    if (ed->scratch == NULL)
        r = ed->alloc.alloc_fn(ed->alloc.ctx, cap, 16);
    else
        r = ed->alloc.realloc_fn(ed->alloc.ctx, ed->scratch, ed->scratch_cap, cap, 16);
    if (!PROVEN_IS_OK(r.err)) return false;
    ed->scratch = (proven_u8 *)r.value.ptr;
    ed->scratch_cap = cap;
    return true;
}

/* Copy [from, from+len) into scratch and return it (or NULL on alloc fail). */
static const proven_u8 *read_slice(prov_editor_t *ed, proven_size_t from,
                                   proven_size_t len) {
    if (len == 0) return ed->scratch;          /* may be NULL; caller checks len */
    if (!ensure_scratch(ed, len)) return NULL;
    prov_buffer_copy_range(ed->buf, from, len, ed->scratch, len);
    return ed->scratch;
}

/* Count Unicode code points in [from, to). */
static proven_size_t count_cp(prov_editor_t *ed, proven_size_t from,
                              proven_size_t to) {
    if (to <= from) return 0;
    proven_size_t len = to - from;
    const proven_u8 *s = read_slice(ed, from, len);
    if (!s) return 0;
    proven_size_t i = 0, count = 0;
    while (i < len) {
        prov_decode_t d = prov_utf8_decode(s + i, len - i);
        i += d.len ? d.len : 1;
        count++;
    }
    return count;
}

static proven_size_t line_of(const prov_editor_t *ed, proven_size_t at) {
    proven_size_t lc = prov_buffer_line_count(ed->buf);
    proven_size_t lo = 0, hi = lc, ans = 0;
    while (lo < hi) {
        proven_size_t mid = lo + (hi - lo) / 2;
        if (prov_buffer_line_start(ed->buf, mid) <= at) { ans = mid; lo = mid + 1; }
        else hi = mid;
    }
    return ans;
}

/* Byte offset of the end of line L (just before its '\n', or document end). */
static proven_size_t line_end(const prov_editor_t *ed, proven_size_t L) {
    proven_size_t lc = prov_buffer_line_count(ed->buf);
    if (L + 1 < lc) return prov_buffer_line_start(ed->buf, L + 1) - 1;
    return doc_len(ed);
}

/* Byte offset of code-point column `col` on line L, clamped to the line end. */
static proven_size_t pos_at_col(prov_editor_t *ed, proven_size_t L,
                                proven_size_t col) {
    proven_size_t start = prov_buffer_line_start(ed->buf, L);
    proven_size_t end = line_end(ed, L);
    if (end <= start || col == 0) return start;

    proven_size_t len = end - start;
    const proven_u8 *s = read_slice(ed, start, len);
    if (!s) return start;
    proven_size_t i = 0, c = 0;
    while (i < len && c < col) {
        prov_decode_t d = prov_utf8_decode(s + i, len - i);
        i += d.len ? d.len : 1;
        c++;
    }
    return start + i;
}

proven_size_t prov_editor_line_col_byte(prov_editor_t *ed, proven_size_t line, proven_size_t col) {
    return pos_at_col(ed, line, col);
}

proven_size_t prov_editor_vcol_at(prov_editor_t *ed, proven_size_t line,
                                  proven_size_t pos, proven_size_t tabstop) {
    if (tabstop == 0) tabstop = 1;
    proven_size_t start = prov_buffer_line_start(ed->buf, line);
    proven_size_t e = line_end(ed, line);
    if (pos > e) pos = e;
    if (pos <= start) return 0;
    proven_size_t len = pos - start;
    const proven_u8 *s = read_slice(ed, start, len);
    if (!s) return 0;
    proven_size_t v = 0, i = 0;
    while (i < len) {
        prov_decode_t d = prov_utf8_decode(s + i, len - i);
        proven_u32 cp = d.valid ? d.cp : s[i];
        if (cp == '\t') v = ((v / tabstop) + 1) * tabstop;
        else { int w = prov_char_width(cp); if (w > 0) v += (proven_size_t)w; }
        i += d.len ? d.len : 1;
    }
    return v;
}

proven_size_t prov_editor_byte_at_vcol(prov_editor_t *ed, proven_size_t line,
                                       proven_size_t vcol, proven_size_t tabstop,
                                       proven_size_t *reached) {
    if (tabstop == 0) tabstop = 1;
    proven_size_t start = prov_buffer_line_start(ed->buf, line);
    proven_size_t e = line_end(ed, line);
    proven_size_t len = e - start;
    const proven_u8 *s = len ? read_slice(ed, start, len) : NULL;
    proven_size_t v = 0, i = 0;
    while (i < len && v < vcol) {
        prov_decode_t d = prov_utf8_decode(s + i, len - i);
        proven_u32 cp = d.valid ? d.cp : s[i];
        proven_size_t adv;
        if (cp == '\t') adv = ((v / tabstop) + 1) * tabstop - v;
        else { int w = prov_char_width(cp); adv = (w > 0) ? (proven_size_t)w : 0; }
        if (adv && v + adv > vcol) break;     /* a wide char / tab straddling vcol: stop before it */
        v += adv;
        i += d.len ? d.len : 1;
    }
    if (reached) *reached = v;
    return start + i;
}

proven_size_t prov_editor_vwidth_at(prov_editor_t *ed, proven_size_t line,
                                    proven_size_t pos, proven_size_t tabstop) {
    if (tabstop == 0) tabstop = 1;
    proven_size_t e = line_end(ed, line);
    if (pos >= e) return 1;                  /* at/after line end: one column */
    const proven_u8 *s = read_slice(ed, pos, e - pos);
    if (!s) return 1;
    prov_decode_t d = prov_utf8_decode(s, e - pos);
    proven_u32 cp = d.valid ? d.cp : s[0];
    if (cp == '\t') {
        proven_size_t v = prov_editor_vcol_at(ed, line, pos, tabstop);
        return ((v / tabstop) + 1) * tabstop - v;
    }
    int w = prov_char_width(cp);
    return (w > 0) ? (proven_size_t)w : 1;
}

/* Byte offset of the code point preceding `at` (at must be > 0). */
static proven_size_t prev_boundary(prov_editor_t *ed, proven_size_t at) {
    proven_size_t start = at >= 4 ? at - 4 : 0;
    proven_size_t len = at - start;
    const proven_u8 *s = read_slice(ed, start, len);
    if (!s || len == 0) return at ? at - 1 : 0;
    proven_size_t i = len - 1;
    while (i > 0 && IS_CONT(s[i])) i--;
    return start + i;
}

static void set_goal(prov_editor_t *ed) {
    ed->goal_col = count_cp(ed, prov_buffer_line_start(ed->buf, line_of(ed, ed->cursor)),
                            ed->cursor);
}

static void clamp_cursor(prov_editor_t *ed) {
    proven_size_t n = doc_len(ed);
    if (ed->cursor > n) ed->cursor = n;
}

/* Called at the start of every movement: when extending, anchor the selection
 * at the current cursor on the first extended move; otherwise collapse it. */
static void before_move(prov_editor_t *ed) {
    if (ed->extending) {
        if (!ed->sel_active) { ed->sel_anchor = ed->cursor; ed->sel_active = true; }
    } else {
        ed->sel_active = false;
    }
}

/* Ensure the cut/copy register can hold `n` bytes. */
static bool ensure_reg(prov_editor_t *ed, proven_size_t n) {
    if (ed->reg_cap >= n) return true;
    proven_size_t cap = ed->reg_cap ? ed->reg_cap * 2 : 64;
    while (cap < n) cap *= 2;
    proven_result_mem_mut_t r;
    if (ed->reg == NULL) r = ed->alloc.alloc_fn(ed->alloc.ctx, cap, 16);
    else r = ed->alloc.realloc_fn(ed->alloc.ctx, ed->reg, ed->reg_cap, cap, 16);
    if (!PROVEN_IS_OK(r.err)) return false;
    ed->reg = (proven_u8 *)r.value.ptr;
    ed->reg_cap = cap;
    return true;
}

/* ------------------------------------------------------------ lifecycle */

static prov_result_editor_t wrap(proven_allocator_t alloc, prov_result_buffer_t rb) {
    prov_result_editor_t out = { NULL, rb.err };
    if (!PROVEN_IS_OK(rb.err)) return out;

    proven_result_mem_mut_t r =
        alloc.alloc_fn(alloc.ctx, sizeof(struct prov_editor), 16);
    if (!PROVEN_IS_OK(r.err)) {
        prov_buffer_destroy(rb.value);
        out.err = r.err;
        return out;
    }
    prov_editor_t *ed = (prov_editor_t *)r.value.ptr;
    ed->alloc = alloc;
    ed->buf = rb.value;
    ed->cursor = 0;
    ed->goal_col = 0;
    ed->scratch = NULL;
    ed->scratch_cap = 0;
    ed->sel_active = false;
    ed->sel_anchor = 0;
    ed->extending = false;
    ed->reg = NULL;
    ed->reg_len = 0;
    ed->reg_cap = 0;
    out.value = ed;
    return out;
}

prov_result_editor_t prov_editor_create(proven_allocator_t alloc) {
    if (!proven_alloc_is_valid(alloc)) {
        prov_result_editor_t out = { NULL, PROVEN_ERR_INVALID_ARG };
        return out;
    }
    return wrap(alloc, prov_buffer_create(alloc));
}

prov_result_editor_t prov_editor_open(proven_allocator_t alloc, const char *path,
                                      bool *sanitized, prov_fileinfo_t *out_info,
                                      const char *want_enc, const char *fallback_enc) {
    if (!proven_alloc_is_valid(alloc)) {
        prov_result_editor_t out = { NULL, PROVEN_ERR_INVALID_ARG };
        return out;
    }
    return wrap(alloc, prov_load_file(alloc, path, sanitized, out_info, want_enc, fallback_enc));
}

prov_result_editor_t prov_editor_open_binary(proven_allocator_t alloc, const char *path,
                                             prov_fileinfo_t *out_info, const char *interp_enc) {
    if (!proven_alloc_is_valid(alloc)) {
        prov_result_editor_t out = { NULL, PROVEN_ERR_INVALID_ARG };
        return out;
    }
    return wrap(alloc, prov_load_binary(alloc, path, out_info, interp_enc));
}

void prov_editor_destroy(prov_editor_t *ed) {
    if (!ed) return;
    prov_buffer_destroy(ed->buf);
    if (ed->scratch) ed->alloc.free_fn(ed->alloc.ctx, ed->scratch);
    if (ed->reg) ed->alloc.free_fn(ed->alloc.ctx, ed->reg);
    proven_allocator_t a = ed->alloc;
    a.free_fn(a.ctx, ed);
}

/* ------------------------------------------------------------ accessors */

const prov_buffer_t *prov_editor_buffer(const prov_editor_t *ed) { return ed->buf; }

proven_err_t prov_editor_compact(prov_editor_t *ed) {
    if (!ed) return PROVEN_ERR_INVALID_ARG;
    return prov_buffer_compact(ed->buf);   /* content/cursor unchanged */
}
void prov_editor_set_undo_limit(prov_editor_t *ed, proven_size_t limit) {
    prov_buffer_set_undo_limit(ed->buf, limit);
}
proven_size_t prov_editor_cursor_byte(const prov_editor_t *ed) { return ed->cursor; }

proven_u32 prov_editor_char_at_cursor(const prov_editor_t *ed) {
    if (ed->cursor >= doc_len(ed)) return 0;
    proven_u8 tmp[4];
    proven_size_t got = prov_buffer_copy_range(ed->buf, ed->cursor, 4, tmp, 4);
    prov_decode_t d = prov_utf8_decode(tmp, got);
    return d.cp;
}

proven_size_t prov_editor_cursor_line(const prov_editor_t *ed) {
    return line_of(ed, ed->cursor);
}

proven_size_t prov_editor_cursor_col(const prov_editor_t *ed) {
    prov_editor_t *m = (prov_editor_t *)ed;   /* uses scratch; logically const */
    proven_size_t start = prov_buffer_line_start(ed->buf, line_of(ed, ed->cursor));
    return count_cp(m, start, ed->cursor);
}

proven_size_t prov_editor_cursor_line_len(const prov_editor_t *ed) {
    prov_editor_t *m = (prov_editor_t *)ed;   /* uses scratch; logically const */
    proven_size_t L = line_of(ed, ed->cursor);
    proven_size_t start = prov_buffer_line_start(ed->buf, L);
    return count_cp(m, start, line_end(ed, L));   /* excludes the trailing newline */
}

/* ------------------------------------------------------------ editing */

proven_err_t prov_editor_insert(prov_editor_t *ed, const proven_u8 *bytes,
                                proven_size_t len) {
    if (prov_editor_has_selection(ed)) {
        proven_err_t e = prov_editor_delete_selection(ed);
        if (!PROVEN_IS_OK(e)) return e;
    }
    proven_err_t err = prov_buffer_insert(ed->buf, ed->cursor, bytes, len);
    if (PROVEN_IS_OK(err)) {
        ed->cursor += len;
        set_goal(ed);
    }
    return err;
}

proven_err_t prov_editor_backspace(prov_editor_t *ed) {
    if (prov_editor_has_selection(ed)) return prov_editor_delete_selection(ed);
    if (ed->cursor == 0) return PROVEN_OK;
    proven_size_t prev = prev_boundary(ed, ed->cursor);
    proven_err_t err = prov_buffer_delete(ed->buf, prev, ed->cursor - prev);
    if (PROVEN_IS_OK(err)) {
        ed->cursor = prev;
        set_goal(ed);
    }
    return err;
}

proven_err_t prov_editor_delete(prov_editor_t *ed) {
    if (prov_editor_has_selection(ed)) return prov_editor_delete_selection(ed);
    proven_size_t n = doc_len(ed);
    if (ed->cursor >= n) return PROVEN_OK;
    proven_size_t avail = n - ed->cursor;
    if (avail > 4) avail = 4;
    const proven_u8 *s = read_slice(ed, ed->cursor, avail);
    prov_decode_t d = prov_utf8_decode(s, avail);
    proven_size_t dl = d.len ? d.len : 1;
    proven_err_t err = prov_buffer_delete(ed->buf, ed->cursor, dl);
    if (PROVEN_IS_OK(err)) set_goal(ed);
    return err;
}

/* ------------------------------------------------------------ selection */

void prov_editor_set_extending(prov_editor_t *ed, bool on) { ed->extending = on; }

prov_selection_t prov_editor_selection(const prov_editor_t *ed) {
    prov_selection_t s = { false, ed->cursor, ed->cursor };
    if (ed->sel_active && ed->sel_anchor != ed->cursor) {
        s.active = true;
        s.start = ed->sel_anchor < ed->cursor ? ed->sel_anchor : ed->cursor;
        s.end   = ed->sel_anchor < ed->cursor ? ed->cursor : ed->sel_anchor;
    }
    return s;
}

bool prov_editor_has_selection(const prov_editor_t *ed) {
    return ed->sel_active && ed->sel_anchor != ed->cursor;
}

void prov_editor_clear_selection(prov_editor_t *ed) { ed->sel_active = false; }

void prov_editor_select_all(prov_editor_t *ed) {
    proven_size_t n = doc_len(ed);
    if (n == 0) { ed->sel_active = false; return; }
    ed->sel_anchor = 0;
    ed->cursor = n;
    ed->sel_active = true;
    set_goal(ed);
}

proven_err_t prov_editor_delete_selection(prov_editor_t *ed) {
    if (!prov_editor_has_selection(ed)) return PROVEN_OK;
    prov_selection_t s = prov_editor_selection(ed);
    proven_err_t err = prov_buffer_delete(ed->buf, s.start, s.end - s.start);
    if (PROVEN_IS_OK(err)) {
        ed->cursor = s.start;
        ed->sel_active = false;
        set_goal(ed);
    }
    return err;
}

proven_err_t prov_editor_copy_selection(prov_editor_t *ed) {
    if (!prov_editor_has_selection(ed)) return PROVEN_OK;
    prov_selection_t s = prov_editor_selection(ed);
    proven_size_t n = s.end - s.start;
    if (!ensure_reg(ed, n)) return PROVEN_ERR_NOMEM;
    prov_buffer_copy_range(ed->buf, s.start, n, ed->reg, n);
    ed->reg_len = n;
    ed->reg_shape = PROV_REG_CHAR;          /* charwise unless a linewise op re-tags it */
    return PROVEN_OK;
}

void prov_editor_reg_ensure_trailing_newline(prov_editor_t *ed) {
    ed->reg_shape = PROV_REG_LINE;          /* this is the linewise (yy/dd) marker */
    if (ed->reg_len > 0 && ed->reg[ed->reg_len - 1] == '\n') return;
    if (!ensure_reg(ed, ed->reg_len + 1)) return;
    ed->reg[ed->reg_len++] = '\n';
}

prov_reg_shape_t prov_editor_reg_shape(const prov_editor_t *ed) { return ed->reg_shape; }

/* Read/write the unnamed register's bytes, so a session can snapshot it into a
 * named register and restore it before a paste (M4.2). */
proven_size_t prov_editor_reg_len(const prov_editor_t *ed) { return ed->reg_len; }

proven_size_t prov_editor_reg_copy(const prov_editor_t *ed, proven_u8 *dst, proven_size_t cap) {
    proven_size_t n = ed->reg_len < cap ? ed->reg_len : cap;
    if (n && dst) proven_mem_copy(dst, cap, (proven_mem_view_t){ ed->reg, n });
    return ed->reg_len;
}

proven_err_t prov_editor_reg_set(prov_editor_t *ed, const proven_u8 *bytes,
                                 proven_size_t len, prov_reg_shape_t shape) {
    if (len && !ensure_reg(ed, len)) return PROVEN_ERR_NOMEM;
    if (len) proven_mem_copy(ed->reg, ed->reg_cap, (proven_mem_view_t){ bytes, len });
    ed->reg_len = len;
    ed->reg_shape = shape;
    return PROVEN_OK;
}

proven_err_t prov_editor_cut_selection(prov_editor_t *ed) {
    proven_err_t err = prov_editor_copy_selection(ed);
    if (!PROVEN_IS_OK(err)) return err;
    return prov_editor_delete_selection(ed);
}

proven_err_t prov_editor_paste(prov_editor_t *ed) {
    if (prov_editor_has_selection(ed)) {
        proven_err_t e = prov_editor_delete_selection(ed);
        if (!PROVEN_IS_OK(e)) return e;
    }
    if (ed->reg_len == 0) return PROVEN_OK;
    proven_err_t err = prov_buffer_insert(ed->buf, ed->cursor, ed->reg, ed->reg_len);
    if (PROVEN_IS_OK(err)) {
        ed->cursor += ed->reg_len;
        set_goal(ed);
    }
    return err;
}

proven_err_t prov_editor_paste_lines(prov_editor_t *ed, bool below, proven_u32 count) {
    if (ed->reg_len == 0) return PROVEN_OK;
    if (count == 0) count = 1;

    proven_size_t rlen = ed->reg_len;
    proven_size_t tlen = rlen * (proven_size_t)count;
    if (tlen / (proven_size_t)count != rlen) return PROVEN_ERR_OVERFLOW;

    proven_result_mem_mut_t rm = ed->alloc.alloc_fn(ed->alloc.ctx, tlen, 16);
    if (!PROVEN_IS_OK(rm.err)) return rm.err;
    proven_u8 *T = (proven_u8 *)rm.value.ptr;                  /* T = reg x count */
    for (proven_size_t i = 0; i < (proven_size_t)count; i++)
        for (proven_size_t j = 0; j < rlen; j++) T[i * rlen + j] = ed->reg[j];

    proven_err_t err;
    if (ed->reg_shape != PROV_REG_LINE) {                      /* char/block: at the cursor */
        if (prov_editor_has_selection(ed)) {
            err = prov_editor_delete_selection(ed);
            if (!PROVEN_IS_OK(err)) goto done;
        }
        err = prov_buffer_insert(ed->buf, ed->cursor, T, tlen);
        if (PROVEN_IS_OK(err)) ed->cursor += tlen;
        goto done;
    }

    /* LINE shape: whole line(s) above / below, column-independent */
    if (prov_editor_has_selection(ed)) prov_editor_clear_selection(ed);
    const prov_buffer_t *b = ed->buf;
    proven_size_t lc = prov_buffer_line_count(b);
    proven_size_t cur_line = prov_editor_cursor_line(ed);
    proven_size_t total = prov_buffer_byte_len(b);

    if (!below) {                                             /* P: above current line */
        proven_size_t pos = prov_buffer_line_start(b, cur_line);
        err = prov_buffer_insert(ed->buf, pos, T, tlen);
        if (PROVEN_IS_OK(err)) ed->cursor = pos;
    } else if (cur_line + 1 < lc) {                           /* p: before the next line */
        proven_size_t pos = prov_buffer_line_start(b, cur_line + 1);
        err = prov_buffer_insert(ed->buf, pos, T, tlen);
        if (PROVEN_IS_OK(err)) ed->cursor = pos;
    } else {                                                  /* p: after the last line */
        proven_u8 lastb = '\n';
        if (total > 0) prov_buffer_copy_range(b, total - 1, 1, &lastb, 1);
        if (total > 0 && lastb != '\n') {                     /* terminate it: T := "\n" + T sans final \n */
            for (proven_size_t i = tlen - 1; i > 0; i--) T[i] = T[i - 1];
            T[0] = '\n';
            err = prov_buffer_insert(ed->buf, total, T, tlen);
            if (PROVEN_IS_OK(err)) ed->cursor = total + 1;
        } else {
            err = prov_buffer_insert(ed->buf, total, T, tlen);
            if (PROVEN_IS_OK(err)) ed->cursor = total;
        }
    }
done:
    clamp_cursor(ed);
    set_goal(ed);
    ed->alloc.free_fn(ed->alloc.ctx, T);
    return err;
}

/* ------------------------------------------------------------ movement */

void prov_editor_move_left(prov_editor_t *ed) {
    before_move(ed);
    if (ed->cursor == 0) return;
    ed->cursor = prev_boundary(ed, ed->cursor);
    set_goal(ed);
}

void prov_editor_move_right(prov_editor_t *ed) {
    before_move(ed);
    proven_size_t n = doc_len(ed);
    if (ed->cursor >= n) return;
    proven_size_t avail = n - ed->cursor;
    if (avail > 4) avail = 4;
    const proven_u8 *s = read_slice(ed, ed->cursor, avail);
    prov_decode_t d = prov_utf8_decode(s, avail);
    ed->cursor += d.len ? d.len : 1;
    set_goal(ed);
}

void prov_editor_move_up(prov_editor_t *ed) {
    before_move(ed);
    proven_size_t line = line_of(ed, ed->cursor);
    if (line == 0) { ed->cursor = 0; return; }
    ed->cursor = pos_at_col(ed, line - 1, ed->goal_col);   /* keep goal */
}

void prov_editor_move_down(prov_editor_t *ed) {
    before_move(ed);
    proven_size_t line = line_of(ed, ed->cursor);
    proven_size_t lc = prov_buffer_line_count(ed->buf);
    if (line + 1 < lc) ed->cursor = pos_at_col(ed, line + 1, ed->goal_col);
    else ed->cursor = doc_len(ed);                          /* keep goal */
}

void prov_editor_move_home(prov_editor_t *ed) {
    before_move(ed);
    ed->cursor = prov_buffer_line_start(ed->buf, line_of(ed, ed->cursor));
    set_goal(ed);
}

void prov_editor_move_end(prov_editor_t *ed) {
    before_move(ed);
    ed->cursor = line_end(ed, line_of(ed, ed->cursor));
    set_goal(ed);
}

void prov_editor_move_doc_start(prov_editor_t *ed) {
    before_move(ed);
    ed->cursor = 0;
    set_goal(ed);
}

void prov_editor_move_doc_end(prov_editor_t *ed) {
    before_move(ed);
    ed->cursor = doc_len(ed);
    set_goal(ed);
}

void prov_editor_move_to(prov_editor_t *ed, proven_size_t byte) {
    before_move(ed);                         /* extends the selection when extending */
    proven_size_t n = doc_len(ed);
    ed->cursor = byte > n ? n : byte;
    set_goal(ed);
}

void prov_editor_select_range(prov_editor_t *ed, proven_size_t start,
                              proven_size_t end) {
    proven_size_t n = doc_len(ed);
    if (start > n) start = n;
    if (end > n) end = n;
    ed->sel_anchor = start;
    ed->cursor = end;
    ed->sel_active = (start != end);
    set_goal(ed);
}

/* ------------------------------------------------------------ undo / redo */

bool prov_editor_undo(prov_editor_t *ed) {
    prov_edit_info_t info = prov_buffer_undo(ed->buf);
    if (!info.applied) return false;
    ed->cursor = info.at;          /* edit site, for both insert and delete */
    clamp_cursor(ed);
    set_goal(ed);
    return true;
}

bool prov_editor_redo(prov_editor_t *ed) {
    prov_edit_info_t info = prov_buffer_redo(ed->buf);
    if (!info.applied) return false;
    ed->cursor = info.was_insert ? info.at + info.len : info.at;
    clamp_cursor(ed);
    set_goal(ed);
    return true;
}

/* Pin bookmark `idx` (0..25) at the current cursor (M4.3). */
void prov_editor_set_mark(prov_editor_t *ed, int idx) {
    prov_buffer_set_mark(ed->buf, idx, ed->cursor);
}
void prov_editor_clear_mark(prov_editor_t *ed, int idx) {
    prov_buffer_clear_mark(ed->buf, idx);
}

/* ---- field-mode scoped undo + atomic replace (RFC-0007) ---- */
void prov_editor_undo_scope_begin(prov_editor_t *ed) { prov_buffer_undo_scope_begin(ed->buf); }
void prov_editor_undo_scope_end(prov_editor_t *ed)   { prov_buffer_undo_scope_end(ed->buf); }

proven_err_t prov_editor_replace_range(prov_editor_t *ed, proven_size_t pos,
                                       proven_size_t del_len,
                                       const proven_u8 *bytes, proven_size_t ins_len) {
    proven_err_t err = prov_buffer_replace(ed->buf, pos, del_len, bytes, ins_len);
    if (PROVEN_IS_OK(err)) {
        ed->cursor = pos + ins_len;
        clamp_cursor(ed);
        set_goal(ed);
    }
    return err;
}
