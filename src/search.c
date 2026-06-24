#include "search.h"

static proven_u8 foldc(proven_u8 c) { return (c >= 'A' && c <= 'Z') ? (proven_u8)(c + 32) : c; }

bool prov_match_at(const proven_u8 *hay, const proven_u8 *needle,
                   proven_size_t n, proven_size_t pos, bool fold) {
    for (proven_size_t i = 0; i < n; i++) {
        proven_u8 a = hay[pos + i], b = needle[i];
        if (fold) { a = foldc(a); b = foldc(b); }
        if (a != b) return false;
    }
    return true;
}

proven_size_t prov_search_bytes(const proven_u8 *hay, proven_size_t haylen,
                                const proven_u8 *needle, proven_size_t needlelen,
                                proven_size_t from, bool forward, bool wrap,
                                bool fold, bool *found) {
    bool dummy;
    if (!found) found = &dummy;
    *found = false;
    if (needlelen == 0 || needlelen > haylen) return PROV_SEARCH_NPOS;

    proven_size_t maxstart = haylen - needlelen;   /* last valid match start */

    if (forward) {
        for (proven_size_t pos = from; pos <= maxstart; pos++)
            if (prov_match_at(hay, needle, needlelen, pos, fold)) { *found = true; return pos; }
        if (wrap)
            for (proven_size_t pos = 0; pos < from && pos <= maxstart; pos++)
                if (prov_match_at(hay, needle, needlelen, pos, fold)) { *found = true; return pos; }
        return PROV_SEARCH_NPOS;
    }

    /* backward: nearest match whose start is <= from (clamped) */
    proven_size_t start = from < maxstart ? from : maxstart;
    for (proven_size_t pos = start;; pos--) {
        if (prov_match_at(hay, needle, needlelen, pos, fold)) { *found = true; return pos; }
        if (pos == 0) break;
    }
    if (wrap && start < maxstart)
        for (proven_size_t pos = maxstart; pos > start; pos--)
            if (prov_match_at(hay, needle, needlelen, pos, fold)) { *found = true; return pos; }
    return PROV_SEARCH_NPOS;
}
