#include "encoding.h"

#include "unicode.h"
#include "platform_charset.h"   /* iconv/Win32 backends for non-UTF/non-1252 encodings */

#include <string.h>

#include "proven/error.h"
#include "proven/fs.h"
#include "proven/u8str.h"

/* Copy `bytes[0..n)` dropping any byte that is not part of a valid UTF-8 sequence.
 * Returns a fresh allocation (caller frees) with the kept length in *outn and
 * *dropped set when anything was removed; NULL on allocation failure. */
static proven_u8 *sanitize_utf8(proven_allocator_t a, const proven_u8 *bytes,
                                proven_size_t n, proven_size_t *outn, bool *dropped) {
    proven_result_mem_mut_t rm = a.alloc_fn(a.ctx, n ? n : 1, 16);
    if (!PROVEN_IS_OK(rm.err)) return NULL;
    proven_u8 *out = (proven_u8 *)rm.value.ptr;
    proven_size_t i = 0, o = 0;
    bool drop = false;
    while (i < n) {
        prov_decode_t d = prov_utf8_decode(bytes + i, n - i);
        if (d.valid && d.len > 0) {
            for (proven_size_t k = 0; k < d.len; k++) out[o++] = bytes[i + k];
            i += d.len;
        } else { drop = true; i++; }          /* skip one offending byte */
    }
    *outn = o;
    if (dropped) *dropped = drop;
    return out;
}

/* Code pages. UTF series is algorithmic; Windows-1252 is a 128-entry table used
 * as the fallback when a no-BOM file is not valid UTF-8. */
enum { CP_UTF8 = 65001, CP_UTF16LE = 1200, CP_UTF16BE = 1201,
       CP_UTF32LE = 12000, CP_UTF32BE = 12001, CP_WIN1252 = 1252 };

static const char *enc_label(int cp) {
    switch (cp) {
        case CP_UTF16LE: return "UTF-16LE";
        case CP_UTF16BE: return "UTF-16BE";
        case CP_UTF32LE: return "UTF-32LE";
        case CP_UTF32BE: return "UTF-32BE";
        case CP_WIN1252: return "Windows-1252";
        default:         return "UTF-8";
    }
}

static int u8_enc(proven_u32 cp, proven_u8 *o);   /* fwd: defined below */

/* Windows-1252 high half (bytes 0x80..0xFF -> Unicode). 0x80..0x9F carry the CP1252
 * punctuation; the 5 unused slots (81,8D,8F,90,9D) pass through as U+00xx; 0xA0..0xFF
 * are Latin-1 identity. */
static const proven_u16 win1252_hi[128] = {
    0x20AC,0x0081,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,0x02C6,0x2030,0x0160,0x2039,0x0152,0x008D,0x017D,0x008F,
    0x0090,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x02DC,0x2122,0x0161,0x203A,0x0153,0x009D,0x017E,0x0178,
    0x00A0,0x00A1,0x00A2,0x00A3,0x00A4,0x00A5,0x00A6,0x00A7,0x00A8,0x00A9,0x00AA,0x00AB,0x00AC,0x00AD,0x00AE,0x00AF,
    0x00B0,0x00B1,0x00B2,0x00B3,0x00B4,0x00B5,0x00B6,0x00B7,0x00B8,0x00B9,0x00BA,0x00BB,0x00BC,0x00BD,0x00BE,0x00BF,
    0x00C0,0x00C1,0x00C2,0x00C3,0x00C4,0x00C5,0x00C6,0x00C7,0x00C8,0x00C9,0x00CA,0x00CB,0x00CC,0x00CD,0x00CE,0x00CF,
    0x00D0,0x00D1,0x00D2,0x00D3,0x00D4,0x00D5,0x00D6,0x00D7,0x00D8,0x00D9,0x00DA,0x00DB,0x00DC,0x00DD,0x00DE,0x00DF,
    0x00E0,0x00E1,0x00E2,0x00E3,0x00E4,0x00E5,0x00E6,0x00E7,0x00E8,0x00E9,0x00EA,0x00EB,0x00EC,0x00ED,0x00EE,0x00EF,
    0x00F0,0x00F1,0x00F2,0x00F3,0x00F4,0x00F5,0x00F6,0x00F7,0x00F8,0x00F9,0x00FA,0x00FB,0x00FC,0x00FD,0x00FE,0x00FF,
};

/* Decode a single-byte encoding (ASCII low half + a 128-entry high table) to UTF-8. */
static proven_u8 *decode_singlebyte(proven_allocator_t a, const proven_u8 *b, proven_size_t n,
                                    const proven_u16 *hi, proven_size_t *outn) {
    proven_result_mem_mut_t rm = a.alloc_fn(a.ctx, n * 3 + 1, 16);   /* <= 3 UTF-8 bytes per byte */
    if (!PROVEN_IS_OK(rm.err)) return NULL;
    proven_u8 *out = (proven_u8 *)rm.value.ptr;
    proven_size_t o = 0;
    for (proven_size_t i = 0; i < n; i++)
        o += (proven_size_t)u8_enc(b[i] < 0x80 ? b[i] : hi[b[i] - 0x80], out + o);
    *outn = o; return out;
}

/* Encode one code point as UTF-8 into `o` (>= 4 bytes); returns the byte count. */
static int u8_enc(proven_u32 cp, proven_u8 *o) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = PROV_CP_REPLACEMENT;
    if (cp < 0x80)    { o[0] = (proven_u8)cp; return 1; }
    if (cp < 0x800)   { o[0] = 0xC0 | (cp >> 6); o[1] = 0x80 | (cp & 0x3F); return 2; }
    if (cp < 0x10000) { o[0] = 0xE0 | (cp >> 12); o[1] = 0x80 | ((cp >> 6) & 0x3F); o[2] = 0x80 | (cp & 0x3F); return 3; }
    o[0] = 0xF0 | (cp >> 18); o[1] = 0x80 | ((cp >> 12) & 0x3F);
    o[2] = 0x80 | ((cp >> 6) & 0x3F); o[3] = 0x80 | (cp & 0x3F); return 4;
}

/* Detect the byte-order-mark encoding + its BOM length. Defaults to UTF-8. */
static int enc_from_bom(const proven_u8 *b, proven_size_t n, int *bom_len) {
    *bom_len = 0;
    if (n >= 4 && b[0] == 0xFF && b[1] == 0xFE && b[2] == 0 && b[3] == 0) { *bom_len = 4; return CP_UTF32LE; }
    if (n >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 0xFE && b[3] == 0xFF) { *bom_len = 4; return CP_UTF32BE; }
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) { *bom_len = 2; return CP_UTF16LE; }
    if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) { *bom_len = 2; return CP_UTF16BE; }
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) { *bom_len = 3; return CP_UTF8; }
    return CP_UTF8;
}

/* Decode `b[0..n)` (BOM already skipped by the caller) from `cp` to UTF-8. For
 * UTF-8 this is a copy. Returns a fresh allocation (caller frees) or NULL. */
static proven_u8 *to_utf8(proven_allocator_t a, const proven_u8 *b, proven_size_t n,
                          int cp, proven_size_t *outn) {
    if (cp == CP_UTF8) {
        proven_result_mem_mut_t rm = a.alloc_fn(a.ctx, n ? n : 1, 16);
        if (!PROVEN_IS_OK(rm.err)) return NULL;
        for (proven_size_t i = 0; i < n; i++) ((proven_u8 *)rm.value.ptr)[i] = b[i];
        *outn = n; return (proven_u8 *)rm.value.ptr;
    }
    bool le = (cp == CP_UTF16LE || cp == CP_UTF32LE);
    int unit = (cp == CP_UTF32LE || cp == CP_UTF32BE) ? 4 : 2;
    proven_result_mem_mut_t rm = a.alloc_fn(a.ctx, n * 4 + 4, 16);   /* <= 4 UTF-8 bytes per cp */
    if (!PROVEN_IS_OK(rm.err)) return NULL;
    proven_u8 *out = (proven_u8 *)rm.value.ptr;
    proven_size_t o = 0;
    for (proven_size_t i = 0; i + (proven_size_t)unit <= n; i += (proven_size_t)unit) {
        proven_u32 cpv;
        if (unit == 4) {
            cpv = le ? (proven_u32)b[i] | ((proven_u32)b[i+1] << 8) | ((proven_u32)b[i+2] << 16) | ((proven_u32)b[i+3] << 24)
                     : (proven_u32)b[i+3] | ((proven_u32)b[i+2] << 8) | ((proven_u32)b[i+1] << 16) | ((proven_u32)b[i] << 24);
        } else {
            proven_u32 u = le ? (proven_u32)b[i] | ((proven_u32)b[i+1] << 8)
                              : (proven_u32)b[i+1] | ((proven_u32)b[i] << 8);
            if (u >= 0xD800 && u <= 0xDBFF && i + 4 <= n) {        /* high surrogate */
                proven_u32 lo = le ? (proven_u32)b[i+2] | ((proven_u32)b[i+3] << 8)
                                   : (proven_u32)b[i+3] | ((proven_u32)b[i+2] << 8);
                if (lo >= 0xDC00 && lo <= 0xDFFF) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
            }
            cpv = u;
        }
        o += (proven_size_t)u8_enc(cpv, out + o);
    }
    *outn = o; return out;
}

/* Detect EOL style on (decoded) UTF-8 bytes. */
static prov_eol_t detect_eol(const proven_u8 *b, proven_size_t n) {
    bool crlf = false, lf = false, cr = false;
    proven_u8 prev = 0;
    for (proven_size_t i = 0; i < n; i++) {
        proven_u8 c = b[i];
        if (c == '\n')        { if (prev == '\r') crlf = true; else lf = true; }
        else if (prev == '\r') cr = true;
        prev = c;
    }
    if (prev == '\r') cr = true;
    int kinds = (crlf ? 1 : 0) + (lf ? 1 : 0) + (cr ? 1 : 0);
    return kinds > 1 ? PROV_EOL_MIXED : crlf ? PROV_EOL_CRLF : cr ? PROV_EOL_CR : PROV_EOL_LF;
}

/* Normalize UTF-8 bytes: every CRLF / lone CR -> LF. Result <= n; caller frees. */
static proven_u8 *normalize_eol(proven_allocator_t a, const proven_u8 *b, proven_size_t n,
                                proven_size_t *outn) {
    proven_result_mem_mut_t rm = a.alloc_fn(a.ctx, n ? n : 1, 16);
    if (!PROVEN_IS_OK(rm.err)) return NULL;
    proven_u8 *out = (proven_u8 *)rm.value.ptr;
    proven_size_t i = 0, o = 0;
    while (i < n) {
        if (b[i] == '\r') { out[o++] = '\n'; if (i + 1 < n && b[i + 1] == '\n') i++; }
        else out[o++] = b[i];
        i++;
    }
    *outn = o; return out;
}

static bool eq_ci(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return *a == *b;
}
static bool is_utf8_name(const char *e)  { return eq_ci(e, "UTF-8") || eq_ci(e, "UTF8"); }
static bool is_1252_name(const char *e)  { return eq_ci(e, "windows-1252") || eq_ci(e, "cp1252") || eq_ci(e, "1252"); }
static void set_enc_name(prov_fileinfo_t *fi, const char *name) {
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof fi->enc_name; i++) fi->enc_name[i] = name[i];
    fi->enc_name[i] = '\0';
}

prov_result_buffer_t prov_load_file(proven_allocator_t alloc, const char *path,
                                    bool *sanitized, prov_fileinfo_t *out_info,
                                    const char *want_enc, const char *fallback_enc) {
    prov_result_buffer_t out = { NULL, PROVEN_OK };
    if (sanitized) *sanitized = false;
    if (out_info) *out_info = prov_fileinfo_default();
    if (!proven_alloc_is_valid(alloc) || path == NULL) {
        out.err = PROVEN_ERR_INVALID_ARG;
        return out;
    }

    proven_u8str_view_t pv = proven_u8str_view_from_cstr(path);
    proven_result_mem_mut_t rd = proven_fs_read_all(alloc, pv);
    if (!PROVEN_IS_OK(rd.err)) { out.err = rd.err; return out; }
    const proven_u8 *bytes = (const proven_u8 *)rd.value.ptr;
    proven_size_t    n     = rd.value.size;

    prov_fileinfo_t info = prov_fileinfo_default();
    proven_u8 *u8 = NULL;
    proven_size_t un = 0;

    /* explicit pick from the open panel: a forced non-UTF-8 encoding */
    const char *forced = (want_enc && want_enc[0] && !is_utf8_name(want_enc)) ? want_enc : NULL;
    if (forced && !is_1252_name(forced)) {                /* via the charset PAL (iconv/Win32) */
        u8 = prov_charset_to_utf8(alloc, forced, bytes, n, &un);
        if (u8) { set_enc_name(&info, forced); info.codepage = 0; info.encoding = "charset"; }
    } else if (forced) {                                  /* explicit Windows-1252 */
        u8 = decode_singlebyte(alloc, bytes, n, win1252_hi, &un);
        if (u8) { info.codepage = CP_WIN1252; info.encoding = enc_label(CP_WIN1252); }
    }

    if (!u8) {                                            /* auto: BOM UTF -> UTF-8 -> fallback */
        int bom_len = 0, cp = enc_from_bom(bytes, n, &bom_len);
        u8 = to_utf8(alloc, bytes + bom_len, n - (proven_size_t)bom_len, cp, &un);
        if (!u8) { if (rd.value.ptr) alloc.free_fn(alloc.ctx, rd.value.ptr); out.err = PROVEN_ERR_NOMEM; return out; }
        if (cp == CP_UTF8 && !prov_utf8_validate(u8, un)) {     /* not UTF-8: config fallback, else 1252 */
            const char *fb = (fallback_enc && fallback_enc[0] && !is_utf8_name(fallback_enc) && !is_1252_name(fallback_enc))
                             ? fallback_enc : NULL;
            proven_u8 *d = fb ? prov_charset_to_utf8(alloc, fb, bytes, n, &un) : NULL;
            alloc.free_fn(alloc.ctx, u8);
            if (d) { u8 = d; set_enc_name(&info, fb); info.codepage = 0; info.encoding = "charset"; }
            else {
                u8 = decode_singlebyte(alloc, bytes, n, win1252_hi, &un);
                info.codepage = CP_WIN1252; info.encoding = enc_label(CP_WIN1252);
                if (!u8) { if (rd.value.ptr) alloc.free_fn(alloc.ctx, rd.value.ptr); out.err = PROVEN_ERR_NOMEM; return out; }
            }
        } else { info.codepage = cp; info.encoding = enc_label(cp); info.bom = (bom_len > 0); }
    }
    if (rd.value.ptr) alloc.free_fn(alloc.ctx, rd.value.ptr);

    info.eol = detect_eol(u8, un);
    if (out_info) *out_info = info;

    proven_size_t nn = 0;                         /* normalize CRLF/CR -> LF */
    proven_u8 *norm = normalize_eol(alloc, u8, un, &nn);
    alloc.free_fn(alloc.ctx, u8);
    if (!norm) { out.err = PROVEN_ERR_NOMEM; return out; }

    if (!prov_utf8_validate(norm, nn)) {          /* only reachable for cp==UTF-8 */
        if (!sanitized) { alloc.free_fn(alloc.ctx, norm); out.err = PROVEN_ERR_INVALID_ENCODING; return out; }
        proven_size_t cn = 0;
        proven_u8 *clean = sanitize_utf8(alloc, norm, nn, &cn, sanitized);
        alloc.free_fn(alloc.ctx, norm);
        if (!clean) { out.err = PROVEN_ERR_NOMEM; return out; }
        out = prov_buffer_create_from_bytes(alloc, clean, cn);
        alloc.free_fn(alloc.ctx, clean);
        return out;
    }
    out = prov_buffer_create_from_bytes(alloc, norm, nn);
    alloc.free_fn(alloc.ctx, norm);
    return out;
}

prov_result_buffer_t prov_load_utf8_file(proven_allocator_t alloc,
                                         const char *path, bool *sanitized) {
    /* UTF-8-only loader (no BOM/EOL normalization, no fallback): strict rejects
     * invalid UTF-8, lossy drops the offending bytes. prov_load_file is the rich
     * loader; this remains for callers/tests that want raw UTF-8 semantics. */
    prov_result_buffer_t out = { NULL, PROVEN_OK };
    if (sanitized) *sanitized = false;
    if (!proven_alloc_is_valid(alloc) || path == NULL) { out.err = PROVEN_ERR_INVALID_ARG; return out; }
    proven_result_mem_mut_t rd = proven_fs_read_all(alloc, proven_u8str_view_from_cstr(path));
    if (!PROVEN_IS_OK(rd.err)) { out.err = rd.err; return out; }
    const proven_u8 *bytes = (const proven_u8 *)rd.value.ptr;
    proven_size_t    n     = rd.value.size;
    if (!prov_utf8_validate(bytes, n)) {
        if (!sanitized) { if (rd.value.ptr) alloc.free_fn(alloc.ctx, rd.value.ptr); out.err = PROVEN_ERR_INVALID_ENCODING; return out; }
        proven_size_t cn = 0;
        proven_u8 *clean = sanitize_utf8(alloc, bytes, n, &cn, sanitized);
        if (rd.value.ptr) alloc.free_fn(alloc.ctx, rd.value.ptr);
        if (!clean) { out.err = PROVEN_ERR_NOMEM; return out; }
        out = prov_buffer_create_from_bytes(alloc, clean, cn);
        alloc.free_fn(alloc.ctx, clean);
        return out;
    }
    out = prov_buffer_create_from_bytes(alloc, bytes, n);
    if (rd.value.ptr) alloc.free_fn(alloc.ctx, rd.value.ptr);
    return out;
}

prov_result_buffer_t prov_load_binary(proven_allocator_t alloc, const char *path,
                                      prov_fileinfo_t *out_info, const char *interp_enc) {
    prov_result_buffer_t out = { NULL, PROVEN_OK };
    if (out_info) {
        *out_info = prov_fileinfo_default();
        out_info->binary = true;
        out_info->encoding = "binary";
        if (interp_enc && interp_enc[0]) set_enc_name(out_info, interp_enc);
    }
    if (!proven_alloc_is_valid(alloc) || path == NULL) { out.err = PROVEN_ERR_INVALID_ARG; return out; }
    proven_result_mem_mut_t rd = proven_fs_read_all(alloc, proven_u8str_view_from_cstr(path));
    if (!PROVEN_IS_OK(rd.err)) { out.err = rd.err; return out; }
    out = prov_buffer_create_from_bytes(alloc, (const proven_u8 *)rd.value.ptr, rd.value.size);  /* verbatim */
    if (rd.value.ptr) alloc.free_fn(alloc.ctx, rd.value.ptr);
    return out;
}

/* Encode the internal (LF-only, BOM-free UTF-8) bytes `b[0..n)` to the on-disk
 * form described by `info`: LF -> the original EOL, then encode UTF-8 -> the
 * original code page (UTF-8 / UTF-16 / UTF-32), with a leading BOM when
 * info->bom. NULL info = verbatim LF UTF-8. Caller frees *outn bytes. */
proven_u8 *prov_encode_save(proven_allocator_t a, const proven_u8 *b, proven_size_t n,
                            const prov_fileinfo_t *info, proven_size_t *outn) {
    if (info && info->binary) {                   /* RFC-0019: raw bytes — write verbatim */
        proven_result_mem_mut_t r = a.alloc_fn(a.ctx, n ? n : 1, 16);
        if (!PROVEN_IS_OK(r.err)) return NULL;
        proven_u8 *o = (proven_u8 *)r.value.ptr;
        for (proven_size_t i = 0; i < n; i++) o[i] = b[i];
        *outn = n;
        return o;
    }
    bool bom  = info && info->bom;
    bool crlf = info && info->eol == PROV_EOL_CRLF;
    bool cr   = info && info->eol == PROV_EOL_CR;
    int  cp   = info ? info->codepage : CP_UTF8;

    /* step 1: LF -> original EOL (UTF-8 domain) */
    proven_size_t nl = 0;
    for (proven_size_t i = 0; i < n; i++) if (b[i] == '\n') nl++;
    proven_size_t e1cap = n + (crlf ? nl : 0) + 1;
    proven_result_mem_mut_t r1 = a.alloc_fn(a.ctx, e1cap, 16);
    if (!PROVEN_IS_OK(r1.err)) return NULL;
    proven_u8 *e1 = (proven_u8 *)r1.value.ptr;
    proven_size_t m = 0;
    for (proven_size_t i = 0; i < n; i++) {
        if (b[i] == '\n') { if (crlf) { e1[m++] = '\r'; e1[m++] = '\n'; } else e1[m++] = (proven_u8)(cr ? '\r' : '\n'); }
        else e1[m++] = b[i];
    }

    if (info && info->enc_name[0]) {              /* platform encoding -> charset PAL */
        proven_size_t en = 0;
        proven_u8 *enc = prov_charset_from_utf8(a, info->enc_name, e1, m, &en);
        a.free_fn(a.ctx, e1);
        if (!enc) return NULL;                    /* backend/encoding unavailable */
        *outn = en;
        return enc;
    }

    if (cp == CP_UTF8) {                          /* UTF-8: optional BOM + the bytes */
        proven_result_mem_mut_t r2 = a.alloc_fn(a.ctx, m + (bom ? 3 : 0) + 1, 16);
        if (!PROVEN_IS_OK(r2.err)) { a.free_fn(a.ctx, e1); return NULL; }
        proven_u8 *o = (proven_u8 *)r2.value.ptr; proven_size_t k = 0;
        if (bom) { o[k++] = 0xEF; o[k++] = 0xBB; o[k++] = 0xBF; }
        for (proven_size_t i = 0; i < m; i++) o[k++] = e1[i];
        a.free_fn(a.ctx, e1); *outn = k; return o;
    }

    if (cp == CP_WIN1252) {                       /* UTF-8 -> Windows-1252 (un-encodable -> '?') */
        proven_result_mem_mut_t r2 = a.alloc_fn(a.ctx, m + 1, 16);   /* <= 1 byte per code point */
        if (!PROVEN_IS_OK(r2.err)) { a.free_fn(a.ctx, e1); return NULL; }
        proven_u8 *o = (proven_u8 *)r2.value.ptr; proven_size_t k = 0;
        for (proven_size_t i = 0; i < m; ) {
            prov_decode_t d = prov_utf8_decode(e1 + i, m - i);
            proven_u32 v = d.valid ? d.cp : PROV_CP_REPLACEMENT;
            i += d.len ? d.len : 1;
            if (v < 0x80) { o[k++] = (proven_u8)v; continue; }
            int byte = -1;
            for (int j = 0; j < 128; j++) if (win1252_hi[j] == v) { byte = 0x80 + j; break; }
            o[k++] = (proven_u8)(byte >= 0 ? byte : '?');
        }
        a.free_fn(a.ctx, e1); *outn = k; return o;
    }

    /* UTF-16 / UTF-32: encode each code point, BOM first when requested */
    bool le = (cp == CP_UTF16LE || cp == CP_UTF32LE);
    int unit = (cp == CP_UTF32LE || cp == CP_UTF32BE) ? 4 : 2;
    proven_result_mem_mut_t r2 = a.alloc_fn(a.ctx, m * 4 + 8, 16);  /* <= 4 bytes per UTF-8 byte */
    if (!PROVEN_IS_OK(r2.err)) { a.free_fn(a.ctx, e1); return NULL; }
    proven_u8 *o = (proven_u8 *)r2.value.ptr; proven_size_t k = 0;
    #define EMIT16(x) do { proven_u32 _x = (x); if (le) { o[k++] = _x & 0xFF; o[k++] = (_x >> 8) & 0xFF; } \
                                                else    { o[k++] = (_x >> 8) & 0xFF; o[k++] = _x & 0xFF; } } while (0)
    if (bom) { if (unit == 4) { if (le) { o[k++]=0xFF;o[k++]=0xFE;o[k++]=0;o[k++]=0; } else { o[k++]=0;o[k++]=0;o[k++]=0xFE;o[k++]=0xFF; } }
               else { EMIT16(0xFEFF); } }
    for (proven_size_t i = 0; i < m; ) {
        prov_decode_t d = prov_utf8_decode(e1 + i, m - i);
        proven_u32 cpv = d.valid ? d.cp : PROV_CP_REPLACEMENT;
        i += d.len ? d.len : 1;
        if (unit == 4) { if (le) { o[k++]=cpv&0xFF;o[k++]=(cpv>>8)&0xFF;o[k++]=(cpv>>16)&0xFF;o[k++]=(cpv>>24)&0xFF; }
                         else { o[k++]=(cpv>>24)&0xFF;o[k++]=(cpv>>16)&0xFF;o[k++]=(cpv>>8)&0xFF;o[k++]=cpv&0xFF; } }
        else if (cpv > 0xFFFF) { proven_u32 v = cpv - 0x10000; EMIT16(0xD800 + (v >> 10)); EMIT16(0xDC00 + (v & 0x3FF)); }
        else EMIT16(cpv);
    }
    #undef EMIT16
    a.free_fn(a.ctx, e1); *outn = k; return o;
}

prov_fileinfo_t prov_fileinfo_default(void) {
    return (prov_fileinfo_t){ .encoding = "UTF-8", .codepage = 65001, .country = NULL,
                              .eol = PROV_EOL_LF, .bom = false };
}

const char *prov_eol_name(prov_eol_t eol) {
    switch (eol) {
        case PROV_EOL_CRLF:  return "CR/LF";
        case PROV_EOL_CR:    return "CR";
        case PROV_EOL_MIXED: return "MIXED";
        default:             return "LF";
    }
}

prov_fileinfo_t prov_detect_fileinfo(const prov_buffer_t *buf) {
    prov_fileinfo_t info = prov_fileinfo_default();
    proven_size_t n = prov_buffer_byte_len(buf);

    /* BOM: a leading EF BB BF. */
    proven_u8 head[3];
    if (n >= 3 && prov_buffer_copy_range(buf, 0, 3, head, 3) == 3 &&
        head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF)
        info.bom = true;

    /* Line endings: scan once, classifying each newline as CRLF / LF / lone CR. */
    bool crlf = false, lf = false, cr = false;
    proven_u8 chunk[4096];
    proven_u8 prev = 0;
    for (proven_size_t off = 0; off < n; ) {
        proven_size_t want = n - off < sizeof chunk ? n - off : sizeof chunk;
        proven_size_t got = prov_buffer_copy_range(buf, off, want, chunk, sizeof chunk);
        if (got == 0) break;
        for (proven_size_t i = 0; i < got; i++) {
            proven_u8 c = chunk[i];
            if (c == '\n')        { if (prev == '\r') crlf = true; else lf = true; }
            else if (prev == '\r') cr = true;        /* a CR not followed by LF */
            prev = c;
        }
        off += got;
    }
    if (prev == '\r') cr = true;                      /* trailing lone CR */

    int kinds = (crlf ? 1 : 0) + (lf ? 1 : 0) + (cr ? 1 : 0);
    if (kinds > 1)      info.eol = PROV_EOL_MIXED;
    else if (crlf)      info.eol = PROV_EOL_CRLF;
    else if (cr)        info.eol = PROV_EOL_CR;
    else                info.eol = PROV_EOL_LF;       /* lf, or no newline at all */
    return info;
}
