/*
 * nob.c - build driver for the prov text editor.
 *
 * Pure standard C plus the public-domain nob.h single-header. No make/cmake.
 *
 * Bootstrap:   cc -o nob nob.c
 * Usage:       ./nob              incremental debug build -> bin/prov
 *              ./nob test         build, then build and run tests/*.c
 *              ./nob --release    optimized build (-O2 -flto -DNDEBUG, --gc-sections, strip)
 *              ./nob --clean      remove build/ and bin/
 *
 * Incremental model: every translation unit is compiled to its own object
 * under build/obj/. Header dependencies are tracked via the compiler's
 * `-MMD` depfiles, which are fed back into nob_needs_rebuild so a touched
 * header rebuilds exactly the objects that include it. Linking is skipped
 * when every object is older than the output.
 *
 * Layout assumptions (see SPEC.md / AGENTS.md):
 *   src/            editor core (.c) + future headers
 *   src/proven/     vendored proven library sources
 *   platform/       vendored proven PAL + future prov platform backends
 *   include/        public headers (proven/ namespace)
 *   tests/          unit tests (one main() per file, exit 0 == pass)
 */

#define NOB_IMPLEMENTATION
#include "nob.h"

#include <stdlib.h>
#include <string.h>

#define BUILD_DIR "build"
#define OBJ_DIR   "build/obj"
#define TEST_DIR  "build/test"
#define BIN_DIR   "bin"
#define BIN_PATH  "bin/prov"

static const char *g_cc  = NULL;  /* compiler, $CC or "cc"            */
static const char *g_std = NULL;  /* "-std=c23" or "-std=c2x"         */
static int         g_release = 0;
static int         g_win64 = 0;             /* cross-compile for Windows x64 */
static int         g_no_float = 0;          /* opt in to -DPROVEN_FMT_NO_FLOAT */
static const char *g_objdir  = OBJ_DIR;     /* per-target object directory   */
static const char *g_binpath = BIN_PATH;    /* per-target output binary      */
static const char *g_term_backend = "platform/platform_term_posix.c";

/* `dist` cross-build matrix: each produces an optimized, size-reduced binary
 * `bin/prov-<suffix>[.exe]`. A target is skipped (not fatal) when its toolchain
 * is absent, so `./nob dist` builds whatever the host can. Run it on a machine
 * with the cross toolchains installed to get them all.
 *
 * Note: `linux-arm64` is glibc aarch64 (Raspberry Pi 64-bit, aarch64 Linux, and
 * Termux under its glibc layer). A native Android/Termux (bionic) build needs
 * the Android NDK clang — add it as a target once the NDK is available. */
typedef struct {
    const char *suffix;    /* bin/prov-<suffix>[.exe] */
    const char *cc;        /* toolchain; the target is skipped when it is absent */
    const char *backend;   /* terminal backend .c for this platform */
    bool        windows;   /* win32 backend + .exe + -lwinpthread */
} dist_target_t;

static const dist_target_t DIST_TARGETS[] = {
    { "linux-x64",   "cc",                      "platform/platform_term_posix.c", false },
    { "linux-arm64", "aarch64-linux-gnu-gcc",   "platform/platform_term_posix.c", false },
    { "linux-armhf", "arm-linux-gnueabihf-gcc", "platform/platform_term_posix.c", false },
    { "windows-x64", "x86_64-w64-mingw32-gcc",  "platform/platform_term_win32.c", true  },
};

/* ------------------------------------------------------------------ utils */

static char *dupstr(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (!p) { nob_log(NOB_ERROR, "out of memory"); exit(1); }
    memcpy(p, s, n);
    return p;
}

/* Collect *.c files (non-recursive) from `dir` into `out` as "dir/name". */
static bool collect_sources(const char *dir, Nob_File_Paths *out) {
    Nob_File_Paths entries = {0};
    if (!nob_read_entire_dir(dir, &entries)) return false;
    for (size_t i = 0; i < entries.count; i++) {
        const char *name = entries.items[i];
        if (name[0] == '.') continue;                  /* ., .., dotfiles */
        size_t l = strlen(name);
        if (l < 2 || name[l - 2] != '.' || name[l - 1] != 'c') continue;
        char buf[1024];
        snprintf(buf, sizeof buf, "%s/%s", dir, name);
        nob_da_append(out, dupstr(buf));
    }
    return true;
}

/* Collect *.c files whose basename starts with `prefix` from `dir`. */
static bool collect_prefixed(const char *dir, const char *prefix,
                             Nob_File_Paths *out) {
    Nob_File_Paths entries = {0};
    if (!nob_read_entire_dir(dir, &entries)) return false;
    size_t pl = strlen(prefix);
    for (size_t i = 0; i < entries.count; i++) {
        const char *name = entries.items[i];
        if (name[0] == '.') continue;
        size_t l = strlen(name);
        if (l < 2 || name[l - 2] != '.' || name[l - 1] != 'c') continue;
        if (strncmp(name, prefix, pl) != 0) continue;
        char buf[1024];
        snprintf(buf, sizeof buf, "%s/%s", dir, name);
        nob_da_append(out, dupstr(buf));
    }
    return true;
}

/* Map a source path to its object/dep paths under g_objdir (flattened name). */
static void make_artifacts(const char *src, char *obj, size_t objn,
                           char *dep, size_t depn) {
    char flat[1024];
    size_t j = 0;
    for (size_t i = 0; src[i] && j < sizeof(flat) - 1; i++) {
        char c = src[i];
        flat[j++] = (c == '/' || c == '.') ? '_' : c;
    }
    flat[j] = '\0';
    snprintf(obj, objn, "%s/%s.o", g_objdir, flat);
    snprintf(dep, depn, "%s/%s.d", g_objdir, flat);
}

/* True if `obj` must be rebuilt, accounting for the source and the header
 * prerequisites recorded in the `-MMD` depfile. */
static bool obj_needs_rebuild(const char *obj, const char *dep,
                              const char *src) {
    Nob_File_Paths inputs = {0};
    nob_da_append(&inputs, src);

    Nob_String_Builder sb = {0};
    bool have_dep = nob_file_exists(dep) && nob_read_entire_file(dep, &sb);
    if (have_dep) {
        nob_da_append(&sb, '\0');
        char *p = sb.items;
        char *end = sb.items + sb.count;
        while (p < end) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' ||
                               *p == '\r' || *p == '\\' || *p == '\0')) p++;
            if (p >= end) break;
            char *tok = p;
            while (p < end && !(*p == ' ' || *p == '\t' || *p == '\n' ||
                                *p == '\r' || *p == '\\' || *p == '\0')) p++;
            *p = '\0';
            p++;
            size_t tl = strlen(tok);
            if (tl == 0) continue;
            if (tok[tl - 1] == ':') continue;          /* the make target */
            nob_da_append(&inputs, tok);
        }
    }

    int r = nob_needs_rebuild(obj, inputs.items, inputs.count);
    free(inputs.items);
    free(sb.items);
    return r != 0;   /* -1 (error) is treated as "rebuild" */
}

/* Compile one source. Returns a heap-owned object path, or NULL on failure. */
static char *compile_object(const char *src) {
    char obj[1024], dep[1024];
    make_artifacts(src, obj, sizeof obj, dep, sizeof dep);

    if (obj_needs_rebuild(obj, dep, src)) {
        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, g_cc, g_std, "-Wall", "-Wextra");
        /* Float formatting via proven's `{}` selector is KEPT by default — prov
         * may grow float input/output later, and we don't want to foreclose that.
         * `./nob --no-float` opts in to -DPROVEN_FMT_NO_FLOAT, which drops the
         * float formatter so --gc-sections/LTO reclaim ~8 KB (float_format +
         * float_decimal tables). Only enable it once prov is confirmed float-free. */
        if (g_no_float) nob_cmd_append(&cmd, "-DPROVEN_FMT_NO_FLOAT");
        if (g_release) {
            /* -O2 for edit-throughput (interactive editing is imperceptible
             * either way, but -O2 keeps bulk/macro edits ~1.3-1.7x faster than
             * -Os for ~40 KB more). The size levers are retained: per-function/
             * data sections let --gc-sections drop every unreferenced symbol
             * (most of the vendored proven library is unused); -flto culls dead
             * code across translation units; -fno-ident drops the compiler tag. */
            nob_cmd_append(&cmd, "-O2", "-DNDEBUG", "-flto", "-fno-ident",
                           "-ffunction-sections", "-fdata-sections");
            /* The editor has no C++/exceptions, so the DWARF unwind tables
             * (.eh_frame, ~13% of the binary, kept even after strip) are dead
             * weight. The Win64 ABI requires unwind data (.pdata/.xdata), so
             * only drop them on POSIX targets. */
            if (!g_win64)
                nob_cmd_append(&cmd, "-fno-asynchronous-unwind-tables", "-fno-unwind-tables");
        } else {
            nob_cmd_append(&cmd, "-g", "-O0");
        }
        /* POSIX feature macros only apply to the native build; mingw selects
         * Windows code paths via the compiler-defined _WIN32. */
        if (!g_win64)
            nob_cmd_append(&cmd, "-D_DEFAULT_SOURCE", "-D_POSIX_C_SOURCE=200809L");
        nob_cmd_append(&cmd, "-Iinclude", "-Iplatform", "-Isrc");
        nob_cmd_append(&cmd, "-MMD", "-MF", dep, "-c", src, "-o", obj);
        if (!nob_cmd_run_sync_and_reset(&cmd)) return NULL;
    }
    return dupstr(obj);
}

static bool compile_all(const Nob_File_Paths *srcs, Nob_File_Paths *objs) {
    for (size_t i = 0; i < srcs->count; i++) {
        char *o = compile_object(srcs->items[i]);
        if (!o) return false;
        nob_da_append(objs, o);
    }
    return true;
}

/* Link objects in `a` and `b` into `out`. Either group may be empty. */
static bool link_exe(const char *out, const Nob_File_Paths *a,
                     const Nob_File_Paths *b) {
    Nob_File_Paths all = {0};
    for (size_t i = 0; i < a->count; i++) nob_da_append(&all, a->items[i]);
    if (b) for (size_t i = 0; i < b->count; i++) nob_da_append(&all, b->items[i]);

    bool ok = true;
    if (nob_needs_rebuild(out, all.items, all.count) != 0) {
        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, g_cc, "-o", out);
        for (size_t i = 0; i < all.count; i++) nob_cmd_append(&cmd, all.items[i]);
        /* Release: LTO link, drop unreferenced sections, strip symbols. The
         * ELF-only hygiene flags (--as-needed prunes unused DT_NEEDED such as
         * libm/libpthread; --build-id=none drops the note) are POSIX-only. */
        if (g_release) {
            nob_cmd_append(&cmd, "-flto", "-Wl,--gc-sections", "-s");
            if (!g_win64)
                nob_cmd_append(&cmd, "-Wl,--as-needed", "-Wl,--build-id=none");
        }
        if (g_win64) nob_cmd_append(&cmd, "-lwinpthread");
        else         nob_cmd_append(&cmd, "-lm", "-lpthread");
        ok = nob_cmd_run_sync_and_reset(&cmd);
    } else {
        nob_log(NOB_INFO, "%s is up to date", out);
    }
    free(all.items);
    return ok;
}

/* --------------------------------------------------------------- std probe */

static const char *detect_std(void) {
    static const char *cands[] = { "-std=c23", "-std=c2x" };
    for (size_t i = 0; i < NOB_ARRAY_LEN(cands); i++) {
        /* Redirect the probe to /dev/null so a rejected flag stays quiet. */
        Nob_Fd devnull = nob_fd_open_for_write("/dev/null");
        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, g_cc, cands[i], "-x", "c", "-c",
                       "/dev/null", "-o", "/dev/null");
        Nob_Cmd_Redirect redir = { .fdout = &devnull, .fderr = &devnull };
        bool ok = nob_cmd_run_sync_redirect(cmd, redir);
        cmd.count = 0;
        nob_fd_close(devnull);
        if (ok) {
            nob_log(NOB_INFO, "using C standard flag %s", cands[i]);
            return cands[i];
        }
    }
    return NULL;
}

/* --------------------------------------------------------------- targets   */

static bool ensure_dirs(void) {
    if (!nob_mkdir_if_not_exists(BUILD_DIR)) return false;
    if (g_win64 && !nob_mkdir_if_not_exists("build/win64")) return false;
    return nob_mkdir_if_not_exists(g_objdir)
        && nob_mkdir_if_not_exists(BIN_DIR);
}

/* Compile the vendored proven library (src/proven + the proven PAL) plus the
 * terminal backend selected for the current target. */
static bool build_lib(Nob_File_Paths *lib_objs) {
    Nob_File_Paths srcs = {0};
    if (!collect_sources("src/proven", &srcs)) return false;
    if (!collect_prefixed("platform", "proven_sys_", &srcs)) return false;
    nob_da_append(&srcs, dupstr(g_term_backend));
    nob_da_append(&srcs, dupstr("platform/platform_clipboard.c"));  /* posix + win32 via #ifdef */
    nob_da_append(&srcs, dupstr("platform/platform_charset.c"));    /* iconv / win32 / command backends */
    return compile_all(&srcs, lib_objs);
}

/* Compile the editor (src/*.c), splitting the entrypoint (main.c) from the
 * reusable core so tests can link the core without a duplicate main(). */
static bool build_editor(Nob_File_Paths *core_objs, Nob_File_Paths *main_objs) {
    Nob_File_Paths srcs = {0};
    if (!collect_sources("src", &srcs)) return false;
    for (size_t i = 0; i < srcs.count; i++) {
        const char *src = srcs.items[i];
        char *o = compile_object(src);
        if (!o) return false;
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;
        if (strcmp(base, "main.c") == 0) nob_da_append(main_objs, o);
        else nob_da_append(core_objs, o);
    }
    return true;
}

/* Append all items of `src` onto `dst`. */
static void paths_extend(Nob_File_Paths *dst, const Nob_File_Paths *src) {
    for (size_t i = 0; i < src->count; i++) nob_da_append(dst, src->items[i]);
}

/* `base_objs` holds the library + core objects every test links against. */
static bool run_tests(const Nob_File_Paths *base_objs) {
    if (!nob_mkdir_if_not_exists(TEST_DIR)) return false;

    Nob_File_Paths tests = {0};
    if (!collect_sources("tests", &tests)) return false;
    if (tests.count == 0) {
        nob_log(NOB_INFO, "no tests found under tests/");
        return true;
    }

    size_t total = 0, failures = 0;
    for (size_t i = 0; i < tests.count; i++) {
        const char *src = tests.items[i];

        char *obj = compile_object(src);
        if (!obj) { failures++; total++; continue; }

        /* derive bin name: tests/test_x.c -> build/test/test_x */
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;
        char name[1024];
        snprintf(name, sizeof name, "%s", base);
        size_t nl = strlen(name);
        if (nl >= 2 && name[nl - 2] == '.' && name[nl - 1] == 'c')
            name[nl - 2] = '\0';
        char binpath[1024];
        snprintf(binpath, sizeof binpath, "%s/%s", TEST_DIR, name);

        Nob_File_Paths one = {0};
        nob_da_append(&one, obj);
        if (!link_exe(binpath, &one, base_objs)) { free(one.items); free(obj); failures++; total++; continue; }
        free(one.items);
        free(obj);

        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, binpath);
        bool ok = nob_cmd_run_sync_and_reset(&cmd);
        total++;
        if (!ok) { failures++; nob_log(NOB_ERROR, "test FAILED: %s", binpath); }
    }

    nob_log(failures ? NOB_ERROR : NOB_INFO,
            "tests: %zu passed, %zu failed (of %zu)",
            total - failures, failures, total);
    return failures == 0;
}

static bool clean(void) {
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "rm", "-rf", BUILD_DIR, BIN_DIR);
    return nob_cmd_run_sync_and_reset(&cmd);
}

/* Build one dist target (always release/size-reduced) into bin/prov-<suffix>.
 * Skipped — not an error — when the toolchain is missing. */
static bool build_one_dist(const dist_target_t *t) {
    g_cc = t->cc;
    /* Probe the toolchain quietly — a missing compiler makes the std probe fail,
     * which we report as a skip rather than letting nob log it as an error. */
    Nob_Log_Level saved = nob_minimal_log_level;
    nob_minimal_log_level = NOB_NO_LOGS;
    g_std = detect_std();
    nob_minimal_log_level = saved;
    if (!g_std) {
        nob_log(NOB_WARNING, "dist: skipping %-12s (no working %s)", t->suffix, t->cc);
        return true;
    }
    nob_log(NOB_INFO, "dist: building %-12s with %s (%s)", t->suffix, t->cc, g_std);
    g_release = 1;
    g_win64 = t->windows;
    g_term_backend = t->backend;

    char objdir[256], binpath[256];
    snprintf(objdir, sizeof objdir, "build/dist/%s", t->suffix);
    snprintf(binpath, sizeof binpath, "bin/prov-%s%s", t->suffix, t->windows ? ".exe" : "");
    g_objdir = dupstr(objdir);
    g_binpath = dupstr(binpath);

    if (!nob_mkdir_if_not_exists("build/dist") || !nob_mkdir_if_not_exists(g_objdir))
        return false;

    Nob_File_Paths lib = {0}, core = {0}, mains = {0}, all = {0};
    bool ok = build_lib(&lib) && build_editor(&core, &mains);
    if (ok) {
        paths_extend(&all, &core);
        paths_extend(&all, &mains);
        ok = link_exe(g_binpath, &lib, &all);
    }
    if (ok) nob_log(NOB_INFO, "built %s", g_binpath);
    return ok;
}

/* Build every available dist target. */
static bool build_dist(void) {
    if (!nob_mkdir_if_not_exists(BUILD_DIR) || !nob_mkdir_if_not_exists(BIN_DIR))
        return false;
    bool ok = true;
    for (size_t i = 0; i < NOB_ARRAY_LEN(DIST_TARGETS); i++)
        if (!build_one_dist(&DIST_TARGETS[i])) ok = false;
    return ok;
}

static void usage(const char *prog) {
    printf("usage: %s [win64|dist] [--release] [--clean] [test]\n", prog);
    printf("  (no args)   incremental native debug build -> %s\n", BIN_PATH);
    printf("  win64       cross-compile for Windows x64 -> bin/prov.exe\n");
    printf("              (needs x86_64-w64-mingw32-gcc; runs on the build host)\n");
    printf("  dist        build optimized, size-reduced binaries for every available\n");
    printf("              toolchain -> bin/prov-<platform>[.exe] (skips missing ones)\n");
    printf("  test        build and run unit tests in tests/ (native only)\n");
    printf("  --release   optimized build (-O2 -flto -DNDEBUG, --gc-sections, strip)\n");
    printf("  --no-float  drop proven's float `{}` formatter (-DPROVEN_FMT_NO_FLOAT);\n");
    printf("              reclaims ~8 KB in release. Only when prov uses no float I/O.\n");
    printf("  --clean     remove %s/ and %s/\n", BUILD_DIR, BIN_DIR);
}

int main(int argc, char **argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);

    bool do_clean = false, do_test = false, do_dist = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--clean") || !strcmp(a, "-clean")) do_clean = true;
        else if (!strcmp(a, "--release") || !strcmp(a, "-release") || !strcmp(a, "-r")) g_release = 1;
        else if (!strcmp(a, "test")) do_test = true;
        else if (!strcmp(a, "win64")) g_win64 = 1;
        else if (!strcmp(a, "dist")) do_dist = true;
        else if (!strcmp(a, "--no-float") || !strcmp(a, "-no-float")) g_no_float = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); return 0; }
        else { nob_log(NOB_ERROR, "unknown argument: %s", a); usage(argv[0]); return 1; }
    }

    if (do_clean) return clean() ? 0 : 1;
    if (do_dist)  return build_dist() ? 0 : 1;   /* multi-platform release build */

    /* Select the target toolchain, output paths, and terminal backend. */
    g_cc = getenv("CC");
    if (!g_cc || !*g_cc) g_cc = g_win64 ? "x86_64-w64-mingw32-gcc" : "cc";
    if (g_win64) {
        g_objdir = "build/win64/obj";
        g_binpath = "bin/prov.exe";
        g_term_backend = "platform/platform_term_win32.c";
        if (do_test) {
            nob_log(NOB_INFO, "win64: skipping tests (cannot run Windows binaries here)");
            do_test = false;
        }
    }

    /* Release objects are compiled with different flags than debug, so keep
     * them in a separate objdir — otherwise switching debug<->release would
     * silently reuse stale objects (timestamps match, flags don't). */
    if (g_release)
        g_objdir = g_win64 ? "build/win64/obj-release" : "build/obj-release";

    if (!ensure_dirs()) return 1;

    g_std = detect_std();
    if (!g_std) {
        nob_log(NOB_ERROR, "compiler %s accepts neither -std=c23 nor -std=c2x", g_cc);
        return 1;
    }

    /* Debug and release write the same output binary (bin/prov or bin/prov.exe)
     * from separate objdirs, so a config switch can leave the binary "newer
     * than" the new config's objects and skip relinking. Stamp the last config
     * per target; if it changed, drop the stale binary to force a relink. */
    {
        char stamp[256];
        snprintf(stamp, sizeof stamp, "%s/.buildcfg",
                 g_win64 ? "build/win64" : BUILD_DIR);
        const char *tag = g_release ? "release" : "debug";
        Nob_String_Builder prev = {0};
        bool same = nob_file_exists(stamp)
                    && nob_read_entire_file(stamp, &prev)
                    && prev.count == strlen(tag)
                    && memcmp(prev.items, tag, prev.count) == 0;
        free(prev.items);
        if (!same) {
            remove(g_binpath);
            nob_write_entire_file(stamp, tag, strlen(tag));
        }
    }

    Nob_File_Paths lib_objs = {0}, core_objs = {0}, main_objs = {0};
    if (!build_lib(&lib_objs)) return 1;
    if (!build_editor(&core_objs, &main_objs)) return 1;

    Nob_File_Paths editor_all = {0};   /* core + entrypoint */
    paths_extend(&editor_all, &core_objs);
    paths_extend(&editor_all, &main_objs);
    if (!link_exe(g_binpath, &lib_objs, &editor_all)) return 1;
    nob_log(NOB_INFO, "built %s", g_binpath);

    if (do_test) {
        Nob_File_Paths base = {0};     /* library + core, no entrypoint */
        paths_extend(&base, &lib_objs);
        paths_extend(&base, &core_objs);
        if (!run_tests(&base)) return 1;
    }
    return 0;
}
