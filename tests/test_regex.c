/* RFC-0009 S1 — regex parser → AST tests. Asserts the s-expression dump of the
 * parsed AST for valid patterns, and that malformed patterns are rejected. */

#include <stdio.h>
#include <string.h>

#include "proven/heap.h"
#include "regex.h"

static int g_fail;
static proven_allocator_t A;

static proven_u8str_view_t V(const char *s) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

static void ast(const char *pat, const char *want) {
    prov_rx_parse_t p = prov_rx_parse(A, V(pat));
    if (!PROVEN_IS_OK(p.err)) {
        printf("FAIL parse [%s]: %s (off %zu)\n", pat, p.err_msg ? p.err_msg : "?", (size_t)p.err_off);
        g_fail++;
    } else {
        char buf[2048];
        prov_rx_ast_dump(p.root, buf, sizeof buf);
        if (strcmp(buf, want) != 0) {
            printf("FAIL ast [%s]: got %s  want %s\n", pat, buf, want);
            g_fail++;
        }
    }
    prov_rx_parse_free(A, &p);
}

static void bad(const char *pat) {
    prov_rx_parse_t p = prov_rx_parse(A, V(pat));
    if (PROVEN_IS_OK(p.err)) { printf("FAIL [%s] should be rejected\n", pat); g_fail++; }
    prov_rx_parse_free(A, &p);
}

static void prog(const char *pat, unsigned flags, const char *want) {
    prov_result_regex_t r = prov_regex_compile(A, V(pat), flags);
    if (!PROVEN_IS_OK(r.err)) { printf("FAIL compile [%s]: %s\n", pat, r.err_msg ? r.err_msg : "?"); g_fail++; return; }
    char buf[8192];
    prov_rx_prog_dump(r.re, buf, sizeof buf);
    if (strcmp(buf, want) != 0) { printf("FAIL prog [%s]:\n--got--\n%s--want--\n%s\n", pat, buf, want); g_fail++; }
    prov_regex_destroy(A, r.re);
}

static void ok_compile(const char *pat) {
    prov_result_regex_t r = prov_regex_compile(A, V(pat), 0);
    if (!PROVEN_IS_OK(r.err)) { printf("FAIL [%s] should compile: %s\n", pat, r.err_msg ? r.err_msg : "?"); g_fail++; }
    else prov_regex_destroy(A, r.re);
}

static void bad_compile(const char *pat) {
    prov_result_regex_t r = prov_regex_compile(A, V(pat), 0);
    if (PROVEN_IS_OK(r.err)) { printf("FAIL [%s] should not compile\n", pat); g_fail++; prov_regex_destroy(A, r.re); }
}

/* anchored match at 0; want = "no" or "S-E" + per-group " g:S-E" / " g:-" */
static void mf(const char *pat, const char *input, unsigned flags, const char *want) {
    prov_result_regex_t r = prov_regex_compile(A, V(pat), flags);
    if (!PROVEN_IS_OK(r.err)) { printf("FAIL compile [%s]: %s\n", pat, r.err_msg ? r.err_msg : "?"); g_fail++; return; }
    prov_regex_match_t mt;
    bool ok = prov_regex_match_at(r.re, (const proven_u8 *)input, strlen(input), 0, &mt);
    char buf[256]; int p = 0;
    if (!ok) p += snprintf(buf, sizeof buf, "no");
    else {
        p += snprintf(buf + p, sizeof buf - (size_t)p, "%zu-%zu", (size_t)mt.start, (size_t)mt.end);
        for (int g = 1; g <= mt.ngroups; g++) {
            if (mt.groups[g].set)
                p += snprintf(buf + p, sizeof buf - (size_t)p, " %d:%zu-%zu", g, (size_t)mt.groups[g].start, (size_t)mt.groups[g].end);
            else
                p += snprintf(buf + p, sizeof buf - (size_t)p, " %d:-", g);
        }
    }
    if (strcmp(buf, want) != 0) { printf("FAIL match [%s] on [%s]: got '%s' want '%s'\n", pat, input, buf, want); g_fail++; }
    prov_regex_destroy(A, r.re);
}
static void m(const char *pat, const char *input, const char *want) { mf(pat, input, 0, want); }

/* unanchored search from 0; same `want` format as mf */
static void s(const char *pat, const char *input, const char *want) {
    prov_result_regex_t r = prov_regex_compile(A, V(pat), 0);
    if (!PROVEN_IS_OK(r.err)) { printf("FAIL compile [%s]: %s\n", pat, r.err_msg ? r.err_msg : "?"); g_fail++; return; }
    prov_regex_match_t mt;
    bool ok = prov_regex_search(r.re, (const proven_u8 *)input, strlen(input), 0, &mt);
    char buf[256]; int p = 0;
    if (!ok) p += snprintf(buf, sizeof buf, "no");
    else {
        p += snprintf(buf + p, sizeof buf - (size_t)p, "%zu-%zu", (size_t)mt.start, (size_t)mt.end);
        for (int g = 1; g <= mt.ngroups; g++)
            p += mt.groups[g].set ? snprintf(buf + p, sizeof buf - (size_t)p, " %d:%zu-%zu", g, (size_t)mt.groups[g].start, (size_t)mt.groups[g].end)
                                  : snprintf(buf + p, sizeof buf - (size_t)p, " %d:-", g);
    }
    if (strcmp(buf, want) != 0) { printf("FAIL search [%s] on [%s]: got '%s' want '%s'\n", pat, input, buf, want); g_fail++; }
    prov_regex_destroy(A, r.re);
}

/* Collect all leftmost match starts of `re` over `[hay, hay+len)`, advancing past
 * zero-width matches exactly as both the whole-doc search and display.c's
 * per-line highlight loops do. */
static int collect_starts(prov_regex_t *re, const proven_u8 *hay, proven_size_t len,
                          proven_size_t base, proven_size_t *out, int cap, int n) {
    for (proven_size_t off = 0; off <= len && n < cap; ) {
        prov_regex_match_t mm;
        if (!prov_regex_search(re, hay, len, off, &mm)) break;
        out[n++] = base + mm.start;
        off = mm.end > mm.start ? mm.end : mm.start + 1;
    }
    return n;
}

/* The renderer highlights matches per visible line (display.c) while incremental
 * search runs over the whole document (main.c). Both compile with MULTILINE so
 * ^/$ mean line bounds; this asserts the two agree on every match start for an
 * anchored pattern (the consistency the highlight relies on). The per-line slice
 * mirrors the renderer's `line_end_of` (display.c): each line spans [start, '\n')
 * and excludes the trailing newline. */
static void anchor_consistent(const char *pat, const char *input) {
    prov_result_regex_t r = prov_regex_compile(A, V(pat), PROV_RX_MULTILINE);
    if (!PROVEN_IS_OK(r.err)) { printf("FAIL compile [%s]\n", pat); g_fail++; return; }
    const proven_u8 *hay = (const proven_u8 *)input;
    proven_size_t len = strlen(input);

    proven_size_t whole[64];   int nw = collect_starts(r.re, hay, len, 0, whole, 64, 0);
    proven_size_t perline[64]; int np = 0;
    for (proven_size_t lpos = 0; lpos <= len; ) {
        proven_size_t lend = lpos;
        while (lend < len && hay[lend] != '\n') lend++;
        np = collect_starts(r.re, hay + lpos, lend - lpos, lpos, perline, 64, np);   /* [start, '\n') */
        if (lend >= len) break;
        lpos = lend + 1;
    }

    bool eq = (nw == np);
    for (int i = 0; eq && i < nw; i++) if (whole[i] != perline[i]) eq = false;
    if (!eq) {
        printf("FAIL anchor-consistency [%s]: whole={", pat);
        for (int i = 0; i < nw; i++) printf("%zu ", (size_t)whole[i]);
        printf("} per-line={");
        for (int i = 0; i < np; i++) printf("%zu ", (size_t)perline[i]);
        printf("}\n");
        g_fail++;
    }
    prov_regex_destroy(A, r.re);
}

int main(void) {
    A = proven_heap_allocator();

    /* literals, concat, alternation */
    ast("", "eps");
    ast("a", "(lit a)");
    ast("ab", "(cat (lit a) (lit b))");
    ast("a|b", "(alt (lit a) (lit b))");
    ast("a|", "(alt (lit a) eps)");
    ast("ab|cd", "(alt (cat (lit a) (lit b)) (cat (lit c) (lit d)))");

    /* quantifiers */
    ast("a*", "(rep 0 inf (lit a))");
    ast("a+", "(rep 1 inf (lit a))");
    ast("a?", "(rep 0 1 (lit a))");
    ast("a*?", "(rep? 0 inf (lit a))");
    ast("a{3}", "(rep 3 3 (lit a))");
    ast("a{2,5}", "(rep 2 5 (lit a))");
    ast("a{2,}", "(rep 2 inf (lit a))");
    ast("(ab)+", "(rep 1 inf (grp 1 (cat (lit a) (lit b))))");

    /* dot, classes, predefined */
    ast(".", "any");
    ast("[abc]", "(cls:abc)");
    ast("[a-c]", "(cls:abc)");
    ast("[^a]", "(ncls:a)");
    ast("[a-cx]", "(cls:abcx)");
    ast("\\d", "(cls:0123456789)");
    ast("[\\d_]", "(cls:0123456789_)");

    /* groups */
    ast("(ab)", "(grp 1 (cat (lit a) (lit b)))");
    ast("(?:ab)", "(ncg (cat (lit a) (lit b)))");
    ast("(a)(b)", "(cat (grp 1 (lit a)) (grp 2 (lit b)))");

    /* anchors, markers */
    ast("^a$", "(cat bol (lit a) eol)");
    ast("\\bx\\B", "(cat wordb (lit x) nwordb)");
    ast("foo\\zsbar", "(cat (lit f) (lit o) (lit o) zs (lit b) (lit a) (lit r))");
    ast("a\\zeb", "(cat (lit a) ze (lit b))");

    /* escaped metacharacters + control escapes */
    ast("a\\.b", "(cat (lit a) (lit .) (lit b))");
    ast("\\(", "(lit ()");
    ast("a\\tb", "(cat (lit a) (lit \\x09) (lit b))");

    /* multibyte UTF-8 literal = a concat of its bytes (so a quantifier repeats
     * the whole codepoint).  "가" = EA B0 80 */
    ast("\xea\xb0\x80", "(cat (lit \\xea) (lit \\xb0) (lit \\x80))");
    ast("\xea\xb0\x80*", "(rep 0 inf (cat (lit \\xea) (lit \\xb0) (lit \\x80)))");

    /* `{` that is not a valid quantifier is a literal */
    ast("a{", "(cat (lit a) (lit {))");
    ast("a{x}", "(cat (lit a) (lit {) (lit x) (lit }))");

    /* malformed patterns are rejected */
    bad("(a");          /* unmatched ( */
    bad("a)");          /* unmatched ) */
    bad("[a");          /* unterminated class */
    bad("*a");          /* nothing to repeat */
    ast("a**", "(rep 0 inf (rep 0 inf (lit a)))");  /* stacked quantifiers are NFA-safe */
    bad("\\1");         /* backreference */
    bad("(?=a)");       /* lookahead unsupported */
    bad("a\\");         /* trailing backslash */
    bad("[z-a]");       /* reversed range */
    bad("a{5,2}");      /* reversed repeat */

    /* ---- S2: compiler → bytecode ---- */
    prog("a", 0, "0: save 0\n1: byte a\n2: save 1\n3: match\n");
    prog("ab", 0, "0: save 0\n1: byte a\n2: byte b\n3: save 1\n4: match\n");
    prog("a|b", 0, "0: save 0\n1: split 2 4\n2: byte a\n3: jmp 5\n4: byte b\n5: save 1\n6: match\n");
    prog("a*", 0, "0: save 0\n1: split 2 4\n2: byte a\n3: jmp 1\n4: save 1\n5: match\n");
    prog("a+", 0, "0: save 0\n1: byte a\n2: split 1 3\n3: save 1\n4: match\n");
    prog("a?", 0, "0: save 0\n1: split 2 3\n2: byte a\n3: save 1\n4: match\n");
    prog("a*?", 0, "0: save 0\n1: split 4 2\n2: byte a\n3: jmp 1\n4: save 1\n5: match\n");
    prog("(a)", 0, "0: save 0\n1: save 2\n2: byte a\n3: save 3\n4: save 1\n5: match\n");
    prog("[ab]", 0, "0: save 0\n1: class ab\n2: save 1\n3: match\n");
    prog("a{2,3}", 0, "0: save 0\n1: byte a\n2: byte a\n3: split 4 5\n4: byte a\n5: save 1\n6: match\n");
    prog("a{2}", 0, "0: save 0\n1: byte a\n2: byte a\n3: save 1\n4: match\n");
    prog("foo\\zsbar", 0,
         "0: save 0\n1: byte f\n2: byte o\n3: byte o\n4: save 0\n5: byte b\n6: byte a\n7: byte r\n8: save 1\n9: match\n");
    prog("a\\zeb", 0, "0: save 0\n1: byte a\n2: save 1\n3: byte b\n4: match\n");  /* no implicit save 1 */
    prog("^a$", 0, "0: save 0\n1: assert bol\n2: byte a\n3: assert eol\n4: save 1\n5: match\n");
    prog("a", PROV_RX_ICASE, "0: save 0\n1: class Aa\n2: save 1\n3: match\n");

    /* `.` and negated classes compile to a UTF-8 codepoint sub-automaton */
    ok_compile(".");
    ok_compile("[^a]");
    ok_compile("\xea\xb0\x80+");              /* multibyte literal + quantifier */
    ok_compile("(a|b)*c{1,4}\\d");

    /* program-size cap rejects an explosive expansion */
    bad_compile("(a{1000}){1000}");

    /* ---- S3: Pike VM execution (anchored at 0) ---- */
    m("abc", "abc", "0-3");
    m("abc", "abx", "no");
    m("a*", "aaa", "0-3");
    m("a*", "bbb", "0-0");                 /* empty match */
    m("a+", "aaa", "0-3");
    m("a+", "bbb", "no");
    m("a+?", "aaa", "0-1");                /* lazy: minimal */
    /* leftmost-greedy (NOT POSIX leftmost-longest): first alternative wins */
    m("a|ab", "ab", "0-1");
    m("ab|a", "ab", "0-2");
    m("a{2,3}", "aaaa", "0-3");            /* greedy max */
    m("a{2,3}?", "aaaa", "0-2");           /* lazy min */
    m("a{2,3}", "a", "no");
    /* captures */
    m("(a)(b)", "ab", "0-2 1:0-1 2:1-2");
    m("(a+)(b+)", "aabb", "0-4 1:0-2 2:2-4");
    m("a(b|c)d", "acd", "0-3 1:1-2");
    m("(a*)a", "aaa", "0-3 1:0-2");        /* greedy a* yields to the final a */
    m("(ab)+", "ababab", "0-6 1:4-6");     /* last iteration's capture */
    m("(x)?y", "y", "0-1 1:-");            /* optional group unset */
    /* dot, classes, UTF-8 */
    m(".", "x", "0-1");
    m(".", "\n", "no");
    m(".", "\xea\xb0\x80", "0-3");         /* . matches one codepoint (3 bytes) */
    m("\xea\xb0\x80", "\xea\xb0\x80", "0-3");
    m("[^a]", "b", "0-1");
    m("[^a]", "a", "no");
    m("[^a]", "\xea\xb0\x80", "0-3");      /* negated class matches a non-ASCII codepoint */
    m("\\d+", "123x", "0-3");
    /* anchors */
    m("^a", "a", "0-1");
    mf("a$", "a", 0, "0-1");
    m("\\bfoo", "foo", "0-3");
    m("\\bfoo", "xfoo", "no");             /* no boundary between x and f */
    /* \zs / \ze move the reported span */
    m("foo\\zsbar", "foobar", "3-6");
    m("foo\\zebar", "foobar", "0-3");
    /* case-insensitive */
    mf("abc", "ABC", PROV_RX_ICASE, "0-3");
    mf("[a-c]+", "aBc", PROV_RX_ICASE, "0-3");

    /* ---- S4: unanchored search (leftmost match >= 0) ---- */
    s("a", "bba", "2-3");
    s("a", "xyz", "no");
    s("ab", "xaby", "1-3");
    s("a|ab", "xab", "1-2");               /* leftmost start, first-alt greedy */
    s("a*", "xaa", "0-0");                 /* leftmost: empty at 0 */
    s("a+", "xaa", "1-3");                 /* leftmost non-empty */
    s("(\\d+)", "abc123", "3-6 1:3-6");
    s(".", "\n\na", "2-3");                /* . skips the newlines */
    s("foo\\zsbar", "xfoobar", "4-7");     /* search finds foobar@1, \zs reports bar */
    /* regression: search must scan past positions where a leading \b fails */
    s("\\bcat\\b", "cat catalog cat", "0-3");
    s("\\bcat\\b", " catalog cat", "9-12");
    s("\\bcat\\b", "catalog", "no");

    /* ---- ^/$ consistency: whole-doc MULTILINE search == per-line highlight ---- */
    anchor_consistent("^a", "abc\nabd\nxbc\n");
    anchor_consistent("c$", "abc\nabd\nxbc\n");
    anchor_consistent("^a", "abc\nabd\nxbc");        /* no trailing newline */
    anchor_consistent("^abc$", "abc\nabcd\nabc");
    anchor_consistent("^$", "a\n\nb\n");             /* empty lines */
    anchor_consistent("x$", "x\nxx\n\nx");
    anchor_consistent("\\d+$", "a1\nbb\n22\n");
    anchor_consistent("^\\w+", "foo\n  bar\nbaz");   /* unanchored-ish, line-leading word */

    if (g_fail) { printf("test_regex: %d FAILED\n", g_fail); return 1; }
    printf("test_regex: OK\n");
    return 0;
}
