#include "motion.h"

#include "unicode.h"

#define NPOS PROVEN_SIZE_MAX

static const char OPENS[]  = "([{<";
static const char CLOSES[] = ")]}>";

static proven_size_t doc_len(const prov_buffer_t *b) {
    return prov_buffer_byte_len(b);
}

static prov_decode_t decode_at(const prov_buffer_t *b, proven_size_t pos) {
    proven_u8 tmp[4];
    proven_size_t got = prov_buffer_copy_range(b, pos, 4, tmp, 4);
    return prov_utf8_decode(tmp, got);
}

/* Byte offset of the code point before `pos`. */
static proven_size_t prev_cp(const prov_buffer_t *b, proven_size_t pos) {
    if (pos == 0) return 0;
    proven_size_t start = pos >= 4 ? pos - 4 : 0;
    proven_size_t len = pos - start;
    proven_u8 tmp[4];
    prov_buffer_copy_range(b, start, len, tmp, len);
    proven_size_t i = len - 1;
    while (i > 0 && (tmp[i] & 0xC0) == 0x80) i--;
    return start + i;
}

/* Line of byte `at` (binary search over line starts). */
static proven_size_t line_of(const prov_buffer_t *b, proven_size_t at) {
    proven_size_t lc = prov_buffer_line_count(b);
    proven_size_t lo = 0, hi = lc, ans = 0;
    while (lo < hi) {
        proven_size_t mid = lo + (hi - lo) / 2;
        if (prov_buffer_line_start(b, mid) <= at) { ans = mid; lo = mid + 1; }
        else hi = mid;
    }
    return ans;
}

/* Content length of line `L` in bytes, excluding the trailing newline. */
static proven_size_t line_content_len(const prov_buffer_t *b, proven_size_t L) {
    proven_size_t lc = prov_buffer_line_count(b);
    proven_size_t s = prov_buffer_line_start(b, L);
    proven_size_t e = (L + 1 < lc) ? prov_buffer_line_start(b, L + 1) - 1 : doc_len(b);
    return e - s;
}

static bool line_is_blank(const prov_buffer_t *b, proven_size_t L) {
    return line_content_len(b, L) == 0;
}

/* Raw byte at `pos`, or -1 past the end. Tags are ASCII, so '<'/'>'/'/' never
 * collide with UTF-8 continuation bytes. */
static int byte_at(const prov_buffer_t *b, proven_size_t pos) {
    if (pos >= doc_len(b)) return -1;
    proven_u8 c;
    return prov_buffer_copy_range(b, pos, 1, &c, 1) ? (int)c : -1;
}

static bool is_name_ch(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':';
}

static bool names_equal(const prov_buffer_t *b, proven_size_t a, proven_size_t alen,
                        proven_size_t c, proven_size_t clen) {
    if (alen != clen) return false;
    for (proven_size_t k = 0; k < alen; k++)
        if (byte_at(b, a + k) != byte_at(b, c + k)) return false;
    return true;
}

/* 0 = whitespace, 1 = word (alnum/_/non-ASCII), 2 = other (punctuation). */
static int char_class(proven_u32 cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') return 0;
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
        (cp >= '0' && cp <= '9') || cp == '_' || cp >= 0x80) return 1;
    return 2;
}

static int open_index(proven_u32 cp) {
    for (int i = 0; OPENS[i]; i++) if ((proven_u32)OPENS[i] == cp) return i;
    return -1;
}
static int close_index(proven_u32 cp) {
    for (int i = 0; CLOSES[i]; i++) if ((proven_u32)CLOSES[i] == cp) return i;
    return -1;
}

/* ------------------------------------------------------------ word motions */

proven_size_t prov_motion_word_next(const prov_buffer_t *b, proven_size_t at) {
    proven_size_t n = doc_len(b), p = at;
    if (p >= n) return n;
    int c0 = char_class(decode_at(b, p).cp);
    if (c0 != 0) {                          /* skip the current word run */
        while (p < n) {
            prov_decode_t d = decode_at(b, p);
            if (char_class(d.cp) != c0) break;
            p += d.len;
        }
    }
    while (p < n) {                         /* skip whitespace */
        prov_decode_t d = decode_at(b, p);
        if (char_class(d.cp) != 0) break;
        p += d.len;
    }
    return p;
}

proven_size_t prov_motion_word_prev(const prov_buffer_t *b, proven_size_t at) {
    if (at == 0) return 0;
    proven_size_t p = prev_cp(b, at);
    while (p > 0 && char_class(decode_at(b, p).cp) == 0) p = prev_cp(b, p);
    if (char_class(decode_at(b, p).cp) == 0) return p;   /* only whitespace before */
    int c = char_class(decode_at(b, p).cp);
    while (p > 0) {
        proven_size_t q = prev_cp(b, p);
        if (char_class(decode_at(b, q).cp) != c) break;
        p = q;
    }
    return p;
}

proven_size_t prov_motion_word_end(const prov_buffer_t *b, proven_size_t at) {
    proven_size_t n = doc_len(b), p = at;
    if (p >= n) return n;
    p += decode_at(b, p).len;               /* always advance at least one cp */
    while (p < n) {                         /* skip whitespace */
        prov_decode_t d = decode_at(b, p);
        if (char_class(d.cp) != 0) break;
        p += d.len;
    }
    if (p < n) {                            /* consume the word run */
        int c = char_class(decode_at(b, p).cp);
        while (p < n) {
            prov_decode_t d = decode_at(b, p);
            if (char_class(d.cp) != c) break;
            p += d.len;
        }
    }
    return p;
}

/* ------------------------------------------------------------ find / match */

proven_size_t prov_motion_find(const prov_buffer_t *b, proven_size_t at,
                               proven_u32 ch, bool till) {
    proven_size_t n = doc_len(b), p = at;
    if (p < n) p += decode_at(b, p).len;    /* search after the cursor */
    while (p < n) {
        prov_decode_t d = decode_at(b, p);
        if (d.cp == '\n') break;
        if (d.cp == ch) return till ? p : p + d.len;
        p += d.len;
    }
    return at;                              /* not found -> empty range */
}

proven_size_t prov_motion_findc(const prov_buffer_t *b, proven_size_t at,
                                proven_u32 ch, bool till, bool backward) {
    proven_size_t n = doc_len(b);
    if (n == 0) return at;
    if (!backward) {
        proven_size_t p = at;
        if (p < n) p += decode_at(b, p).len;       /* start after the cursor */
        while (p < n) {
            prov_decode_t d = decode_at(b, p);
            if (d.cp == '\n') break;
            if (d.cp == ch) return till ? prev_cp(b, p) : p;
            p += d.len ? d.len : 1;
        }
        return at;
    }
    if (at == 0) return at;
    proven_size_t p = prev_cp(b, at);
    for (;;) {
        prov_decode_t d = decode_at(b, p);
        if (d.cp == '\n') break;                   /* start of the line */
        if (d.cp == ch) return till ? p + (d.len ? d.len : 1) : p;
        if (p == 0) break;
        p = prev_cp(b, p);
    }
    return at;
}

proven_size_t prov_motion_match(const prov_buffer_t *b, proven_size_t at) {
    proven_size_t n = doc_len(b);
    if (at >= n) return at;
    prov_decode_t cur = decode_at(b, at);
    int oi = open_index(cur.cp), ci = close_index(cur.cp);

    if (oi >= 0) {                          /* on an open bracket: scan forward */
        proven_u32 oc = (proven_u32)OPENS[oi], cc = (proven_u32)CLOSES[oi];
        proven_size_t p = at + cur.len;
        int depth = 0;
        while (p < n) {
            prov_decode_t d = decode_at(b, p);
            if (d.cp == oc) depth++;
            else if (d.cp == cc) { if (depth == 0) return p; depth--; }
            p += d.len;
        }
    } else if (ci >= 0) {                   /* on a close bracket: scan backward */
        proven_u32 oc = (proven_u32)OPENS[ci], cc = (proven_u32)CLOSES[ci];
        if (at == 0) return at;
        proven_size_t p = prev_cp(b, at);
        int depth = 0;
        for (;;) {
            prov_decode_t d = decode_at(b, p);
            if (d.cp == cc) depth++;
            else if (d.cp == oc) { if (depth == 0) return p; depth--; }
            if (p == 0) break;
            p = prev_cp(b, p);
        }
    }
    return at;
}

/* ------------------------------------------------------------ text objects */

/* Locate the bracket pair of (openc,closec) enclosing `at`. */
static prov_range_t enclosing_pair(const prov_buffer_t *b, proven_size_t at,
                                   proven_u32 openc, proven_u32 closec) {
    prov_range_t r = { 0, 0, false };
    proven_size_t n = doc_len(b);

    /* find the enclosing open by scanning backward */
    proven_size_t openpos = NPOS;
    {
        int depth = 0;
        proven_size_t p = at;
        for (;;) {
            prov_decode_t d = decode_at(b, p);
            if (d.cp == closec && p < at) depth++;
            else if (d.cp == openc) { if (depth == 0) { openpos = p; break; } depth--; }
            if (p == 0) break;
            p = prev_cp(b, p);
        }
    }
    if (openpos == NPOS) return r;

    /* find its matching close by scanning forward */
    proven_size_t closepos = NPOS;
    {
        prov_decode_t od = decode_at(b, openpos);
        proven_size_t p = openpos + od.len;
        int depth = 0;
        while (p < n) {
            prov_decode_t d = decode_at(b, p);
            if (d.cp == openc) depth++;
            else if (d.cp == closec) { if (depth == 0) { closepos = p; break; } depth--; }
            p += d.len;
        }
    }
    if (closepos == NPOS) return r;

    prov_decode_t od = decode_at(b, openpos);
    prov_decode_t cd = decode_at(b, closepos);
    r.ok = true;
    r.start = openpos + od.len;             /* inner by default */
    r.end = closepos;
    if (closepos < openpos + od.len) r.start = closepos;  /* empty pair guard */
    /* caller widens to "around" if requested */
    (void)cd;
    return r;
}

/* Quote text object on the cursor's line. */
static prov_range_t quote_object(const prov_buffer_t *b, proven_size_t at,
                                 proven_u32 q, bool inner) {
    prov_range_t r = { 0, 0, false };
    proven_size_t n = doc_len(b);

    proven_size_t ls = at;                  /* line start */
    while (ls > 0) {
        proven_size_t prev = prev_cp(b, ls);
        if (decode_at(b, prev).cp == '\n') break;
        ls = prev;
    }
    proven_size_t prev_q = NPOS;
    proven_size_t p = ls;
    while (p < n) {
        prov_decode_t d = decode_at(b, p);
        if (d.cp == '\n') break;
        if (d.cp == q) {
            if (prev_q == NPOS) {
                prev_q = p;
            } else {
                if (at >= prev_q && at <= p) {     /* cursor within this pair */
                    r.ok = true;
                    r.start = inner ? prev_q + 1 : prev_q;
                    r.end = inner ? p : p + d.len;
                    return r;
                }
                prev_q = NPOS;                     /* start a new pair */
            }
        }
        p += d.len;
    }
    return r;
}

/* Innermost <tag>...</tag> pair enclosing `at`. `it` = between the tags,
 * `at` (around) = including both tags. Handles nesting via a small stack;
 * self-closing tags and <!.. <?.. declarations are skipped. Best-effort on
 * malformed markup (a mismatched close tag is ignored). */
static prov_range_t tag_object(const prov_buffer_t *b, proven_size_t at, bool inner) {
    prov_range_t r = { 0, 0, false };
    proven_size_t n = doc_len(b);
    if (n == 0) return r;
    if (at >= n) at = prev_cp(b, at);

    struct { proven_size_t lt, gt, name, nlen; } stk[64];
    int sp = 0;
    proven_size_t bo_lt = 0, bo_gt = 0, bc_lt = 0, bc_gt = 0, best_span = NPOS;

    proven_size_t i = 0;
    while (i < n) {
        if (byte_at(b, i) != '<') { i++; continue; }
        proven_size_t lt = i;
        int c1 = byte_at(b, i + 1);
        if (c1 == '!' || c1 == '?') {                 /* comment / declaration */
            proven_size_t j = i + 1;
            while (j < n && byte_at(b, j) != '>') j++;
            i = (j < n) ? j + 1 : n;
            continue;
        }
        bool close = (c1 == '/');
        proven_size_t ns = i + (close ? 2 : 1), ne = ns;
        while (ne < n && is_name_ch(byte_at(b, ne))) ne++;
        proven_size_t j = ne;
        while (j < n && byte_at(b, j) != '>') j++;
        if (j >= n) break;                            /* unterminated tag */
        bool selfclose = (byte_at(b, j - 1) == '/');
        proven_size_t gt = j + 1;

        if (close) {
            if (sp > 0 && names_equal(b, stk[sp - 1].name, stk[sp - 1].nlen, ns, ne - ns)) {
                sp--;
                proven_size_t o_lt = stk[sp].lt, o_gt = stk[sp].gt;
                if (o_lt <= at && at < gt && (gt - o_lt) < best_span) {
                    best_span = gt - o_lt;
                    bo_lt = o_lt; bo_gt = o_gt; bc_lt = lt; bc_gt = gt;
                }
            }
        } else if (!selfclose && ne > ns && sp < 64) {
            stk[sp].lt = lt; stk[sp].gt = gt; stk[sp].name = ns; stk[sp].nlen = ne - ns; sp++;
        }
        i = gt;
    }

    if (best_span == NPOS) return r;
    if (inner) { r.start = bo_gt; r.end = bc_lt; }
    else       { r.start = bo_lt; r.end = bc_gt; }
    r.ok = r.end >= r.start;
    return r;
}

prov_range_t prov_motion_textobj(const prov_buffer_t *b, proven_size_t at,
                                 prov_textobj_t obj, bool inner) {
    proven_size_t n = doc_len(b);

    switch (obj) {
        case PROV_TOBJ_WORD: {
            prov_range_t r = { 0, 0, false };
            if (n == 0) return r;
            proven_size_t p = at >= n ? prev_cp(b, at) : at;
            int c = char_class(decode_at(b, p).cp);
            proven_size_t start = p, end = p;
            while (start > 0) {
                proven_size_t q = prev_cp(b, start);
                if (char_class(decode_at(b, q).cp) != c) break;
                start = q;
            }
            while (end < n) {
                prov_decode_t d = decode_at(b, end);
                if (char_class(d.cp) != c) break;
                end += d.len;
            }
            if (!inner) {                   /* aw: add trailing (else leading) ws */
                proven_size_t e = end;
                while (e < n) {
                    prov_decode_t d = decode_at(b, e);
                    if (char_class(d.cp) != 0 || d.cp == '\n') break;
                    e += d.len;
                }
                if (e > end) end = e;
                else while (start > 0) {
                    proven_size_t qprev = prev_cp(b, start);
                    prov_decode_t d = decode_at(b, qprev);
                    if (char_class(d.cp) != 0 || d.cp == '\n') break;
                    start = qprev;
                }
            }
            r.start = start; r.end = end; r.ok = true;
            return r;
        }
        case PROV_TOBJ_PAREN:   { prov_range_t r = enclosing_pair(b, at, '(', ')'); if (r.ok && !inner) { r.start = prev_cp(b, r.start); r.end += 1; } return r; }
        case PROV_TOBJ_BRACE:   { prov_range_t r = enclosing_pair(b, at, '{', '}'); if (r.ok && !inner) { r.start = prev_cp(b, r.start); r.end += 1; } return r; }
        case PROV_TOBJ_BRACKET: { prov_range_t r = enclosing_pair(b, at, '[', ']'); if (r.ok && !inner) { r.start = prev_cp(b, r.start); r.end += 1; } return r; }
        case PROV_TOBJ_ANGLE:   { prov_range_t r = enclosing_pair(b, at, '<', '>'); if (r.ok && !inner) { r.start = prev_cp(b, r.start); r.end += 1; } return r; }
        case PROV_TOBJ_DQUOTE:  return quote_object(b, at, '"', inner);
        case PROV_TOBJ_SQUOTE:  return quote_object(b, at, '\'', inner);
        case PROV_TOBJ_PARAGRAPH: {
            /* A paragraph is a maximal run of lines with the same blankness as
             * the cursor's line (linewise). `ap` also takes the adjacent run of
             * the opposite blankness (trailing, else leading), like Vim. */
            prov_range_t r = { 0, 0, false };
            proven_size_t lc = prov_buffer_line_count(b);
            if (lc == 0) return r;
            proven_size_t at2 = (at >= n && n > 0) ? prev_cp(b, at) : at;
            proven_size_t L = line_of(b, at2);
            bool blank = line_is_blank(b, L);
            proven_size_t first = L, last = L;
            while (first > 0 && line_is_blank(b, first - 1) == blank) first--;
            while (last + 1 < lc && line_is_blank(b, last + 1) == blank) last++;
            if (!inner) {
                proven_size_t l2 = last;
                while (l2 + 1 < lc && line_is_blank(b, l2 + 1) != blank) l2++;
                if (l2 > last) { last = l2; }
                else {
                    proven_size_t f2 = first;
                    while (f2 > 0 && line_is_blank(b, f2 - 1) != blank) f2--;
                    first = f2;
                }
            }
            r.start = prov_buffer_line_start(b, first);
            r.end = (last + 1 < lc) ? prov_buffer_line_start(b, last + 1) : doc_len(b);
            r.ok = r.end > r.start;
            return r;
        }
        case PROV_TOBJ_TAG:     return tag_object(b, at, inner);
        default: {
            prov_range_t r = { 0, 0, false };
            return r;
        }
    }
}
