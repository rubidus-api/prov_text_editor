#ifndef PROV_ENCODING_H
#define PROV_ENCODING_H

#include "proven/types.h"
#include "proven/allocator.h"

#include "buffer.h"

/*
 * File loading (SPEC.md §6.2, §20).
 *
 * Read-all strategy: the whole file is read into memory, validated as UTF-8, and
 * copied into a new buffer. The buffer holds the *internal* form — LF-only,
 * BOM-free UTF-8: `prov_load_file` strips a leading UTF-8 BOM and normalizes every
 * CRLF / lone CR to LF, recording the original encoding / EOL / BOM so a save can
 * reproduce them. Decoding non-UTF-8 encodings and `mmap`-backed loading are
 * deferred to later steps.
 */

/* Load `path` as a UTF-8 document into a freshly created buffer.
 *
 * If `sanitized` is NULL the load is strict: a file that is not well-formed UTF-8
 * fails with PROVEN_ERR_INVALID_ENCODING. If `sanitized` is non-NULL the load is
 * lossy: bytes that are not part of a valid UTF-8 sequence are dropped, the file
 * still loads, and *sanitized is set true when anything was removed.
 *
 * Other err values: PROVEN_ERR_INVALID_ARG (invalid allocator / NULL path); fs /
 * allocation errors from proven_fs_read_all are propagated. An empty file yields
 * an empty buffer (1 logical line, 0 bytes). */
prov_result_buffer_t prov_load_utf8_file(proven_allocator_t alloc,
                                         const char *path, bool *sanitized);

/* ---- file metadata (for the per-window status bar) ---- */

typedef enum {
    PROV_EOL_LF,      /* "\n"   (Unix) */
    PROV_EOL_CRLF,    /* "\r\n" (DOS)  */
    PROV_EOL_CR,      /* "\r"   (classic Mac) */
    PROV_EOL_MIXED    /* more than one style present */
} prov_eol_t;

typedef struct {
    const char *encoding;   /* static label, e.g. "UTF-8" (self-contained UTF/1252 path) */
    int         codepage;   /* code-page number, e.g. 65001 (UTF-8), 1252 (Windows-1252) */
    const char *country;    /* 2-letter country tag for the code page, or NULL */
    prov_eol_t  eol;        /* dominant line-ending style */
    bool        bom;        /* leading BOM present (UTF-8/16/32) */
    char        enc_name[24]; /* non-empty: a platform (iconv/Win32) encoding name (e.g. "CP949");
                               * load/save convert via the charset PAL instead of the built-ins.
                               * In a binary buffer this is the *interpretation* charset for the
                               * hex decoded-string line / range string ops (RFC-0019), not a
                               * load/save conversion (binary load/save is verbatim). */
    bool        binary;       /* RFC-0019: raw bytes — load + save verbatim, no encoding/EOL/BOM
                               * conversion; the window is shown as a hex dump. */
} prov_fileinfo_t;

/* Metadata for a freshly created / unnamed buffer: UTF-8, LF, no BOM. */
prov_fileinfo_t prov_fileinfo_default(void);

/* Detect encoding / line-ending / BOM by scanning `buf` once (call at load). */
prov_fileinfo_t prov_detect_fileinfo(const prov_buffer_t *buf);

/* Load `path` into the internal form and report what was detected. Like
 * prov_load_utf8_file, but additionally: strips a leading UTF-8 BOM, normalizes
 * every CRLF / lone CR to LF, and (when `out_info` is non-NULL) fills it with the
 * file's original encoding / EOL / BOM so a later save can reproduce them. The
 * buffer always holds LF-only, BOM-free UTF-8. `sanitized` behaves as above. */
/* `want_enc` (non-NULL/non-empty, != "UTF-8") forces that encoding via the charset
 * PAL (the open panel's pick). `fallback_enc` (config) is used for a no-BOM file
 * that is not valid UTF-8 before the built-in Windows-1252 fallback. Either NULL =
 * auto. The detected/forced encoding is recorded in out_info for a faithful save. */
prov_result_buffer_t prov_load_file(proven_allocator_t alloc, const char *path,
                                    bool *sanitized, prov_fileinfo_t *out_info,
                                    const char *want_enc, const char *fallback_enc);

/* Load `path` as raw bytes, verbatim, into a freshly created buffer — no UTF-8
 * validation, no BOM/EOL normalization, no decoding (RFC-0019 binary mode). The
 * buffer holds exactly the file's bytes; a later verbatim save reproduces them.
 * `out_info` (when non-NULL) is the default info with `binary = true` and
 * `enc_name` set to `interp_enc` (the interpretation charset, "" = UTF-8). */
prov_result_buffer_t prov_load_binary(proven_allocator_t alloc, const char *path,
                                      prov_fileinfo_t *out_info, const char *interp_enc);

/* Encode internal (LF-only, BOM-free UTF-8) bytes to the on-disk form in `info`:
 * LF -> the original EOL, then UTF-8 -> the original code page (UTF-8/UTF-16/
 * UTF-32), with a leading BOM when info->bom. NULL info = verbatim LF UTF-8.
 * Returns a fresh allocation of *outn bytes (caller frees), or NULL on OOM. */
proven_u8 *prov_encode_save(proven_allocator_t a, const proven_u8 *b, proven_size_t n,
                            const prov_fileinfo_t *info, proven_size_t *outn);

/* Short name for an EOL style: "LF" / "CR/LF" / "CR" / "MIXED". */
const char *prov_eol_name(prov_eol_t eol);

#endif /* PROV_ENCODING_H */
