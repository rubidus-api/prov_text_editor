#ifndef PROV_THEME_H
#define PROV_THEME_H

/*
 * Color theme model (RFC-0021). This pass implements only the DATA model + the
 * `*.theme.ini` reader + resolution; nothing renders color yet — applying a
 * token class's color to buffer text (the highlighter) is RFC-0022.
 *
 * A prov_theme_t is plain POD (fixed arrays, no ownership) so the session can
 * hold one by value and re-resolve freely.
 */

#include "proven/types.h"
#include "proven/allocator.h"

typedef enum {
    PROV_TOK_DEFAULT = 0,   /* identifiers / plain text */
    PROV_TOK_KEYWORD,
    PROV_TOK_TYPE,
    PROV_TOK_STRING,
    PROV_TOK_COMMENT,
    PROV_TOK_NUMBER,
    PROV_TOK_CONSTANT,
    PROV_TOK_FUNCTION,
    PROV_TOK_OPERATOR,
    PROV_TOK_PREPROC,
    PROV_TOK_PUNCTUATION,
    PROV_TOK_BUILTIN,
    PROV_TOK_ERROR,
    PROV_TOK_KEY,           /* config/data keys, JSON/TOML/YAML key names, attrs */
    PROV_TOK_VALUE,         /* config/data scalar values */
    PROV_TOK_COUNT
} prov_tok_class_t;

/* Theme-local attribute bits (independent of display.h PROV_ATTR_*). */
enum {
    PROV_THEME_BOLD      = 1u << 0,
    PROV_THEME_DIM       = 1u << 1,
    PROV_THEME_UNDERLINE = 1u << 2
};

/* fg/bg: -1 = terminal default, 0..15 = ANSI palette index (RFC-0021 table). */
typedef struct {
    proven_i8 fg;
    proven_i8 bg;
    proven_u8 attr;   /* PROV_THEME_* */
} prov_color_t;

typedef struct {
    char         name[32];
    bool         light;                  /* background hint: true = light terminal */
    prov_color_t cls[PROV_TOK_COUNT];
} prov_theme_t;

/* A built-in theme by name: "prov-dark", "prov-light", "mono". Unknown -> prov-dark. */
prov_theme_t prov_theme_builtin(const char *name);

/* Resolve the active theme for `name`:
 *   1. a [theme:<name>] (or [<name>]) section in any *.theme.ini in `config_dir`,
 *   2. else a built-in of that name,
 *   3. else prov-dark.
 * NO_COLOR in the environment, or name == "mono", forces the mono theme.
 * `config_dir` may be NULL/"" to skip file scanning. Returns by value (POD). */
prov_theme_t prov_theme_resolve(proven_allocator_t a, const char *config_dir, const char *name);

/* --- exposed for tests --------------------------------------------------- */

/* Parse one color spec ("green", "white on red", "gray +dim") into *out.
 * Returns false on a malformed spec (then *out is left unchanged). */
bool prov_theme_parse_color(const char *spec, prov_color_t *out);

/* Token-class key ("comment", "preproc"/"preprocessor", ...) -> enum, or
 * PROV_TOK_COUNT when unknown. */
prov_tok_class_t prov_theme_class_from_key(const char *key);

/* Color name ("brightblue", "gray", "default", ...) -> ANSI index (-1 default),
 * or -2 when the name is unknown. */
int prov_theme_color_index(const char *name);

#endif /* PROV_THEME_H */
