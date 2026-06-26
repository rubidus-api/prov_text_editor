#ifndef PROV_MOTION_H
#define PROV_MOTION_H

#include "proven/types.h"

#include "buffer.h"
#include "command.h"   /* prov_textobj_t */

/*
 * Text motions and objects over a buffer (SPEC §10.4). These are pure
 * functions of (buffer, cursor byte offset); they compute target offsets and
 * ranges for the zx operator family (dw/cw/de/df/dt/dm/diw/ci(/...). The editor
 * applies the result via a selection range.
 *
 * Offsets are byte offsets into the document; ranges are half-open [start,end).
 */

typedef struct {
    proven_size_t start;
    proven_size_t end;
    bool          ok;     /* false when no object/match was found */
} prov_range_t;

/* Next word start (forward). Returns document end if none. */
proven_size_t prov_motion_word_next(const prov_buffer_t *b, proven_size_t at);
/* Previous word start (backward). Returns 0 if none. */
proven_size_t prov_motion_word_prev(const prov_buffer_t *b, proven_size_t at);
/* Exclusive end of the current/next word (one past its last code point). */
proven_size_t prov_motion_word_end(const prov_buffer_t *b, proven_size_t at);

/* Exclusive end offset for an `f`/`t` operator: the next `ch` on the current
 * line, including it (find) or stopping before it (till). Returns `at` (an
 * empty range, i.e. no-op) when `ch` is not found on the line after the cursor. */
proven_size_t prov_motion_find(const prov_buffer_t *b, proven_size_t at,
                               proven_u32 ch, bool till);

/* Cursor target for the standalone f/t/;/, motions: the byte the cursor should
 * move to when finding `ch` on the current line. `till` stops one code point
 * short of `ch`; `backward` searches to the left. Returns `at` unchanged when
 * `ch` is not found (so the caller can leave the cursor in place). */
proven_size_t prov_motion_findc(const prov_buffer_t *b, proven_size_t at,
                                proven_u32 ch, bool till, bool backward);

/* Byte offset of the bracket matching the one at/after the cursor on its line,
 * or `at` when there is no match. */
proven_size_t prov_motion_match(const prov_buffer_t *b, proven_size_t at);

/* Range of a text object around the cursor (iw/aw, i"/a", i(/a(, ...).
 * Unsupported objects (tag/paragraph) return {0,0,false}. */
prov_range_t prov_motion_textobj(const prov_buffer_t *b, proven_size_t at,
                                 prov_textobj_t obj, bool inner);

#endif /* PROV_MOTION_H */
