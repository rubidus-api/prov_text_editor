/* RFC-0009 S3b — differential fuzz: compare the Pike VM (anchored match at 0)
 * against an INDEPENDENT recursive backtracking reference matcher with the same
 * leftmost-greedy semantics. POSIX libc regcomp is the wrong oracle (it is
 * leftmost-longest), so the oracle is this small backtracking matcher. It walks
 * the same AST but uses a completely different algorithm, so it catches
 * submatch/priority bugs in the VM (the subtle part). Span (group 0) only;
 * captures are covered by the golden table in test_regex.c. ASCII inputs. */

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"
#include "regex.h"

/* ---- deterministic RNG ---- */
static unsigned long long rs = 0x9e3779b97f4a7c15ULL;
static unsigned rng(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static int rr(int n) { return (int)(rng() % (unsigned)n); }

/* ---- pattern generator (safe subset: no anchors/markers; quantifiers only
 * over consuming atoms so the backtracking reference cannot loop) ---- */
static void gen_alt(char *b, int *p, int cap, int depth);

static void put(char *b, int *p, int cap, char c) { if (*p < cap - 1) b[(*p)++] = c; }

static void gen_atom(char *b, int *p, int cap, int depth) {
    int k = rr(depth > 0 ? 5 : 4);
    if (k == 0 || k == 1) { put(b, p, cap, (char)('a' + rr(3))); }
    else if (k == 2) { put(b, p, cap, '.'); }
    else if (k == 3) {
        put(b, p, cap, '[');
        if (rr(2)) put(b, p, cap, '^');
        int m = 1 + rr(2);
        for (int i = 0; i < m; i++) put(b, p, cap, (char)('a' + rr(3)));
        put(b, p, cap, ']');
    } else { /* group — never quantified (keeps repeat bodies consuming) */
        put(b, p, cap, '('); gen_alt(b, p, cap, depth - 1); put(b, p, cap, ')');
    }
}

static void gen_factor(char *b, int *p, int cap, int depth) {
    if (rr(7) == 6) {                            /* ~1/7: a bare anchor (no quantifier) */
        int k = rr(4);
        if (k == 0) put(b, p, cap, '^');
        else if (k == 1) put(b, p, cap, '$');
        else { put(b, p, cap, '\\'); put(b, p, cap, k == 2 ? 'b' : 'B'); }
        return;
    }
    int before = *p;
    bool is_group = (depth > 0) && (rr(5) == 4);
    if (is_group) { put(b, p, cap, '('); gen_alt(b, p, cap, depth - 1); put(b, p, cap, ')'); }
    else gen_atom(b, p, cap, 0);                 /* atoms here never recurse into groups */
    (void)before;
    if (!is_group) {
        int q = rr(6);
        if (q == 0) put(b, p, cap, '*');
        else if (q == 1) put(b, p, cap, '+');
        else if (q == 2) put(b, p, cap, '?');
        else if (q == 3) {
            int lo = rr(3), hi = lo + rr(3);
            char t[16]; int tn = snprintf(t, sizeof t, "{%d,%d}", lo, hi);
            for (int i = 0; i < tn; i++) put(b, p, cap, t[i]);
        }
        /* q==4,5: no quantifier */
        if (q <= 3 && rr(3) == 0) put(b, p, cap, '?');   /* lazy */
    }
}

static void gen_term(char *b, int *p, int cap, int depth) {
    int n = 1 + rr(3);
    for (int i = 0; i < n; i++) gen_factor(b, p, cap, depth);
}

static void gen_alt(char *b, int *p, int cap, int depth) {
    int n = 1 + rr(3);
    for (int i = 0; i < n; i++) { if (i) put(b, p, cap, '|'); gen_term(b, p, cap, depth); }
}

/* ---- backtracking reference (span end only, with a step budget) ---- */
typedef struct cont {
    int tag;                                       /* 0=done, 1=seq, 2=rep */
    const prov_rx_node_t *n; struct cont *next;    /* seq */
    const prov_rx_node_t *body; unsigned lo, hi; bool lazy; unsigned count; struct cont *after; /* rep */
} cont;

static long g_budget;
static bool g_gaveup;
static bool bt(const prov_rx_node_t *n, const unsigned char *h, size_t len, size_t pos, cont *k, size_t *end);

static bool bt_cont(cont *k, const unsigned char *h, size_t len, size_t pos, size_t *end);
static bool bt_rep(const prov_rx_node_t *body, unsigned lo, unsigned hi, bool lazy, unsigned count,
                   const unsigned char *h, size_t len, size_t pos, cont *after, size_t *end) {
    if (g_gaveup) return false;
    bool more = count < hi, stop = count >= lo;
    cont rc = { .tag = 2, .body = body, .lo = lo, .hi = hi, .lazy = lazy, .count = count + 1, .after = after };
    if (lazy) {
        if (stop && bt_cont(after, h, len, pos, end)) return true;
        if (more && bt(body, h, len, pos, &rc, end)) return true;
    } else {
        if (more && bt(body, h, len, pos, &rc, end)) return true;
        if (stop && bt_cont(after, h, len, pos, end)) return true;
    }
    return false;
}
static bool bt_cont(cont *k, const unsigned char *h, size_t len, size_t pos, size_t *end) {
    if (--g_budget <= 0) { g_gaveup = true; return false; }
    if (k->tag == 0) { *end = pos; return true; }
    if (k->tag == 1) return bt(k->n, h, len, pos, k->next, end);
    return bt_rep(k->body, k->lo, k->hi, k->lazy, k->count, h, len, pos, k->after, end);
}

static bool bt(const prov_rx_node_t *n, const unsigned char *h, size_t len, size_t pos, cont *k, size_t *end) {
    if (g_gaveup) return false;
    if (--g_budget <= 0) { g_gaveup = true; return false; }
    switch (n->kind) {
        case RX_EMPTY: return bt_cont(k, h, len, pos, end);
        case RX_LIT:   return (pos < len && h[pos] == n->byte) ? bt_cont(k, h, len, pos + 1, end) : false;
        case RX_ANY:   return (pos < len && h[pos] != '\n') ? bt_cont(k, h, len, pos + 1, end) : false;
        case RX_CLASS: {
            if (pos >= len) return false;
            int b = h[pos]; bool inb = (n->set[b >> 3] >> (b & 7)) & 1;
            return (inb != n->negated) ? bt_cont(k, h, len, pos + 1, end) : false;
        }
        case RX_CONCAT: {
            cont chain[40];
            if (n->nkids > 40) { g_gaveup = true; return false; }
            cont *after = k;
            for (size_t i = n->nkids; i-- > 1; ) { chain[i] = (cont){ .tag = 1, .n = n->kids[i], .next = after }; after = &chain[i]; }
            return bt(n->kids[0], h, len, pos, after, end);
        }
        case RX_ALT:
            for (size_t i = 0; i < n->nkids; i++) if (bt(n->kids[i], h, len, pos, k, end)) return true;
            return false;
        case RX_REPEAT: {
            unsigned hi = (n->rmax == RX_INF) ? (unsigned)(len + 1) : n->rmax;
            return bt_rep(n->kids[0], n->rmin, hi, n->lazy, 0, h, len, pos, k, end);
        }
        case RX_GROUP:  return bt(n->kids[0], h, len, pos, k, end);
        case RX_ANCHOR: {                            /* non-MULTILINE (fuzz compiles flags=0) */
            bool a = pos > 0 && (h[pos - 1] == '_' || (h[pos - 1] >= '0' && h[pos - 1] <= '9') ||
                     (h[pos - 1] >= 'A' && h[pos - 1] <= 'Z') || (h[pos - 1] >= 'a' && h[pos - 1] <= 'z'));
            bool bb = pos < len && (h[pos] == '_' || (h[pos] >= '0' && h[pos] <= '9') ||
                      (h[pos] >= 'A' && h[pos] <= 'Z') || (h[pos] >= 'a' && h[pos] <= 'z'));
            bool ok = n->anchor == RX_A_BOL ? pos == 0 : n->anchor == RX_A_EOL ? pos == len :
                      n->anchor == RX_A_WORDB ? (a != bb) : (a == bb);
            return ok ? bt_cont(k, h, len, pos, end) : false;
        }
        case RX_MARK: return bt_cont(k, h, len, pos, end);  /* not generated */
    }
    return false;
}

int main(void) {
    proven_allocator_t A = proven_heap_allocator();
    int fails = 0, compared = 0, gaveups = 0;
    const int N = 60000;
    for (int it = 0; it < N; it++) {
        char pat[128]; int pp = 0;
        gen_alt(pat, &pp, (int)sizeof pat, 2);
        pat[pp] = '\0';
        char inp[8]; int il = rr(7);
        for (int i = 0; i < il; i++) inp[i] = (char)('a' + rr(3));
        inp[il] = '\0';

        proven_u8str_view_t pv = { .ptr = (const proven_byte_t *)pat, .size = (proven_size_t)pp };
        prov_result_regex_t r = prov_regex_compile(A, pv, 0);
        if (!PROVEN_IS_OK(r.err)) continue;                 /* size cap etc.: skip */
        prov_regex_match_t mt;
        bool vm = prov_regex_match_at(r.re, (const proven_u8 *)inp, (proven_size_t)il, 0, &mt);
        prov_regex_destroy(A, r.re);

        prov_rx_parse_t pr = prov_rx_parse(A, pv);
        if (!PROVEN_IS_OK(pr.err)) { prov_rx_parse_free(A, &pr); continue; }
        g_budget = 3000000; g_gaveup = false;
        cont done = { .tag = 0 };
        size_t ref_end = 0;
        bool ref = bt(pr.root, (const unsigned char *)inp, (size_t)il, 0, &done, &ref_end);
        prov_rx_parse_free(A, &pr);
        if (g_gaveup) { gaveups++; continue; }

        compared++;
        if (vm != ref || (vm && (size_t)mt.end != ref_end)) {
            if (fails < 25)
                printf("MISMATCH pat=[%s] inp=[%s]  vm=%s/%zu  ref=%s/%zu\n",
                       pat, inp, vm ? "y" : "n", (size_t)mt.end, ref ? "y" : "n", ref_end);
            fails++;
        }
    }
    /* ---- S4: single-pass search vs naive (first pos where match_at succeeds);
     * match_at is already validated above, so it is a sound search oracle. ---- */
    int scompared = 0, sfails = 0;
    for (int it = 0; it < N; it++) {
        char pat[128]; int pp = 0; gen_alt(pat, &pp, (int)sizeof pat, 2); pat[pp] = '\0';
        char inp[10]; int il = rr(9);
        for (int i = 0; i < il; i++) inp[i] = (char)('a' + rr(3));
        inp[il] = '\0';
        proven_u8str_view_t pv = { .ptr = (const proven_byte_t *)pat, .size = (proven_size_t)pp };
        prov_result_regex_t r = prov_regex_compile(A, pv, 0);
        if (!PROVEN_IS_OK(r.err)) continue;

        prov_regex_match_t sm;
        bool sok = prov_regex_search(r.re, (const proven_u8 *)inp, (proven_size_t)il, 0, &sm);

        /* naive: leftmost position where the anchored match succeeds */
        bool nok = false; prov_regex_match_t nm; memset(&nm, 0, sizeof nm);
        for (proven_size_t at = 0; at <= (proven_size_t)il; at++)
            if (prov_regex_match_at(r.re, (const proven_u8 *)inp, (proven_size_t)il, at, &nm)) { nok = true; break; }
        prov_regex_destroy(A, r.re);

        scompared++;
        if (sok != nok || (sok && (sm.start != nm.start || sm.end != nm.end))) {
            if (sfails < 25)
                printf("SEARCH MISMATCH pat=[%s] inp=[%s] search=%s/%zu-%zu naive=%s/%zu-%zu\n",
                       pat, inp, sok ? "y" : "n", (size_t)sm.start, (size_t)sm.end,
                       nok ? "y" : "n", (size_t)nm.start, (size_t)nm.end);
            sfails++;
        }
    }

    printf("test_regex_fuzz: match %d cmp/%d gaveup/%d miss; search %d cmp/%d miss\n",
           compared, gaveups, fails, scompared, sfails);
    if (fails || sfails) return 1;
    printf("test_regex_fuzz: OK\n");
    return 0;
}
