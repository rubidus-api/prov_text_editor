#ifndef PROV_PLATFORM_CHARSET_H
#define PROV_PLATFORM_CHARSET_H

#include "proven/types.h"
#include "proven/allocator.h"

/*
 * Charset conversion via pluggable, runtime-probed backends.
 *
 * prov decodes UTF-8/16/32 and Windows-1252 itself; every *other* encoding
 * (CP949/Johab, Shift-JIS, GBK/GB18030, Big5, EUC-JP, ISO-8859-x, …) is converted
 * to/from the internal UTF-8 by delegating to whatever the host already provides.
 * There is no single right delegate, so backends are registered and **probed at
 * startup** — the first that actually works (or a configured preference) wins:
 *
 *   - "libc"     : the linked iconv (glibc / a linked libiconv)
 *   - "win32"    : MultiByteToWideChar / WideCharToMultiByte (Windows)
 *   - "dll"      : a dynamically loaded libiconv (dlopen/LoadLibrary)   [planned]
 *   - "command"  : the external `iconv` command-line tool (subprocess)  [planned]
 *
 * `enc` is a canonical, upper-case name (e.g. "CP949", "SHIFT_JIS", "GBK",
 * "BIG5", "EUC-JP"); each backend maps it to its own identifier / code page.
 */

typedef struct {
    const char *name;                  /* backend id, e.g. "libc" */
    bool        (*probe)(void);        /* usable in this process right now? */
    /* Does this backend support code page / encoding `enc`? Answered the backend's
     * own way (iconv_open trial / IsValidCodePage / a trial `iconv` run) — there is
     * no universal query, so each backend reports for itself. */
    bool        (*supports)(const char *enc);
    /* Convert `b[0..n)` between `enc` and UTF-8. Fresh allocation of *outn bytes
     * (caller frees) or NULL on failure (unknown encoding / conversion error). */
    proven_u8  *(*to_utf8)(proven_allocator_t a, const char *enc,
                           const proven_u8 *b, proven_size_t n, proven_size_t *outn);
    proven_u8  *(*from_utf8)(proven_allocator_t a, const char *enc,
                             const proven_u8 *b, proven_size_t n, proven_size_t *outn);
} prov_charset_backend_t;

/* Record the preferred backend name from config — NO probing or I/O. Safe to call
 * at startup even when the document is plain UTF-8 (which needs no backend at all).
 * "auto"/NULL = pick automatically. The backend is actually resolved lazily, the
 * first time a non-UTF conversion is needed (see below). */
void prov_charset_configure(const char *preferred);

/* Lazily resolve + cache the active backend (only on first real use). The config
 * value wins when its named backend probes OK; otherwise fall back to the first
 * backend that probes OK in the platform's registry (priority) order. Returns the
 * active backend's name, or NULL when none works (e.g. freestanding). The probe
 * (incl. the `command` backend's process spawn) runs at most once per run. */
const char *prov_charset_active(void);

/* Enumerate the backends actually compiled into this platform's registry (e.g.
 * "libc"/"command" on POSIX, "win32"/"command" on Windows — never a backend that
 * can't exist here). Fills `out[0..min(count,max))` with static name strings and
 * returns the registry count. The UI builds its picker from this so it never offers
 * an impossible backend. (The "auto" pseudo-choice is the caller's to prepend.) */
int prov_charset_backend_names(const char **out, int max);

/* Point the external-`iconv` ("command") backend at a specific executable — a bare
 * name resolved via PATH (the default, "iconv") or an absolute/relative path for an
 * iconv that isn't on PATH. NULL/empty restores the default. Re-probed on next use. */
void prov_charset_set_iconv_path(const char *path);

/* True when the active backend supports `enc`. Resolved on demand and **cached per
 * encoding** — each (backend, encoding) is checked at most once per run, so the
 * cost is paid incrementally only for encodings actually requested. */
bool prov_charset_supports(const char *enc);

/* Convert via the active backend; NULL when no backend or the conversion fails. */
proven_u8 *prov_charset_to_utf8(proven_allocator_t a, const char *enc,
                                const proven_u8 *b, proven_size_t n, proven_size_t *outn);
proven_u8 *prov_charset_from_utf8(proven_allocator_t a, const char *enc,
                                  const proven_u8 *b, proven_size_t n, proven_size_t *outn);

#endif /* PROV_PLATFORM_CHARSET_H */
