#ifndef PROV_LINEEDIT_H
#define PROV_LINEEDIT_H

#include "proven/types.h"

/*
 * Reusable single-line text-input control (RFC-0015): a UTF-8 buffer with a
 * code-point cursor and an optional selection anchor, plus the editing ops a
 * panel field needs — insert / backspace / delete, left/right/home/end movement
 * (with Shift-style extend), and selection query. Pure and render-free, so it is
 * unit-testable; the platform clipboard (Ctrl+C/X/V) is wired by the caller via
 * prov_le_seltext / prov_le_replace_sel. Cursor and anchor always sit on a
 * code-point boundary.
 */

enum { PROV_LE_CAP = 1024 };

typedef struct {
    char          buf[PROV_LE_CAP];   /* NUL-terminated UTF-8 */
    proven_size_t len;                /* byte length (< PROV_LE_CAP) */
    proven_size_t cur;                /* cursor byte offset (cp boundary) */
    proven_size_t anchor;             /* selection anchor; == cur ⇒ no selection */
} prov_lineedit_t;

typedef enum {
    PROV_LE_LEFT, PROV_LE_RIGHT, PROV_LE_HOME, PROV_LE_END,
    PROV_LE_WORD_LEFT, PROV_LE_WORD_RIGHT      /* Ctrl+arrow: word-wise */
} prov_le_dir_t;

/* Up/down history shared by a field, owned by the caller so each context keeps
 * its own (paths, search terms, …). A fixed ring — pushes overwrite the oldest
 * with no copy/shift. PROV_LE_HIST_MAX entries; navigation tracks a draft so
 * Down past the newest restores what was being typed. */
enum { PROV_LE_HIST_MAX = 32 };
typedef struct {
    char buf[PROV_LE_HIST_MAX][PROV_LE_CAP];
    int  count;                       /* total pushed (entries = min(count, MAX)) */
    int  pos;                         /* -1 = draft; 0 = newest … entries-1 = oldest */
    char draft[PROV_LE_CAP];          /* the in-progress text saved while navigating */
} prov_lehist_t;

void prov_le_clear(prov_lineedit_t *le);
/* Replace the whole content (cursor goes to the end, no selection). */
void prov_le_set(prov_lineedit_t *le, const char *text);

/* True when there is a non-empty selection; fills the ordered byte range. */
bool prov_le_has_sel(const prov_lineedit_t *le);
void prov_le_sel_range(const prov_lineedit_t *le, proven_size_t *start, proven_size_t *end);

/* Insert `n` UTF-8 bytes at the cursor, replacing any selection first. */
void prov_le_insert(prov_lineedit_t *le, const char *bytes, proven_size_t n);
/* Delete the selection, or (if none) the code point before / at the cursor. */
void prov_le_backspace(prov_lineedit_t *le);
void prov_le_delete(prov_lineedit_t *le);
/* Delete the current selection (no-op if none). Used by cut. */
void prov_le_delete_sel(prov_lineedit_t *le);

/* Move the cursor; `extend` keeps the anchor (Shift-move), else collapses it.
 * WORD_LEFT/RIGHT jump by word; with extend they make a word-wise selection. */
void prov_le_move(prov_lineedit_t *le, prov_le_dir_t dir, bool extend);

/* ---- history (ring) ---- */
/* Append `text` as the newest entry (skips empty / a repeat of the newest). */
void prov_lehist_push(prov_lehist_t *h, const char *text);
/* Recall older (`up`) / newer history into `le`; Down past newest restores the
 * draft. No-op when the ring is empty. Resets le's selection. */
void prov_le_history(prov_lineedit_t *le, prov_lehist_t *h, bool up);

/* Stored-entry introspection for persistence: prov_lehist_len() is the entry
 * count; prov_lehist_get() returns entry `i` in oldest→newest order (NUL-
 * terminated, or NULL when out of range), so replaying the entries through
 * prov_lehist_push() reproduces the ring. */
int         prov_lehist_len(const prov_lehist_t *h);
const char *prov_lehist_get(const prov_lehist_t *h, int i);

/* ---- rendering ---- */
/* Produce the visible slice for a `width`-column field, scrolled horizontally to
 * keep the cursor in view. `out` (cap) gets the NUL-terminated UTF-8 slice;
 * curcol = the cursor's column within it [0, width]; sel_a..sel_b = the
 * selection's column span clipped to the window (equal ⇒ no visible selection). */
void prov_le_render(const prov_lineedit_t *le, int width, char *out, proven_size_t cap,
                    int *curcol, int *sel_a, int *sel_b);

#endif /* PROV_LINEEDIT_H */
