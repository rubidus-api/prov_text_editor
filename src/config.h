#ifndef PROV_CONFIG_H
#define PROV_CONFIG_H

#include "proven/types.h"
#include "proven/allocator.h"

/*
 * Editor configuration (SPEC §17). A strict TOML subset: `[section]` headers,
 * `key = value` pairs with string / integer / boolean values, and `#` comments.
 * Unknown sections and keys are tolerated (parsed and ignored) so config files
 * stay forward/backward compatible; only malformed *syntax* is an error.
 */

/* [editor] line_numbers gutter style (§17.4). */
typedef enum {
    PROV_LINENUM_OFF = 0,    /* no gutter (default) */
    PROV_LINENUM_ABSOLUTE,   /* every line shows its 1-based number */
    PROV_LINENUM_RELATIVE,   /* cursor line absolute, others their distance */
} prov_linenum_t;

/* [editor] wrap — long-line handling (§17.4). */
typedef enum {
    PROV_WRAP_CHAR = 0,      /* soft-wrap at the column edge (default) */
    PROV_WRAP_WORD,          /* soft-wrap at word boundaries (pending; falls back to char) */
    PROV_WRAP_OFF,           /* no wrap: scroll horizontally instead */
} prov_wrap_t;

typedef struct {
    proven_u32 tabstop;      /* [editor] tabstop      — default 4, clamped 1..64 */
    char       trigger[8];   /* [editor] trigger      — default "zx" (2 chars used) */
    proven_u32 scrolloff;    /* [editor] scrolloff    — default 0 */
    bool       expandtab;    /* [editor] expandtab    — default false */
    proven_u32 undo_limit;   /* [editor] undo_limit   — default 1000 */
    prov_linenum_t line_numbers;   /* [editor] line_numbers — default off */
    prov_wrap_t    wrap;           /* [editor] wrap          — default char */
    char       charset_backend[16];   /* [editor] charset_backend — auto/libc/command/win32 */
    char       charset_iconv_path[256]; /* [editor] charset_iconv_path — iconv exe for the "command" backend (PATH name or full path; "" = "iconv") */
    char       fallback_encoding[32]; /* [editor] fallback_encoding — for no-BOM non-UTF-8 (default Windows-1252) */
    bool       search_ignorecase;  /* [search] ignorecase — default false (case-sensitive) */
    bool       search_highlight;   /* [search] highlight  — default true  */
    bool       search_wrapscan;    /* [search] wrapscan   — default true  */
    bool       clipboard_sync;     /* [clipboard] sync    — default true  */
    bool       mouse;              /* [editor] mouse      — default true  */
} prov_config_t;

/* Settings with their documented defaults. The defaults reproduce the editor's
 * built-in behavior, so a missing or empty config file changes nothing. */
prov_config_t prov_config_default(void);

/* A canonical, commented starter config (the SPEC §17.3 example) as a static
 * NUL-terminated string. Parses cleanly back through prov_config_parse. Active
 * keys are the ones in prov_config_t; the rest are documented as reserved. */
const char *prov_config_default_text(void);

typedef struct {
    bool          ok;        /* false on a syntax error */
    proven_size_t line;      /* 1-based line of the error, 0 when ok */
    const char   *message;   /* static reason, NULL when ok */
} prov_config_result_t;

/* Parse a TOML-subset config from an in-memory buffer, applying recognized keys
 * onto `cfg` (which should start from prov_config_default()). Returns ok=false
 * with a line + message on a syntax error; recognized keys parsed before the
 * error remain applied. The buffer is not modified and no I/O is performed.
 * `alloc` backs a transient owning string used to decode escaped string values
 * (no allocation occurs for files without quoted values). */
prov_config_result_t prov_config_parse(prov_config_t *cfg, proven_allocator_t alloc,
                                       const char *text, proven_size_t len);

#endif /* PROV_CONFIG_H */
