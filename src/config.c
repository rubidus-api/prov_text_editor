#include "config.h"

#include "pstr.h"
#include "proven/u8str.h"
#include "proven/scan.h"
#include "proven/memory.h"

/*
 * TOML-subset config parser (SPEC §17). Built on proven's string system
 * (RFC-0004 / Special Milestone S): each line and token is a read-only
 * `proven_u8str_view_t`, keys/sections are compared with
 * `proven_u8str_view_eq` against `PROVEN_LIT` (no fixed copy buffers),
 * integers are read with `proven_scan_i64`, and an escaped string value is
 * decoded into a transient owning `proven_u8str_t`.
 */

prov_config_t prov_config_default(void) {
    prov_config_t c = {0};
    c.tabstop = 4;
    c.trigger[0] = 'z'; c.trigger[1] = 'x'; c.trigger[2] = '\0';
    c.scrolloff = 0;
    c.expandtab = false;
    c.undo_limit = 1000;
    c.line_numbers = PROV_LINENUM_OFF;
    c.wrap = PROV_WRAP_CHAR;
    c.charset_backend[0] = '\0';        /* "" = auto */
    c.charset_iconv_path[0] = '\0';     /* "" = bare "iconv" (PATH-resolved) */
    c.fallback_encoding[0] = '\0';      /* "" = built-in Windows-1252 */
    c.search_ignorecase = false;   /* defaults mirror the built-in behavior */
    c.search_highlight = true;
    c.search_wrapscan = true;
    c.clipboard_sync = true;
    c.mouse = true;
    return c;
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static bool is_key_ch(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static char at(proven_u8str_view_t v, proven_size_t i) { return (char)v.ptr[i]; }
static proven_u8str_view_t sub(proven_u8str_view_t v, proven_size_t off, proven_size_t len) {
    return proven_u8str_view_slice(v, off, len);
}

/* value kinds */
enum { V_STR = 0, V_INT = 1, V_BOOL = 2 };

static bool keyeq(proven_u8str_view_t key, const char *lit) {
    return proven_u8str_view_eq(key, proven_u8str_view_from_cstr(lit));
}

static void apply(prov_config_t *c, proven_u8str_view_t sec, proven_u8str_view_t key,
                  int vt, proven_u8str_view_t sv, long iv, bool bv) {
    if (proven_u8str_view_eq(sec, PROVEN_LIT("editor"))) {
        if (keyeq(key, "tabstop") && vt == V_INT && iv >= 1 && iv <= 64) {
            c->tabstop = (proven_u32)iv;
        } else if (keyeq(key, "trigger") && vt == V_STR) {
            prov_cstr_set(c->trigger, sizeof c->trigger, sv);   /* bounded, NUL-terminated */
        } else if (keyeq(key, "scrolloff") && vt == V_INT && iv >= 0) {
            c->scrolloff = (proven_u32)iv;
        } else if (keyeq(key, "expandtab") && vt == V_BOOL) {
            c->expandtab = bv;
        } else if (keyeq(key, "undo_limit") && vt == V_INT && iv >= 0) {
            c->undo_limit = (proven_u32)iv;
        } else if (keyeq(key, "line_numbers") && vt == V_STR) {
            if      (proven_u8str_view_eq(sv, PROVEN_LIT("absolute"))) c->line_numbers = PROV_LINENUM_ABSOLUTE;
            else if (proven_u8str_view_eq(sv, PROVEN_LIT("relative"))) c->line_numbers = PROV_LINENUM_RELATIVE;
            else                                                       c->line_numbers = PROV_LINENUM_OFF;
        } else if (keyeq(key, "wrap") && vt == V_STR) {
            if      (proven_u8str_view_eq(sv, PROVEN_LIT("off")))  c->wrap = PROV_WRAP_OFF;
            else if (proven_u8str_view_eq(sv, PROVEN_LIT("word"))) c->wrap = PROV_WRAP_WORD;
            else                                                   c->wrap = PROV_WRAP_CHAR;
        } else if (keyeq(key, "wrap") && vt == V_BOOL) {
            c->wrap = bv ? PROV_WRAP_CHAR : PROV_WRAP_OFF;   /* true = soft-wrap, false = h-scroll */
        } else if (keyeq(key, "charset_backend") && vt == V_STR) {
            prov_cstr_set(c->charset_backend, sizeof c->charset_backend, sv);
        } else if (keyeq(key, "charset_iconv_path") && vt == V_STR) {
            prov_cstr_set(c->charset_iconv_path, sizeof c->charset_iconv_path, sv);
        } else if (keyeq(key, "fallback_encoding") && vt == V_STR) {
            prov_cstr_set(c->fallback_encoding, sizeof c->fallback_encoding, sv);
        } else if (keyeq(key, "mouse") && vt == V_BOOL) {
            c->mouse = bv;
        }
    } else if (proven_u8str_view_eq(sec, PROVEN_LIT("search"))) {
        if (keyeq(key, "ignorecase") && vt == V_BOOL)      c->search_ignorecase = bv;
        else if (keyeq(key, "highlight") && vt == V_BOOL)  c->search_highlight = bv;
        else if (keyeq(key, "wrapscan") && vt == V_BOOL)   c->search_wrapscan = bv;
    } else if (proven_u8str_view_eq(sec, PROVEN_LIT("clipboard"))) {
        if (keyeq(key, "sync") && vt == V_BOOL) c->clipboard_sync = bv;
    }
    /* unrecognized sections/keys (theme, syntax, shiftwidth, …) are tolerated */
}

prov_config_result_t prov_config_parse(prov_config_t *cfg, proven_allocator_t alloc,
                                       const char *text, proven_size_t len) {
    prov_config_result_t r = { true, 0, NULL };
    proven_u8str_view_t section = PROVEN_LIT("");
    proven_byte_t section_store[32];          /* backs `section` across lines */
    proven_size_t i = 0, line = 0;

    while (i < len) {
        line++;
        proven_size_t ls = i;
        while (i < len && text[i] != '\n') i++;
        proven_size_t le = i;
        if (i < len) i++;                              /* skip '\n' */

        /* the line content as a view, trailing '\r' stripped */
        proven_size_t n = le - ls;
        while (n > 0 && text[ls + n - 1] == '\r') n--;
        proven_u8str_view_t s = { (const proven_byte_t *)(text + ls), n };

        proven_size_t a = 0;
        while (a < n && is_space(at(s, a))) a++;
        if (a >= n || at(s, a) == '#') continue;       /* blank / comment */

        if (at(s, a) == '[') {                         /* [section] */
            a++;
            proven_size_t b = a;
            while (b < n && at(s, b) != ']') b++;
            if (b >= n) { r.ok = false; r.line = line; r.message = "missing ']' in table header"; return r; }
            proven_size_t sl = b - a;
            if (sl > sizeof section_store) sl = sizeof section_store;
            proven_mem_copy(section_store, sizeof section_store, (proven_mem_view_t){ s.ptr + a, sl });
            section = (proven_u8str_view_t){ section_store, sl };
            continue;
        }

        /* key = value */
        proven_size_t k0 = a;
        while (a < n && is_key_ch(at(s, a))) a++;
        if (a == k0) { r.ok = false; r.line = line; r.message = "expected a key"; return r; }
        proven_u8str_view_t key = sub(s, k0, a - k0);

        while (a < n && is_space(at(s, a))) a++;
        if (a >= n || at(s, a) != '=') { r.ok = false; r.line = line; r.message = "expected '=' after key"; return r; }
        a++;
        while (a < n && is_space(at(s, a))) a++;
        if (a >= n) { r.ok = false; r.line = line; r.message = "missing value"; return r; }

        proven_u8str_view_t sv = PROVEN_LIT("");
        proven_u8str_t decoded = {0};
        bool have_decoded = false;
        long iv = 0;
        bool bv = false;
        int vt;
        if (at(s, a) == '"') {                         /* string (with \n \t \\ escapes) */
            a++;
            proven_result_u8str_t cr = proven_u8str_create(alloc, n + 1);
            if (!PROVEN_IS_OK(cr.err)) { r.ok = false; r.line = line; r.message = "out of memory"; return r; }
            decoded = cr.value; have_decoded = true;
            while (a < n && at(s, a) != '"') {
                char c = at(s, a);
                if (c == '\\' && a + 1 < n) {
                    a++;
                    char e = at(s, a);
                    c = (e == 'n') ? '\n' : (e == 't') ? '\t' : e;
                }
                (void)proven_u8str_append_byte(alloc, &decoded, (proven_u8)c);
                a++;
            }
            if (a >= n) { proven_u8str_destroy(alloc, &decoded); r.ok = false; r.line = line; r.message = "unterminated string"; return r; }
            a++;
            sv = proven_u8str_as_view(&decoded);
            vt = V_STR;
        } else if (n - a >= 4 && proven_u8str_view_eq(sub(s, a, 4), PROVEN_LIT("true"))) {
            bv = true; a += 4; vt = V_BOOL;
        } else if (n - a >= 5 && proven_u8str_view_eq(sub(s, a, 5), PROVEN_LIT("false"))) {
            bv = false; a += 5; vt = V_BOOL;
        } else if (at(s, a) == '-' || (at(s, a) >= '0' && at(s, a) <= '9')) {
            proven_scan_t sc = proven_scan_init(sub(s, a, n - a));
            proven_result_i64_t ir = proven_scan_i64(&sc);
            if (!PROVEN_IS_OK(ir.err)) { r.ok = false; r.line = line; r.message = "invalid integer"; return r; }
            iv = (long)ir.val;
            a += sc.cursor;
            vt = V_INT;
        } else {
            r.ok = false; r.line = line; r.message = "invalid value"; return r;
        }

        while (a < n && is_space(at(s, a))) a++;        /* trailing ws + comment */
        if (a < n && at(s, a) != '#') {
            if (have_decoded) proven_u8str_destroy(alloc, &decoded);
            r.ok = false; r.line = line; r.message = "trailing characters after value"; return r;
        }

        apply(cfg, section, key, vt, sv, iv, bv);
        if (have_decoded) proven_u8str_destroy(alloc, &decoded);
    }
    return r;
}

const char *prov_config_default_text(void) {
    return
        "# prov configuration  (provconf.ini)\n"
        "# The common subset of TOML and INI: [section] headers, key = value,\n"
        "# and # comments. Unknown keys are ignored (forward-compatible).\n"
        "# Read from ~/.prov/provconf.ini, or from a provconf.ini next to the\n"
        "# executable (portable mode, which then takes priority).\n"
        "# The values below are the built-in defaults; edit to taste.\n"
        "\n"
        "[editor]\n"
        "tabstop = 4          # width of a tab stop (1..64)\n"
        "expandtab = false    # insert spaces instead of a tab\n"
        "scrolloff = 0        # keep N lines visible above/below the cursor\n"
        "trigger = \"zx\"       # the two-key Ed <-> zx mode toggle\n"
        "undo_limit = 1000    # max undo steps kept per buffer\n"
        "mouse = true         # wheel scroll / click; set false to keep terminal text selection\n"
        "\n"
        "[search]\n"
        "ignorecase = false   # case-insensitive search (soc toggles at runtime)\n"
        "highlight = true     # highlight matches (soh toggles)\n"
        "wrapscan = true      # wrap past end/start of file when searching\n"
        "\n"
        "[clipboard]\n"
        "sync = true          # mirror yanks/pastes through the OS clipboard\n"
        "\n"
        "# --- reserved: recognized by the parser but not yet wired to behavior ---\n"
        "# [editor]\n"
        "# shiftwidth = 4\n"
        "# softtabstop = 4\n"
        "# line_numbers = \"off\"     # off | absolute | relative\n"
        "# wrap = \"char\"            # char | word | off  (off = horizontal scroll)\n"
        "# [theme]\n"
        "# colorscheme = \"dark\"\n"
        "# [syntax]\n"
        "# enable = true\n"
        "# max_file_size_mb = 10\n";
}
