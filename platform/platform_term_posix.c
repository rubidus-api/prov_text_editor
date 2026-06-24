/*
 * POSIX terminal backend: raw termios mode, ANSI escape rendering, window-size
 * query, and raw byte input fed to the pure key decoder. This is the only
 * editor file (besides the proven PAL) that touches OS/terminal headers.
 *
 * Not covered by `./nob test`; verified manually in a real terminal (TEST.md).
 */

#include "platform.h"

#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>

/* SIGWINCH (terminal resize) sets this; the key reader returns PROV_KEY_NONE so
 * the event loop re-renders at the new size without waiting for a key. */
static volatile sig_atomic_t g_winch = 0;
static void on_winch(int sig) { (void)sig; g_winch = 1; }

/* ------------------------------------------------------------ output buffer */

static proven_u8 *g_out;
static size_t     g_out_len, g_out_cap;

static void ob_reset(void) { g_out_len = 0; }

static void ob_put(const void *data, size_t n) {
    if (g_out_len + n > g_out_cap) {
        size_t cap = g_out_cap ? g_out_cap * 2 : 4096;
        while (cap < g_out_len + n) cap *= 2;
        proven_u8 *p = (proven_u8 *)realloc(g_out, cap);
        if (!p) return;            /* drop output rather than crash */
        g_out = p;
        g_out_cap = cap;
    }
    memcpy(g_out + g_out_len, data, n);
    g_out_len += n;
}

static void ob_str(const char *s) { ob_put(s, strlen(s)); }

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

static struct termios g_orig;

bool prov_term_is_tty(void) {
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

bool prov_term_init(void) {
    if (!prov_term_is_tty()) return false;
    if (tcgetattr(STDIN_FILENO, &g_orig) != 0) return false;

    struct termios raw = g_orig;
    raw.c_lflag &= ~((tcflag_t)(ECHO | ICANON | ISIG | IEXTEN));
    raw.c_iflag &= ~((tcflag_t)(IXON | ICRNL | BRKINT | INPCK | ISTRIP));
    raw.c_oflag &= ~((tcflag_t)OPOST);
    raw.c_cflag |= (tcflag_t)CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return false;

    /* Resize handler WITHOUT SA_RESTART, so a blocked read() returns EINTR. */
    struct sigaction sa = {0};
    sa.sa_handler = on_winch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, NULL);

    ob_reset();
    ob_str("\x1b[?1049h\x1b[?7l\x1b[2J\x1b[H");   /* alt screen, no auto-wrap, clear, home */
    (void)write(STDOUT_FILENO, g_out, g_out_len);
    return true;
}

void prov_term_shutdown(void) {
    ob_reset();
    /* disable mouse (harmless if never enabled), restore auto-wrap, show cursor, leave alt screen */
    ob_str("\x1b[?1000l\x1b[?1002l\x1b[?1006l\x1b[?7h\x1b[?25h\x1b[?1049l");
    (void)write(STDOUT_FILENO, g_out, g_out_len);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
    free(g_out);
    g_out = NULL;
    g_out_len = g_out_cap = 0;
}

/* Enable/disable mouse reporting: button press/release (1000) + drag (1002) in
 * SGR extended encoding (1006). Off resets all three. Written immediately. */
void prov_term_enable_mouse(bool on) {
    ob_reset();
    ob_str(on ? "\x1b[?1000h\x1b[?1002h\x1b[?1006h"
              : "\x1b[?1000l\x1b[?1002l\x1b[?1006l");
    (void)write(STDOUT_FILENO, g_out, g_out_len);
}

prov_term_size_t prov_term_size(void) {
    prov_term_size_t s = { 24, 80 };
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        s.rows = ws.ws_row;
        s.cols = ws.ws_col;
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
        for (proven_size_t c = 0; c < cols; c++) {
            prov_cell_t cell = grid[r * cols + c];
            if (cell.cont) continue;             /* wide tail: terminal owns it */
            bool want_rev  = cell.selected || (cell.attr & PROV_ATTR_REVERSE);
            bool want_bold = (cell.attr & PROV_ATTR_BOLD) != 0;
            bool want_dim  = (cell.attr & PROV_ATTR_DIM) != 0;
            bool want_ul   = (cell.attr & PROV_ATTR_UNDERLINE) != 0;
            bool want_mt   = (cell.attr & PROV_ATTR_MATCH) != 0;
            if (want_rev != rev) { ob_str(want_rev ? "\x1b[7m" : "\x1b[27m"); rev = want_rev; }
            if (want_bold != bold || want_dim != dim) {   /* bold & dim share the SGR 22 reset */
                ob_str("\x1b[22m");
                if (want_bold) ob_str("\x1b[1m");
                if (want_dim)  ob_str("\x1b[2m");
                bold = want_bold; dim = want_dim;
            }
            if (want_ul  != ul ) { ob_str(want_ul  ? "\x1b[4m" : "\x1b[24m"); ul  = want_ul;  }
            if (want_mt  != mt ) { ob_str(want_mt  ? "\x1b[43m\x1b[30m" : "\x1b[39m\x1b[49m"); mt = want_mt; }
            proven_u8 u[4];
            int m = encode_utf8(cell.cp ? cell.cp : 0x20, u);
            ob_put(u, (size_t)m);
        }
        if (rev) ob_str("\x1b[27m");             /* leave reverse/bold/dim/underline/match */
        if (bold || dim) ob_str("\x1b[22m");
        if (ul)  ob_str("\x1b[24m");
        if (mt)  ob_str("\x1b[39m\x1b[49m");
        /* full-width row: no \x1b[K (auto-wrap is off, so the cursor pins on the
         * last column and erase-to-EOL would blank it). The full write covers it. */
        ob_str("\r\n");
    }

    /* global status line (normal video; the leading `X` is reverse — a mouse
     * close-button cue), padded/truncated to the width */
    size_t sl = status ? strlen(status) : 0;
    for (proven_size_t c = 0; c < cols; c++) {
        proven_u8 ch = (c < sl) ? (proven_u8)status[c] : (proven_u8)' ';
        if (c == 0 && sl > 0) ob_str("\x1b[7m");
        ob_put(&ch, 1);
        if (c == 0 && sl > 0) ob_str("\x1b[27m");
    }
    ob_str("\r\n");                              /* full width: no erase (see above) */

    /* command line (normal video) below the status bar */
    size_t cl = cmdline ? strlen(cmdline) : 0;
    for (proven_size_t c = 0; c < cols && c < cl; c++) {
        proven_u8 ch = (proven_u8)cmdline[c];
        ob_put(&ch, 1);
    }
    ob_str("\x1b[K");

    char pos[32];
    int t = snprintf(pos, sizeof pos, "\x1b[%zu;%zuH",
                     (size_t)cur_row + 1 + (tabbar ? 1 : 0), (size_t)cur_col + 1);
    if (t > 0) ob_put(pos, (size_t)t);
    ob_str("\x1b[?25h");

    (void)write(STDOUT_FILENO, g_out, g_out_len);
}

/* ------------------------------------------------------------ input */

prov_key_t prov_term_read_key(void) {
    static proven_u8 buf[64];
    static size_t    len;

    for (;;) {
        if (g_winch) {                          /* pending resize: re-render now */
            g_winch = 0;
            prov_key_t none = { .kind = PROV_KEY_NONE };
            return none;
        }
        if (len > 0) {
            prov_key_t k = prov_decode_key(buf, len);
            if (k.kind != PROV_KEY_NONE) {
                /* A lone ESC may be the split lead of an escape sequence (arrow,
                 * Home/End, …) whose remaining bytes are still in flight — under
                 * scheduling latency the pty can deliver `\x1b` separately from
                 * the `[A` that follows. Wait briefly (ttimeout): if more bytes
                 * arrive, re-decode them as one sequence; otherwise it is a real
                 * Esc keypress. This costs a standalone Esc ~30ms, nothing else. */
                if (k.kind == PROV_KEY_ESC && k.consumed == 1 && len == 1) {
                    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
                    if (poll(&pfd, 1, 30) > 0 && (pfd.revents & POLLIN)) {
                        proven_u8 more[32];
                        ssize_t m = read(STDIN_FILENO, more, sizeof more);
                        if (m > 0) {
                            size_t room = sizeof buf - len;
                            size_t take = (size_t)m < room ? (size_t)m : room;
                            memcpy(buf + len, more, take);
                            len += take;
                            continue;              /* re-decode the completed sequence */
                        }
                    }
                }
                memmove(buf, buf + k.consumed, len - k.consumed);
                len -= k.consumed;
                return k;
            }
            if (len == sizeof buf) {           /* full but undecodable: drop one */
                memmove(buf, buf + 1, --len);
            }
        }

        proven_u8 chunk[32];
        ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
        if (n < 0) {
            if (errno == EINTR) {               /* interrupted (e.g. SIGWINCH) */
                prov_key_t none = { .kind = PROV_KEY_NONE };
                return none;
            }
            prov_key_t q = { .kind = PROV_KEY_CTRL, .cp = 'q' };
            return q;
        }
        if (n == 0) {                           /* EOF -> quit */
            prov_key_t q = { .kind = PROV_KEY_CTRL, .cp = 'q' };
            return q;
        }
        size_t room = sizeof buf - len;
        size_t take = (size_t)n < room ? (size_t)n : room;
        memcpy(buf + len, chunk, take);
        len += take;
    }
}

bool prov_platform_exe_dir(char *out, proven_size_t cap) {
    if (cap == 0) return false;
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return false;
    path[n] = '\0';
    char *slash = strrchr(path, '/');
    if (!slash) return false;
    *slash = '\0';                       /* strip the file name -> directory */
    size_t len = strlen(path);
    if (len + 1 > cap) return false;
    memcpy(out, path, len + 1);
    return true;
}
