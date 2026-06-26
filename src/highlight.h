#ifndef PROV_HIGHLIGHT_H
#define PROV_HIGHLIGHT_H

/*
 * Lightweight, per-language syntax highlighting (RFC-0022). A single line-oriented
 * engine: each language is a `prov_hl_line` case that walks one line's bytes given
 * a tiny carry state and writes a per-byte token class, which the active theme
 * (RFC-0021) maps to a biased cell foreground. No parser, no dependency.
 */

#include "proven/types.h"
#include "theme.h"        /* prov_theme_t, prov_tok_class_t */

typedef enum {
    PROV_HL_NONE = 0,
    PROV_HL_MARKDOWN,
    PROV_HL_C,
    PROV_HL_PYTHON,
    PROV_HL_JAVASCRIPT,
    PROV_HL_TYPESCRIPT,
    PROV_HL_SHELL,
    PROV_HL_JSON,
    PROV_HL_TOML,        /* also .ini/.cfg/.conf/.env (key=value) */
    PROV_HL_YAML,
    PROV_HL_CSS,
    PROV_HL_RUST,
    PROV_HL_GO,
    PROV_HL_JAVA,
    PROV_HL_KOTLIN,
    PROV_HL_SWIFT,
    PROV_HL_LUA,
    PROV_HL_SQL,
    PROV_HL_MARKUP,      /* html / xml */
    /* more packs added incrementally (RFC-0022) */
} prov_hl_lang_t;

/* Carry state across lines (multi-line constructs: fences, block comments). */
typedef struct {
    proven_u8  kind;    /* language-defined: 0 = NORMAL, else an in-region kind */
    proven_u8  depth;   /* nesting depth (e.g. nestable comments) */
    proven_u32 param;   /* the open delimiter (fence char/len, quote, ...) */
} prov_hl_state_t;

#define PROV_HL_STATE0 ((prov_hl_state_t){ 0, 0, 0 })

/* Pick a language pack from a file path (extension/basename). NONE if unknown. */
prov_hl_lang_t prov_hl_detect(const char *path);

/* Highlight one line. `line[0..len)` are the line's bytes (no trailing newline).
 * `st` is the carry state at the line start; the return value is the carry state
 * for the next line. `out_fg` (len bytes) receives the biased cell fg per byte
 * (0 = default), resolved through `theme`. `theme` may be NULL (then out_fg is
 * filled with class indices is NOT done — pass a theme). */
prov_hl_state_t prov_hl_line(prov_hl_lang_t lang, prov_hl_state_t st,
                             const proven_u8 *line, proven_size_t len,
                             const prov_theme_t *theme, proven_u8 *out_fg);

#endif /* PROV_HIGHLIGHT_H */
