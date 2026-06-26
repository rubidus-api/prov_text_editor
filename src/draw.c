#include "draw.h"

#include "unicode.h"   /* prov_utf8_decode, prov_char_width, prov_decode_t */
#include "panel.h"     /* PANEL_K_* */
#include "pstr.h"      /* FMT_INTO, prov_cstr_view */
#include "proven/u8str.h"
#include "proven/fmt.h"
#include "proven/time.h"

/* ---- value formatting ----------------------------------------------------- */

void prov_fmt_count(char *buf, proven_size_t cap, proven_size_t v) {
    char digits[24];
    FMT_INTO(digits, "{}", PROVEN_ARG((unsigned long)v));
    int dn = (int)proven_cstr_len(digits);
    int oi = 0;
    for (int i = 0; i < dn && oi + 2 < (int)cap; i++) {
        if (i > 0 && (dn - i) % 3 == 0) buf[oi++] = ',';   /* group every 3 from the right */
        buf[oi++] = digits[i];
    }
    buf[oi] = '\0';
}

void prov_fmt_size(char *b, proven_size_t cap, proven_size_t sz, bool is_dir) {
    proven_u8str_t s = proven_u8str_borrow((proven_byte_t *)b, cap);
    if (is_dir) {
        (void)proven_u8str_append_fmt_trunc(&s, "-");
    } else if (sz < 1024) {
        (void)proven_u8str_append_fmt_trunc(&s, "{}", PROVEN_ARG((unsigned long)sz));
    } else {
        proven_size_t v = sz, rem = 0; int u = 0;
        while (v >= 1024 && u < 4) { rem = v % 1024; v /= 1024; u++; }
        char uc[2] = { "BKMGT"[u], 0 };
        (void)proven_u8str_append_fmt_trunc(&s, "{}.{}{}", PROVEN_ARG((unsigned long)v),
            PROVEN_ARG((unsigned)((rem * 10) / 1024)), PROVEN_ARG(prov_cstr_view(uc)));
    }
    (void)proven_u8str_as_cstr(&s);          /* NUL-terminate the borrowed buffer */
}

void prov_fmt_perms(char *b, proven_fs_perms_t p, bool is_dir) {
    b[0] = is_dir ? 'd' : '-';
    b[1] = (p & PROVEN_FS_PERM_OWNER_R) ? 'r' : '-';
    b[2] = (p & PROVEN_FS_PERM_OWNER_W) ? 'w' : '-';
    b[3] = (p & PROVEN_FS_PERM_OWNER_X) ? 'x' : '-';
    b[4] = (p & PROVEN_FS_PERM_GROUP_R) ? 'r' : '-';
    b[5] = (p & PROVEN_FS_PERM_GROUP_W) ? 'w' : '-';
    b[6] = (p & PROVEN_FS_PERM_GROUP_X) ? 'x' : '-';
    b[7] = (p & PROVEN_FS_PERM_OTHER_R) ? 'r' : '-';
    b[8] = (p & PROVEN_FS_PERM_OTHER_W) ? 'w' : '-';
    b[9] = (p & PROVEN_FS_PERM_OTHER_X) ? 'x' : '-';
    b[10] = '\0';
}

void prov_fmt_mtime(char *b, proven_size_t cap, proven_i64 secs) {
    if (secs <= 0) { if (cap) b[0] = '-', b[1 < cap ? 1 : 0] = '\0'; return; }
    proven_datetime_t dt = proven_time_breakdown((proven_time_t)secs * 1000000000LL);
    proven_u8str_t s = proven_u8str_borrow((proven_byte_t *)b, cap);
    (void)proven_u8str_append_fmt_trunc(&s, "{}-{}{}-{}{} {}{}:{}{}",
        PROVEN_ARG((unsigned)dt.year),
        PROVEN_ARG(prov_cstr_view(dt.month < 10 ? "0" : "")), PROVEN_ARG((unsigned)dt.month),
        PROVEN_ARG(prov_cstr_view(dt.day   < 10 ? "0" : "")), PROVEN_ARG((unsigned)dt.day),
        PROVEN_ARG(prov_cstr_view(dt.hour  < 10 ? "0" : "")), PROVEN_ARG((unsigned)dt.hour),
        PROVEN_ARG(prov_cstr_view(dt.min   < 10 ? "0" : "")), PROVEN_ARG((unsigned)dt.min));
    (void)proven_u8str_as_cstr(&s);
}

/* ---- glyph blitting ------------------------------------------------------- */

int prov_blit_utf8(prov_cell_t *grid, int cols, int row, int col, proven_u8 attr,
                   const char *str) {
    proven_size_t i = 0, len = proven_cstr_len(str);
    while (str[i] && col < cols) {
        prov_decode_t d = prov_utf8_decode((const proven_u8 *)str + i, len - i);
        proven_u32 cp = d.valid ? d.cp : (proven_u8)str[i];
        if (col >= 0) grid[row * cols + col] = (prov_cell_t){ .cp = cp, .attr = attr };
        col++;
        i += d.len ? d.len : 1;
    }
    return col;
}

void prov_blit_utf8_clip(prov_cell_t *grid, int cols, int row, int col,
                         int maxw, proven_u8 attr, const char *str) {
    proven_size_t i = 0, len = proven_cstr_len(str);
    int x = 0;
    while (str[i] && x < maxw) {
        prov_decode_t d = prov_utf8_decode((const proven_u8 *)str + i, len - i);
        proven_u32 cp = d.valid ? d.cp : (proven_u8)str[i];
        int cw = prov_char_width(cp); if (cw < 1) cw = 1; if (cw > 2) cw = 2;
        if (x + cw > maxw || col + x >= cols) break;
        grid[row * cols + col + x] = (prov_cell_t){ .cp = cp, .attr = attr };
        if (cw == 2 && col + x + 1 < cols)
            grid[row * cols + col + x + 1] = (prov_cell_t){ .cont = true, .attr = attr };
        x += cw;
        i += d.len ? d.len : 1;
    }
}


void prov_draw_splash(prov_cell_t *grid, int cols, int rows) {
    /* A centered reverse-video card with a bold title, a version next to it, and
     * a soft drop shadow at the bottom-right. */
    static const char *body[] = {
        "Press h for help.",
        "Type z then x to change modes.",
        "",
        "zx mode: Vim-style command mode",
        "ed mode: Classic editor mode",
        "",
        "Type z then x then press Enter to type 'zx' in ed mode.",
    };
    int nbody = (int)(sizeof body / sizeof body[0]);
    const char *name = "prov text editor";
    char ver[24]; FMT_INTO(ver, "v{}", PROVEN_ARG(prov_cstr_view(PROV_VERSION)));
    int namew = (int)prov_str_disp_width(name);
    int verw  = (int)prov_str_disp_width(ver);
    int titlew = namew + 3 + verw;                 /* "name   vX.Y" */

    int contentw = titlew;
    for (int i = 0; i < nbody; i++) {
        int w = (int)prov_str_disp_width(body[i]);
        if (w > contentw) contentw = w;
    }

    int padx = 4, pady = 1;
    int boxw = contentw + 2 * padx;
    int boxh = (1 /*title*/ + 1 /*gap*/ + nbody) + 2 * pady;
    if (boxw > cols - 3) boxw = cols - 3;          /* keep two cells for the right shadow */
    if (boxh > rows - 2) boxh = rows - 2;
    if (boxw < 8 || boxh < 5) return;              /* too small to look right */
    int top = (rows - boxh) / 2; if (top < 0) top = 0;
    int left = (cols - boxw) / 2; if (left < 0) left = 0;

    /* drop shadow: an offset (down+right) L of shade cells, drawn first. The
     * right (vertical) arm is two columns wide; the bottom arm is one row. */
    for (int r = top + 1; r <= top + boxh && r < rows; r++)
        for (int dc = 0; dc < 2; dc++) {
            int c = left + boxw + dc;
            if (c >= 0 && c < cols) grid[r * cols + c] = (prov_cell_t){ .cp = 0x2591 };
        }
    for (int c = left + 1; c <= left + boxw + 1 && c < cols; c++) {
        int r = top + boxh;
        if (r >= 0 && r < rows) grid[r * cols + c] = (prov_cell_t){ .cp = 0x2591 };
    }

    /* reverse-video card fill */
    for (int r = top; r < top + boxh && r < rows; r++)
        for (int c = left; c < left + boxw && c < cols; c++)
            grid[r * cols + c] = (prov_cell_t){ .cp = 0x20, .attr = PROV_ATTR_REVERSE };

    /* title: the name in the original (non-reversed) colors but bold, so it pops
     * against the reverse-video card; version beside it stays reverse. Centered. */
    int trow = top + pady;
    int tstart = left + (boxw - titlew) / 2; if (tstart < left + 1) tstart = left + 1;
    prov_blit_utf8(grid, cols, trow, tstart, PROV_ATTR_BOLD, name);
    prov_blit_utf8(grid, cols, trow, tstart + namew + 3, PROV_ATTR_REVERSE, ver);

    /* body, one blank line under the title, each centered */
    for (int i = 0; i < nbody; i++) {
        int row = top + pady + 2 + i;
        if (row >= top + boxh || row >= rows) break;
        if (!body[i][0]) continue;                 /* spacer line */
        int w = (int)prov_str_disp_width(body[i]);
        int c = left + (boxw - w) / 2; if (c < left + 1) c = left + 1;
        prov_blit_utf8(grid, cols, row, c, PROV_ATTR_REVERSE, body[i]);
    }
}

/* ---- scrollbar widgets ---------------------------------------------------- */

void prov_draw_vscroll(prov_cell_t *grid, int cols, int row, int col, int h,
                       proven_size_t top, proven_size_t total, proven_u8 attr) {
    if (h <= 0) return;
    proven_size_t vis = (proven_size_t)h;
    if (total <= vis) {                              /* fits: no scrollbar, just the `│` border */
        for (int r = 0; r < h; r++)
            grid[(row + r) * cols + col] = (prov_cell_t){ .cp = 0x2502, .attr = attr };
        return;
    }
    /* overflow: a `█` thumb on a `│` track, with `▲`/`▼` buttons on the bottom
     * two rows when there is room (h >= 4). */
    int track = h, up_row = -1, down_row = -1;
    if (h >= 4) { track = h - 2; up_row = h - 2; down_row = h - 1; }
    int th = (int)((proven_size_t)track * vis / total); if (th < 1) th = 1; if (th > track) th = track;
    proven_size_t denom = total - vis;
    proven_size_t t = top < denom ? top : denom;
    int ty = (int)((proven_size_t)(track - th) * t / denom);
    int ts = ty, te = ty + th;
    for (int r = 0; r < track; r++)
        grid[(row + r) * cols + col] =
            (prov_cell_t){ .cp = (r >= ts && r < te) ? 0x2588 : 0x2502, .attr = attr };
    if (up_row >= 0) {
        grid[(row + up_row) * cols + col]   = (prov_cell_t){ .cp = 0x25B2, .attr = attr };  /* ▲ */
        grid[(row + down_row) * cols + col] = (prov_cell_t){ .cp = 0x25BC, .attr = attr };  /* ▼ */
    }
}

void prov_draw_hscroll(prov_cell_t *grid, int cols, int row, int col, int w,
                       proven_size_t left, proven_size_t total, proven_u8 attr) {
    if (w <= 0) return;
    int ts = 0, te = 0;                              /* thumb cols; none -> plain `─` track */
    if (total > (proven_size_t)w) {
        int tw = (int)((proven_size_t)w * (proven_size_t)w / total); if (tw < 1) tw = 1; if (tw > w) tw = w;
        proven_size_t denom = total - (proven_size_t)w;
        proven_size_t l = left < denom ? left : denom;
        int tx = (int)((proven_size_t)(w - tw) * l / denom);
        ts = tx; te = tx + tw;
    }
    for (int c = 0; c < w; c++)
        grid[row * cols + col + c] =
            (prov_cell_t){ .cp = (te > ts && c >= ts && c < te) ? 0x2588 : 0x2500, .attr = attr };
}

/* ---- panel help text ------------------------------------------------------ */

int prov_panel_help_lines(int kind, const char *out[16]) {
    int n = 0;
    switch (kind) {
        case PANEL_K_WINDOWS:
            out[n++] = "Switch focus among the split windows of this tab.";
            out[n++] = "Ng (e.g. 2g) focuses window N; ik + Enter also works.";
            out[n++] = "The > row is the active window.";
            out[n++] = "S marks a window (*); S on another swaps their contents.";
            break;
        case PANEL_K_TABS:
            out[n++] = "Switch and manage the open tabs.";
            out[n++] = "Ng (e.g. 2g) switches to tab N; ik moves the selection.";
            out[n++] = "Each row: a tab's focused buffer, its Ln/Co, window count.";
            out[n++] = "I / K  reorder the tab up / down one position.";
            out[n++] = "J / L  send the tab to the top / bottom.";
            out[n++] = "f  fold open a tab to list its windows (indented).";
            out[n++] = "n  open a new tab on a fresh empty buffer.";
            out[n++] = "x  close the tab (asks to save each modified window).";
            break;
        case PANEL_K_REGS:
            out[n++] = "Named registers a-z and 0-9 holding yanked/cut text.";
            out[n++] = "Each row: slot, byte size, and a one-line preview.";
            out[n++] = "Pick to paste the selected register at the cursor.";
            out[n++] = "x <slot>  deletes a register.";
            break;
        case PANEL_K_MACROS:
            out[n++] = "Recorded macro slots a-z and 0-9 (key sequences).";
            out[n++] = "r <slot>  starts recording (E stops); x <slot> deletes.";
            out[n++] = "Pick to replay the selected macro.";
            break;
        case PANEL_K_BOOKMARKS:
            out[n++] = "Bookmarks a-z in the current buffer (auto-follow edits).";
            out[n++] = "n <slot>  sets one at the cursor; x <slot> deletes.";
            out[n++] = "Pick to jump to the selected bookmark.";
            break;
        case PANEL_K_BROWSER:
            out[n++] = "Open-file dialog. The title bar shows the current directory.";
            out[n++] = "Layout: file list / preview / path input / options row.";
            out[n++] = "Each row is numbered (\"..\" is always row 1).";
            out[n++] = "Tab / Shift+Tab  cycle focus: list \xe2\x86\x92 preview \xe2\x86\x92 path \xe2\x86\x92 options.";
            out[n++] = "i k  move    j l (or \xe2\x86\x90\xe2\x86\x92)  page    Enter / o  open";
            out[n++] = "c / q  close    d  reset open options    h  help    w  move panel";
            out[n++] = "Ng  jump to item N    0g  jump to last item";
            out[n++] = "K  enter folder    I  parent    J / L  back / forward";
            out[n++] = "p  jump to the path field; type a path (relative or absolute) +";
            out[n++] = "  Enter to go into a folder or open a file (control bytes rejected).";
            out[n++] = "read options:  e encoding   m backend   b BOM   r EOL   x hex   R read-only";
            out[n++] = "view:  v preview   C info columns   f type filter (\xe2\x86\x91\xe2\x86\x93 recalls)";
            out[n++] = "t  sort: cycles field (name/date/ext) \xc3\x97 direction (asc/desc)";
            break;
        case PANEL_K_SEARCH:
            out[n++] = "Recent search terms, newest first.";
            out[n++] = "Pick one to search for it from the cursor.";
            break;
        case PANEL_K_FIND:
            out[n++] = "Find / replace dialog (opened with / ).";
            out[n++] = "Tab / Shift+Tab cycle: pattern \xe2\x86\x92 replace \xe2\x86\x92 options.";
            out[n++] = "Typing the pattern searches live from the start point.";
            out[n++] = "\xe2\x86\x91\xe2\x86\x93 in a field recall its history; Enter commits.";
            out[n++] = "n next   N previous   r replace one   a replace all.";
            out[n++] = "x regex   w whole-word   c case   (highlight: Tab + Enter).";
            out[n++] = "Replacement supports \\1..\\9 / & captures in regex mode.";
            out[n++] = "q or Esc closes; Esc restores the original cursor.";
            break;
        case PANEL_K_CMDS:
            out[n++] = "A searchable list of commands (ss to filter).";
            out[n++] = "Pick one to open its keyboard-help page.";
            break;
        case PANEL_K_MOVES:
            out[n++] = "Recent jump origins in this buffer, newest first.";
            out[n++] = "Pick one to jump back to it.";
            break;
        case PANEL_K_UNDO:
            out[n++] = "Undo history, newest action first (+ add, - delete, ~ replace).";
            out[n++] = "Pick one to undo back to that point.";
            break;
        default: out[n++] = "A selectable list panel."; break;
    }
    out[n++] = "";
    out[n++] = "Move   i k / \xe2\x86\x91\xe2\x86\x93   (Ng = item N, 0g = last)";
    out[n++] = "Find   ss then type the filter";
    out[n++] = "Pick   Space or Enter      Position  w";
    out[n++] = "Close  c or Esc            Help  h (back)";
    return n;
}
