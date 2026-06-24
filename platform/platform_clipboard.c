#include "platform_clipboard.h"

#ifdef _WIN32
/*
 * Native Win32 clipboard — no subprocess. The old `clip` / `powershell
 * Get-Clipboard` route spawned a console program that shares our console: it is
 * slow to start and leaves the console input mode in its own (line-input/echo)
 * state, which broke key input after a yank/paste. The clipboard API touches
 * nothing of the console, so it is both fast and safe.
 */
#include <windows.h>

bool prov_os_clip_set(const proven_u8 *data, proven_size_t len) {
    if (len > (proven_size_t)0x7FFFFFFF) return false;        /* MultiByteToWideChar takes int */
    if (!OpenClipboard(NULL)) return false;
    bool ok = false;
    EmptyClipboard();
    int wlen = (len > 0)
        ? MultiByteToWideChar(CP_UTF8, 0, (const char *)data, (int)len, NULL, 0) : 0;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(wlen + 1) * sizeof(wchar_t));
    if (h) {
        wchar_t *w = (wchar_t *)GlobalLock(h);
        if (w) {
            if (len > 0) MultiByteToWideChar(CP_UTF8, 0, (const char *)data, (int)len, w, wlen);
            w[wlen] = L'\0';
            GlobalUnlock(h);
            if (SetClipboardData(CF_UNICODETEXT, h)) { ok = true; h = NULL; }  /* system owns h now */
        }
        if (h) GlobalFree(h);                                 /* only if we still own it */
    }
    CloseClipboard();
    return ok;
}

proven_size_t prov_os_clip_get(proven_u8 *buf, proven_size_t cap) {
    if (cap == 0 || cap > (proven_size_t)0x7FFFFFFF) return 0;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return 0;
    if (!OpenClipboard(NULL)) return 0;
    proven_size_t got = 0;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t *w = (const wchar_t *)GlobalLock(h);
        if (w) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, (char *)buf, (int)cap, NULL, NULL);
            if (n > 0) got = (proven_size_t)(n - 1);          /* drop the terminating NUL */
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return got;
}

#else  /* ---- POSIX: spawn a clipboard tool, guarding the terminal mode ---- */

#include <stdio.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

/* Candidate clipboard tools, tried in order; the first that exits 0 wins.
 * stdin stays the pipe (the data we write) for set / stdout stays the pipe (the
 * data we read) for get; the other standard streams go to /dev/null so a tool
 * that forks a background selection server cannot inherit our terminal. */
static const char *const SET_CMDS[] = {
    "wl-copy >/dev/null 2>&1",
    "xclip -selection clipboard >/dev/null 2>&1",
    "xsel -b -i >/dev/null 2>&1",
    NULL
};
static const char *const GET_CMDS[] = {
    "wl-paste -n </dev/null 2>/dev/null",
    "xclip -selection clipboard -o </dev/null 2>/dev/null",
    "xsel -b -o </dev/null 2>/dev/null",
    NULL
};

/* A clipboard helper (or the /bin/sh that runs it) shares our controlling
 * terminal and can leave it in a different mode — most damagingly, dropping our
 * raw mode back to canonical/echo, which makes key input echo and stop reaching
 * the editor after a yank/paste. Snapshot the terminal settings before spawning
 * and put them back afterwards (TCSANOW keeps any queued keystrokes). */
typedef struct { struct termios t; int ok; } tty_guard_t;
static tty_guard_t tty_guard_begin(void) {
    tty_guard_t g; g.ok = tcgetattr(STDIN_FILENO, &g.t);
    return g;
}
static void tty_guard_end(const tty_guard_t *g) {
    if (g->ok == 0) tcsetattr(STDIN_FILENO, TCSANOW, &g->t);
}

bool prov_os_clip_set(const proven_u8 *data, proven_size_t len) {
    /* a dead pipe (tool missing) would otherwise SIGPIPE us mid-write */
    void (*old)(int) = signal(SIGPIPE, SIG_IGN);
    tty_guard_t tg = tty_guard_begin();
    bool ok = false;
    for (int i = 0; SET_CMDS[i]; i++) {
        FILE *p = popen(SET_CMDS[i], "w");
        if (!p) continue;
        size_t w = len ? fwrite(data, 1, (size_t)len, p) : 0;
        int rc = pclose(p);
        if (rc == 0 && (len == 0 || w == (size_t)len)) { ok = true; break; }
    }
    tty_guard_end(&tg);
    signal(SIGPIPE, old);
    return ok;
}

proven_size_t prov_os_clip_get(proven_u8 *buf, proven_size_t cap) {
    tty_guard_t tg = tty_guard_begin();
    proven_size_t got = 0;
    for (int i = 0; GET_CMDS[i]; i++) {
        FILE *p = popen(GET_CMDS[i], "r");
        if (!p) continue;
        size_t n = fread(buf, 1, (size_t)cap, p);
        int rc = pclose(p);
        if (rc == 0 && n > 0) { got = (proven_size_t)n; break; }
    }
    tty_guard_end(&tg);
    return got;
}

#endif
