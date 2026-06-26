#ifndef PROV_SAVE_H
#define PROV_SAVE_H

#include "proven/types.h"
#include "proven/allocator.h"

#include "buffer.h"
#include "encoding.h"   /* prov_fileinfo_t */

/*
 * Atomic file save (SPEC.md §7.2).
 *
 * The buffer (internal LF-only, BOM-free UTF-8) is serialized to bytes, written
 * to a temporary file in the same directory as the target, then moved onto the
 * target with an atomic rename, so a crash mid-write leaves the previous file
 * intact. Durability fsync and permission/owner preservation are deferred.
 *
 * `info` (when non-NULL) reproduces the file's original form: LF is rewritten to
 * the original EOL (CRLF/CR; LF and MIXED stay LF) and a UTF-8 BOM is re-added
 * when `info->bom`. NULL saves verbatim LF-only UTF-8.
 *
 * err: PROVEN_ERR_INVALID_ARG for a bad allocator/buffer/path; filesystem
 * errors from open/write/rename are propagated (the temp file is removed on
 * failure). */
proven_err_t prov_save_buffer(proven_allocator_t alloc, const prov_buffer_t *buf,
                              const char *path, const prov_fileinfo_t *info);

#endif /* PROV_SAVE_H */
