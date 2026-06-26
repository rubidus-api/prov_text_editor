#ifndef PROV_BROWSER_H
#define PROV_BROWSER_H

/* Directory model for the file-open browser (goal 4). Loads a directory's
 * entries with brief info (type/size/perms/mtime), sorted dirs-first then by
 * name, with a leading ".." parent entry. The UI lives in main.c. */

#include "proven/types.h"
#include "proven/allocator.h"
#include "proven/fs.h"

typedef struct {
    char               name[256];   /* entry name (UTF-8, truncated if longer) */
    bool               is_dir;
    proven_fs_type_t   type;
    proven_size_t      size;
    proven_fs_perms_t  perms;
    proven_i64         mtime;       /* seconds; 0 until stat'd / if unavailable */
    bool               stat_ok;     /* false when info could not be read */
    bool               stat_done;   /* perms/mtime filled (stat is lazy, per row) */
    unsigned long long uid, gid;    /* owner/group ids (filled with perms/mtime) */
    char               owner[40];   /* resolved owner name, or numeric uid as text */
    char               group[40];   /* resolved group name, or numeric gid as text */
    char               label[80];   /* drive-list rows only: "[Vol]  N free / M" (else "") */
} prov_dirent_t;

typedef struct {
    char           dir[1024];       /* the directory being shown (no trailing '/') */
    prov_dirent_t *entries;
    proven_size_t  count, cap;
} prov_browser_t;

/* Load `dir` into `b` (frees any previous content). Returns false on failure
 * (e.g. unreadable directory), leaving `b` empty. */
bool prov_browser_load(prov_browser_t *b, proven_allocator_t a, const char *dir);

/* Free the entry array (keeps the struct reusable). */
void prov_browser_free(prov_browser_t *b, proven_allocator_t a);

/* Lazily fill perms/mtime for entry `idx` (a no-op once done). proven_fs_list
 * already stats every entry for type+size, so this second stat is deferred to
 * the rows actually displayed — opening a huge directory stays cheap. */
void prov_browser_ensure_stat(prov_browser_t *b, proven_allocator_t a, proven_size_t idx);

/* Join b->dir and entries[idx].name into `out` (NUL-terminated, bounded);
 * resolves ".." to the parent of b->dir. */
void prov_browser_path_at(const prov_browser_t *b, proven_size_t idx, char *out, proven_size_t cap);

/* Resolve a typed path (RFC-0015) against `base` into an absolute, normalized
 * path in `out`: an absolute `input` (leading '/', or a drive/`\` on Windows)
 * starts from root, else it is joined onto `base`; `.` / `..` / empty / duplicate
 * separators collapse; a trailing '/' is dropped (root stays "/"). On Windows '\'
 * is also a separator (normalized to '/'); on POSIX '\' is a literal byte. Pure. */
void prov_browser_resolve_path(const char *base, const char *input,
                               char *out, proven_size_t cap);

#endif /* PROV_BROWSER_H */
