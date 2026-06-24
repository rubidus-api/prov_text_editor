#include "browser.h"
#include "proven/u8str.h"
#include "proven/array.h"

#if !defined(_WIN32) && !defined(_WIN64)
#include <pwd.h>     /* getpwuid — resolve uid -> user name */
#include <grp.h>     /* getgrgid — resolve gid -> group name */
#endif

static proven_size_t scopy(char *dst, proven_size_t cap, const char *src) {
    proven_size_t i = 0;
    if (cap == 0) return 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
    return i;
}

/* out = dir joined with name ("/" handled; root stays "/name"). */
static void join(const char *dir, const char *name, char *out, proven_size_t cap) {
    proven_size_t n = scopy(out, cap, dir);
    if (n > 0 && out[n - 1] != '/' && n + 1 < cap) { out[n++] = '/'; out[n] = '\0'; }
    scopy(out + n, cap - n, name);
}

void prov_browser_free(prov_browser_t *b, proven_allocator_t a) {
    if (b->entries) a.free_fn(a.ctx, b->entries);
    b->entries = NULL;
    b->count = b->cap = 0;
}

static bool ent_push(prov_browser_t *b, proven_allocator_t a, const prov_dirent_t *e) {
    if (b->count >= b->cap) {
        proven_size_t cap = b->cap ? b->cap * 2 : 32;
        proven_result_mem_mut_t rm = b->entries
            ? a.realloc_fn(a.ctx, b->entries, b->cap * sizeof(prov_dirent_t), cap * sizeof(prov_dirent_t), 16)
            : a.alloc_fn(a.ctx, cap * sizeof(prov_dirent_t), 16);
        if (!PROVEN_IS_OK(rm.err)) return false;
        b->entries = (prov_dirent_t *)rm.value.ptr;
        b->cap = cap;
    }
    b->entries[b->count++] = *e;
    return true;
}

bool prov_browser_load(prov_browser_t *b, proven_allocator_t a, const char *dir) {
    /* Atomic: list the target FIRST and only replace `b` on success, so a failed
     * load (e.g. a permission-denied directory) leaves the current listing — and
     * any view indices into it — intact rather than freed-but-referenced. */
    char newdir[sizeof b->dir];
    proven_size_t dn = scopy(newdir, sizeof newdir, dir && dir[0] ? dir : ".");
    while (dn > 1 && newdir[dn - 1] == '/') newdir[--dn] = '\0';

    proven_result_array_t r = proven_fs_list(a, proven_u8str_view_from_cstr(newdir));
    if (!PROVEN_IS_OK(r.err)) return false;                /* `b` untouched */

    prov_browser_free(b, a);                               /* commit: replace the old listing */
    scopy(b->dir, sizeof b->dir, newdir);

    prov_dirent_t up = { "..", true, PROVEN_FS_TYPE_DIR, 0, PROVEN_FS_PERM_DEFAULT, 0, true, true,
                         0, 0, "", "" };
    ent_push(b, a, &up);                                   /* parent always first (synthetic) */

    for (proven_size_t i = 0; i < r.value.len; i++) {
        const proven_fs_entry_t *fe = proven_array_get(&r.value, i);
        proven_u8str_view_t nv = proven_u8str_as_view((proven_u8str_t *)&fe->name);
        prov_dirent_t e = {0};
        proven_size_t nn = nv.size < sizeof e.name - 1 ? nv.size : sizeof e.name - 1;
        if (nn < nv.size)                              /* truncated: back off to a UTF-8 boundary */
            while (nn > 0 && (nv.ptr[nn] & 0xC0) == 0x80) nn--;
        for (proven_size_t k = 0; k < nn; k++) e.name[k] = (char)nv.ptr[k];
        e.name[nn] = '\0';
        if (e.name[0] == '.' && (e.name[1] == '\0' || (e.name[1] == '.' && e.name[2] == '\0')))
            continue;                                      /* skip "." and a duplicate ".." */
        e.type = fe->type;
        e.is_dir = fe->type == PROVEN_FS_TYPE_DIR;
        e.size = fe->size;                          /* type+size come free from the listing */
        e.perms = PROVEN_FS_PERM_DEFAULT;
        e.stat_done = false;                        /* perms/mtime filled lazily, when shown */
        ent_push(b, a, &e);
    }
    proven_fs_list_destroy(a, &r.value);
    /* proven_fs_list already orders entries dirs-first then by name, so the
     * pushed order (after the leading "..") is exactly what we want. */
    return true;
}

/* Format an unsigned id as decimal into `out` (no libc string funcs). */
static void u64_dec(unsigned long long v, char *out, proven_size_t cap) {
    char tmp[24];
    int n = 0;
    do { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v && n < (int)sizeof tmp);
    proven_size_t o = 0;
    while (n > 0 && o + 1 < cap) out[o++] = tmp[--n];
    if (cap) out[o] = '\0';
}

/* Resolve e->uid/gid to owner/group names (numeric fallback). POSIX uses the
 * passwd/group databases; other platforms (no uid/gid) just show the number. */
static void resolve_ids(prov_dirent_t *e) {
#if !defined(_WIN32) && !defined(_WIN64)
    struct passwd *pw = getpwuid((uid_t)e->uid);
    if (pw && pw->pw_name) scopy(e->owner, sizeof e->owner, pw->pw_name);
    else u64_dec(e->uid, e->owner, sizeof e->owner);
    struct group *gr = getgrgid((gid_t)e->gid);
    if (gr && gr->gr_name) scopy(e->group, sizeof e->group, gr->gr_name);
    else u64_dec(e->gid, e->group, sizeof e->group);
#else
    u64_dec(e->uid, e->owner, sizeof e->owner);
    u64_dec(e->gid, e->group, sizeof e->group);
#endif
}

void prov_browser_resolve_path(const char *base, const char *input,
                               char *out, proven_size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    bool win_sep = false;
#if defined(_WIN32) || defined(_WIN64)
    win_sep = true;
#endif
    bool absolute = input[0] == '/'
                    || (win_sep && (input[0] == '\\' || (input[0] && input[1] == ':')));
    /* assemble the raw path (base + '/' + input, unless input is absolute) */
    char work[2048];
    proven_size_t w = 0;
    if (!absolute) {
        for (proven_size_t i = 0; base[i] && w + 1 < sizeof work; i++) work[w++] = base[i];
        if (w == 0 || work[w - 1] != '/') { if (w + 1 < sizeof work) work[w++] = '/'; }
    }
    for (proven_size_t i = 0; input[i] && w + 1 < sizeof work; i++) {
        char c = input[i];
        if (win_sep && c == '\\') c = '/';
        work[w++] = c;
    }
    work[w] = '\0';
    /* collapse into a segment stack, applying . / .. / empty */
    struct { proven_size_t off, len; } seg[256];
    int ns = 0;
    for (proven_size_t i = 0; work[i]; ) {
        while (work[i] == '/') i++;
        proven_size_t st = i;
        while (work[i] && work[i] != '/') i++;
        proven_size_t ln = i - st;
        if (ln == 0) continue;
        if (ln == 1 && work[st] == '.') continue;
        if (ln == 2 && work[st] == '.' && work[st + 1] == '.') { if (ns > 0) ns--; continue; }
        if (ns < 256) { seg[ns].off = st; seg[ns].len = ln; ns++; }
    }
    if (ns == 0) { if (cap >= 2) { out[0] = '/'; out[1] = '\0'; } return; }   /* root */
    proven_size_t o = 0;
    for (int k = 0; k < ns; k++) {
        if (o + 1 < cap) out[o++] = '/';
        for (proven_size_t j = 0; j < seg[k].len && o + 1 < cap; j++) out[o++] = work[seg[k].off + j];
    }
    out[o] = '\0';
}

void prov_browser_ensure_stat(prov_browser_t *b, proven_allocator_t a, proven_size_t idx) {
    if (idx >= b->count || b->entries[idx].stat_done) return;
    prov_dirent_t *e = &b->entries[idx];
    e->stat_done = true;
    char full[1280];
    join(b->dir, e->name, full, sizeof full);
    proven_fs_stat_t st;
    if (PROVEN_IS_OK(proven_fs_stat(a, proven_u8str_view_from_cstr(full), &st))) {
        e->perms = st.perms;
        e->mtime = st.modified_at;
        e->uid = st.uid;
        e->gid = st.gid;
        resolve_ids(e);
        e->stat_ok = true;
    }
}

void prov_browser_path_at(const prov_browser_t *b, proven_size_t idx, char *out, proven_size_t cap) {
    if (idx >= b->count) { if (cap) out[0] = '\0'; return; }
    const char *name = b->entries[idx].name;
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {   /* parent of b->dir */
        proven_size_t n = scopy(out, cap, b->dir);
        while (n > 1 && out[n - 1] != '/') n--;                  /* drop the last component */
        if (n > 1 && out[n - 1] == '/') n--;                     /* and its separator */
        if (n == 0) n = 1;                                       /* keep root "/" */
        out[n] = '\0';
        if (out[0] == '\0') scopy(out, cap, "/");
        return;
    }
    join(b->dir, name, out, cap);
}
