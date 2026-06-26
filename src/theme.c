#include "theme.h"
#include "pstr.h"                 /* prov_cstr_view */
#include "proven/fs.h"
#include "proven/u8str.h"
#include "proven/array.h"

#include <string.h>
#include <stdlib.h>               /* getenv */

/* ---- small ASCII helpers (locale-free) --------------------------------- */

static char lo(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* case-insensitive equality of NUL-terminated ASCII strings */
static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (lo(*a) != lo(*b)) return false;
    return *a == *b;
}

static bool ends_with_ci(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (lf > ls) return false;
    for (size_t i = 0; i < lf; i++) if (lo(s[ls - lf + i]) != lo(suf[i])) return false;
    return true;
}

/* ---- tables ------------------------------------------------------------- */

static const struct { const char *n; int idx; } COLORS[] = {
    { "default", -1 },
    { "black", 0 }, { "red", 1 }, { "green", 2 }, { "yellow", 3 },
    { "blue", 4 }, { "magenta", 5 }, { "cyan", 6 }, { "white", 7 },
    { "brightblack", 8 }, { "gray", 8 }, { "grey", 8 },
    { "brightred", 9 }, { "brightgreen", 10 }, { "brightyellow", 11 },
    { "brightblue", 12 }, { "brightmagenta", 13 }, { "brightcyan", 14 },
    { "brightwhite", 15 },
};

static const struct { const char *k; prov_tok_class_t c; } CLASSES[] = {
    { "default", PROV_TOK_DEFAULT }, { "keyword", PROV_TOK_KEYWORD },
    { "type", PROV_TOK_TYPE }, { "string", PROV_TOK_STRING },
    { "comment", PROV_TOK_COMMENT }, { "number", PROV_TOK_NUMBER },
    { "constant", PROV_TOK_CONSTANT }, { "function", PROV_TOK_FUNCTION },
    { "operator", PROV_TOK_OPERATOR }, { "preproc", PROV_TOK_PREPROC },
    { "preprocessor", PROV_TOK_PREPROC }, { "punctuation", PROV_TOK_PUNCTUATION },
    { "punct", PROV_TOK_PUNCTUATION }, { "builtin", PROV_TOK_BUILTIN },
    { "error", PROV_TOK_ERROR }, { "key", PROV_TOK_KEY }, { "value", PROV_TOK_VALUE },
};

int prov_theme_color_index(const char *name) {
    for (size_t i = 0; i < sizeof COLORS / sizeof COLORS[0]; i++)
        if (ieq(name, COLORS[i].n)) return COLORS[i].idx;
    return -2;   /* unknown */
}

prov_tok_class_t prov_theme_class_from_key(const char *key) {
    for (size_t i = 0; i < sizeof CLASSES / sizeof CLASSES[0]; i++)
        if (ieq(key, CLASSES[i].k)) return CLASSES[i].c;
    return PROV_TOK_COUNT;
}

/* ---- color spec: "<fg> [on <bg>] [+bold] [+dim] [+underline]" ----------- */

bool prov_theme_parse_color(const char *spec, prov_color_t *out) {
    char buf[128];
    size_t n = 0;
    for (const char *p = spec; *p && n + 1 < sizeof buf; p++) buf[n++] = lo(*p);
    buf[n] = '\0';

    prov_color_t c = { -1, -1, 0 };
    bool have_fg = false, expect_bg = false;
    char *save = NULL;
    for (char *tok = buf; ; tok = NULL) {
        char *t = NULL;
        /* hand-rolled strtok over spaces/tabs (avoid strtok_r portability) */
        char *start = tok ? tok : save;
        while (start && (*start == ' ' || *start == '\t')) start++;
        if (!start || !*start) break;
        char *end = start;
        while (*end && *end != ' ' && *end != '\t') end++;
        save = (*end) ? end + 1 : end;
        *end = '\0';
        t = start;

        if (ieq(t, "on")) { expect_bg = true; continue; }
        if (t[0] == '+') {
            if (ieq(t, "+bold")) c.attr |= PROV_THEME_BOLD;
            else if (ieq(t, "+dim")) c.attr |= PROV_THEME_DIM;
            else if (ieq(t, "+underline") || ieq(t, "+ul")) c.attr |= PROV_THEME_UNDERLINE;
            /* unknown +attr: tolerated (ignored) */
            continue;
        }
        int idx = prov_theme_color_index(t);
        if (idx == -2) continue;           /* unknown color name: tolerated */
        if (expect_bg) { c.bg = (proven_i8)idx; expect_bg = false; }
        else if (!have_fg) { c.fg = (proven_i8)idx; have_fg = true; }
        /* extra bare color tokens: ignored */
    }
    if (!have_fg && c.attr == 0 && c.bg == -1) return false;   /* nothing usable */
    *out = c;
    return true;
}

/* ---- built-in themes ---------------------------------------------------- */

static void set_name(prov_theme_t *t, const char *n) {
    size_t i = 0; for (; n[i] && i + 1 < sizeof t->name; i++) t->name[i] = n[i]; t->name[i] = '\0';
}

prov_theme_t prov_theme_builtin(const char *name) {
    prov_theme_t t;
    memset(&t, 0, sizeof t);
    for (int i = 0; i < PROV_TOK_COUNT; i++) t.cls[i] = (prov_color_t){ -1, -1, 0 };

    if (name && ieq(name, "mono")) {
        set_name(&t, "mono"); t.light = false;
        t.cls[PROV_TOK_KEYWORD] = (prov_color_t){ -1, -1, PROV_THEME_BOLD };
        t.cls[PROV_TOK_COMMENT] = (prov_color_t){ -1, -1, PROV_THEME_DIM };
        return t;
    }
    if (name && ieq(name, "prov_light")) {
        set_name(&t, "prov_light"); t.light = true;
        t.cls[PROV_TOK_KEYWORD]  = (prov_color_t){ 4, -1, 0 };
        t.cls[PROV_TOK_TYPE]     = (prov_color_t){ 6, -1, 0 };
        t.cls[PROV_TOK_STRING]   = (prov_color_t){ 2, -1, 0 };
        t.cls[PROV_TOK_COMMENT]  = (prov_color_t){ 8, -1, PROV_THEME_DIM };
        t.cls[PROV_TOK_NUMBER]   = (prov_color_t){ 5, -1, 0 };
        t.cls[PROV_TOK_CONSTANT] = (prov_color_t){ 5, -1, 0 };
        t.cls[PROV_TOK_FUNCTION] = (prov_color_t){ 4, -1, PROV_THEME_BOLD };
        t.cls[PROV_TOK_PREPROC]  = (prov_color_t){ 5, -1, 0 };
        t.cls[PROV_TOK_BUILTIN]  = (prov_color_t){ 6, -1, 0 };
        t.cls[PROV_TOK_ERROR]    = (prov_color_t){ 1, -1, 0 };
        t.cls[PROV_TOK_KEY]      = (prov_color_t){ 4, -1, 0 };   /* blue */
        t.cls[PROV_TOK_VALUE]    = (prov_color_t){ 2, -1, 0 };   /* green */
        return t;
    }
    /* default: prov_dark (also for any unknown name) */
    set_name(&t, "prov_dark"); t.light = false;
    t.cls[PROV_TOK_KEYWORD]  = (prov_color_t){ 12, -1, 0 };   /* brightblue   */
    t.cls[PROV_TOK_TYPE]     = (prov_color_t){ 6,  -1, 0 };   /* cyan         */
    t.cls[PROV_TOK_STRING]   = (prov_color_t){ 2,  -1, 0 };   /* green        */
    t.cls[PROV_TOK_COMMENT]  = (prov_color_t){ 8,  -1, PROV_THEME_DIM };  /* gray */
    t.cls[PROV_TOK_NUMBER]   = (prov_color_t){ 5,  -1, 0 };   /* magenta      */
    t.cls[PROV_TOK_CONSTANT] = (prov_color_t){ 5,  -1, 0 };   /* magenta      */
    t.cls[PROV_TOK_FUNCTION] = (prov_color_t){ 3,  -1, 0 };   /* yellow       */
    t.cls[PROV_TOK_PREPROC]  = (prov_color_t){ 13, -1, 0 };   /* brightmagenta*/
    t.cls[PROV_TOK_BUILTIN]  = (prov_color_t){ 6,  -1, 0 };   /* cyan         */
    t.cls[PROV_TOK_ERROR]    = (prov_color_t){ 9,  -1, 0 };   /* brightred    */
    t.cls[PROV_TOK_KEY]      = (prov_color_t){ 6,  -1, 0 };   /* cyan (config/data keys) */
    t.cls[PROV_TOK_VALUE]    = (prov_color_t){ 2,  -1, 0 };   /* green (config/data values) */
    return t;
}

/* ---- *.theme.ini parsing ----------------------------------------------- */

/* trim leading/trailing spaces/tabs of [*ps, *pe) in place (adjust pointers) */
static void trim(const char **ps, const char **pe) {
    const char *s = *ps, *e = *pe;
    while (s < e && (*s == ' ' || *s == '\t')) s++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    *ps = s; *pe = e;
}

/* copy [s,e) into a NUL-terminated small buffer (truncates) */
static void copyz(char *dst, size_t cap, const char *s, const char *e) {
    size_t i = 0; for (; s < e && i + 1 < cap; i++, s++) dst[i] = *s; dst[i] = '\0';
}

/* If `line` is a section header "[...]", write the (theme:-stripped) section name
 * into `out` and return true. */
static bool section_name(const char *s, const char *e, char *out, size_t cap) {
    trim(&s, &e);
    if (s >= e || *s != '[' || e[-1] != ']') return false;
    s++; e--;                                   /* inside the brackets */
    /* `theme_` prefix: a `*.theme.ini` section is `[theme_<name>]`, matching the
     * INI naming rule [a-z][a-z0-9_]* (no `:` or `-`). The `<name>` after the
     * prefix is what `theme = <name>` selects. A bare `[<name>]` is also accepted. */
    if ((size_t)(e - s) > 6 && strncmp(s, "theme_", 6) == 0) s += 6;
    trim(&s, &e);
    copyz(out, cap, s, e);
    return true;
}

/* Parse the body of section `want` from a file buffer into `out`.
 * Returns true if the section was found. */
static bool parse_section(const char *data, size_t len, const char *want, prov_theme_t *out) {
    /* locate section start (line after the matching header) and end (next header) */
    size_t i = 0; bool in = false; size_t body_start = 0, body_end = len;
    while (i < len) {
        size_t ls = i; while (i < len && data[i] != '\n') i++; size_t le = i; if (i < len) i++;
        char sec[64];
        if (section_name(data + ls, data + le, sec, sizeof sec)) {
            if (in) { body_end = ls; break; }            /* next header ends the body */
            if (ieq(sec, want)) { in = true; body_start = i; }
        }
    }
    if (!in) return false;

    /* pass A: seed (extends, else built-in by name, else prov_dark) */
    char ext[32] = { 0 };
    {
        size_t j = body_start;
        while (j < body_end) {
            size_t ls = j; while (j < body_end && data[j] != '\n') j++; size_t le = j; if (j < body_end) j++;
            const char *s = data + ls, *e = data + le; trim(&s, &e);
            if (s >= e || *s == '#' || *s == ';') continue;
            const char *eq = s; while (eq < e && *eq != '=') eq++;
            if (eq >= e) continue;
            const char *ks = s, *ke = eq; trim(&ks, &ke);
            char key[32]; copyz(key, sizeof key, ks, ke);
            if (ieq(key, "extends")) { const char *vs = eq + 1, *ve = e; trim(&vs, &ve); copyz(ext, sizeof ext, vs, ve); }
        }
    }
    *out = prov_theme_builtin(ext[0] ? ext : want);   /* unknown name -> prov_dark seed */
    set_name(out, want);

    /* pass B: apply background + class colors */
    size_t j = body_start;
    while (j < body_end) {
        size_t ls = j; while (j < body_end && data[j] != '\n') j++; size_t le = j; if (j < body_end) j++;
        const char *s = data + ls, *e = data + le; trim(&s, &e);
        if (s >= e || *s == '#' || *s == ';') continue;
        const char *eq = s; while (eq < e && *eq != '=') eq++;
        if (eq >= e) continue;
        const char *ks = s, *ke = eq; trim(&ks, &ke);
        const char *vs = eq + 1, *ve = e; trim(&vs, &ve);
        char key[32], val[96];
        copyz(key, sizeof key, ks, ke);
        copyz(val, sizeof val, vs, ve);
        if (ieq(key, "background")) { out->light = ieq(val, "light"); continue; }
        if (ieq(key, "extends")) continue;
        prov_tok_class_t c = prov_theme_class_from_key(key);
        if (c == PROV_TOK_COUNT) continue;             /* unknown key: tolerated */
        prov_color_t col;
        if (prov_theme_parse_color(val, &col)) out->cls[c] = col;   /* bad spec: tolerated */
    }
    return true;
}

/* Scan `dir` for *.theme.ini and parse `want` out of the first file defining it. */
static bool load_from_dir(proven_allocator_t a, const char *dir,
                          const char *want, prov_theme_t *out) {
    proven_result_array_t lr = proven_fs_list(a, prov_cstr_view(dir));
    if (!proven_is_ok(lr.err)) return false;
    bool found = false;
    for (proven_size_t k = 0; k < lr.value.len && !found; k++) {
        const proven_fs_entry_t *fe = proven_array_get(&lr.value, k);
        proven_u8str_view_t nv = proven_u8str_as_view((proven_u8str_t *)&fe->name);
        char nm[256]; copyz(nm, sizeof nm, (const char *)nv.ptr, (const char *)nv.ptr + nv.size);
        if (!ends_with_ci(nm, ".theme.ini")) continue;

        char path[1100];
        FMT_INTO(path, "{}/{}", PROVEN_ARG(prov_cstr_view(dir)), PROVEN_ARG(prov_cstr_view(nm)));
        proven_result_mem_mut_t rd = proven_fs_read_all(a, prov_cstr_view(path));
        if (!proven_is_ok(rd.err)) continue;
        if (parse_section((const char *)rd.value.ptr, rd.value.size, want, out)) found = true;
        a.free_fn(a.ctx, rd.value.ptr);
    }
    proven_fs_list_destroy(a, &lr.value);
    return found;
}

prov_theme_t prov_theme_resolve(proven_allocator_t a, const char *config_dir, const char *name) {
    if (!name || !*name) name = "prov_dark";
    const char *nc = getenv("NO_COLOR");
    if ((nc && *nc) || ieq(name, "mono")) return prov_theme_builtin("mono");
    if (config_dir && *config_dir) {
        prov_theme_t t;
        if (load_from_dir(a, config_dir, name, &t)) return t;
    }
    return prov_theme_builtin(name);   /* prov_dark for any unknown name */
}
