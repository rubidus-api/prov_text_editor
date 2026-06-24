#include "unicode.h"

#include "unicode_width_table.h"

/*
 * UTF-8 decoding rejects overlong encodings, surrogate code points, and values
 * above U+10FFFF, matching the Unicode "shortest form" rules. On any malformed
 * or truncated sequence the decoder reports one byte consumed so callers can
 * resynchronize without losing the rest of the stream.
 */

prov_decode_t prov_utf8_decode(const proven_u8 *bytes, proven_size_t len) {
    prov_decode_t r = { PROV_CP_REPLACEMENT, 0, false };
    if (len == 0) return r;

    proven_u8 c0 = bytes[0];
    if (c0 < 0x80) {                 /* fast path: ASCII */
        r.cp = c0;
        r.len = 1;
        r.valid = true;
        return r;
    }

    r.len = 1;                       /* default: resync by one byte on error */

    proven_u32 cp;
    int        need;                 /* number of continuation bytes */
    proven_u32 min;                  /* smallest value not overlong  */
    if ((c0 & 0xE0) == 0xC0)      { cp = c0 & 0x1Fu; need = 1; min = 0x80; }
    else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0Fu; need = 2; min = 0x800; }
    else if ((c0 & 0xF8) == 0xF0) { cp = c0 & 0x07u; need = 3; min = 0x10000; }
    else                          { return r; }   /* stray continuation / 0xF8+ */

    if ((proven_size_t)need >= len) return r;      /* truncated sequence */

    for (int i = 1; i <= need; i++) {
        proven_u8 cc = bytes[i];
        if ((cc & 0xC0) != 0x80) return r;         /* bad continuation byte */
        cp = (cp << 6) | (proven_u32)(cc & 0x3Fu);
    }

    if (cp < min) return r;                         /* overlong encoding */
    if (cp > 0x10FFFF) return r;                    /* out of range */
    if (cp >= 0xD800 && cp <= 0xDFFF) return r;     /* UTF-16 surrogate */

    r.cp = cp;
    r.len = (proven_size_t)(need + 1);
    r.valid = true;
    return r;
}

bool prov_utf8_validate(const proven_u8 *bytes, proven_size_t len) {
    proven_size_t i = 0;
    while (i < len) {
        prov_decode_t d = prov_utf8_decode(bytes + i, len - i);
        if (!d.valid) return false;
        i += d.len;
    }
    return true;
}

static bool in_ranges(proven_u32 cp, const proven_u32 ranges[][2],
                      proven_size_t n) {
    proven_size_t lo = 0, hi = n;
    while (lo < hi) {
        proven_size_t mid = lo + (hi - lo) / 2;
        if (cp < ranges[mid][0])      hi = mid;
        else if (cp > ranges[mid][1]) lo = mid + 1;
        else                          return true;
    }
    return false;
}

int prov_char_width(proven_u32 cp) {
    if (in_ranges(cp, prov_width_zero, PROV_WIDTH_ZERO_COUNT)) return 0;
    if (in_ranges(cp, prov_width_wide, PROV_WIDTH_WIDE_COUNT)) return 2;
    return 1;
}
