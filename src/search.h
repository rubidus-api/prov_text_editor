#ifndef PROV_SEARCH_H
#define PROV_SEARCH_H

/* Literal (byte-exact) search over a contiguous byte range. The buffer-aware
 * incremental search (M4.5) materializes the document once and calls this for
 * each pattern keystroke. A small built-in regex may layer on top later
 * (RFC-0008 M4.5b). */

#include "proven/types.h"

#define PROV_SEARCH_NPOS ((proven_size_t)-1)

/* Find `needle` (needlelen bytes) in hay[0..haylen).
 * - forward: first match whose start is >= `from`; else last match whose start
 *   is <= `from`.
 * - wrap: if nothing is found on that side, search the rest of the document.
 * Returns the match start, or PROV_SEARCH_NPOS when there is no match (also when
 * needlelen is 0 or needlelen > haylen). `*found` mirrors the result (may be 0). */
proven_size_t prov_search_bytes(const proven_u8 *hay, proven_size_t haylen,
                                const proven_u8 *needle, proven_size_t needlelen,
                                proven_size_t from, bool forward, bool wrap,
                                bool fold, bool *found);

/* True if `needle` matches hay at `pos` (ASCII case-fold when `fold`). Shared by
 * the search core and the render highlight so both agree. */
bool prov_match_at(const proven_u8 *hay, const proven_u8 *needle,
                   proven_size_t n, proven_size_t pos, bool fold);

#endif /* PROV_SEARCH_H */
