#!/usr/bin/env python3
"""Drive bin/prov under a PTY and reconstruct its final screen.

Why this exists
---------------
prov repaints the terminal *differentially*: it moves the cursor with CUP
(`ESC [ row ; col H`) and rewrites only the cells that changed, and it does **not**
emit a clear-home (`ESC [ H`) or full clear between frames. So you cannot recover
the visible screen by splitting the byte stream on `ESC [ H` — that only ever shows
the boot frame. You must replay the escape sequences onto a cell grid. This module
does the minimum for prov's output: CUP cursor positioning, ED (`ESC [ 2 J`) clear,
EL (`ESC [ K`) erase-to-end-of-line, and CR/LF/BS; SGR (`m`) and mode (`h`/`l`) are
parsed and ignored. It is a test/inspection aid, not part of the build.

Usage
-----
As a module (preferred for assertions):

    import sys; sys.path.insert(0, "scripts"); import pty_screen as pty
    scr = pty.screen(pty.render(["zo", "t"], ["somefile.txt"]))   # -> list[str] rows
    assert any("sort" in r for r in scr)

Keys are sent one entry at a time with a settle delay between them; use "\\t" for
Tab, "\\r" for Enter, "\\x1b" for Esc, "\\x13" for Ctrl-S, "\\x1b[A" for Up, etc.
Note prov boots in zx (command) mode — do not prefix a literal "zx".

As a CLI (quick eyeball):

    python3 scripts/pty_screen.py 'zo' 't' -- somefile.txt
    #   args before `--` are keystrokes, after `--` are prov argv.

Run from the repo root (it execs ./bin/prov).
"""
import os, pty, time, select, fcntl, termios, struct, re, sys

ROWS, COLS = 40, 120
EXE = "./bin/prov"


def render(keys, argv=None, settle=0.2, boot=0.4):
    """Spawn EXE under a PTY, send `keys`, return the raw decoded output stream."""
    argv = argv or []
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.execv(EXE, [EXE] + argv)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
    time.sleep(boot)
    buf = bytearray()

    def drain(t):
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    d = os.read(fd, 65536)
                except OSError:
                    break
                if not d:
                    break
                buf.extend(d)

    drain(settle)
    for k in keys:
        os.write(fd, k.encode())
        time.sleep(0.18)
        drain(0.18)
    drain(0.4)
    try:
        os.kill(pid, 9)          # prov has no clean headless exit path; SIGKILL the child
    except ProcessLookupError:
        pass
    os.close(fd)
    return bytes(buf).decode("utf-8", "replace")


def screen(data):
    """Replay `data`'s escape stream onto a ROWS x COLS grid; return rstripped rows."""
    grid = [[' '] * COLS for _ in range(ROWS)]
    r = c = 0
    i, n = 0, len(data)
    while i < n:
        ch = data[i]
        if ch == '\x1b':
            m = re.match(r'\x1b\[([0-9;?]*)([A-Za-z])', data[i:])
            if not m:
                i += 1
                continue
            params, cmd = m.group(1), m.group(2)
            i += m.end()
            if cmd == 'H':                                    # CUP: cursor position
                a = [p for p in params.split(';') if p]
                r = (int(a[0]) - 1) if len(a) >= 1 else 0
                c = (int(a[1]) - 1) if len(a) >= 2 else 0
                r = max(0, min(ROWS - 1, r))
                c = max(0, min(COLS - 1, c))
            elif cmd == 'J' and params in ('2', ''):          # ED: clear screen
                grid = [[' '] * COLS for _ in range(ROWS)]
            elif cmd == 'K':                                  # EL: erase to end of line
                for cc in range(c, COLS):
                    grid[r][cc] = ' '
            # 'm' (SGR), 'h'/'l' (modes), etc. are intentionally ignored
            continue
        if ch == '\r':
            c = 0
        elif ch == '\n':
            r = min(ROWS - 1, r + 1)
        elif ch == '\b':
            c = max(0, c - 1)
        elif ord(ch) >= 32:
            if c < COLS:
                grid[r][c] = ch
                c += 1
        i += 1
    return [''.join(row).rstrip() for row in grid]


if __name__ == "__main__":
    args = sys.argv[1:]
    keys, argv = (args[:args.index("--")], args[args.index("--") + 1:]) if "--" in args else (args, [])
    keys = [k.encode().decode("unicode_escape") for k in keys]   # turn \t \r \x1b into bytes
    for row in screen(render(keys, argv)):
        print(row)
