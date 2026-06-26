/*
 * Win32 console terminal backend (64-bit). Implements the same platform.h seam
 * as the POSIX backend, using the Console API in virtual-terminal mode so the
 * ANSI rendering and the pure key decoder are shared across platforms:
 *
 *   - ENABLE_VIRTUAL_TERMINAL_PROCESSING  -> WriteFile interprets ANSI escapes
 *   - ENABLE_VIRTUAL_TERMINAL_INPUT       -> stdin delivers keys as VT sequences
 *   - CP_UTF8 input/output code pages     -> bytes are UTF-8, like POSIX
 *
 * Not covered by `./nob test`; verified manually on Windows (TEST.md).
 */

#include "platform.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Older SDK headers may miss these console-mode flags. */
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef DISABLE_NEWLINE_AUTO_RETURN
#define DISABLE_NEWLINE_AUTO_RETURN 0x0008
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif

static HANDLE g_in, g_out;
static DWORD  g_orig_in_mode, g_orig_out_mode;
static UINT   g_orig_in_cp, g_orig_out_cp;

/* ------------------------------------------------------------ output buffer */

static proven_u8 *g_buf;
static size_t     g_buf_len, g_buf_cap;

static void ob_reset(void) { g_buf_len = 0; }

static void ob_put(const void *data, size_t n) {
    if (g_buf_len + n > g_buf_cap) {
        size_t cap = g_buf_cap ? g_buf_cap * 2 : 4096;
        while (cap < g_buf_len + n) cap *= 2;
        proven_u8 *p = (proven_u8 *)realloc(g_buf, cap);
        if (!p) return;
        g_buf = p;
        g_buf_cap = cap;
    }
    memcpy(g_buf + g_buf_len, data, n);
    g_buf_len += n;
}

static void ob_str(const char *s) { ob_put(s, strlen(s)); }

static void ob_flush(void) {
    DWORD written;
    WriteFile(g_out, g_buf, (DWORD)g_buf_len, &written, NULL);
}

static int encode_utf8(proven_u32 cp, proven_u8 out[4]) {
    if (cp < 0x80) { out[0] = (proven_u8)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (proven_u8)(0xC0 | (cp >> 6));
        out[1] = (proven_u8)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (proven_u8)(0xE0 | (cp >> 12));
        out[1] = (proven_u8)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (proven_u8)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (proven_u8)(0xF0 | (cp >> 18));
    out[1] = (proven_u8)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (proven_u8)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (proven_u8)(0x80 | (cp & 0x3F));
    return 4;
}

/* ------------------------------------------------------------ terminal state */

bool prov_term_is_tty(void) {
    DWORD m;
    HANDLE i = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE);
    return GetConsoleMode(i, &m) && GetConsoleMode(o, &m);
}

bool prov_term_init(void) {
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(g_in, &g_orig_in_mode)) return false;
    if (!GetConsoleMode(g_out, &g_orig_out_mode)) return false;

    g_orig_in_cp = GetConsoleCP();
    g_orig_out_cp = GetConsoleOutputCP();
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    /* raw input: VT sequences, no line buffering/echo/Ctrl handling */
    DWORD inm = ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (!SetConsoleMode(g_in, inm)) return false;

    DWORD outm = g_orig_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                 DISABLE_NEWLINE_AUTO_RETURN;
    if (!SetConsoleMode(g_out, outm)) return false;

    ob_reset();
    ob_str("\x1b[?1049h\x1b[?7l\x1b[2J\x1b[H");  /* alt screen, no auto-wrap, clear, home */
    ob_flush();
    return true;
}

void prov_term_shutdown(void) {
    ob_reset();
    ob_str("\x1b[?7h\x1b[?25h\x1b[?1049l");   /* restore auto-wrap, show cursor, leave alt */
    ob_flush();
    SetConsoleMode(g_in, g_orig_in_mode);
    SetConsoleMode(g_out, g_orig_out_mode);
    SetConsoleCP(g_orig_in_cp);
    SetConsoleOutputCP(g_orig_out_cp);
    free(g_buf);
    g_buf = NULL;
    g_buf_len = g_buf_cap = 0;
}

/* Mouse reporting (RFC-0014). The Windows console delivers mouse as MOUSE_EVENT
 * input records rather than SGR byte sequences, and the current input path reads
 * only key records; wiring that translation is deferred (RFC-0014 D3). For now
 * this is a no-op so mouse stays inert on Windows without breaking the build. */
void prov_term_enable_mouse(bool on) {
    (void)on;
}

prov_term_size_t prov_term_size(void) {
    prov_term_size_t s = { 24, 80 };
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(g_out, &csbi)) {
        s.cols = (proven_size_t)(csbi.srWindow.Right - csbi.srWindow.Left + 1);
        s.rows = (proven_size_t)(csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
    }
    return s;
}

void prov_term_present(const prov_cell_t *grid, proven_size_t rows,
                       proven_size_t cols, proven_size_t cur_row,
                       proven_size_t cur_col, const char *status,
                       const char *cmdline, const char *tabbar) {
    ob_reset();
    ob_str("\x1b[?25l\x1b[H");

    if (tabbar) {                         /* tab bar (reverse video) above the grid */
        ob_str("\x1b[7m");
        size_t tl = strlen(tabbar);
        for (proven_size_t c = 0; c < cols; c++) {
            proven_u8 ch = (c < tl) ? (proven_u8)tabbar[c] : (proven_u8)' ';
            ob_put(&ch, 1);
        }
        ob_str("\x1b[m\r\n");
    }

    for (proven_size_t r = 0; r < rows; r++) {
        bool rev = false, bold = false, dim = false, ul = false, mt = false;
        int fg = 0;                              /* biased: 0 = default fg (display.h) */
        for (proven_size_t c = 0; c < cols; c++) {
            prov_cell_t cell = grid[r * cols + c];
            if (cell.cont) continue;
            bool want_rev  = cell.selected || (cell.attr & PROV_ATTR_REVERSE);
            bool want_bold = (cell.attr & PROV_ATTR_BOLD) != 0;
            bool want_dim  = (cell.attr & PROV_ATTR_DIM) != 0;
            bool want_ul   = (cell.attr & PROV_ATTR_UNDERLINE) != 0;
            bool want_mt   = (cell.attr & PROV_ATTR_MATCH) != 0;
            int  want_fg   = (int)cell.fg;
            if (want_rev != rev) { ob_str(want_rev ? "\x1b[7m" : "\x1b[27m"); rev = want_rev; }
            if (want_bold != bold || want_dim != dim) {   /* bold & dim share the SGR 22 reset */
                ob_str("\x1b[22m");
                if (want_bold) ob_str("\x1b[1m");
                if (want_dim)  ob_str("\x1b[2m");
                bold = want_bold; dim = want_dim;
            }
            if (want_ul  != ul ) { ob_str(want_ul  ? "\x1b[4m" : "\x1b[24m"); ul  = want_ul;  }
            if (want_mt  != mt ) { ob_str(want_mt  ? "\x1b[43m\x1b[30m" : "\x1b[39m\x1b[49m"); mt = want_mt; }
            if (want_fg != fg) {                 /* syntax-highlight foreground (RFC-0022) */
                if (want_fg == 0) ob_str("\x1b[39m");
                else {
                    int idx = want_fg - 1;
                    int code = idx < 8 ? 30 + idx : 90 + (idx - 8);
                    char seq[8]; int sl2 = 0; seq[sl2++]='\x1b'; seq[sl2++]='[';
                    if (code >= 100) { seq[sl2++]=(char)('0'+code/100); seq[sl2++]=(char)('0'+(code/10)%10); seq[sl2++]=(char)('0'+code%10);}
                    else { seq[sl2++]=(char)('0'+code/10); seq[sl2++]=(char)('0'+code%10); }
                    seq[sl2++]='m';
                    ob_put((const proven_u8 *)seq, (size_t)sl2);
                }
                fg = want_fg;
            }
            proven_u8 u[4];
            int m = encode_utf8(cell.cp ? cell.cp : 0x20, u);
            ob_put(u, (size_t)m);
        }
        if (rev) ob_str("\x1b[27m");
        if (bold || dim) ob_str("\x1b[22m");
        if (ul)  ob_str("\x1b[24m");
        if (mt)  ob_str("\x1b[39m\x1b[49m");
        if (fg)  ob_str("\x1b[39m");
        /* full-width row: no \x1b[K — with auto-wrap off the cursor pins on the
         * last column, where erase-to-EOL would blank it (the black rightmost
         * column on Windows consoles). The full-width write already overwrites. */
        ob_str("\r\n");
    }

    size_t sl = status ? strlen(status) : 0;     /* global status line: normal video,
                                                  * leading `X` reverse (mouse cue) */
    for (proven_size_t c = 0; c < cols; c++) {
        proven_u8 ch = (c < sl) ? (proven_u8)status[c] : (proven_u8)' ';
        if (c == 0 && sl > 0) ob_str("\x1b[7m");
        ob_put(&ch, 1);
        if (c == 0 && sl > 0) ob_str("\x1b[27m");
    }
    ob_str("\r\n");                              /* full width: no erase (see above) */

    /* command line (normal video) below the status bar. \x01 toggles shortcut
     * emphasis (bold+underline) for legends; it is not printed. */
    size_t cl = cmdline ? strlen(cmdline) : 0;
    bool emph = false;
    for (proven_size_t i = 0, col = 0; i < cl && col < cols; i++) {
        proven_u8 ch = (proven_u8)cmdline[i];
        if (ch == 0x01) { ob_str(emph ? "\x1b[22m\x1b[24m" : "\x1b[1m\x1b[4m"); emph = !emph; continue; }
        ob_put(&ch, 1);
        if ((ch & 0xC0) != 0x80) col++;
    }
    if (emph) ob_str("\x1b[22m\x1b[24m");
    ob_str("\x1b[K");

    char pos[32];
    int t = snprintf(pos, sizeof pos, "\x1b[%zu;%zuH",
                     (size_t)cur_row + 1 + (tabbar ? 1 : 0), (size_t)cur_col + 1);
    if (t > 0) ob_put(pos, (size_t)t);
    ob_str("\x1b[?25h");

    ob_flush();
}

prov_key_t prov_term_read_key(void) {
    static proven_u8 buf[64];
    static size_t    len;
    static SHORT     lw = -1, lh = -1;       /* last seen console window size */
    prov_key_t none = { .kind = PROV_KEY_NONE };

    for (;;) {
        if (len > 0) {
            prov_key_t k = prov_decode_key(buf, len);
            if (k.kind != PROV_KEY_NONE) {
                memmove(buf, buf + k.consumed, len - k.consumed);
                len -= k.consumed;
                return k;
            }
            if (len == sizeof buf) memmove(buf, buf + 1, --len);
        }

        /* Windows has no SIGWINCH; poll the console size. Wait for input with a
         * short timeout so a resize is picked up within the interval even when
         * no key is pressed. */
        DWORD wr = WaitForSingleObject(g_in, 100);

        CONSOLE_SCREEN_BUFFER_INFO ci;
        if (GetConsoleScreenBufferInfo(g_out, &ci)) {
            SHORT w = (SHORT)(ci.srWindow.Right - ci.srWindow.Left + 1);
            SHORT h = (SHORT)(ci.srWindow.Bottom - ci.srWindow.Top + 1);
            if (lw != -1 && (w != lw || h != lh)) {
                lw = w; lh = h;
                /* drain leading non-key records so the next ReadFile won't block */
                INPUT_RECORD r; DWORD g;
                while (PeekConsoleInputW(g_in, &r, 1, &g) && g > 0 && r.EventType != KEY_EVENT)
                    ReadConsoleInputW(g_in, &r, 1, &g);
                return none;                 /* terminal resized: re-render */
            }
            lw = w; lh = h;
        }

        if (wr == WAIT_TIMEOUT) continue;    /* nothing happened — keep polling */

        /* Skip any leading non-key records (resize/mouse/focus) so ReadFile
         * reads only character/VT key bytes. */
        for (;;) {
            INPUT_RECORD r; DWORD g;
            if (!PeekConsoleInputW(g_in, &r, 1, &g) || g == 0) break;
            if (r.EventType == KEY_EVENT) break;
            ReadConsoleInputW(g_in, &r, 1, &g);
        }

        proven_u8 chunk[32];
        DWORD n = 0;
        if (!ReadFile(g_in, chunk, (DWORD)sizeof chunk, &n, NULL)) {
            prov_key_t q = { .kind = PROV_KEY_CTRL, .cp = 'q' };
            return q;
        }
        if (n == 0) continue;
        size_t room = sizeof buf - len;
        size_t take = (size_t)n < room ? (size_t)n : room;
        memcpy(buf + len, chunk, take);
        len += take;
    }
}

/* Convert a wide string to UTF-8 into `out` (cap bytes). false if it does not
 * fit. prov's fs layer expects UTF-8 paths, so all Win32 path/env strings are
 * fetched with the wide (…W) APIs and converted here — this is what makes
 * non-ASCII home/exe paths (e.g. C:\Users\홍길동) work. */
static bool w_to_utf8(const WCHAR *w, char *out, proven_size_t cap) {
    int blen = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (blen <= 0 || (proven_size_t)blen > cap) return false;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, blen, NULL, NULL);
    return true;
}

/* Read an environment variable as UTF-8 (wide API). false if unset/too long. */
static bool env_w_utf8(const WCHAR *name, char *out, proven_size_t cap) {
    WCHAR wbuf[1024];
    DWORD n = GetEnvironmentVariableW(name, wbuf, (DWORD)(sizeof wbuf / sizeof wbuf[0]));
    if (n == 0 || n >= (DWORD)(sizeof wbuf / sizeof wbuf[0])) return false;
    return w_to_utf8(wbuf, out, cap);
}

bool prov_platform_exe_dir(char *out, proven_size_t cap) {
    if (cap == 0) return false;
    WCHAR wpath[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, wpath, (DWORD)(sizeof wpath / sizeof wpath[0]));
    if (n == 0 || n >= (DWORD)(sizeof wpath / sizeof wpath[0])) return false;
    WCHAR *slash = NULL;                  /* strip the file name -> directory */
    for (WCHAR *p = wpath; *p; p++) if (*p == L'\\' || *p == L'/') slash = p;
    if (!slash) return false;
    *slash = L'\0';
    return w_to_utf8(wpath, out, cap);
}

bool prov_platform_home_dir(char *out, proven_size_t cap) {
    if (cap == 0) return false;
    if (env_w_utf8(L"USERPROFILE", out, cap)) return true;   /* C:\Users\<name> */
    char drive[16], path[1024];                              /* HOMEDRIVE + HOMEPATH */
    if (env_w_utf8(L"HOMEDRIVE", drive, sizeof drive) &&
        env_w_utf8(L"HOMEPATH", path, sizeof path)) {
        size_t dl = strlen(drive), pl = strlen(path);
        if (dl + pl + 1 <= cap) { memcpy(out, drive, dl); memcpy(out + dl, path, pl + 1); return true; }
    }
    return env_w_utf8(L"APPDATA", out, cap);                 /* last resort */
}

int prov_platform_list_drives(prov_drive_t *out, int cap) {
    if (cap <= 0) return 0;
    DWORD mask = GetLogicalDrives();
    int n = 0;
    for (int i = 0; i < 26 && n < cap; i++) {
        if (!(mask & (1u << i))) continue;
        WCHAR root[4] = { (WCHAR)(L'A' + i), L':', L'\\', 0 };
        prov_drive_t d; memset(&d, 0, sizeof d);
        d.letter = (char)('A' + i);
        WCHAR vol[64] = {0};
        if (GetVolumeInformationW(root, vol, 63, NULL, NULL, NULL, NULL, 0))
            (void)w_to_utf8(vol, d.label, sizeof d.label);     /* "" if no label */
        ULARGE_INTEGER avail, total, freeb;
        if (GetDiskFreeSpaceExW(root, &avail, &total, &freeb)) {
            d.total = total.QuadPart; d.avail = avail.QuadPart;
        }
        out[n++] = d;
    }
    return n;
}
