#include "platform_charset.h"

#include "proven/error.h"
#include "proven/heap.h"

#include <string.h>

/* ===================================================================== *
 *  Backend dispatch: a registry of probed backends, one selected active. *
 * ===================================================================== */

static const prov_charset_backend_t *g_active = NULL;
static bool g_resolved = false;            /* has the active backend been picked yet? */
static char g_pref[24] = "auto";           /* configured preference (no probing until used) */
static struct { char enc[40]; bool ok; } g_supcache[32];   /* (active backend, enc) -> supported */
static int  g_supn = 0;

/* --------------------------------------------------------------------- */
#ifdef _WIN32
/* ---- backend: win32 (MultiByteToWideChar / WideCharToMultiByte) ------ */
#include <windows.h>

/* Canonical prov encoding name -> Windows code page. */
static unsigned win_codepage(const char *enc) {
    if (!strcmp(enc, "CP949") || !strcmp(enc, "EUC-KR")) return 949;
    if (!strcmp(enc, "JOHAB"))                            return 1361;
    if (!strcmp(enc, "SHIFT_JIS") || !strcmp(enc, "CP932")) return 932;
    if (!strcmp(enc, "EUC-JP"))                           return 20932;
    if (!strcmp(enc, "GBK") || !strcmp(enc, "CP936"))     return 936;
    if (!strcmp(enc, "GB18030"))                          return 54936;
    if (!strcmp(enc, "BIG5") || !strcmp(enc, "CP950"))    return 950;
    if (!strcmp(enc, "ISO-8859-1"))                       return 28591;
    if (!strcmp(enc, "KOI8-R"))                           return 20866;
    return 0;
}

static bool win_probe(void) { return true; }

static bool win_supports(const char *enc) {
    unsigned cp = win_codepage(enc);
    return cp != 0 && IsValidCodePage(cp);     /* the code page is installed on this host */
}

static proven_u8 *win_to_utf8(proven_allocator_t a, const char *enc,
                              const proven_u8 *b, proven_size_t n, proven_size_t *outn) {
    unsigned cp = win_codepage(enc);
    if (!cp || n == 0) { if (n == 0) { *outn = 0; proven_result_mem_mut_t r = a.alloc_fn(a.ctx, 1, 16); return PROVEN_IS_OK(r.err) ? (proven_u8 *)r.value.ptr : NULL; } return NULL; }
    int wn = MultiByteToWideChar(cp, 0, (const char *)b, (int)n, NULL, 0);
    if (wn <= 0) return NULL;
    wchar_t *w = (wchar_t *)a.alloc_fn(a.ctx, (proven_size_t)wn * sizeof(wchar_t), 16).value.ptr;
    if (!w) return NULL;
    MultiByteToWideChar(cp, 0, (const char *)b, (int)n, w, wn);
    int un = WideCharToMultiByte(CP_UTF8, 0, w, wn, NULL, 0, NULL, NULL);
    proven_u8 *out = (proven_u8 *)a.alloc_fn(a.ctx, un > 0 ? (proven_size_t)un : 1, 16).value.ptr;
    if (out && un > 0) WideCharToMultiByte(CP_UTF8, 0, w, wn, (char *)out, un, NULL, NULL);
    a.free_fn(a.ctx, w);
    if (!out) return NULL;
    *outn = un > 0 ? (proven_size_t)un : 0;
    return out;
}

static proven_u8 *win_from_utf8(proven_allocator_t a, const char *enc,
                                const proven_u8 *b, proven_size_t n, proven_size_t *outn) {
    unsigned cp = win_codepage(enc);
    if (!cp) return NULL;
    int wn = MultiByteToWideChar(CP_UTF8, 0, (const char *)b, (int)n, NULL, 0);
    if (wn < 0) return NULL;
    wchar_t *w = (wchar_t *)a.alloc_fn(a.ctx, (proven_size_t)(wn ? wn : 1) * sizeof(wchar_t), 16).value.ptr;
    if (!w) return NULL;
    if (wn > 0) MultiByteToWideChar(CP_UTF8, 0, (const char *)b, (int)n, w, wn);
    int on = WideCharToMultiByte(cp, 0, w, wn, NULL, 0, NULL, NULL);
    proven_u8 *out = (proven_u8 *)a.alloc_fn(a.ctx, on > 0 ? (proven_size_t)on : 1, 16).value.ptr;
    if (out && on > 0) WideCharToMultiByte(cp, 0, w, wn, (char *)out, on, NULL, NULL);  /* unmappable -> default '?' */
    a.free_fn(a.ctx, w);
    if (!out) return NULL;
    *outn = on > 0 ? (proven_size_t)on : 0;
    return out;
}

static const prov_charset_backend_t BK_WIN32 = { "win32", win_probe, win_supports, win_to_utf8, win_from_utf8 };

#else  /* ---------------------------------- POSIX ---------------------- */
#include <iconv.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* ---- backend: libc (the linked iconv) -------------------------------- */
static proven_u8 *iconv_run(proven_allocator_t a, const char *from, const char *to,
                            const proven_u8 *b, proven_size_t n, proven_size_t *outn) {
    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) return NULL;
    proven_size_t cap = n * 4 + 16;
    proven_result_mem_mut_t rm = a.alloc_fn(a.ctx, cap, 16);
    if (!PROVEN_IS_OK(rm.err)) { iconv_close(cd); return NULL; }
    proven_u8 *out = (proven_u8 *)rm.value.ptr;
    char *inp = (char *)(void *)b; size_t inleft = n;
    char *outp = (char *)out; size_t outleft = cap;
    while (inleft > 0) {
        size_t r = iconv(cd, &inp, &inleft, &outp, &outleft);
        if (r != (size_t)-1) break;
        if (errno == E2BIG) {                                  /* grow the output */
            proven_size_t used = cap - outleft, ncap = cap * 2;
            proven_result_mem_mut_t gr = a.realloc_fn(a.ctx, out, cap, ncap, 16);
            if (!PROVEN_IS_OK(gr.err)) { iconv_close(cd); a.free_fn(a.ctx, out); return NULL; }
            out = (proven_u8 *)gr.value.ptr; cap = ncap;
            outp = (char *)out + used; outleft = cap - used;
            continue;
        }
        if (errno == EILSEQ || errno == EINVAL) { inp++; inleft--; continue; }  /* skip one bad byte (lossy) */
        iconv_close(cd); a.free_fn(a.ctx, out); return NULL;
    }
    /* flush shift state */
    (void)iconv(cd, NULL, NULL, &outp, &outleft);
    iconv_close(cd);
    *outn = cap - outleft;
    return out;
}

static bool libc_probe(void) {
    iconv_t cd = iconv_open("UTF-8", "UTF-8");
    if (cd == (iconv_t)-1) return false;
    iconv_close(cd);
    return true;
}
static bool libc_supports(const char *enc) {       /* iconv knows this encoding? */
    iconv_t cd = iconv_open("UTF-8", enc);
    if (cd == (iconv_t)-1) return false;
    iconv_close(cd);
    return true;
}
static proven_u8 *libc_to_utf8(proven_allocator_t a, const char *enc,
                               const proven_u8 *b, proven_size_t n, proven_size_t *outn) {
    return iconv_run(a, enc, "UTF-8", b, n, outn);
}
static proven_u8 *libc_from_utf8(proven_allocator_t a, const char *enc,
                                 const proven_u8 *b, proven_size_t n, proven_size_t *outn) {
    char to[64];                                   /* //TRANSLIT: substitute unmappable chars */
    snprintf(to, sizeof to, "%s//TRANSLIT", enc);
    return iconv_run(a, "UTF-8", to, b, n, outn);
}
static const prov_charset_backend_t BK_LIBC = { "libc", libc_probe, libc_supports, libc_to_utf8, libc_from_utf8 };

#endif  /* _WIN32 / POSIX */

/* ---- backend: command (the external `iconv` tool) — every OS ----------- *
 * iconv (or iconv.exe) may live on PATH or anywhere the user points us, so this
 * backend exists on Windows too — it just shells out. The executable is named by
 * `g_iconv_path` (default the bare "iconv", PATH-resolved). Input goes through a
 * temp file (no bidirectional pipe); output is read from the child's stdout. */
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#  include <windows.h>
#  define PROV_POPEN  _popen
#  define PROV_PCLOSE _pclose
#  define PROV_QUOTE  '"'                 /* cmd.exe quoting */
#else
#  include <unistd.h>
#  define PROV_POPEN  popen
#  define PROV_PCLOSE pclose
#  define PROV_QUOTE  '\''                /* POSIX shell quoting */
#endif
#ifdef MAX_PATH
#  define MAX_PATH_FALLBACK MAX_PATH
#else
#  define MAX_PATH_FALLBACK 4096
#endif

static char g_iconv_path[260] = "iconv";  /* the iconv executable: PATH name or a full path */

void prov_charset_set_iconv_path(const char *path) {
    const char *p = (path && path[0]) ? path : "iconv";
    size_t i = 0;
    for (; p[i] && i + 1 < sizeof g_iconv_path; i++) g_iconv_path[i] = p[i];
    g_iconv_path[i] = '\0';
    g_resolved = false; g_active = NULL; g_supn = 0;   /* re-probe with the new tool */
}

static bool enc_name_ok(const char *e) {            /* guard the encoding args (no metachars) */
    for (; *e; e++) if (!((*e >= 'A' && *e <= 'Z') || (*e >= 'a' && *e <= 'z') ||
                          (*e >= '0' && *e <= '9') || *e == '-' || *e == '_' || *e == '/')) return false;
    return true;   /* '/' allowed for the //TRANSLIT suffix; none of these are shell-special */
}
/* Open a fresh temp file for writing (binary) + return its path in `path`. */
static FILE *cmd_tmp(char *path, size_t cap) {
#ifdef _WIN32
    char dir[MAX_PATH];
    if (!GetTempPathA((DWORD)sizeof dir, dir)) return NULL;
    if (!GetTempFileNameA(dir, "prv", 0, path)) return NULL;   /* creates the file */
    (void)cap;
    return fopen(path, "wb");
#else
    if (cap < 24) return NULL;
    for (size_t i = 0; "/tmp/prov-iconv-XXXXXX"[i]; i++) path[i] = "/tmp/prov-iconv-XXXXXX"[i];
    path[22] = '\0';
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    return fdopen(fd, "wb");
#endif
}
static proven_u8 *cmd_run(proven_allocator_t a, const char *from, const char *to,
                          const proven_u8 *b, proven_size_t n, proven_size_t *outn) {
    if (!enc_name_ok(from) || !enc_name_ok(to)) return NULL;
    char tpath[MAX_PATH_FALLBACK];
    FILE *tf = cmd_tmp(tpath, sizeof tpath);
    if (!tf) return NULL;
    if (n > 0 && fwrite(b, 1, n, tf) != (size_t)n) { fclose(tf); remove(tpath); return NULL; }
    fclose(tf);
    char cmd[MAX_PATH_FALLBACK + 512];              /* "PATH" -f FROM -t TO -- "TMP" */
    snprintf(cmd, sizeof cmd, "%c%s%c -f %s -t %s -- %c%s%c 2>%s",
             PROV_QUOTE, g_iconv_path, PROV_QUOTE, from, to, PROV_QUOTE, tpath, PROV_QUOTE,
#ifdef _WIN32
             "NUL");
#else
             "/dev/null");
#endif
    FILE *p = PROV_POPEN(cmd, "r");
    if (!p) { remove(tpath); return NULL; }
    proven_size_t cap = n * 4 + 64, used = 0;
    proven_result_mem_mut_t rm = a.alloc_fn(a.ctx, cap, 16);
    if (!PROVEN_IS_OK(rm.err)) { PROV_PCLOSE(p); remove(tpath); return NULL; }
    proven_u8 *out = (proven_u8 *)rm.value.ptr;
    for (;;) {
        if (used == cap) {
            proven_size_t ncap = cap * 2;
            proven_result_mem_mut_t gr = a.realloc_fn(a.ctx, out, cap, ncap, 16);
            if (!PROVEN_IS_OK(gr.err)) { PROV_PCLOSE(p); remove(tpath); a.free_fn(a.ctx, out); return NULL; }
            out = (proven_u8 *)gr.value.ptr; cap = ncap;
        }
        size_t got = fread(out + used, 1, cap - used, p);
        used += got;
        if (got == 0) break;
    }
    int rc = PROV_PCLOSE(p);
    remove(tpath);
    if (rc != 0) { a.free_fn(a.ctx, out); return NULL; }
    *outn = used;
    return out;
}
static bool cmd_probe(void) {
    char cmd[300];
    snprintf(cmd, sizeof cmd, "%c%s%c --version >%s 2>&1",
             PROV_QUOTE, g_iconv_path, PROV_QUOTE,
#ifdef _WIN32
             "NUL");
#else
             "/dev/null");
#endif
    return system(cmd) == 0;
}
static bool cmd_supports(const char *enc) {        /* `iconv -f ENC` accepts the name? */
    proven_allocator_t a = proven_heap_allocator();
    proven_size_t on = 0;
    proven_u8 *r = cmd_run(a, enc, "UTF-8", (const proven_u8 *)"A", 1, &on);
    if (!r) return false;
    a.free_fn(a.ctx, r);
    return true;
}
static proven_u8 *cmd_to_utf8(proven_allocator_t a, const char *enc,
                              const proven_u8 *b, proven_size_t n, proven_size_t *outn) {
    return cmd_run(a, enc, "UTF-8", b, n, outn);
}
static proven_u8 *cmd_from_utf8(proven_allocator_t a, const char *enc,
                                const proven_u8 *b, proven_size_t n, proven_size_t *outn) {
    char to[64]; snprintf(to, sizeof to, "%s//TRANSLIT", enc);
    return cmd_run(a, "UTF-8", to, b, n, outn);
}
static const prov_charset_backend_t BK_CMD = { "command", cmd_probe, cmd_supports, cmd_to_utf8, cmd_from_utf8 };

/* --------------------------------------------------------------------- */
/* Registry, in preference order (first that probes OK wins under "auto"). */
static const prov_charset_backend_t *const REGISTRY[] = {
#ifdef _WIN32
    &BK_WIN32,
#else
    &BK_LIBC,
#endif
    &BK_CMD,                  /* external iconv: available on every OS (probed at use) */
};

int prov_charset_backend_names(const char **out, int max) {
    int nreg = (int)(sizeof REGISTRY / sizeof REGISTRY[0]);
    for (int i = 0; i < nreg && i < max; i++) out[i] = REGISTRY[i]->name;
    return nreg;
}

/* Resolve the active backend once (probes run here, at first real use — not at
 * startup). Config preference wins when it probes OK, else first working. */
static const prov_charset_backend_t *active_backend(void) {
    if (g_resolved) return g_active;
    g_resolved = true;
    g_active = NULL;
    size_t nreg = sizeof REGISTRY / sizeof REGISTRY[0];
    if (g_pref[0] && strcmp(g_pref, "auto") != 0)
        for (size_t i = 0; i < nreg; i++)
            if (!strcmp(REGISTRY[i]->name, g_pref) && REGISTRY[i]->probe()) { g_active = REGISTRY[i]; return g_active; }
    for (size_t i = 0; i < nreg; i++)
        if (REGISTRY[i]->probe()) { g_active = REGISTRY[i]; return g_active; }
    return g_active;
}

void prov_charset_configure(const char *preferred) {
    const char *p = (preferred && preferred[0]) ? preferred : "auto";
    size_t i = 0;
    for (; p[i] && i + 1 < sizeof g_pref; i++) g_pref[i] = p[i];
    g_pref[i] = '\0';
    g_resolved = false; g_active = NULL; g_supn = 0;   /* re-resolve + drop the support cache */
}

const char *prov_charset_active(void) {
    const prov_charset_backend_t *b = active_backend();
    return b ? b->name : NULL;
}

proven_u8 *prov_charset_to_utf8(proven_allocator_t a, const char *enc,
                                const proven_u8 *b, proven_size_t n, proven_size_t *outn) {
    const prov_charset_backend_t *bk = active_backend();
    return bk ? bk->to_utf8(a, enc, b, n, outn) : NULL;
}
proven_u8 *prov_charset_from_utf8(proven_allocator_t a, const char *enc,
                                  const proven_u8 *b, proven_size_t n, proven_size_t *outn) {
    const prov_charset_backend_t *bk = active_backend();
    return bk ? bk->from_utf8(a, enc, b, n, outn) : NULL;
}

bool prov_charset_supports(const char *enc) {
    const prov_charset_backend_t *bk = active_backend();
    if (!bk) return false;
    for (int i = 0; i < g_supn; i++)                  /* cache hit: checked already this run */
        if (!strcmp(g_supcache[i].enc, enc)) return g_supcache[i].ok;
    bool ok = bk->supports(enc);
    if (g_supn < (int)(sizeof g_supcache / sizeof g_supcache[0])) {   /* memoize */
        size_t k = 0;
        for (; enc[k] && k + 1 < sizeof g_supcache[0].enc; k++) g_supcache[g_supn].enc[k] = enc[k];
        g_supcache[g_supn].enc[k] = '\0';
        g_supcache[g_supn].ok = ok;
        g_supn++;
    }
    return ok;
}
