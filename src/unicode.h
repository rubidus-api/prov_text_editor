#ifndef PROV_UNICODE_H
#define PROV_UNICODE_H

#include "proven/types.h"

/*
 * UTF-8 decoding and display-width queries (SPEC.md §5).
 *
 * Milestone 1 operates on Unicode code points (not extended grapheme
 * clusters, per §5.1). Width follows UAX #11 East Asian Width: combining /
 * zero-width code points occupy 0 cells, Wide/Fullwidth occupy 2, the rest 1.
 */

/* The Unicode replacement character, returned for malformed input. */
#define PROV_CP_REPLACEMENT ((proven_u32)0xFFFDu)

typedef struct {
    proven_u32    cp;    /* decoded code point (PROV_CP_REPLACEMENT on error) */
    proven_size_t len;   /* bytes consumed, always >= 1 (1 to resync on error) */
    bool          valid; /* true iff a well-formed sequence was decoded        */
} prov_decode_t;

/* Decode one UTF-8 code point from the front of [bytes, bytes+len).
 * On a malformed or truncated sequence, returns PROV_CP_REPLACEMENT with
 * len == 1 (advance one byte to resync) and valid == false.
 * If len == 0, returns {PROV_CP_REPLACEMENT, 0, false}. */
prov_decode_t prov_utf8_decode(const proven_u8 *bytes, proven_size_t len);

/* True iff the whole byte range is well-formed UTF-8. An empty range is valid. */
bool prov_utf8_validate(const proven_u8 *bytes, proven_size_t len);

/* Terminal cells occupied by a code point: 0 (zero-width / combining),
 * 2 (East Asian Wide or Fullwidth), or 1 otherwise. Control characters and
 * unknown code points use the conservative default of 1 (SPEC §12.3). */
int prov_char_width(proven_u32 cp);

#endif /* PROV_UNICODE_H */
