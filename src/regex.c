#include "regex.h"
#include "unicode.h"
#include "proven/arena.h"
#include "proven/memory.h"

/* ---- parser state ------------------------------------------------------- */

typedef struct {
    const proven_u8 *p;
    proven_size_t    n, i;
    proven_arena_t  *ar;
    int              ngroups;
    bool             oom;
    proven_err_t     err;
    proven_size_t    err_off;
    const char      *err_msg;
} rxp_t;

static void fail(rxp_t *s, proven_size_t off, const char *msg) {
    if (PROVEN_IS_OK(s->err)) { s->err = PROVEN_ERR_INVALID_ARG; s->err_off = off; s->err_msg = msg; }
}

static prov_rx_node_t *node(rxp_t *s, prov_rx_kind_t k) {
    proven_result_mem_mut_t m = proven_arena_alloc(s->ar, sizeof(prov_rx_node_t));
    if (!PROVEN_IS_OK(m.err)) { s->oom = true; return NULL; }
    prov_rx_node_t *nd = (prov_rx_node_t *)m.value.ptr;
    *nd = (prov_rx_node_t){ .kind = k };
    return nd;
}

/* doubling vector of child pointers, allocated from the (transient) arena */
typedef struct { prov_rx_node_t **a; proven_size_t n, cap; } nodevec;

static bool nv_push(rxp_t *s, nodevec *v, prov_rx_node_t *nd) {
    if (v->n == v->cap) {
        proven_size_t cap = v->cap ? v->cap * 2 : 8;
        proven_result_mem_mut_t m = proven_arena_alloc(s->ar, cap * sizeof(prov_rx_node_t *));
        if (!PROVEN_IS_OK(m.err)) { s->oom = true; return false; }
        prov_rx_node_t **na = (prov_rx_node_t **)m.value.ptr;
        for (proven_size_t k = 0; k < v->n; k++) na[k] = v->a[k];
        v->a = na; v->cap = cap;
    }
    v->a[v->n++] = nd;
    return true;
}

/* ---- character class helpers (ASCII, v1) -------------------------------- */

static void set_bit(proven_u8 set[32], proven_u8 b) { set[b >> 3] |= (proven_u8)(1u << (b & 7)); }

enum { PRED_D, PRED_W, PRED_S };
static void set_pred(proven_u8 set[32], int which) {
    if (which == PRED_D) { for (int c = '0'; c <= '9'; c++) set_bit(set, (proven_u8)c); }
    else if (which == PRED_W) {
        for (int c = '0'; c <= '9'; c++) set_bit(set, (proven_u8)c);
        for (int c = 'A'; c <= 'Z'; c++) set_bit(set, (proven_u8)c);
        for (int c = 'a'; c <= 'z'; c++) set_bit(set, (proven_u8)c);
        set_bit(set, '_');
    } else { /* PRED_S */
        set_bit(set, ' '); set_bit(set, '\t'); set_bit(set, '\n');
        set_bit(set, '\r'); set_bit(set, '\f'); set_bit(set, '\v');
    }
}

/* complement of a predefined set within 0..127 (for \D \W \S inside a class) */
static void set_pred_compl(proven_u8 set[32], int which) {
    proven_u8 tmp[32] = {0};
    set_pred(tmp, which);
    for (int b = 0; b < 128; b++) if (!(tmp[b >> 3] & (1u << (b & 7)))) set_bit(set, (proven_u8)b);
}

/* ---- forward decls ------------------------------------------------------ */

static prov_rx_node_t *parse_alt(rxp_t *s);
static prov_rx_node_t *parse_atom(rxp_t *s);

/* ---- escapes ------------------------------------------------------------ */

static prov_rx_node_t *parse_escape(rxp_t *s) {
    /* at the backslash */
    proven_size_t bs = s->i;
    s->i++;                                              /* consume '\' */
    if (s->i >= s->n) { fail(s, bs, "trailing backslash"); return NULL; }
    proven_u8 c = s->p[s->i++];
    switch (c) {
        case 'd': case 'D': case 'w': case 'W': case 's': case 'S': {
            prov_rx_node_t *nd = node(s, RX_CLASS); if (!nd) return NULL;
            int which = (c == 'd' || c == 'D') ? PRED_D : (c == 'w' || c == 'W') ? PRED_W : PRED_S;
            set_pred(nd->set, which);
            nd->negated = (c >= 'A' && c <= 'Z');        /* uppercase = negated */
            return nd;
        }
        case 'b': case 'B': {
            prov_rx_node_t *nd = node(s, RX_ANCHOR); if (!nd) return NULL;
            nd->anchor = (c == 'b') ? RX_A_WORDB : RX_A_NWORDB;
            return nd;
        }
        case 'z': {
            if (s->i >= s->n || (s->p[s->i] != 's' && s->p[s->i] != 'e')) {
                fail(s, bs, "bad escape (use \\zs or \\ze)"); return NULL;
            }
            prov_rx_node_t *nd = node(s, RX_MARK); if (!nd) return NULL;
            nd->mark = (s->p[s->i] == 's') ? RX_M_ZS : RX_M_ZE;
            s->i++;
            return nd;
        }
        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9':
            fail(s, bs, "backreferences are not supported"); return NULL;
        default: {
            prov_rx_node_t *nd = node(s, RX_LIT); if (!nd) return NULL;
            switch (c) { case 'n': nd->byte = '\n'; break; case 't': nd->byte = '\t'; break;
                         case 'r': nd->byte = '\r'; break; case 'f': nd->byte = '\f'; break;
                         case 'v': nd->byte = '\v'; break; case '0': nd->byte = '\0'; break;
                         default: nd->byte = c; break; }       /* \. \* \\ … literal */
            return nd;
        }
    }
}

/* ---- character class [ ... ] -------------------------------------------- */

static prov_rx_node_t *parse_class(rxp_t *s) {
    proven_size_t open = s->i;
    s->i++;                                              /* consume '[' */
    prov_rx_node_t *nd = node(s, RX_CLASS); if (!nd) return NULL;
    if (s->i < s->n && s->p[s->i] == '^') { nd->negated = true; s->i++; }
    bool first = true;
    while (s->i < s->n && (s->p[s->i] != ']' || first)) {
        first = false;
        proven_u8 c = s->p[s->i];
        if (c == '\\') {                                 /* escape inside a class */
            s->i++;
            if (s->i >= s->n) { fail(s, open, "unterminated character class"); return NULL; }
            proven_u8 e = s->p[s->i++];
            switch (e) {
                case 'd': set_pred(nd->set, PRED_D); continue;
                case 'w': set_pred(nd->set, PRED_W); continue;
                case 's': set_pred(nd->set, PRED_S); continue;
                case 'D': set_pred_compl(nd->set, PRED_D); continue;
                case 'W': set_pred_compl(nd->set, PRED_W); continue;
                case 'S': set_pred_compl(nd->set, PRED_S); continue;
                case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break;
                case 'f': c = '\f'; break; case 'v': c = '\v'; break; case '0': c = '\0'; break;
                default:  c = e; break;
            }
        } else {
            s->i++;
        }
        /* range a-z ? (only when '-' is not the last char before ']') */
        if (s->i + 1 < s->n && s->p[s->i] == '-' && s->p[s->i + 1] != ']') {
            s->i++;                                       /* consume '-' */
            proven_u8 hi = s->p[s->i];
            if (hi == '\\') { s->i++; if (s->i >= s->n) { fail(s, open, "unterminated character class"); return NULL; } hi = s->p[s->i]; }
            s->i++;
            if (hi < c) { fail(s, open, "reversed range in character class"); return NULL; }
            for (int b = c; b <= (int)hi; b++) set_bit(nd->set, (proven_u8)b);
        } else {
            set_bit(nd->set, c);
        }
    }
    if (s->i >= s->n || s->p[s->i] != ']') { fail(s, open, "unterminated character class"); return NULL; }
    s->i++;                                               /* consume ']' */
    return nd;
}

/* ---- {n,m} -------------------------------------------------------------- */

/* returns 1 = quantifier parsed (lo/hi set), 0 = not a quantifier (i restored,
 * treat '{' as a literal later), -1 = error. */
static int parse_brace(rxp_t *s, proven_u32 *lo, proven_u32 *hi) {
    proven_size_t save = s->i;
    s->i++;                                              /* consume '{' */
    if (s->i >= s->n || s->p[s->i] < '0' || s->p[s->i] > '9') { s->i = save; return 0; }
    proven_u32 a = 0;
    while (s->i < s->n && s->p[s->i] >= '0' && s->p[s->i] <= '9') {
        a = a * 10u + (proven_u32)(s->p[s->i++] - '0');
        if (a > 100000u) { fail(s, save, "repeat count too large"); return -1; }
    }
    proven_u32 b = a;
    if (s->i < s->n && s->p[s->i] == ',') {
        s->i++;
        if (s->i < s->n && s->p[s->i] == '}') { b = RX_INF; }
        else {
            if (s->i >= s->n || s->p[s->i] < '0' || s->p[s->i] > '9') { s->i = save; return 0; }
            b = 0;
            while (s->i < s->n && s->p[s->i] >= '0' && s->p[s->i] <= '9') {
                b = b * 10u + (proven_u32)(s->p[s->i++] - '0');
                if (b > 100000u) { fail(s, save, "repeat count too large"); return -1; }
            }
        }
    }
    if (s->i >= s->n || s->p[s->i] != '}') { s->i = save; return 0; }
    s->i++;                                               /* consume '}' */
    if (b != RX_INF && b < a) { fail(s, save, "reversed repeat range {n,m}"); return -1; }
    *lo = a; *hi = b;
    return 1;
}

/* ---- atom + repeat ------------------------------------------------------ */

static prov_rx_node_t *parse_atom(rxp_t *s) {
    proven_u8 c = s->p[s->i];
    switch (c) {
        case '(': {
            proven_size_t open = s->i;
            s->i++;
            int grp;
            if (s->i + 1 < s->n && s->p[s->i] == '?' && s->p[s->i + 1] == ':') { s->i += 2; grp = 0; }
            else if (s->i < s->n && s->p[s->i] == '?') {
                fail(s, open, "unsupported group ((?: only; no lookaround/named)"); return NULL;
            } else {
                if (s->ngroups >= PROV_RX_MAX_GROUPS - 1) { fail(s, open, "too many capturing groups"); return NULL; }
                grp = ++s->ngroups;
            }
            prov_rx_node_t *body = parse_alt(s); if (!body) return NULL;
            if (s->i >= s->n || s->p[s->i] != ')') { fail(s, open, "unmatched ("); return NULL; }
            s->i++;
            prov_rx_node_t *g = node(s, RX_GROUP); if (!g) return NULL;
            g->group = grp;
            g->kids = (prov_rx_node_t **)proven_arena_alloc(s->ar, sizeof(prov_rx_node_t *)).value.ptr;
            if (!g->kids) { s->oom = true; return NULL; }
            g->kids[0] = body; g->nkids = 1;
            return g;
        }
        case '[': return parse_class(s);
        case '.': { s->i++; return node(s, RX_ANY); }
        case '^': { s->i++; prov_rx_node_t *a = node(s, RX_ANCHOR); if (a) a->anchor = RX_A_BOL; return a; }
        case '$': { s->i++; prov_rx_node_t *a = node(s, RX_ANCHOR); if (a) a->anchor = RX_A_EOL; return a; }
        case '\\': return parse_escape(s);
        case '*': case '+': case '?': fail(s, s->i, "nothing to repeat"); return NULL;
        default: {
            prov_decode_t d = prov_utf8_decode(s->p + s->i, s->n - s->i);
            int blen = (d.valid && d.len > 0) ? (int)d.len : 1;
            if (blen <= 1) {
                prov_rx_node_t *nd = node(s, RX_LIT); if (!nd) return NULL;
                nd->byte = s->p[s->i++];
                return nd;
            }
            prov_rx_node_t *cn = node(s, RX_CONCAT); if (!cn) return NULL;
            proven_result_mem_mut_t m = proven_arena_alloc(s->ar, (proven_size_t)blen * sizeof(prov_rx_node_t *));
            if (!PROVEN_IS_OK(m.err)) { s->oom = true; return NULL; }
            cn->kids = (prov_rx_node_t **)m.value.ptr; cn->nkids = (proven_size_t)blen;
            for (int k = 0; k < blen; k++) {
                prov_rx_node_t *b = node(s, RX_LIT); if (!b) return NULL;
                b->byte = s->p[s->i + (proven_size_t)k];
                cn->kids[k] = b;
            }
            s->i += (proven_size_t)blen;
            return cn;
        }
    }
}

static prov_rx_node_t *parse_repeat(rxp_t *s) {
    prov_rx_node_t *atom = parse_atom(s); if (!atom) return NULL;
    for (;;) {
        if (s->i >= s->n) break;
        proven_u8 c = s->p[s->i];
        proven_u32 lo, hi;
        if (c == '*') { lo = 0; hi = RX_INF; s->i++; }
        else if (c == '+') { lo = 1; hi = RX_INF; s->i++; }
        else if (c == '?') { lo = 0; hi = 1; s->i++; }
        else if (c == '{') {
            int r = parse_brace(s, &lo, &hi);
            if (r < 0) return NULL;
            if (r == 0) break;                            /* '{' is a literal */
        } else break;
        bool lazy = false;
        if (s->i < s->n && s->p[s->i] == '?') { lazy = true; s->i++; }
        prov_rx_node_t *rep = node(s, RX_REPEAT); if (!rep) return NULL;
        proven_result_mem_mut_t m = proven_arena_alloc(s->ar, sizeof(prov_rx_node_t *));
        if (!PROVEN_IS_OK(m.err)) { s->oom = true; return NULL; }
        rep->kids = (prov_rx_node_t **)m.value.ptr; rep->kids[0] = atom; rep->nkids = 1;
        rep->rmin = lo; rep->rmax = hi; rep->lazy = lazy;
        atom = rep;
    }
    return atom;
}

/* ---- concat / alt ------------------------------------------------------- */

static prov_rx_node_t *parse_concat(rxp_t *s) {
    nodevec v = {0};
    while (s->i < s->n && s->p[s->i] != '|' && s->p[s->i] != ')') {
        prov_rx_node_t *child = parse_repeat(s); if (!child) return NULL;
        if (!nv_push(s, &v, child)) return NULL;
    }
    if (v.n == 0) return node(s, RX_EMPTY);
    if (v.n == 1) return v.a[0];
    prov_rx_node_t *cn = node(s, RX_CONCAT); if (!cn) return NULL;
    cn->kids = v.a; cn->nkids = v.n;
    return cn;
}

static prov_rx_node_t *parse_alt(rxp_t *s) {
    prov_rx_node_t *first = parse_concat(s); if (!first) return NULL;
    if (s->i >= s->n || s->p[s->i] != '|') return first;
    nodevec v = {0};
    if (!nv_push(s, &v, first)) return NULL;
    while (s->i < s->n && s->p[s->i] == '|') {
        s->i++;
        prov_rx_node_t *c = parse_concat(s); if (!c) return NULL;
        if (!nv_push(s, &v, c)) return NULL;
    }
    prov_rx_node_t *an = node(s, RX_ALT); if (!an) return NULL;
    an->kids = v.a; an->nkids = v.n;
    return an;
}

/* ---- public ------------------------------------------------------------- */

prov_rx_parse_t prov_rx_parse(proven_allocator_t a, proven_u8str_view_t pattern) {
    prov_rx_parse_t out = {0};
    /* Bump-arena backing for the AST: parsing is O(pattern) nodes; a generous
     * linear bound avoids rejecting a valid pattern (over-limit fails safe). */
    proven_size_t cap = 4096 + pattern.size * 256;
    proven_result_mem_mut_t bm = a.alloc_fn(a.ctx, cap, 16);
    if (!PROVEN_IS_OK(bm.err)) { out.err = bm.err; out.err_msg = "out of memory"; return out; }
    out.backing = bm.value.ptr;
    proven_arena_t ar = proven_arena_create((proven_mem_mut_t){ .ptr = bm.value.ptr, .size = cap });

    rxp_t s = { .p = pattern.ptr, .n = pattern.size, .i = 0, .ar = &ar };
    prov_rx_node_t *root = parse_alt(&s);
    if (root && PROVEN_IS_OK(s.err) && !s.oom && s.i != s.n) {
        /* a leftover ')' is the usual cause */
        fail(&s, s.i, (s.i < s.n && s.p[s.i] == ')') ? "unmatched )" : "unexpected trailing input");
    }
    if (s.oom && PROVEN_IS_OK(s.err)) { s.err = PROVEN_ERR_NOMEM; s.err_off = s.i; s.err_msg = "pattern too complex"; }

    out.err = s.err; out.err_off = s.err_off; out.err_msg = s.err_msg;
    out.ngroups = s.ngroups;
    out.root = PROVEN_IS_OK(s.err) ? root : NULL;
    return out;
}

void prov_rx_parse_free(proven_allocator_t a, prov_rx_parse_t *p) {
    if (p->backing) a.free_fn(a.ctx, p->backing);
    p->backing = NULL; p->root = NULL;
}

/* ---- AST dump (s-expression) -------------------------------------------- */

typedef struct { char *b; proven_size_t cap, len; } dbuf;
static void d_ch(dbuf *d, char c) { if (d->len + 1 < d->cap) d->b[d->len] = c; d->len++; }
static void d_str(dbuf *d, const char *s) { for (; *s; s++) d_ch(d, *s); }
static void d_uint(dbuf *d, proven_u32 v) {
    char t[12]; int k = 0;
    if (v == 0) { d_ch(d, '0'); return; }
    while (v && k < 12) { t[k++] = (char)('0' + v % 10u); v /= 10u; }
    while (k) d_ch(d, t[--k]);
}

static void d_node(dbuf *d, const prov_rx_node_t *n) {
    if (!n) { d_str(d, "nil"); return; }
    switch (n->kind) {
        case RX_EMPTY:  d_str(d, "eps"); break;
        case RX_ANY:    d_str(d, "any"); break;
        case RX_LIT:
            d_str(d, "(lit ");
            if (n->byte >= 0x20 && n->byte < 0x7f) d_ch(d, (char)n->byte);
            else { d_str(d, "\\x"); const char *hx = "0123456789abcdef"; d_ch(d, hx[n->byte >> 4]); d_ch(d, hx[n->byte & 15]); }
            d_ch(d, ')');
            break;
        case RX_CLASS:
            d_str(d, n->negated ? "(ncls:" : "(cls:");
            for (int b = 0x20; b < 0x7f; b++) if (n->set[b >> 3] & (1u << (b & 7))) d_ch(d, (char)b);
            d_ch(d, ')');
            break;
        case RX_ANCHOR:
            d_str(d, n->anchor == RX_A_BOL ? "bol" : n->anchor == RX_A_EOL ? "eol" :
                     n->anchor == RX_A_WORDB ? "wordb" : "nwordb");
            break;
        case RX_MARK:   d_str(d, n->mark == RX_M_ZS ? "zs" : "ze"); break;
        case RX_CONCAT:
            d_str(d, "(cat");
            for (proven_size_t k = 0; k < n->nkids; k++) { d_ch(d, ' '); d_node(d, n->kids[k]); }
            d_ch(d, ')');
            break;
        case RX_ALT:
            d_str(d, "(alt");
            for (proven_size_t k = 0; k < n->nkids; k++) { d_ch(d, ' '); d_node(d, n->kids[k]); }
            d_ch(d, ')');
            break;
        case RX_REPEAT:
            d_str(d, n->lazy ? "(rep? " : "(rep ");
            d_uint(d, n->rmin); d_ch(d, ' ');
            if (n->rmax == RX_INF) d_str(d, "inf"); else d_uint(d, n->rmax);
            d_ch(d, ' '); d_node(d, n->kids[0]); d_ch(d, ')');
            break;
        case RX_GROUP:
            if (n->group) { d_str(d, "(grp "); d_uint(d, (proven_u32)n->group); d_ch(d, ' '); }
            else d_str(d, "(ncg ");
            d_node(d, n->kids[0]); d_ch(d, ')');
            break;
    }
}

proven_size_t prov_rx_ast_dump(const prov_rx_node_t *n, char *buf, proven_size_t cap) {
    dbuf d = { .b = buf, .cap = cap, .len = 0 };
    d_node(&d, n);
    if (cap) d.b[d.len < cap ? d.len : cap - 1] = '\0';
    return d.len;
}

/* ======================================================================== */
/* S2 — compiler: AST → Pike VM bytecode                                     */
/* ======================================================================== */

typedef struct { prov_regex_t *re; proven_allocator_t a; bool oom, toobig; } rxc_t;

static proven_u32 emit(rxc_t *c, proven_u32 op, proven_u32 x, proven_u32 y) {
    prov_regex_t *re = c->re;
    if (c->oom || c->toobig) return 0;
    if (re->ninsts >= PROV_RX_MAX_INSTS) { c->toobig = true; return 0; }
    if (re->ninsts >= re->instcap) {
        proven_size_t cap = re->instcap ? re->instcap * 2 : 64;
        proven_result_mem_mut_t m = re->insts
            ? c->a.realloc_fn(c->a.ctx, re->insts, re->instcap * sizeof(prov_rx_inst_t),
                              cap * sizeof(prov_rx_inst_t), 16)
            : c->a.alloc_fn(c->a.ctx, cap * sizeof(prov_rx_inst_t), 16);
        if (!PROVEN_IS_OK(m.err)) { c->oom = true; return 0; }
        re->insts = (prov_rx_inst_t *)m.value.ptr; re->instcap = cap;
    }
    proven_u32 pc = (proven_u32)re->ninsts;
    re->insts[re->ninsts++] = (prov_rx_inst_t){ .op = op, .x = x, .y = y };
    return pc;
}

static proven_u32 add_set(rxc_t *c, const proven_u8 set[32]) {
    prov_regex_t *re = c->re;
    if (c->oom || c->toobig) return 0;
    if (re->nsets >= re->setcap) {
        proven_size_t cap = re->setcap ? re->setcap * 2 : 8;
        proven_result_mem_mut_t m = re->sets
            ? c->a.realloc_fn(c->a.ctx, re->sets, re->setcap * 32, cap * 32, 16)
            : c->a.alloc_fn(c->a.ctx, cap * 32, 16);
        if (!PROVEN_IS_OK(m.err)) { c->oom = true; return 0; }
        re->sets = (proven_u8 (*)[32])m.value.ptr; re->setcap = cap;
    }
    proven_u32 idx = (proven_u32)re->nsets;
    for (int k = 0; k < 32; k++) re->sets[re->nsets][k] = set[k];
    re->nsets++;
    return idx;
}

static bool in_set(const proven_u8 set[32], int b) { return (set[b >> 3] >> (b & 7)) & 1; }
static void clr_bit(proven_u8 set[32], int b) { set[b >> 3] &= (proven_u8)~(1u << (b & 7)); }
static void fold_set(proven_u8 set[32]) {
    for (int c = 'a'; c <= 'z'; c++) if (in_set(set, c)) set_bit(set, (proven_u8)(c - 32));
    for (int c = 'A'; c <= 'Z'; c++) if (in_set(set, c)) set_bit(set, (proven_u8)(c + 32));
}

/* one valid UTF-8 codepoint: 1-byte from `onebyte` (ASCII), else a 2/3/4-byte
 * lead+continuation sequence (correct on well-formed UTF-8 input). */
static void emit_codepoint(rxc_t *c, const proven_u8 onebyte[32]) {
    proven_u32 idx = add_set(c, onebyte);
    proven_u32 s1 = emit(c, RXO_SPLIT, 0, 0);
    proven_u32 A = c->re->ninsts; emit(c, RXO_CLASS, idx, 0); proven_u32 jA = emit(c, RXO_JMP, 0, 0);
    proven_u32 s2 = emit(c, RXO_SPLIT, 0, 0);
    proven_u32 B = c->re->ninsts; emit(c, RXO_RANGE, 0xC0, 0xDF); emit(c, RXO_RANGE, 0x80, 0xBF); proven_u32 jB = emit(c, RXO_JMP, 0, 0);
    proven_u32 s3 = emit(c, RXO_SPLIT, 0, 0);
    proven_u32 C = c->re->ninsts; emit(c, RXO_RANGE, 0xE0, 0xEF); emit(c, RXO_RANGE, 0x80, 0xBF); emit(c, RXO_RANGE, 0x80, 0xBF); proven_u32 jC = emit(c, RXO_JMP, 0, 0);
    proven_u32 D = c->re->ninsts; emit(c, RXO_RANGE, 0xF0, 0xF7); emit(c, RXO_RANGE, 0x80, 0xBF); emit(c, RXO_RANGE, 0x80, 0xBF); emit(c, RXO_RANGE, 0x80, 0xBF);
    proven_u32 END = c->re->ninsts;
    if (c->oom || c->toobig) return;
    c->re->insts[s1].x = A;  c->re->insts[s1].y = s2;
    c->re->insts[s2].x = B;  c->re->insts[s2].y = s3;
    c->re->insts[s3].x = C;  c->re->insts[s3].y = D;
    c->re->insts[jA].x = END; c->re->insts[jB].x = END; c->re->insts[jC].x = END;
}

static void emit_node(rxc_t *c, const prov_rx_node_t *n);

static void emit_star(rxc_t *c, const prov_rx_node_t *body, bool lazy) {
    proven_u32 L1 = c->re->ninsts;
    proven_u32 sp = emit(c, RXO_SPLIT, 0, 0);
    proven_u32 bs = c->re->ninsts; emit_node(c, body); emit(c, RXO_JMP, L1, 0);
    proven_u32 ex = c->re->ninsts;
    if (c->oom || c->toobig) return;
    if (lazy) { c->re->insts[sp].x = ex; c->re->insts[sp].y = bs; }
    else      { c->re->insts[sp].x = bs; c->re->insts[sp].y = ex; }
}
static void emit_plus(rxc_t *c, const prov_rx_node_t *body, bool lazy) {
    proven_u32 bs = c->re->ninsts; emit_node(c, body);
    proven_u32 sp = emit(c, RXO_SPLIT, 0, 0);
    proven_u32 ex = c->re->ninsts;
    if (c->oom || c->toobig) return;
    if (lazy) { c->re->insts[sp].x = ex; c->re->insts[sp].y = bs; }
    else      { c->re->insts[sp].x = bs; c->re->insts[sp].y = ex; }
}
static void emit_quest(rxc_t *c, const prov_rx_node_t *body, bool lazy) {
    proven_u32 sp = emit(c, RXO_SPLIT, 0, 0);
    proven_u32 bs = c->re->ninsts; emit_node(c, body);
    proven_u32 ex = c->re->ninsts;
    if (c->oom || c->toobig) return;
    if (lazy) { c->re->insts[sp].x = ex; c->re->insts[sp].y = bs; }
    else      { c->re->insts[sp].x = bs; c->re->insts[sp].y = ex; }
}

static void emit_repeat(rxc_t *c, const prov_rx_node_t *n) {
    const prov_rx_node_t *body = n->kids[0];
    proven_u32 lo = n->rmin, hi = n->rmax; bool lazy = n->lazy;
    if (lo == 0 && hi == RX_INF) { emit_star(c, body, lazy); return; }
    if (lo == 1 && hi == RX_INF) { emit_plus(c, body, lazy); return; }
    if (lo == 0 && hi == 1)      { emit_quest(c, body, lazy); return; }
    for (proven_u32 i = 0; i < lo; i++) emit_node(c, body);
    if (hi == RX_INF) { if (lo > 0) emit_star(c, body, lazy); else emit_star(c, body, lazy); return; }
    proven_u32 extra = hi - lo;
    if (extra == 0) return;
    if (extra > PROV_RX_MAX_INSTS) { c->toobig = true; return; }
    proven_result_mem_mut_t tm = c->a.alloc_fn(c->a.ctx, extra * sizeof(proven_u32), 16);
    if (!PROVEN_IS_OK(tm.err)) { c->oom = true; return; }
    proven_u32 *sp = (proven_u32 *)tm.value.ptr;
    for (proven_u32 i = 0; i < extra; i++) {
        sp[i] = emit(c, RXO_SPLIT, 0, 0);
        proven_u32 bs = c->re->ninsts;
        if (!c->oom && !c->toobig) { if (lazy) c->re->insts[sp[i]].y = bs; else c->re->insts[sp[i]].x = bs; }
        emit_node(c, body);
    }
    proven_u32 END = c->re->ninsts;
    if (!c->oom && !c->toobig)
        for (proven_u32 i = 0; i < extra; i++) { if (lazy) c->re->insts[sp[i]].x = END; else c->re->insts[sp[i]].y = END; }
    c->a.free_fn(c->a.ctx, sp);
}

static void emit_alt(rxc_t *c, const prov_rx_node_t *n) {
    proven_size_t k = n->nkids;
    if (k == 0) return;
    if (k == 1) { emit_node(c, n->kids[0]); return; }
    proven_result_mem_mut_t tm = c->a.alloc_fn(c->a.ctx, (k - 1) * sizeof(proven_u32), 16);
    if (!PROVEN_IS_OK(tm.err)) { c->oom = true; return; }
    proven_u32 *jm = (proven_u32 *)tm.value.ptr;
    for (proven_size_t i = 0; i < k; i++) {
        if (i < k - 1) {
            proven_u32 spc = emit(c, RXO_SPLIT, 0, 0);
            if (!c->oom && !c->toobig) c->re->insts[spc].x = c->re->ninsts;
            emit_node(c, n->kids[i]);
            jm[i] = emit(c, RXO_JMP, 0, 0);
            if (!c->oom && !c->toobig) c->re->insts[spc].y = c->re->ninsts;
        } else {
            emit_node(c, n->kids[i]);
        }
    }
    proven_u32 END = c->re->ninsts;
    if (!c->oom && !c->toobig)
        for (proven_size_t i = 0; i < k - 1; i++) c->re->insts[jm[i]].x = END;
    c->a.free_fn(c->a.ctx, jm);
}

static void emit_node(rxc_t *c, const prov_rx_node_t *n) {
    if (c->oom || c->toobig || !n) return;
    bool icase = (c->re->flags & PROV_RX_ICASE) != 0;
    switch (n->kind) {
        case RX_EMPTY: break;
        case RX_LIT:
            if (icase && ((n->byte >= 'a' && n->byte <= 'z') || (n->byte >= 'A' && n->byte <= 'Z'))) {
                proven_u8 set[32] = {0};
                set_bit(set, (proven_u8)(n->byte | 0x20)); set_bit(set, (proven_u8)(n->byte & ~0x20));
                emit(c, RXO_CLASS, add_set(c, set), 0);
            } else emit(c, RXO_BYTE, n->byte, 0);
            break;
        case RX_ANY: {
            proven_u8 set[32]; for (int k = 0; k < 32; k++) set[k] = 0;
            for (int b = 0; b < 128; b++) set_bit(set, (proven_u8)b);
            if (!(c->re->flags & PROV_RX_DOTALL)) clr_bit(set, '\n');
            emit_codepoint(c, set);
            break;
        }
        case RX_CLASS:
            if (!n->negated) {
                proven_u8 set[32]; for (int k = 0; k < 32; k++) set[k] = n->set[k];
                if (icase) fold_set(set);
                emit(c, RXO_CLASS, add_set(c, set), 0);
            } else {
                proven_u8 cls[32]; for (int k = 0; k < 32; k++) cls[k] = n->set[k];
                if (icase) fold_set(cls);
                proven_u8 one[32]; for (int k = 0; k < 32; k++) one[k] = 0;
                for (int b = 0; b < 128; b++) if (!in_set(cls, b)) set_bit(one, (proven_u8)b);
                emit_codepoint(c, one);
            }
            break;
        case RX_ANCHOR: emit(c, RXO_ASSERT, (proven_u32)n->anchor, 0); break;
        case RX_MARK:
            if (n->mark == RX_M_ZS) emit(c, RXO_SAVE, 0, 0);
            else { emit(c, RXO_SAVE, 1, 0); c->re->has_ze = true; }
            break;
        case RX_CONCAT: for (proven_size_t k = 0; k < n->nkids; k++) emit_node(c, n->kids[k]); break;
        case RX_ALT: emit_alt(c, n); break;
        case RX_REPEAT: emit_repeat(c, n); break;
        case RX_GROUP:
            if (n->group) emit(c, RXO_SAVE, 2u * (proven_u32)n->group, 0);
            emit_node(c, n->kids[0]);
            if (n->group) emit(c, RXO_SAVE, 2u * (proven_u32)n->group + 1u, 0);
            break;
    }
}

static bool vm_alloc(proven_allocator_t a, prov_regex_t *re) {
    re->nsave = (proven_size_t)(2 * (re->ngroups + 1));
    proven_size_t ni = re->ninsts;
    proven_result_mem_mut_t m;
    m = a.alloc_fn(a.ctx, ni * sizeof(proven_u32), 16);          if (!PROVEN_IS_OK(m.err)) return false; re->vis   = (proven_u32 *)m.value.ptr;
    for (proven_size_t i = 0; i < ni; i++) re->vis[i] = 0;
    m = a.alloc_fn(a.ctx, ni * sizeof(proven_u32), 16);          if (!PROVEN_IS_OK(m.err)) return false; re->cl_pc = (proven_u32 *)m.value.ptr;
    m = a.alloc_fn(a.ctx, ni * sizeof(proven_u32), 16);          if (!PROVEN_IS_OK(m.err)) return false; re->nl_pc = (proven_u32 *)m.value.ptr;
    m = a.alloc_fn(a.ctx, ni * re->nsave * sizeof(proven_size_t), 16); if (!PROVEN_IS_OK(m.err)) return false; re->cl_sv = (proven_size_t *)m.value.ptr;
    m = a.alloc_fn(a.ctx, ni * re->nsave * sizeof(proven_size_t), 16); if (!PROVEN_IS_OK(m.err)) return false; re->nl_sv = (proven_size_t *)m.value.ptr;
    re->gen = 0;
    return true;
}

prov_result_regex_t prov_regex_compile(proven_allocator_t a, proven_u8str_view_t pattern,
                                       unsigned flags) {
    prov_result_regex_t out = {0};
    prov_rx_parse_t pr = prov_rx_parse(a, pattern);
    if (!PROVEN_IS_OK(pr.err)) {
        out.err = pr.err; out.err_off = pr.err_off; out.err_msg = pr.err_msg;
        prov_rx_parse_free(a, &pr);
        return out;
    }
    proven_result_mem_mut_t rm = a.alloc_fn(a.ctx, sizeof(prov_regex_t), 16);
    if (!PROVEN_IS_OK(rm.err)) { prov_rx_parse_free(a, &pr); out.err = rm.err; out.err_msg = "out of memory"; return out; }
    prov_regex_t *re = (prov_regex_t *)rm.value.ptr;
    *re = (prov_regex_t){ .flags = flags, .ngroups = pr.ngroups };

    rxc_t c = { .re = re, .a = a };
    emit(&c, RXO_SAVE, 0, 0);
    emit_node(&c, pr.root);
    if (!re->has_ze) emit(&c, RXO_SAVE, 1, 0);
    emit(&c, RXO_MATCH, 0, 0);
    prov_rx_parse_free(a, &pr);

    if (c.oom)    { prov_regex_destroy(a, re); out.err = PROVEN_ERR_NOMEM;       out.err_msg = "out of memory";      return out; }
    if (c.toobig) { prov_regex_destroy(a, re); out.err = PROVEN_ERR_INVALID_ARG; out.err_msg = "pattern too complex"; return out; }
    if (!vm_alloc(a, re)) { prov_regex_destroy(a, re); out.err = PROVEN_ERR_NOMEM; out.err_msg = "out of memory"; return out; }
    out.re = re;
    return out;
}

void prov_regex_destroy(proven_allocator_t a, prov_regex_t *re) {
    if (!re) return;
    if (re->insts) a.free_fn(a.ctx, re->insts);
    if (re->sets)  a.free_fn(a.ctx, re->sets);
    if (re->vis)   a.free_fn(a.ctx, re->vis);
    if (re->cl_pc) a.free_fn(a.ctx, re->cl_pc);
    if (re->nl_pc) a.free_fn(a.ctx, re->nl_pc);
    if (re->cl_sv) a.free_fn(a.ctx, re->cl_sv);
    if (re->nl_sv) a.free_fn(a.ctx, re->nl_sv);
    a.free_fn(a.ctx, re);
}

proven_size_t prov_rx_prog_dump(const prov_regex_t *re, char *buf, proven_size_t cap) {
    dbuf d = { .b = buf, .cap = cap, .len = 0 };
    const char *hx = "0123456789abcdef";
    for (proven_size_t i = 0; i < re->ninsts; i++) {
        prov_rx_inst_t in = re->insts[i];
        d_uint(&d, (proven_u32)i); d_str(&d, ": ");
        switch (in.op) {
            case RXO_BYTE:
                d_str(&d, "byte ");
                if (in.x >= 0x20 && in.x < 0x7f) d_ch(&d, (char)in.x);
                else { d_str(&d, "\\x"); d_ch(&d, hx[(in.x >> 4) & 15]); d_ch(&d, hx[in.x & 15]); }
                break;
            case RXO_RANGE:
                d_str(&d, "range \\x"); d_ch(&d, hx[(in.x >> 4) & 15]); d_ch(&d, hx[in.x & 15]);
                d_ch(&d, '-'); d_str(&d, "\\x"); d_ch(&d, hx[(in.y >> 4) & 15]); d_ch(&d, hx[in.y & 15]);
                break;
            case RXO_CLASS:
                d_str(&d, "class ");
                for (int b = 0x20; b < 0x7f; b++) if (in_set(re->sets[in.x], b)) d_ch(&d, (char)b);
                break;
            case RXO_SPLIT: d_str(&d, "split "); d_uint(&d, in.x); d_ch(&d, ' '); d_uint(&d, in.y); break;
            case RXO_JMP:   d_str(&d, "jmp ");   d_uint(&d, in.x); break;
            case RXO_SAVE:  d_str(&d, "save ");  d_uint(&d, in.x); break;
            case RXO_ASSERT:
                d_str(&d, "assert ");
                d_str(&d, in.x == RX_A_BOL ? "bol" : in.x == RX_A_EOL ? "eol" :
                          in.x == RX_A_WORDB ? "wordb" : "nwordb");
                break;
            case RXO_MATCH: d_str(&d, "match"); break;
        }
        d_ch(&d, '\n');
    }
    if (cap) d.b[d.len < cap ? d.len : cap - 1] = '\0';
    return d.len;
}

/* ======================================================================== */
/* S3 — Pike VM executor (anchored match)                                    */
/* ======================================================================== */

static bool is_word(int b) {
    return b == '_' || (b >= '0' && b <= '9') || (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z');
}

static bool assert_holds(const proven_u8 *hay, proven_size_t len, proven_size_t sp,
                         proven_u32 kind, unsigned flags) {
    bool ml = (flags & PROV_RX_MULTILINE) != 0;
    switch (kind) {
        case RX_A_BOL: return sp == 0 || (ml && hay[sp - 1] == '\n');
        case RX_A_EOL: return sp == len || (ml && sp < len && hay[sp] == '\n');
        case RX_A_WORDB: {
            bool a = sp > 0 && is_word(hay[sp - 1]); bool b = sp < len && is_word(hay[sp]);
            return a != b;
        }
        case RX_A_NWORDB: {
            bool a = sp > 0 && is_word(hay[sp - 1]); bool b = sp < len && is_word(hay[sp]);
            return a == b;
        }
    }
    return false;
}

typedef struct {
    prov_regex_t    *re;
    const proven_u8 *hay;
    proven_size_t    len;
    proven_u32      *lpc;
    proven_size_t   *lsv;
    proven_size_t    ln;
} rxv_t;

/* follow epsilon transitions from `pc`, appending executable threads to the list,
 * deduped by program counter per generation (the linear-time bound) */
static void addthread(rxv_t *v, proven_u32 pc, proven_size_t *saved, proven_size_t sp) {
    prov_regex_t *re = v->re;
    if (re->vis[pc] == re->gen) return;
    re->vis[pc] = re->gen;
    prov_rx_inst_t in = re->insts[pc];
    switch (in.op) {
        case RXO_JMP:   addthread(v, in.x, saved, sp); break;
        case RXO_SPLIT: addthread(v, in.x, saved, sp); addthread(v, in.y, saved, sp); break;
        case RXO_SAVE:
            if (in.x < re->nsave) {
                proven_size_t old = saved[in.x]; saved[in.x] = sp;
                addthread(v, pc + 1, saved, sp);
                saved[in.x] = old;
            } else addthread(v, pc + 1, saved, sp);
            break;
        case RXO_ASSERT:
            if (assert_holds(v->hay, v->len, sp, in.x, re->flags)) addthread(v, pc + 1, saved, sp);
            break;
        default: {                                   /* BYTE / RANGE / CLASS / MATCH */
            proven_size_t k = v->ln++;
            v->lpc[k] = pc;
            proven_size_t base = k * re->nsave;
            for (proven_size_t j = 0; j < re->nsave; j++) v->lsv[base + j] = saved[j];
            break;
        }
    }
}

bool prov_regex_match_at(prov_regex_t *re, const proven_u8 *hay, proven_size_t len,
                         proven_size_t at, prov_regex_match_t *out) {
    if (re->gen > 0xFFFFFF00u) { for (proven_size_t i = 0; i < re->ninsts; i++) re->vis[i] = 0; re->gen = 0; }

    proven_size_t init[2 * PROV_RX_MAX_GROUPS];
    for (proven_size_t j = 0; j < re->nsave; j++) init[j] = PROV_RX_NONE;

    re->gen++;
    rxv_t seed = { re, hay, len, re->cl_pc, re->cl_sv, 0 };
    addthread(&seed, 0, init, at);
    proven_size_t cln = seed.ln;

    proven_u32 *cpc = re->cl_pc, *npc = re->nl_pc;
    proven_size_t *csv = re->cl_sv, *nsv = re->nl_sv;
    bool matched = false;
    proven_size_t msaved[2 * PROV_RX_MAX_GROUPS];

    for (proven_size_t sp = at; ; sp++) {
        if (cln == 0) break;
        int byte = sp < len ? (int)hay[sp] : -1;
        re->gen++;
        rxv_t nl = { re, hay, len, npc, nsv, 0 };
        for (proven_size_t i = 0; i < cln; i++) {
            proven_u32 pc = cpc[i];
            proven_size_t *saved = &csv[i * re->nsave];
            prov_rx_inst_t in = re->insts[pc];
            bool consume = false;
            switch (in.op) {
                case RXO_BYTE:  consume = (byte == (int)in.x); break;
                case RXO_RANGE: consume = (byte >= (int)in.x && byte <= (int)in.y); break;
                case RXO_CLASS: consume = (byte >= 0 && in_set(re->sets[in.x], byte)); break;
                case RXO_MATCH:
                    matched = true;
                    for (proven_size_t j = 0; j < re->nsave; j++) msaved[j] = saved[j];
                    goto stepdone;                   /* discard lower-priority threads */
                default: break;
            }
            if (consume) addthread(&nl, pc + 1, saved, sp + 1);
        }
    stepdone:
        { proven_u32 *tp = cpc; cpc = npc; npc = tp;
          proven_size_t *ts = csv; csv = nsv; nsv = ts; cln = nl.ln; }
        if (sp >= len) break;
    }

    if (!matched) return false;
    out->ngroups = re->ngroups;
    out->start = msaved[0]; out->end = msaved[1];
    for (int g = 1; g <= re->ngroups; g++) {
        proven_size_t s = msaved[2 * g], e = msaved[2 * g + 1];
        out->groups[g].start = s; out->groups[g].end = e;
        out->groups[g].set = (s != PROV_RX_NONE && e != PROV_RX_NONE);
    }
    return true;
}

/* ======================================================================== */
/* S4 — unanchored single-pass search (leftmost match start >= from)         */
/* ======================================================================== */

bool prov_regex_search(prov_regex_t *re, const proven_u8 *hay, proven_size_t len,
                       proven_size_t from, prov_regex_match_t *out) {
    if (from > len) return false;
    if (re->gen > 0xFFFFFF00u) { for (proven_size_t i = 0; i < re->ninsts; i++) re->vis[i] = 0; re->gen = 0; }

    proven_size_t init[2 * PROV_RX_MAX_GROUPS];
    for (proven_size_t j = 0; j < re->nsave; j++) init[j] = PROV_RX_NONE;

    proven_u32 *cpc = re->cl_pc, *npc = re->nl_pc;
    proven_size_t *csv = re->cl_sv, *nsv = re->nl_sv;
    proven_size_t cln = 0;
    proven_u32 clgen = ++re->gen;          /* gen of the (initially empty) clist */
    bool matched = false;
    proven_size_t msaved[2 * PROV_RX_MAX_GROUPS];

    for (proven_size_t sp = from; sp <= len; sp++) {
        if (!matched) {                    /* seed a fresh start at sp (lowest priority) */
            re->gen = clgen;
            rxv_t cl = { re, hay, len, cpc, csv, cln };
            addthread(&cl, 0, init, sp);
            cln = cl.ln;
        }
        /* Only stop when matched and drained. If unmatched with no live threads
         * (e.g. a leading \b/^ assertion failed here), keep scanning — a later
         * start may match. The empty-clist pass below advances the generation and
         * seeds the next sp. */
        if (cln == 0 && matched) break;
        int byte = sp < len ? (int)hay[sp] : -1;
        proven_u32 nlgen = clgen + 1;
        re->gen = nlgen;
        rxv_t nl = { re, hay, len, npc, nsv, 0 };
        for (proven_size_t i = 0; i < cln; i++) {
            proven_u32 pc = cpc[i];
            proven_size_t *saved = &csv[i * re->nsave];
            prov_rx_inst_t in = re->insts[pc];
            bool consume = false;
            switch (in.op) {
                case RXO_BYTE:  consume = (byte == (int)in.x); break;
                case RXO_RANGE: consume = (byte >= (int)in.x && byte <= (int)in.y); break;
                case RXO_CLASS: consume = (byte >= 0 && in_set(re->sets[in.x], byte)); break;
                case RXO_MATCH:
                    matched = true;
                    for (proven_size_t j = 0; j < re->nsave; j++) msaved[j] = saved[j];
                    goto stepdone;          /* discard lower-priority (later-start) threads */
                default: break;
            }
            if (consume) addthread(&nl, pc + 1, saved, sp + 1);
        }
    stepdone:
        { proven_u32 *tp = cpc; cpc = npc; npc = tp;
          proven_size_t *ts = csv; csv = nsv; nsv = ts; cln = nl.ln; clgen = nlgen; }
    }

    if (!matched) return false;
    out->ngroups = re->ngroups;
    out->start = msaved[0]; out->end = msaved[1];
    for (int g = 1; g <= re->ngroups; g++) {
        proven_size_t s = msaved[2 * g], e = msaved[2 * g + 1];
        out->groups[g].start = s; out->groups[g].end = e;
        out->groups[g].set = (s != PROV_RX_NONE && e != PROV_RX_NONE);
    }
    return true;
}
