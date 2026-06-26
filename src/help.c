#include "help.h"

/* Each page is a title plus a NULL-free array of static lines. The overlay
 * (main.c draw_help) renders these as UTF-8, so box-drawing, arrows (← ↑ ↓ →)
 * and the middot (·) are welcome. A line beginning with \x01 (the D macro) is
 * drawn dim — used for secondary text, key-cap rows, and "not active yet"
 * notes. Keep visible content within ~78 columns.
 *
 * Navigation (in the help panel): `h` then a key opens that key's page (`h`
 * then 1-9 = the count page); w repositions; ikjl/arrows scroll; Space/Enter/Esc
 * return to this overview; q/x close the panel.
 *
 * Key status convention: a "·" prefix on an overview label, and a dim
 * "Status: planned …" line on a detail page, mark a key that the parser
 * recognises but does not act on yet (reserved for a later milestone). */

#define D "\x01"        /* leading marker: render this line dim */

static const char *OVERVIEW[] = {
    "Press  h  then a key for its page  (h then 1-9 = count prefixes).",
    D "A \"·\" prefix marks a planned key — recognised, but not active yet.",
    "",
    D "    q       w       e       r       t       y       u       i       o       p",
    "    ·macro  window  ·macros ·replc  tab     yank    undo    ↑ up    open    paste",
    "",
    D "      a       s       d       f       g       h       j       k       l",
    "      field   search  delete  find    goto    help    ← left  ↓ down  → right",
    "",
    D "        z       x       c       v       b       n       m",
    "        meta    cut     change  select  ·reg    repeat  ·bmark",
    "",
    D "  ──────────────────────────────────────────────────────────────────────────",
    "  0   special prefix (0g = last line)        1-9   repeat count",
    "  . ,  repeat / reverse the last find        zx   leave to Ed (text entry)",
    "  u   undo  ·  U   redo  (both take a count)  hL   screen layout · F1 help",
    D "  h+key help \xc2\xb7 w move \xc2\xb7 ikjl scroll \xc2\xb7 Space/Enter overview \xc2\xb7 q/x close",
};

static const char *P_ZX[] = {        /* topic 'h' — basics / syntax / zx */
    "zx is command mode. Leave Ed (text entry) with Esc, or by typing the trigger",
    "(default \"zx\"). Return to Ed with the `zx` command. Esc in zx mode cancels a",
    "pending command (it does not leave zx mode).",
    "",
    "  Syntax:   [count] command       e.g.  5k = down 5,   3dd = delete 3 lines.",
    "",
    "  • A leading count repeats movements, single edits, and operators.",
    "  • a    jumps straight into Ed insert at the cursor.",
    "  • n    repeats the last command ([N]n repeats it N times).",
    "  • Namespaces: g goto · o open-line · z meta · w window · t tab —",
    "    type the prefix, then a key. The command line lists each prefix's keys.",
    "  • 0 is the special prefix for browsers/overviews (0g jumps to the last line).",
    "",
    D "  Several keys are reserved for later milestones — see the overview's",
    D "  ·-marked keys (b m e s, and r q) and their pages for details.",
};

static const char *P_ZERO[] = {      /* '0' */
    "0   — special prefix (not a count; for counts press 1-9).",
    "",
    "  0g   go to the last line of the file.",
    "",
    "  Panels (a modal list over the editor — ik/↑↓ move, [N]g jump,",
    "  ss filter, Space/Enter pick, h help, w reposition, c/Esc close):",
    "    0w  windows        0t  tabs          0b  registers (pick pastes)",
    "    0e  macros         0m  bookmarks     0s  search history",
    "    0z  command list   zo  file browser",
    "",
    "  Manage from a panel: 0b/0e/0m take verbs — r record · n set · x",
    "  delete, then a slot key (a-z/0-9). In macros, E stops recording.",
    "",
    D "  Reserved for later: 0u undo  0n jumps.",
};

static const char *P_COUNT[] = {     /* '#' — the 1-9 count-prefix explainer */
    "1-9   — a count prefix: type digits, then a command.",
    "",
    "  A leading number repeats or scales the command that follows it:",
    "",
    "    5k     down 5 lines           3l     right 3 chars",
    "    3dd    delete 3 lines         2yy    yank 2 lines",
    "    d3w    delete 3 words         10g    go to line 10",
    "    4n     repeat the last command 4 times",
    "",
    "  • The count echoes on the command line as you type it.",
    "  • 0 alone is NOT a count — it is the special prefix (press 0 for its page).",
    "  • Esc clears a half-typed count.",
};

static const char *P_UNDO[] = {      /* 'u' */
    "u   — undo the last edit (a count repeats: 3u).",
    "U   — redo (a count repeats: 3U).",
    "",
    "  • Ed mode: Ctrl-Z undo, Ctrl-Y redo.",
    "  • History is bounded by the undo_limit setting.",
};

static const char *P_CURSOR[] = {    /* 'i' (covers i k j l) */
    "Cursor — i ↑ up   k ↓ down   j ← left   l → right   (a count repeats: 5l).",
    "",
    "  • Uppercase: I PgUp   K PgDn   J line start (Home)   L line end (End).",
    "  • Arrows, Home / End, PgUp / PgDn also move the cursor.",
    "  • Word / line / page / document motions live in the g namespace (press g).",
    "  • f<char> jumps to a character on the line;  . and , repeat it.",
};

static const char *P_APPEND[] = {    /* 'a' / field mode (topic 'a', also F1 in field mode) */
    "Field mode — a bounded, underlined input region (entered by a / [N]a).",
    "",
    "  Entering:",
    "  • a        start a field at the cursor; type, then Esc to commit.",
    "  • [N]a     stamp the typed fragment N times:  80a- + Esc => 80 dashes.",
    "  • c{motion}  change a target; o/O open filled lines — all use field mode.",
    "",
    "  Editing inside the region (you cannot move/edit outside it):",
    "  • printable / Enter / Tab   insert text (Tab obeys expandtab/tabstop).",
    "  • Backspace / Delete        only within the region.",
    "  • arrows / PageUp / PageDn   move (clamped to the region); Shift selects.",
    "  • Ctrl-A select region · Ctrl-C copy · Ctrl-X cut · Ctrl-V paste.",
    "  • Ctrl-Z undo · Ctrl-Y redo  (scoped to this field session).",
    "",
    "  • Esc commits the field; the whole session is one undo step.",
    "  • Insert-only: any other key is inert.",
};

static const char *P_NCMD[] = {      /* 'n' */
    "n   — repeat the last command.    [N]n repeats it N times.",
    "",
    "  • n replays the most recent editing or movement command.",
    "  • 3n replays it three times (after dd, 3n deletes three more lines).",
    "  • n never records itself: a run of n's keeps replaying the nearest",
    "    earlier non-n command, so you can tap n n n to keep going.",
    "  • The original command's own count carries: d3w then n = three more words.",
};

static const char *P_EDIT[] = {      /* 'x' (covers x r o) */
    "Quick edits:",
    "",
    "  • x   delete the character under the cursor, or the selection.",
    "  • o   open-line namespace:  on new line below,  op above (enters Ed insert).",
    "",
    D "  • r   replace one character (r<char>) — reserved, not active yet.",
};

static const char *P_GOTO[] = {      /* 'g' */
    "g   — goto / movement namespace.",
    "",
    "  [count]g  go to line <count>      gg start of file      ge end of file",
    "  gn next word     gp previous word     gf line start     gl line end",
    "  gu half-page up     gd half-page down",
};

static const char *P_REG[] = {       /* 'b' (covers b register, m bookmark) */
    "Named registers (b) and bookmarks (m).",
    "",
    "  • b<reg>c / b<reg>x   copy / cut the selection into register <reg> (a-z 0-9).",
    "  • b<reg>v             paste from register <reg>.  [N] copies.",
    "  • 0m<letter>  pin bookmark a-z at the cursor (buffer-local; follows edits).",
    "  • m<letter>   jump to that bookmark.",
};

static const char *P_SELECT[] = {    /* 'v' */
    "v   — toggle visual select; subsequent movement extends the selection.",
    "V   — toggle visual BLOCK: a rectangular column selection.",
    "",
    "  • Operators c/d/y act on the selection; x cuts it; p pastes over it.",
    "  • g-namespace moves (1g, 0g, gn, gl, ...) keep extending the block.",
    "  • Visual block (V): move to size the rectangle, then y yanks / d cuts the",
    "    columns; p pastes a block column-wise across rows (status shows Zb).",
    "    I inserts / A appends on every row: type on the top row, Esc replicates.",
    "  • Ed mode: Shift+arrows select; Ctrl-A/C/X/V all / copy / cut / paste.",
};

static const char *P_PASTE[] = {     /* 'p' */
    "p / P  — paste the register (last yank or cut).  [N] = N copies.",
    "",
    "  • Characterwise register (yw, x, visual yank): inserts at the cursor;",
    "    p and P are the same.",
    "  • Linewise register (yy / dd): p pastes whole line(s) BELOW the current",
    "    line, P ABOVE — column-independent.  p on the last line appends cleanly.",
    "  • y yanks (copies); x and the d operator cut into the same register.",
};

static const char *P_OPER[] = {      /* 'c' (covers c d y) */
    "Operators take a target:   <op><target>.",
    "",
    "  • op:        c change (→ Ed insert)    d delete    y yank (copy)",
    "  • linewise:  cc dd yy        word: w b e        line end: l      match: m",
    "  • find:      f<char> t<char>",
    "  • text objects: iw aw  i( a(  i\" a\"  ip ap  it at  ...",
    "  • A count applies inside:  d3w,  2yy.",
};

static const char *P_WIN[] = {       /* 'w' */
    "w   — windows / panes.",
    "",
    "  wh split horizontal    wv split vertical    wq close window",
    "  0w window panel (Ng focuses, e.g. 2g)   wp / wn previous / next window",
    "  wi wk wj wl  focus the window up / down / left / right",
    "  ws resize: i/k (↑/↓) height, j/l (←/→) width    wr read-only",
    "  wx binary/hex — reload the file as raw bytes and edit them as hex",
    "",
    "  A window is one view of a buffer; several windows can share a buffer",
    "  (edits show in every view). A tab is a whole window layout.",
    "",
    "  Hex editor (wx, or the file-open 'x' toggle): edits the RAW file bytes",
    "  (loaded + saved verbatim — no encoding/EOL/BOM conversion). Tab switches",
    "  the hex / ascii pane. Move: ikjl / arrows; I/K = PgUp/PgDn; J/L = Home/End;",
    "  g/G doc start/end; ^G goto offset; [ ] nudge the byte window. Edit: 0-9 a-f",
    "  set a byte, o/Insert insert 00, x/Del delete, y/p copy/paste bytes,",
    "  ^Z/^Y undo, h help. ^S saves+exits; ^Q/Esc exit (asking to save if changed).",
    "  Under each hex row a decoded line shows the bytes in the interpretation",
    "  charset (open dialog 'e'). v / Shift+arrows select a byte range; r opens a",
    "  multi-line ed-mode editor on the decoded text (full shortcuts) — ^S writes",
    "  it back charset-encoded (any length), Esc cancels.",
};

static const char *P_TAB[] = {       /* 't' */
    "t   — tabs / buffers.   A tab is a whole window layout (a \"desk\").",
    "",
    "  [count]t go to tab N    tn new tab    tq close    tj prev    tl next",
    "",
    "  • Buffers (open files) are shared across tabs — the same file can be",
    "    open and edited in several tabs at once.",
    "  • zb lists buffers — a digit switches, n = new, q = close active.",
    "  • zo open dialog — common: o open, c/q close, d reset options, h help,",
    "    w move panel, Ng/0g jump (0g=last), Tab cycles list/preview/path/options.",
    "    read options: e encoding, m backend, b BOM, r EOL, x hex; view: f types,",
    "    t sort, v preview, C columns, R read-only, p path field, I/K up/open dir,",
    "    J/L history; type a path + Enter to go (dirs) / open (files).",
    "  • Mouse (config mouse=true): wheel scroll, click to focus/place the",
    "    cursor, drag to select, scrollbar, the X closes a window.",
    "  • A buffer closes when its last window closes (prompts to save if changed).",
};

static const char *P_SEARCH[] = {    /* 's' */
    "s   — search namespace.",
    "",
    "  /    open the find/replace panel: pattern + replacement fields, the",
    "       regex/word/case/highlight toggles, live count, n/N next/prev,",
    "       r replace-one, a replace-all (Tab cycles fields; q/Esc closes).",
    "  ss   prompt for a term (incremental: jumps as you type; Esc restores).",
    "  sw   search the word under the cursor.",
    "  sn   next match      sp   previous match   (both wrap around).",
    "  sr   replace every match of the current term (one undo step).",
    "       in regex mode, the replacement uses \\1..\\9 / & (captures), and a",
    "       g/re/ or v/re/ prefix limits it to lines that do / don't match re.",
    "  soc  toggle case-insensitive    soh  toggle match highlight.",
    "  sox  toggle regex (RFC-0009: a linear-time Pike VM — groups, classes,",
    "       quantifiers, |, anchors, \\zs/\\ze; no backrefs/lookaround).",
    "  sow  toggle whole-word (wraps the term in \\b...\\b; implies regex).",
    "       In regex, ^ and $ match at line boundaries.",
    "  Matches are highlighted (Esc clears).",
};

static const char *P_META[] = {      /* 'z' */
    "z   — meta namespace.",
    "",
    "  zx return to Ed mode    zw write    za write-as    zq quit    (redo: U)",
    "  zb buffer list    zo open file    zp command prompt",
    "  zc edit config (live-applies on save; the buffer shows a [config] tag)",
    "",
    D "  Planned (M4), not active yet:  zi/zd indent/dedent · zs syntax.",
};

static const char *P_MACRO[] = {     /* 'e' (covers e E) */
    "Keystroke macros (a-z / 0-9 slots).",
    "",
    "  • 0e<slot>   start recording; 0e<slot> again to stop.",
    "  • e<slot>    run the macro.   [N]e<slot> runs it N times.",
    "  • E          replay the last run/recorded macro.  [N]E = N times.",
};

static const char *P_ED[] = {        /* 'E' — ed (modeless) mode */
    "Ed mode — modeless editing (type to insert).",
    "",
    "  • Printable keys insert text; Enter / Tab / Backspace / Delete as usual.",
    "  • Arrows move; Shift+arrows select; Ctrl+arrows by word.",
    "  • Ctrl-S save · Ctrl-Z undo · Ctrl-Y redo.",
    "  • Ctrl-A select all · Ctrl-C copy · Ctrl-X cut · Ctrl-V paste.",
    "  • Insert toggles overwrite; Ctrl-Q quit.",
    "  • Esc switches to zx (Vim-style command) mode.",
    "  • Type the trigger zx then Enter to insert the literal text \"zx\".",
    "",
    "  F1 opens this page from ed mode; in zx mode h (or F1) opens the zx help.",
    "",
    "  Space returns to the keyboard overview; Enter closes help.",
};

static const char *P_SCREEN[] = {    /* 'L' — screen layout / part names */
    "Screen layout — the parts of the prov window, top to bottom.",
    "",
    "  • Tab bar       — top row (reverse video): the open tabs; [..] is active.",
    "  • Window / pane — an editing view of a buffer. Splits tile the tab into",
    "                    several windows; the focused one is bright, others dim.",
    "  • Gutter        — left column of a window: line numbers (zc line_numbers).",
    "  • Scrollbar     — right column of a window: │ track, █ thumb, ▲▼ at the ends.",
    "  • Window status — a window's own bottom row: X(close) name [*]modified [RO].",
    "",
    "  Global rows at the very bottom:",
    "  • Status line   — X(mouse close) · mode · codepage · encoding · BOM · EOL ·",
    "                    byte and char position. One per screen.",
    "  • Command line  — the bottom-most row: typed commands (zp), the search/replace",
    "                    input, and the key legend for the current mode or panel",
    "                    (shortcut keys shown bold + underlined).",
    "",
    "  • Panel         — a modal box (help, file browser, 0-series lists). Its keys",
    "                    appear on the command line; Esc / q / x closes it.",
    "",
    "  Color is used only for syntax highlighting; the chrome above stays monochrome",
    "  (grays via dim/bold, plus reverse video for emphasis).",
    "",
    "  Space returns to the keyboard overview; Enter closes help.",
};

static const char *P_REGEX[] = {     /* 'X' — regex reference (sh) */
    "Regular expressions (prov engine, RFC-0009).  Enable with so x; sh shows this.",
    "Case follows the search 'case' option (so c). ASCII semantics.",
    "",
    "  LITERALS",
    "  • Ordinary characters match themselves.",
    "  • \\\\ escapes a metacharacter:  \\. \\* \\+ \\? \\( \\) \\[ \\] \\{ \\} \\| \\^ \\$ \\\\",
    "  • Control escapes:  \\n tab \\t  \\r  \\f  \\v  \\0 (NUL).",
    "",
    "  ANY & CLASSES",
    "  • .            any character except newline.",
    "  • [abc]        any one of a, b, c.        [^abc]  none of them.",
    "  • [a-z0-9]     ranges.  Put ] or - first/last to mean them literally.",
    "  • \\d \\w \\s    digit / word [A-Za-z0-9_] / whitespace.",
    "  • \\D \\W \\S    their complements.  (All usable inside [ ] too.)",
    "",
    "  ANCHORS (zero-width)",
    "  • ^  start of text/line.    $  end of text/line.",
    "  • \\b word boundary.        \\B  non-boundary.",
    "  • \\zs / \\ze  set where the MATCH starts / ends (look-behind/ahead-ish,",
    "                 Vim-style): foo\\zsbar matches 'bar' only after 'foo'.",
    "",
    "  QUANTIFIERS (greedy; append ? to make lazy)",
    "  • *   0 or more     +   1 or more     ?   0 or 1",
    "  • {n} exactly n     {n,} n or more    {n,m} between n and m",
    "  • *? +? ?? {n,m}?   lazy (match as few as possible).",
    "",
    "  GROUPS & ALTERNATION",
    "  • (...)        capturing group.        (?:...)  non-capturing group.",
    "  • a|b          alternation (match a or b).",
    "  • In replace, captured groups are reused by the replace string (sr).",
    "",
    "  NOT SUPPORTED (engine kept small)",
    "  • Backreferences (\\1..\\9), look-around ((?=) (?!) (?<= )), named groups,",
    "    inline flags ((?i) …), POSIX [:class:], and Unicode property classes.",
    "  • Matching is per-line for search; . never crosses a newline.",
    "",
    "  Space returns to the keyboard overview; Enter closes help.",
};

static const char *P_OTHER[] = {     /* '?' fallback / unbound keys */
    "Unbound key.",
    "",
    "  This key has no command in this build.",
    "",
    "  Space returns to the keyboard overview; Enter closes help.",
};

#define PAGE(arr) do { *title = TITLE; src = (arr); n = (int)(sizeof(arr)/sizeof((arr)[0])); } while (0)

/* The "0<key>" companion for a topic key, appended to every page that has one,
 * so each function's help advertises its leading-0 variant. NULL = none. */
static const char *zero_note(int topic) {
    switch (topic) {
        case 'g': return "0-prefix:  0g  jump to the last line.";
        case 'u': return "0-prefix:  0u  undo-history browser (planned).";
        case 's': return "0-prefix:  0s  search-history browser (planned).";
        case 'b': return "0-prefix:  0m<letter> set a bookmark  ·  0b register list (planned).";
        case 'z': return "0-prefix:  0z  command-history browser (planned).";
        case 'w': return "0-prefix:  0w  window / pane overview (planned).";
        case 't': return "0-prefix:  0t  tab overview (planned).";
        case 'e': return "0-prefix:  0e<slot>  start / stop recording a macro.";
        case 'n': return "0-prefix:  0n  movement-history browser (planned).";
        default:  return 0;
    }
}

int prov_help_page(int topic, const char **title, const char **lines, int cap) {
    const char **src = OVERVIEW;
    int n = 0;
    const char *TITLE = "prov help";
    switch (topic) {
        case 0:   TITLE = "prov help — keyboard overview"; PAGE(OVERVIEW); break;
        case 'h': TITLE = "zx basics & command syntax";    PAGE(P_ZX);     break;
        case '0': TITLE = "0 — special prefix";            PAGE(P_ZERO);   break;
        case '#': TITLE = "1-9 — count prefix";            PAGE(P_COUNT);  break;
        case 'u': TITLE = "u — undo / redo";               PAGE(P_UNDO);   break;
        case 'i': TITLE = "i k j l — cursor";              PAGE(P_CURSOR); break;
        case 'a': TITLE = "a — field mode (bounded input)";  PAGE(P_APPEND); break;
        case 'n': TITLE = "n — repeat last command";       PAGE(P_NCMD);   break;
        case 'x': TITLE = "x o — quick edits (r reserved)"; PAGE(P_EDIT);  break;
        case 'g': TITLE = "g — goto / movement";           PAGE(P_GOTO);   break;
        case 'b': TITLE = "b registers · m bookmarks";       PAGE(P_REG); break;
        case 'v': TITLE = "v — selection (visual)";        PAGE(P_SELECT); break;
        case 'p': TITLE = "p — paste";                     PAGE(P_PASTE);  break;
        case 'c': TITLE = "c d y — operators";             PAGE(P_OPER);   break;
        case 'w': TITLE = "w — windows / panes";           PAGE(P_WIN);    break;
        case 't': TITLE = "t — tabs / buffers";            PAGE(P_TAB);    break;
        case 's': TITLE = "s — search";                    PAGE(P_SEARCH); break;
        case 'z': TITLE = "z — meta namespace";            PAGE(P_META);   break;
        case 'e': TITLE = "e / E — keystroke macros";      PAGE(P_MACRO);  break;
        case 'E': TITLE = "ed mode — modeless editing";    PAGE(P_ED);     break;
        case 'L': TITLE = "screen layout — part names";    PAGE(P_SCREEN); break;
        case 'X': TITLE = "regular expressions (sh)";      PAGE(P_REGEX);  break;
        default:  TITLE = "unbound key";                   PAGE(P_OTHER);  break;
    }
    int out = n < cap ? n : cap;
    for (int i = 0; i < out; i++) lines[i] = src[i];
    const char *zn = zero_note(topic);              /* advertise the 0<key> variant */
    if (zn && out + 2 <= cap) { lines[out++] = ""; lines[out++] = zn; }
    return out;
}

int prov_help_topic_for_key(int key) {
    switch (key) {
        case 'i': case 'k': case 'j': case 'l': return 'i';
        case 'c': case 'd': case 'y':           return 'c';   /* y is the yank operator */
        case 'x': case 'r': case 'o':           return 'x';
        case 'e': case 'q':                     return 'e';
        case 'b': case 'm':                     return 'b';
        case 'h':                               return 0;     /* back to overview */
        case 'u': case 'a': case 'n': case 'g': case 'v':
        case 'p': case 'w': case 't': case 's':
        case 'z': case '0':                     return key;
        case 'L':                               return 'L';   /* screen layout */
        default:                                return '?';
    }
}
