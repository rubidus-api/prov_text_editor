#ifndef PROV_BUFSET_H
#define PROV_BUFSET_H

#include "proven/types.h"
#include "editor.h"
#include "encoding.h"   /* prov_fileinfo_t */

/*
 * Buffer set (SPEC §10.7 `zb`): the open buffers and which one is active. Each
 * entry owns an editor plus the per-buffer working state that must survive a
 * switch (path, modified flag, viewport top, INS/OVR). The list operations here
 * are pure; the caller creates/destroys the editors.
 */

#define PROV_MAX_BUFFERS 32

typedef struct {
    prov_editor_t  *ed;
    char            path[1024];  /* empty string = unnamed buffer */
    bool            modified;
    proven_size_t   top;         /* saved viewport (first visible line) */
    bool            overwrite;   /* saved Ed INS/OVR */
    prov_fileinfo_t info;        /* encoding / line-ending / BOM (detected at load) */
} prov_buf_t;

typedef struct {
    prov_buf_t entries[PROV_MAX_BUFFERS];
    int        count;
    int        active;
} prov_bufset_t;

void prov_bufset_init(prov_bufset_t *bs);

/* Append a buffer (path may be NULL/empty). Returns its index, or -1 if full. */
int  prov_bufset_add(prov_bufset_t *bs, prov_editor_t *ed, const char *path);

/* Remove entry `i` (the caller destroys its editor first). Returns the new
 * active index, or -1 when the set is now empty. */
int  prov_bufset_close(prov_bufset_t *bs, int i);

/* Index of the buffer whose path matches exactly, or -1. */
int  prov_bufset_find(const prov_bufset_t *bs, const char *path);

void prov_bufset_next(prov_bufset_t *bs);   /* cycle active forward (wraps) */
void prov_bufset_prev(prov_bufset_t *bs);   /* cycle active backward (wraps) */

#endif /* PROV_BUFSET_H */
