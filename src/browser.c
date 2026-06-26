#include "browser.h"
#include "platform.h"        /* prov_platform_list_drives (Windows drives) */
#include "pstr.h"            /* prov_cstr_view */
#include "proven/u8str.h"
#include "proven/array.h"
#include "proven/fmt.h"      /* PROVEN_ARG / append_fmt */

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

/* Pretty-print a byte count as e.g. "12.3G" / "950M" / "512K" / "42". */
static void fmt_bytes(char *out, proven_size_t cap, unsigned long long v) {
    const char *u[] = { "", "K", "M", "G", "T", "P" };
    int ui = 0; unsigned long long whole = v, rem = 0;
    while (whole >= 1024 && ui < 5) { rem = whole % 1024; whole /= 1024; ui++; }
    proven_u8str_t s = proven_u8str_borrow((proven_byte_t *)out, cap);
    if (ui == 0) (void)proven_u8str_append_fmt_trunc(&s, "{}", PROVEN_ARG((unsigned long)whole));
    else (void)proven_u8str_append_fmt_trunc(&s, "{}.{}{}", PROVEN_ARG((unsigned long)whole),
            PROVEN_ARG((unsigned)((rem * 10) / 1024)), PROVEN_ARG(prov_cstr_view(u[ui])));
    (void)proven_u8str_as_cstr(&s);
}

/* The "drives" pseudo-directory (Windows): the parent of a drive root. Loads the
 * mounted drives as navigable rows. Identified by an empty dir string. */
static bool load_drives(prov_browser_t *b, proven_allocator_t a) {
    prov_drive_t dv[26];
    int n = prov_platform_list_drives(dv, 26);
    if (n <= 0) return false;                              /* POSIX / none: caller falls back */
    prov_browser_free(b, a);
    b->dir[0] = '\0';                                      /* empty = the drives screen */
    for (int i = 0; i < n; i++) {
        prov_dirent_t e = {0};
        e.is_dir = true; e.type = PROVEN_FS_TYPE_DIR; e.stat_ok = true; e.stat_done = true;
        e.name[0] = dv[i].letter; e.name[1] = ':'; e.name[2] = '\0';   /* navigates to "X:/" */
        char tot[24] = "?", fre[24] = "?";
        if (dv[i].total) { fmt_bytes(tot, sizeof tot, dv[i].total); fmt_bytes(fre, sizeof fre, dv[i].avail); }
        proven_u8str_t l = proven_u8str_borrow((proven_byte_t *)e.label, sizeof e.label);
        (void)proven_u8str_append_fmt_trunc(&l, "{}{}  {} free / {}",
            PROVEN_ARG(prov_cstr_view(dv[i].label[0] ? dv[i].label : "(no label)")),
            PROVEN_ARG(prov_cstr_view("")), PROVEN_ARG(prov_cstr_view(fre)), PROVEN_ARG(prov_cstr_view(tot)));
        (void)proven_u8str_as_cstr(&l);
        if (!ent_push(b, a, &e)) break;
    }
    return true;
}

bool prov_browser_load(prov_browser_t *b, proven_allocator_t a, const char *dir) {
    /* The drives screen (Windows): an empty dir string lists mounted drives. On
     * POSIX there are no drives, so fall through to load the filesystem root. */
    if (dir && dir[0] == '\0') {
        if (load_drives(b, a)) return true;
        dir = "/";
    }
    /* Atomic: list the target FIRST and only replace `b` on success, so a failed
     * load (e.g. a permission-denied directory) leaves the current listing — and
     * any view indices into it — intact rather than freed-but-referenced. */
    char newdir[sizeof b->dir];
    proven_size_t dn = scopy(newdir, sizeof newdir, dir && dir[0] ? dir : ".");
    while (dn > 1 && newdir[dn - 1] == '/') newdir[--dn] = '\0';
    /* a Windows drive root must keep its slash ("X:/" lists the root, "X:" would
     * mean the current dir on that drive). */
    if (dn == 2 && newdir[1] == ':' && dn + 1 < sizeof newdir) { newdir[dn++] = '/'; newdir[dn] = '\0'; }

    proven_result_array_t r = proven_fs_list(a, proven_u8str_view_from_cstr(newdir));
    if (!PROVEN_IS_OK(r.err)) return false;                /* `b` untouched */

    prov_browser_free(b, a);                               /* commit: replace the old listing */
    scopy(b->dir, sizeof b->dir, newdir);

    prov_dirent_t up = { "..", true, PROVEN_FS_TYPE_DIR, 0, PROVEN_FS_PERM_DEFAULT, 0, true, true,
                         0, 0, "", "", "" };
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
    /* Windows drive: a leading "X:" segment keeps the drive prefix and gets a
     * trailing slash, with no leading slash — "C:", "c:\Users" -> "C:/Users". */
    bool drive0 = win_sep && seg[0].len == 2 && work[seg[0].off + 1] == ':' &&
        ((work[seg[0].off] >= 'A' && work[seg[0].off] <= 'Z') ||
         (work[seg[0].off] >= 'a' && work[seg[0].off] <= 'z'));
    proven_size_t o = 0;
    for (int k = 0; k < ns; k++) {
        if (k == 0 && drive0) {
            char dl = work[seg[0].off];
            if (dl >= 'a' && dl <= 'z') dl = (char)(dl - 32);    /* uppercase the drive */
            if (o + 1 < cap) out[o++] = dl;
            if (o + 1 < cap) out[o++] = ':';
            if (o + 1 < cap) out[o++] = '/';                     /* "X:/" */
            continue;
        }
        if (!(drive0 && k == 1) && o + 1 < cap) out[o++] = '/';   /* no extra '/' right after "X:/" */
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

/* true when `d` is a Windows drive root: "X:" or "X:/". */
static bool is_drive_root(const char *d) {
    return d[0] && d[1] == ':' && (d[2] == '\0' || (d[2] == '/' && d[3] == '\0'));
}

void prov_browser_path_at(const prov_browser_t *b, proven_size_t idx, char *out, proven_size_t cap) {
    if (idx >= b->count) { if (cap) out[0] = '\0'; return; }
    const char *name = b->entries[idx].name;

    if (b->dir[0] == '\0') {                                     /* drives screen: "X:" -> "X:/" */
        proven_size_t n = scopy(out, cap, name);
        if (n + 1 < cap && (n == 0 || out[n - 1] != '/')) { out[n++] = '/'; out[n] = '\0'; }
        return;
    }
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {   /* parent of b->dir */
        if (is_drive_root(b->dir)) { if (cap) out[0] = '\0'; return; }   /* "" = drives screen */
        proven_size_t n = scopy(out, cap, b->dir);
        while (n > 1 && out[n - 1] != '/') n--;                  /* drop the last component */
        out[n] = '\0';
        if (n >= 3 && out[1] == ':') {                          /* leave a drive root as "X:/" */
            out[2] = '/'; out[3] = '\0';
        } else {
            if (n > 1 && out[n - 1] == '/') out[--n] = '\0';     /* POSIX: drop the separator */
            if (out[0] == '\0') scopy(out, cap, "/");
        }
        return;
    }
    join(b->dir, name, out, cap);
}
