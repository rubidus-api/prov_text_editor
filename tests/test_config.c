/* Unit tests for the TOML-subset config parser (no I/O; heap allocator for
 * transient string decoding). */
#include <stdio.h>
#include <string.h>

#include "proven/heap.h"
#include "config.h"

static int failures = 0;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } \
    } while (0)

static prov_config_result_t parse(prov_config_t *c, const char *s) {
    *c = prov_config_default();
    return prov_config_parse(c, proven_heap_allocator(), s, strlen(s));
}

int main(void) {
    prov_config_t c;

    /* defaults */
    c = prov_config_default();
    CHECK(c.tabstop == 4, "default tabstop 4");
    CHECK(strcmp(c.trigger, "zx") == 0, "default trigger zx");
    CHECK(c.undo_limit == 1000, "default undo_limit");

    /* a valid sample applies recognized keys */
    prov_config_result_t r = parse(&c,
        "# a comment\n"
        "\n"
        "[editor]\n"
        "tabstop = 8   # inline comment\n"
        "trigger = \"qq\"\n"
        "expandtab = true\n"
        "scrolloff = 5\n"
        "undo_limit = 250\n");
    CHECK(r.ok, "valid sample parses");
    CHECK(c.tabstop == 8, "tabstop 8");
    CHECK(strcmp(c.trigger, "qq") == 0, "trigger qq");
    CHECK(c.expandtab == true, "expandtab true");
    CHECK(c.scrolloff == 5, "scrolloff 5");
    CHECK(c.undo_limit == 250, "undo_limit 250");

    /* unknown key + unknown section tolerated (no error, defaults kept) */
    r = parse(&c, "[editor]\nshiftwidth = 2\n[theme]\ncolorscheme = \"dark\"\n");
    CHECK(r.ok, "unknown key/section tolerated");
    CHECK(c.tabstop == 4, "unknown keys leave defaults");

    /* clamp: tabstop out of range ignored */
    r = parse(&c, "[editor]\ntabstop = 0\n");
    CHECK(r.ok && c.tabstop == 4, "tabstop 0 rejected -> default kept");

    /* line_numbers enum: default off; absolute / relative recognized; junk -> off */
    c = prov_config_default();
    CHECK(c.line_numbers == PROV_LINENUM_OFF, "line_numbers default off");
    r = parse(&c, "[editor]\nline_numbers = \"absolute\"\n");
    CHECK(r.ok && c.line_numbers == PROV_LINENUM_ABSOLUTE, "line_numbers absolute");
    r = parse(&c, "[editor]\nline_numbers = \"relative\"\n");
    CHECK(r.ok && c.line_numbers == PROV_LINENUM_RELATIVE, "line_numbers relative");
    r = parse(&c, "[editor]\nline_numbers = \"nonsense\"\n");
    CHECK(r.ok && c.line_numbers == PROV_LINENUM_OFF, "line_numbers junk -> off");

    /* wrap enum: default char; off/word strings; bool true=char / false=off */
    c = prov_config_default();
    CHECK(c.wrap == PROV_WRAP_CHAR, "wrap default char");
    r = parse(&c, "[editor]\nwrap = \"off\"\n");
    CHECK(r.ok && c.wrap == PROV_WRAP_OFF, "wrap off");
    r = parse(&c, "[editor]\nwrap = \"word\"\n");
    CHECK(r.ok && c.wrap == PROV_WRAP_WORD, "wrap word");
    r = parse(&c, "[editor]\nwrap = false\n");
    CHECK(r.ok && c.wrap == PROV_WRAP_OFF, "wrap false -> off");
    r = parse(&c, "[editor]\nwrap = true\n");
    CHECK(r.ok && c.wrap == PROV_WRAP_CHAR, "wrap true -> char");

    /* syntax errors report a line */
    r = parse(&c, "[editor]\ntabstop 8\n");
    CHECK(!r.ok && r.line == 2, "missing '=' -> error on line 2");
    r = parse(&c, "[editor\n");
    CHECK(!r.ok && r.line == 1, "missing ']' -> error on line 1");
    r = parse(&c, "[editor]\ntabstop = 8 oops\n");
    CHECK(!r.ok && r.line == 2, "trailing chars -> error");
    r = parse(&c, "[editor]\ntrigger = \"unterminated\n");
    CHECK(!r.ok, "unterminated string -> error");

    /* string escapes decode */
    r = parse(&c, "[editor]\ntrigger = \"\\t\\n\"\n");
    CHECK(r.ok && c.trigger[0] == '\t' && c.trigger[1] == '\n' && c.trigger[2] == '\0',
          "escapes \\t \\n decode");

    /* over-long values do not corrupt or truncate the parse (no fixed caps) */
    r = parse(&c, "[editor]\ntrigger = \"abcdefghijklmnop\"\nundo_limit = 7\n");
    CHECK(r.ok, "over-long string value parses cleanly");
    CHECK(c.undo_limit == 7, "key after over-long value still applied");
    CHECK(strlen(c.trigger) <= sizeof c.trigger - 1, "trigger stays within its field");

    /* a very long section name is bounded, parse still succeeds */
    r = parse(&c,
        "[this_is_an_extremely_long_section_header_name_well_past_thirty_two]\n"
        "[editor]\ntabstop = 6\n");
    CHECK(r.ok && c.tabstop == 6, "long section header tolerated, editor still applied");

    /* large integer via proven_scan_i64 */
    r = parse(&c, "[editor]\nundo_limit = 1000000\n");
    CHECK(r.ok && c.undo_limit == 1000000, "large integer parsed");

    /* new defaults reproduce built-in behavior */
    c = prov_config_default();
    CHECK(c.search_ignorecase == false, "default ignorecase off");
    CHECK(c.search_highlight == true,   "default highlight on");
    CHECK(c.search_wrapscan == true,    "default wrapscan on");
    CHECK(c.clipboard_sync == true,     "default clipboard sync on");

    /* [search] and [clipboard] keys apply */
    r = parse(&c,
        "[search]\n"
        "ignorecase = true\n"
        "highlight = false\n"
        "wrapscan = false\n"
        "[clipboard]\n"
        "sync = false\n");
    CHECK(r.ok, "search/clipboard sections parse");
    CHECK(c.search_ignorecase == true,  "ignorecase = true applied");
    CHECK(c.search_highlight == false,  "highlight = false applied");
    CHECK(c.search_wrapscan == false,   "wrapscan = false applied");
    CHECK(c.clipboard_sync == false,    "clipboard sync = false applied");

    /* the shipped default template parses cleanly and keeps built-in defaults */
    {
        const char *tmpl = prov_config_default_text();
        prov_config_t dc = prov_config_default();
        prov_config_result_t tr =
            prov_config_parse(&dc, proven_heap_allocator(), tmpl, strlen(tmpl));
        CHECK(tr.ok, "default template parses without error");
        prov_config_t def = prov_config_default();
        CHECK(dc.tabstop == def.tabstop && dc.undo_limit == def.undo_limit &&
              dc.search_ignorecase == def.search_ignorecase &&
              dc.search_highlight == def.search_highlight &&
              dc.search_wrapscan == def.search_wrapscan &&
              dc.clipboard_sync == def.clipboard_sync,
              "default template yields the built-in defaults");
    }

    if (failures) {
        fprintf(stderr, "config: %d checks failed\n", failures);
        return 1;
    }
    printf("ok: config tests passed\n");
    return 0;
}
