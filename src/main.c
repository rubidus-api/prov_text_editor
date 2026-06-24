/*
 * prov - a terminal text editor. Entry point and event loop.
 *
 * Wires the tested core (editor / display / save / input / command parser) to
 * the terminal backend (platform). Supports the Ed editing mode (INS/OVR with
 * conventional shortcuts, SPEC §11) and the zx command mode (SPEC §9, §10),
 * switched by the "zx" trigger. Editing logic itself lives in the core modules.
 */

#include <stdio.h>    /* app entry only: config file load + stderr diagnostics */
#include <stdlib.h>
#ifdef _WIN32
#include <direct.h>   /* _getcwd: resolve the browser's start dir to absolute */
#define prov_getcwd _getcwd
#else
#include <unistd.h>   /* getcwd */
#define prov_getcwd getcwd
#endif

#include "proven/heap.h"
#include "proven/allocator.h"
#include "proven/error.h"

#include "editor.h"
#include "buffer.h"
#include "display.h"
#include "search.h"
#include "regex.h"
#include "save.h"
#include "command.h"
#include "config.h"
#include "bufset.h"
#include "layout.h"
#include "help.h"
#include "browser.h"
#include "panel.h"
#include "platform_clipboard.h"
#include "platform_charset.h"
#include "textbox.h"
#include "hexview.h"
#include "lineedit.h"
#include "motion.h"
#include "unicode.h"
#include "draw.h"
#include "pstr.h"
#include "proven/u8str.h"
#include "proven/fmt.h"
#include "proven/time.h"
#include "platform.h"

enum { MODE_ED = 0, MODE_ZX = 1, MODE_FIELD = 2 };

typedef struct prov_session {
    proven_allocator_t a;
    prov_editor_t     *ed;
    const char        *path;
    int                mode;
    bool               overwrite;    /* Ed mode: INS vs OVR */
    bool               readonly;     /* focused window is read-only (per-window) */
    bool               zx_visual;     /* zx mode: extend selection on movement */
    struct {   /* binary/hex view/edit of the focused buffer (RFC-0019; buffer info.binary is the toggle) */
        bool       ascii;     /* cursor is in the ASCII pane (Tab toggles) */
        bool       pend;      /* a high nibble was typed, awaiting the low nibble */
        proven_u8  hi;        /* the pending high nibble's value (0..15) */
        bool       sel;       /* visual byte-range selection active (v; P3) */
        proven_size_t sel_anchor;  /* the selection's fixed end (byte offset) */
        proven_u8 *buf;       /* per-frame materialized bytes for hex rendering (reused) */
        proven_size_t cap;
        proven_u8 *clip;      /* byte yank/paste register (y copies, p pastes) */
        proven_size_t clip_len, clip_cap;
    } hex;
    struct {   /* PANEL_K_HEXEDIT: editing a decoded byte range as text (RFC-0019 P3) */
        prov_editor_t *ed;     /* temp editor holding the decoded selection */
        proven_size_t  lo, hi; /* the byte range in the main buffer being replaced */
        proven_size_t  top;    /* panel viewport top line */
        bool           overwrite;
    } hexedit;
    bool               block_visual;  /* V: rectangular (visual-block) selection */
    proven_size_t      block_anchor_line, block_anchor_col;  /* the block's fixed corner */
    bool               block_insert;  /* I/A: typing on the top row, replicate on Esc */
    proven_size_t      block_ins_r0, block_ins_r1, block_ins_col, block_ins_anchor;
    /* field mode (RFC-0007): a bounded fragment-input session over [origin,end).
     * end is derived as (buffer length - field_after); field_after = bytes that
     * follow the region and never change, so end survives in-field undo/redo. */
    proven_size_t      field_origin;  /* region start (clamped lower bound) */
    proven_size_t      field_after;   /* constant: bytes after the region */
    proven_u32         field_count;   /* repeat N applied at commit (a/o); 1 for c */
    proven_size_t      field_tgt_len; /* c: original target length at [origin,origin+len); 0 for a/o */
    proven_u32         trig_pending;  /* held first trigger char ('z') */
    bool               zx_from_trigger; /* just entered zx via the "zx" trigger; an immediate Enter types the literal trigger */
    prov_cmd_parser_t  parser;
    bool               modified;
    bool               running;
    proven_size_t      top;          /* viewport: first visible buffer line */
    proven_size_t      leftcol;      /* viewport: horizontal origin (visual col) when wrap=off */
    char               zx_pending[24]; /* keys typed for the in-progress zx command */
    char               zx_last[80];    /* "keys  function" of the last zx command */
    prov_command_t     last_repeatable; /* last non-repeat command, for `n` */
    bool               has_repeatable;  /* a command has been recorded for `n` */
    proven_u32         last_find_char;  /* for ; / , : the last f/t target char */
    bool               last_find_till;  /* the last find was a `t` (till) not an `f` */
    bool               last_find_valid; /* an f/t has been used this session */
    prov_config_t      cfg;             /* resolved ~/.prov/config.toml settings */
    prov_bufset_t      bufs;            /* open buffers; s.ed/path/top/... mirror the active one */
    bool               buf_select;      /* zb: awaiting a digit to switch buffers */
    bool               buf_new;         /* zb then n: create a new empty buffer */
    prov_layout_t      tabs[PROV_MAX_TABS]; /* each tab is a whole window layout */
    int                tab;             /* active tab index */
    int                tab_count;       /* number of open tabs */
    unsigned           tab_fold;        /* 0t panel: bitmask of tabs expanded to show their windows */
    int                win_swap;        /* 0w panel: leaf node marked for swap (-1 = none) */
    int                pane_mode;       /* 0 none, 2 ws resize submode */
    int                mouse_drag;      /* RFC-0014: what a left-drag does (MDRAG_*) */
    int                area_h, area_w;  /* last text-area size, for pane geometry */
    int                help_topic;      /* current keyboard-help page key (0 = overview); PANEL_K_HELP */
    bool               help_await;      /* PANEL_K_HELP: `h` pressed, awaiting the key to look up */
    /* file-open browser (goal 4 / RFC-0010 P6): a heavy/virtual panel over a
     * directory model; .view holds the filtered entry indices the vsource reads. */
    struct {
        prov_browser_t       model;     /* current directory model */
        prov_panel_vsource_t vs;        /* the panel's virtual source (ctx = session) */
        unsigned             cols;      /* BCOL_* bitmask: visible info columns */
        proven_size_t       *view;      /* filtered entry indices (into model.entries) */
        proven_size_t        view_n, view_cap;
        char                 hist[16][1024];   /* visited-dir list (J back / L forward) */
        int                  hist_n, hist_cur; /* cur = the entry being shown */
        int                  sort;             /* BSORT_NAME / _MTIME / _EXT */
        bool                 sort_desc;         /* reverse the active sort field */
        int                  num_w;             /* row-number column width (digits of view_n) */
        int                  ext_w;             /* extension column width: clamp(longest shown ext, 3, 10) */
        char                 postfix[256];     /* `;`-separated suffix filter (".txt;.md;…") */
        prov_lehist_t        pfhist;            /* up/down history for the type-filter box (session ring) */
        bool                 pf_edit;          /* editing the postfix box */
        /* preview pane (RFC-0013): decode of the selected file, stacked below the list */
        bool                 preview;          /* `p`: show the preview box */
        int                  focus;            /* Tab scroll/edit focus: BF_LIST/BF_PREVIEW/BF_PATH */
        proven_u8           *pv_buf;           /* decoded preview content (session-owned) */
        proven_size_t        pv_len, pv_cap;
        bool                 pv_hex;           /* binary -> hex view */
        proven_size_t        pv_top;           /* preview scroll (first visible row) */
        char                 pv_path[1280];    /* cache key: which file is decoded */
        char                 pv_enc[24];       /* cache key: with which encoding */
        prov_lineedit_t      pathedit;         /* RFC-0015: editable path/name field (BF_PATH) */
        prov_lehist_t        pathhist;         /* up/down history for the path field (ring) */
        prov_lineedit_t      pfedit;           /* `f`: the type-filter editor (synced to .postfix) */
        int                  subscreen;        /* RFC-0015: 0 none, BSUB_ENC, BSUB_BACKEND */
        int                  sub_sel;          /* selection within the sub-screen list */
    } browse;
    /* common modal panel (RFC-0010): one open at a time */
    bool               panel_open;
    int                panel_kind;      /* which consumer (PANEL_K_*) */
    bool               panel_filter;    /* in the `ss` text-filter sub-mode */
    bool               panel_help;      /* `h`: the panel's intent/keys page is open */
    int                panel_verb;      /* >0: awaiting a slot key (a-z/0-9) for this verb action; 0 = none */
    proven_size_t      panel_help_scroll;
    proven_size_t      panel_scroll;    /* first visible view row (recomputed per frame) */
    proven_size_t      panel_page;      /* last-rendered content height (for PgUp/Dn) */
    prov_panel_t       panel;
    prov_panel_row_t   panel_rowbuf[64];
    char               panel_txt[64][96];
    bool               quit_wizard;     /* zq with dirty buffers: w/d/c choice on the cmd line */
    int                hex_pending;     /* hex<->text reload awaiting a save decision: 0 none / 1 to-text / 2 to-hex */
    int                orphan_buf;      /* buffer being prompted to save before close (-1=none) */
    prov_editor_t     *close_q[PROV_MAX_BUFFERS]; /* buffers whose last window just closed */
    int                close_qn;
    int                prompt_kind;     /* 0=none, 1=open file, 2=command, 3=save-as */
    char               prompt_label[16];
    prov_lineedit_t    prompt_le;       /* line editor for the cmd-line / save-as prompts (zp/save-as/search) */
    prov_lehist_t      prompt_hist;     /* up/down history for the cmd-line prompt (command/open) */
    int                saveas_state;    /* PANEL_K_SAVEAS: SA_PATH editing vs SA_CONFIRM overwrite */
    int                saveas_buf;      /* buffer index being saved by the save-as dialog */
    char               saveas_msg[160]; /* save dialog: the existing-file / error notice */
    /* RFC-0012 P2: a "picker" panel returns a value to its host. When panel_pick
     * is set, the open panel is a sub-picker; Enter on a leaf calls it with the
     * selected value, Esc calls it with NULL (cancel). The host stashes any text
     * it needs to restore in picker_stash. */
    void              (*panel_pick)(struct prov_session *s, const char *value);
    char               picker_stash[1024];
    char               open_enc[24];   /* open-panel: encoding to load the next file as ("" = auto/UTF-8) */
    bool               open_bom;       /* open-panel: write a BOM on save (UTF encodings) */
    bool               open_ro;        /* open-panel: open the next file read-only */
    bool               open_binary;    /* open-panel: open the next file as raw binary/hex (RFC-0019) */
    int                open_eol;       /* open-panel: forced EOL — 0=auto, 1=LF, 2=CRLF, 3=CR */
    proven_u8         *clip_last;       /* last bytes synced to/from the OS clipboard */
    proven_size_t      clip_last_len, clip_last_cap;
    struct {   /* 0n jumplist: recent jump origins (ring; newest = hist[head-1]) */
        struct { prov_editor_t *ed; proven_size_t pos, line; char preview[80]; } hist[32];
        int n, head;       /* count + ring write index — O(1) push, no struct shift */
    } jump;
    struct {   /* search / replace state (M4.5, RFC-0009) */
        char           term[256];       /* last committed search pattern */
        char           hist[32][256];   /* recent committed terms (ring; newest = hist[hist_head-1]) */
        int            hist_n, hist_head; /* count + ring write index — O(1) push, no shift */
        bool           valid;           /* a search term has been set */
        bool           hl;              /* highlight visible matches (Esc in zx clears) */
        bool           icase;           /* case-insensitive (soc toggles) */
        bool           regex;           /* sox: interpret the term as a regex */
        bool           word;            /* sow: whole-word match (wraps the regex in \b...\b) */
        prov_regex_t  *re;              /* compiled regex when regex && valid; else NULL */
        proven_size_t  origin;          /* cursor when the prompt opened (restore on cancel) */
        proven_u8     *hay;             /* document materialized for the duration of the prompt */
        proven_size_t  haylen;          /* so incremental keystrokes don't re-copy the buffer */
    } search;
    struct {   /* RFC-0016: the find/replace dialog (PANEL_K_FIND) */
        prov_lineedit_t pat, repl;      /* pattern + replacement fields */
        prov_lehist_t   pathist, replhist; /* ↑/↓ history rings (session-scoped) */
        int             focus;          /* FF_PAT / FF_REPL / FF_REGEX / … */
        proven_size_t   matches, index; /* total matches + 1-based current (0 = none) */
        /* scoped replace (RFC-0016 §7): when a selection is active as the panel
         * opens, replace-all (`a`) is restricted to [scope_lo, scope_hi). The `s`
         * toggle turns it off (whole buffer). has_scope gates the toggle's UI. */
        bool            has_scope, scoped;
        proven_size_t   scope_lo, scope_hi;
    } find;
    /* named registers a-z (0..25) + 0-9 (26..35), session-global (M4.2) */
    struct { proven_u8 *bytes; proven_size_t len; prov_reg_shape_t shape; } regs[36];
    struct {   /* macros (M4.6): 36 slots record key sequences */
        prov_key_t    *slot[36];
        proven_size_t  len[36], cap[36];
        bool           rec;             /* currently recording */
        int            rec_slot;        /* slot being recorded */
        int            last;            /* last executed slot (for E); -1 = none */
    } macro;
    struct {   /* pending keys to replay (FIFO), consumed before terminal input */
        prov_key_t    *keys;
        proven_size_t  len, pos, cap;
    } feed;
    proven_u8str_t     scratch;         /* reused per-frame formatting buffer (status line) */
    char               message[120];    /* transient one-shot message on the cmd line */
} prov_session_t;

enum { PROMPT_NONE = 0, PROMPT_OPEN = 1, PROMPT_CMD = 2, PROMPT_SAVEAS = 3, PROMPT_SEARCH = 4, PROMPT_REPLACE = 5,
       PROMPT_HEXGOTO = 6 };/* hex editor: type a hex byte offset to jump to */

/* browser info columns (browse_cols bitmask); the `o` verb cycles presets. */
enum { BF_LIST = 0, BF_PREVIEW, BF_PATH, BF_ENC, BF_BACKEND, BF_BOM, BF_RO, BF_COUNT };  /* browser Tab focus (RFC-0015) */
enum { BSUB_NONE = 0, BSUB_ENC, BSUB_BACKEND };   /* browser option sub-screen (RFC-0015) */
enum { FF_PAT = 0, FF_REPL, FF_REGEX, FF_WORD, FF_CASE, FF_HL, FF_COUNT };   /* find panel Tab focus (RFC-0016) */
/* OS-appropriate backend choices for the picker: "auto" + exactly the backends the
 * charset layer compiled in for this platform (libc/command on POSIX, win32/command
 * on Windows) — so the list never offers an impossible backend (item: no win32 on
 * Linux, no libc on Windows; external iconv shows everywhere). */
#define BR_BACKEND_MAX 6
static int br_backends(const char *out[BR_BACKEND_MAX]) {
    out[0] = "auto";
    const char *names[BR_BACKEND_MAX];
    int m = prov_charset_backend_names(names, BR_BACKEND_MAX - 1);
    int k = 1;
    for (int i = 0; i < m && k < BR_BACKEND_MAX; i++) out[k++] = names[i];
    return k;
}
enum { BCOL_SIZE = 1 << 0, BCOL_PERMS = 1 << 1, BCOL_MTIME = 1 << 2, BCOL_TYPE = 1 << 3,
       BCOL_OWNER = 1 << 4, BCOL_GROUP = 1 << 5 };

static void arm_or_quit(prov_session_t *s);   /* defined below; used by zx_execute */
static void prompt_open(prov_session_t *s, int kind, const char *label);
static void open_config(prov_session_t *s);   /* zc: edit ~/.prov/config.toml */
static void panel_open_saveas(prov_session_t *s, int bufidx);   /* za: full-panel save dialog */
static void panel_open_browser(prov_session_t *s);
static void panel_open_windows(prov_session_t *s);
static void panel_open_tabs(prov_session_t *s);
static void panel_open_regs(prov_session_t *s);
static void panel_open_macros(prov_session_t *s);
static void panel_open_bookmarks(prov_session_t *s);
static void panel_open_search(prov_session_t *s);
static void panel_open_cmds(prov_session_t *s);
static void panel_open_moves(prov_session_t *s);
static void panel_open_undo(prov_session_t *s);
static void panel_open_help(prov_session_t *s, int topic);
static void panel_close(prov_session_t *s);
static void field_begin(prov_session_t *s, proven_size_t origin, proven_u32 count,
                        proven_size_t tgt_len);
static void do_search(prov_session_t *s, bool forward, bool advance);
static void search_recompile(prov_session_t *s);
static void search_run(prov_session_t *s, const proven_u8 *hay, proven_size_t haylen,
                       bool forward, bool advance);
static void search_word(prov_session_t *s);
static void search_cache_begin(prov_session_t *s);
static void search_cache_end(prov_session_t *s);
static void do_replace(prov_session_t *s, const char *repl);
static void do_replace_scoped(prov_session_t *s, const char *repl, proven_size_t lo, proven_size_t hi);
static bool ro_guard(prov_session_t *s);
static bool any_dirty(const prov_session_t *s);
static void prompt_open(prov_session_t *s, int kind, const char *label);
static void hex_replace_open(prov_session_t *s);
static void handle_hexedit_key(prov_session_t *s, prov_key_t k);
static bool is_movement(prov_key_kind_t k);
static void move_by(prov_session_t *s, prov_key_kind_t kind, bool extend, bool word, proven_size_t page);
static void find_replace_one(prov_session_t *s);   /* RFC-0016: replace the match under the cursor, advance */
static void panel_open_find(prov_session_t *s);    /* RFC-0016: open the find/replace dialog */
static void search_hist_push(prov_session_t *s, const char *term);
static void maybe_reapply_config(prov_session_t *s, prov_editor_t *ed, const char *path);  /* zc live-apply */
static int  reg_index(int ch);
static void reg_store(prov_session_t *s, int idx);
static bool reg_load(prov_session_t *s, int idx);
static void clip_push(prov_session_t *s);   /* unnamed register -> OS clipboard */
static void clip_pull(prov_session_t *s);   /* OS clipboard -> unnamed register (if changed) */
static void yank_synced(prov_session_t *s); /* after an unnamed yank/cut: OS clip + numbered ring */
static void jump_push(prov_session_t *s);   /* record the cursor in the 0n jumplist before a jump */
static void block_insert_commit(prov_session_t *s);   /* I/A: replicate the typed text across block rows */
static void macro_append(prov_session_t *s, int slot, prov_key_t k);
static void macro_start(prov_session_t *s, int slot);
static void macro_stop(prov_session_t *s, proven_size_t trailing);
static void feed_push(prov_session_t *s, const prov_key_t *keys, proven_size_t n);

/* The editing target is the focused pane: its buffer (modified/overwrite live in
 * the buffer set) and its own viewport top. The session mirrors that state so
 * the rest of the loop keeps using s->... directly. `bufs.active` is kept equal
 * to the focused leaf's buffer so the existing buffer/tab code still works. */
/* The active tab's layout (a tab is a whole window split tree). */
static prov_layout_t *cur_layout(prov_session_t *s) { return &s->tabs[s->tab]; }
static prov_pane_node_t *focused_win(prov_session_t *s) {
    prov_layout_t *L = cur_layout(s);
    return &L->nodes[L->focus];
}

static int active_buf(const prov_session_t *s) {
    const prov_layout_t *L = &s->tabs[s->tab];
    return L->nodes[L->focus].buf;
}

/* RFC-0019: a buffer is shown/edited as a hex dump iff it was loaded as raw
 * binary. Hex-view ⟺ binary-buffer (every window of a binary buffer is hex). */
static bool buf_is_binary(const prov_session_t *s, int b) { return s->bufs.entries[b].info.binary; }

static void buf_load_active(prov_session_t *s) {
    int b = active_buf(s);
    s->bufs.active = b;
    prov_buf_t *e = &s->bufs.entries[b];
    prov_pane_node_t *w = focused_win(s);
    s->ed = e->ed;
    s->path = e->path[0] ? e->path : NULL;
    s->modified = e->modified;
    s->overwrite = e->overwrite;
    s->top = w->top;
    s->leftcol = w->leftcol;
    s->readonly = w->readonly;        /* per-window */
}

static void buf_save_active(prov_session_t *s) {
    prov_buf_t *e = &s->bufs.entries[active_buf(s)];
    prov_pane_node_t *w = focused_win(s);
    e->modified = s->modified;
    e->overwrite = s->overwrite;
    w->top = s->top;
    w->leftcol = s->leftcol;
    w->readonly = s->readonly;
}

static void buf_reset_idle(prov_session_t *s) {
    s->parser = (prov_cmd_parser_t){0};   /* land at zx idle in the new buffer */
    s->zx_pending[0] = '\0';
    s->zx_last[0] = '\0';
    s->zx_visual = false;
}

/* How many windows (across every tab) currently show buffer `b`. Counts only
 * reachable leaves — prov_layout_close leaves dead nodes in the array. */
static int buf_refcount(prov_session_t *s, int b) {
    int n = 0;
    for (int t = 0; t < s->tab_count; t++) {
        int leaves[PROV_MAX_PANE_NODES];
        int m = prov_layout_leaves(&s->tabs[t], leaves, PROV_MAX_PANE_NODES);
        for (int i = 0; i < m; i++)
            if (s->tabs[t].nodes[leaves[i]].buf == b) n++;
    }
    return n;
}

/* Drop buffer `b` (which must be unreferenced): destroy its editor, remove it
 * from the set, and fix up every window's buffer index (those above b shift
 * down with the array). */
static void remove_buffer(prov_session_t *s, int b) {
    prov_editor_t *dead = s->bufs.entries[b].ed;
    prov_bufset_close(&s->bufs, b);
    prov_editor_destroy(dead);
    for (int t = 0; t < s->tab_count; t++) {
        prov_layout_t *L = &s->tabs[t];
        for (int i = 0; i < L->count; i++)
            if (L->nodes[i].kind == PROV_PANE_LEAF && L->nodes[i].buf > b) L->nodes[i].buf--;
    }
    buf_load_active(s);
}

static int bufset_index_of(prov_session_t *s, const prov_editor_t *ed) {
    for (int i = 0; i < s->bufs.count; i++)
        if (s->bufs.entries[i].ed == ed) return i;
    return -1;
}

/* Queue a buffer (by its stable editor pointer) as a close candidate — used when
 * a window showing it is removed. Only these are auto-closed, so a buffer you
 * merely switched away from, or one loaded but never shown, stays in the list. */
static void queue_close(prov_session_t *s, prov_editor_t *ed) {
    for (int i = 0; i < s->close_qn; i++) if (s->close_q[i] == ed) return;
    if (s->close_qn < PROV_MAX_BUFFERS) s->close_q[s->close_qn++] = ed;
}

/* Resolve queued candidates whose last window just closed: skip ones still shown
 * (or already gone), drop unmodified ones, and on the first modified one open the
 * save-before-close prompt (orphan_buf). Answering it resumes this sweep. */
static void resolve_close(prov_session_t *s) {
    while (s->close_qn > 0) {
        int b = bufset_index_of(s, s->close_q[s->close_qn - 1]);
        if (b < 0 || buf_refcount(s, b) > 0) { s->close_qn--; continue; }
        if (!s->bufs.entries[b].modified) { s->close_qn--; remove_buffer(s, b); continue; }
        s->orphan_buf = b;            /* ask whether to save this one */
        return;
    }
    s->orphan_buf = -1;              /* nothing left to resolve */
}

/* The save-before-close prompt (active while orphan_buf >= 0). For a named
 * buffer: y saves then drops it, n discards. For an unnamed buffer: y opens a
 * save-as text prompt, n discards. Returns true if it consumed the key. */
static bool orphan_prompt_key(prov_session_t *s, prov_key_t k) {
    if (s->orphan_buf < 0) return false;
    if (k.kind != PROV_KEY_CHAR || k.nbytes != 1) return true;   /* ignore; stay */
    int b = s->orphan_buf;
    bool named = s->bufs.entries[b].path[0] != '\0';
    if (k.cp == 'y') {
        if (named) {
            prov_save_buffer(s->a, prov_editor_buffer(s->bufs.entries[b].ed),
                             s->bufs.entries[b].path, &s->bufs.entries[b].info);
            remove_buffer(s, b);
            resolve_close(s);
        } else {
            prompt_open(s, PROMPT_SAVEAS, "save as");   /* keep orphan_buf; chain to save-as */
        }
    } else if (k.cp == 'n') {
        remove_buffer(s, b);            /* discard */
        resolve_close(s);
    }
    return true;                        /* other keys: stay on the prompt */
}

/* Point the focused window at buffer `i` (zb). */
static void buf_switch(prov_session_t *s, int i) {
    if (i < 0 || i >= s->bufs.count || i == active_buf(s)) return;
    buf_save_active(s);
    focused_win(s)->buf = i;
    buf_load_active(s);
    buf_reset_idle(s);
}

/* zb then n: open a new empty buffer in the focused window. */
static void buffer_new(prov_session_t *s) {
    if (s->bufs.count >= PROV_MAX_BUFFERS) { FMT_INTO(s->message, "too many buffers"); return; }
    prov_result_editor_t er = prov_editor_create(s->a);
    if (!PROVEN_IS_OK(er.err)) return;
    buf_save_active(s);
    int i = prov_bufset_add(&s->bufs, er.value, NULL);
    if (i < 0) { prov_editor_destroy(er.value); return; }
    prov_editor_set_undo_limit(er.value, s->cfg.undo_limit);
    focused_win(s)->buf = i;
    buf_load_active(s);
    buf_reset_idle(s);
}

/* zb then q: close the active buffer. Every leaf window (across all tabs) showing
 * it is repointed to a neighbor — a fresh empty buffer is created when it was the
 * only one — so the buffer becomes unreferenced; it is then queued for close and
 * resolve_close drops it (or opens the save-before-close prompt if modified). */
static void buf_close_active(prov_session_t *s) {
    int b = active_buf(s);
    prov_editor_t *dead = s->bufs.entries[b].ed;
    buf_save_active(s);
    int fb;
    if (s->bufs.count <= 1) {                  /* last buffer: replace with a fresh empty one */
        if (s->bufs.count >= PROV_MAX_BUFFERS) { FMT_INTO(s->message, "too many buffers"); return; }
        prov_result_editor_t er = prov_editor_create(s->a);
        if (!PROVEN_IS_OK(er.err)) return;
        fb = prov_bufset_add(&s->bufs, er.value, NULL);
        if (fb < 0) { prov_editor_destroy(er.value); return; }
        prov_editor_set_undo_limit(er.value, s->cfg.undo_limit);
    } else {
        fb = (b == 0) ? 1 : 0;                 /* a different existing buffer */
    }
    for (int t = 0; t < s->tab_count; t++) {   /* repoint windows off the dying buffer */
        prov_layout_t *L = &s->tabs[t];
        for (int i = 0; i < L->count; i++)
            if (L->nodes[i].kind == PROV_PANE_LEAF && L->nodes[i].buf == b) L->nodes[i].buf = fb;
    }
    buf_load_active(s);                        /* the focused window now shows the neighbor */
    queue_close(s, dead);
    resolve_close(s);                          /* drops it, or prompts if modified */
    buf_reset_idle(s);
}

/* ---- tabs (t namespace): each tab is a whole window layout (a "desk") ---- */

/* tn: open a new tab — a fresh single-window layout showing the current buffer. */
static void tab_new(prov_session_t *s) {
    if (s->tab_count >= PROV_MAX_TABS) { FMT_INTO(s->message, "too many tabs"); return; }
    if (s->bufs.count >= PROV_MAX_BUFFERS) { FMT_INTO(s->message, "too many buffers"); return; }
    prov_result_editor_t er = prov_editor_create(s->a);   /* a new tab starts on a fresh empty buffer */
    if (!PROVEN_IS_OK(er.err)) return;
    buf_save_active(s);
    int bi = prov_bufset_add(&s->bufs, er.value, NULL);
    if (bi < 0) { prov_editor_destroy(er.value); FMT_INTO(s->message, "too many buffers"); return; }
    prov_editor_set_undo_limit(er.value, s->cfg.undo_limit);
    int nt = s->tab_count++;
    prov_layout_init(&s->tabs[nt], bi);
    s->tab = nt;
    buf_load_active(s);
    buf_reset_idle(s);
}

/* tq: close the current tab (its windows; buffers stay open). Closing the last
 * tab leaves nothing to show, so it quits (honoring unsaved changes). */
static void tab_close(prov_session_t *s) {
    if (s->tab_count <= 1) { arm_or_quit(s); return; }
    buf_save_active(s);
    prov_layout_t *T = &s->tabs[s->tab];           /* queue this tab's window buffers */
    int leaves[PROV_MAX_PANE_NODES];
    int m = prov_layout_leaves(T, leaves, PROV_MAX_PANE_NODES);
    for (int i = 0; i < m; i++)
        queue_close(s, s->bufs.entries[T->nodes[leaves[i]].buf].ed);
    for (int i = s->tab; i < s->tab_count - 1; i++) s->tabs[i] = s->tabs[i + 1];
    s->tab_count--;
    if (s->tab >= s->tab_count) s->tab = s->tab_count - 1;
    buf_load_active(s);
    buf_reset_idle(s);
    resolve_close(s);                 /* close any buffer whose last window was in this tab */
}

/* [N]t / tj / tl: switch the active tab. */
static void tab_goto(prov_session_t *s, int i) {
    if (i < 0 || i >= s->tab_count || i == s->tab) return;
    buf_save_active(s);
    s->tab = i;
    buf_load_active(s);
    buf_reset_idle(s);
}
static void tab_step(prov_session_t *s, int dir) {
    if (s->tab_count <= 1) return;
    buf_save_active(s);
    s->tab = (s->tab + dir + s->tab_count) % s->tab_count;
    buf_load_active(s);
    buf_reset_idle(s);
}

/* 0t row ids: a tab row carries id = the tab index (0..15); a folded tab's child
 * (window) rows carry id = TAB_CHILD + tab*64 + leaf-node-index. */
enum { TAB_CHILD = 1000 };

/* Move the tab at `from` to position `to`, shifting the others. Only reorders the
 * `tabs[]` array (the active layout's data travels with it), so it just remaps
 * the active index — no buffer reload. The folded view is reset by the caller. */
static void tab_move(prov_session_t *s, int from, int to) {
    if (from < 0 || from >= s->tab_count) return;
    if (to < 0) to = 0;
    if (to >= s->tab_count) to = s->tab_count - 1;
    if (to == from) return;
    prov_layout_t moved = s->tabs[from];
    if (from < to) for (int i = from; i < to; i++) s->tabs[i] = s->tabs[i + 1];
    else           for (int i = from; i > to; i--) s->tabs[i] = s->tabs[i - 1];
    s->tabs[to] = moved;
    if (s->tab == from) s->tab = to;                         /* the active tab followed */
    else if (from < to && s->tab > from && s->tab <= to) s->tab--;
    else if (to < from && s->tab >= to && s->tab < from) s->tab++;
}

/* Close the tab at `idx` (any tab, not just the active one). Its windows' buffers
 * are queued; resolve_close then frees the unmodified ones and prompts to save
 * the modified ones (one at a time, via orphan_buf). Closing the last tab arms
 * the quit flow. */
static void tab_close_at(prov_session_t *s, int idx) {
    if (idx < 0 || idx >= s->tab_count) return;
    if (s->tab_count <= 1) { arm_or_quit(s); return; }
    bool closing_active = (idx == s->tab);
    if (closing_active) buf_save_active(s);
    prov_layout_t *T = &s->tabs[idx];
    int leaves[PROV_MAX_PANE_NODES];
    int m = prov_layout_leaves(T, leaves, PROV_MAX_PANE_NODES);
    for (int i = 0; i < m; i++)
        queue_close(s, s->bufs.entries[T->nodes[leaves[i]].buf].ed);
    for (int i = idx; i < s->tab_count - 1; i++) s->tabs[i] = s->tabs[i + 1];
    s->tab_count--;
    if (s->tab == idx) s->tab = idx < s->tab_count ? idx : s->tab_count - 1;
    else if (s->tab > idx) s->tab--;
    buf_load_active(s);              /* the active window may have shifted index */
    buf_reset_idle(s);
    resolve_close(s);               /* free / prompt-to-save the orphaned buffers */
}

/* The tab index the 0t selection points at: a tab row directly, or a child
 * (window) row's parent tab. */
static int tabs_sel_tab(prov_session_t *s) {
    int id = prov_panel_selected_id(&s->panel);
    if (id < 0) return -1;
    return id < TAB_CHILD ? id : (id - TAB_CHILD) / 64;
}

/* ---- panes / windows (w namespace), within the current tab ---- */
static void pane_split(prov_session_t *s, bool vertical) {
    buf_save_active(s);
    prov_layout_split(cur_layout(s), vertical);   /* new window copies buf+top, gets focus */
    buf_load_active(s);
    buf_reset_idle(s);
}
static void pane_focus_next(prov_session_t *s) {
    if (prov_layout_leaf_count(cur_layout(s)) <= 1) return;
    buf_save_active(s);
    prov_layout_focus_next(cur_layout(s));
    buf_load_active(s);
    buf_reset_idle(s);
}
static void pane_close(prov_session_t *s) {
    buf_save_active(s);
    prov_editor_t *closing = s->bufs.entries[active_buf(s)].ed;
    if (prov_layout_close(cur_layout(s))) {
        buf_load_active(s);
        buf_reset_idle(s);
        queue_close(s, closing);       /* may have freed its buffer */
        resolve_close(s);
    }
}

/* wr: toggle the focused window's read-only flag. */
static void pane_toggle_readonly(prov_session_t *s) {
    s->readonly = !s->readonly;
    focused_win(s)->readonly = s->readonly;
    FMT_INTO(s->message,
             s->readonly ? "window is now read-only (wr to unset)" : "window is writable");
}

/* wx: toggle the focused buffer between text and binary/hex (RFC-0019). Hex always
 * means raw bytes, so this *reloads* the file in the target mode: text→binary reads
 * the bytes verbatim, binary→text re-decodes. Refuses on unsaved changes (a reload
 * would lose them); an unnamed buffer (no disk source) just flips the flag. */
enum { HEX_PEND_NONE = 0, HEX_PEND_TOTEXT = 1, HEX_PEND_TOHEX = 2 };

/* Reload the focused buffer in the target mode (binary<->text), discarding any
 * in-memory edits. Named buffers re-read from disk; an unnamed buffer just flips
 * the flag (its bytes are reinterpreted). Caller has already resolved saving. */
static void hex_reload(prov_session_t *s, bool to_bin) {
    int b = active_buf(s);
    prov_buf_t *e = &s->bufs.entries[b];
    if (e->path[0]) {                                  /* named: reload from disk in the new mode */
        prov_fileinfo_t info;
        const char *interp = s->open_enc[0] ? s->open_enc : (e->info.enc_name[0] ? e->info.enc_name : NULL);
        prov_result_editor_t er = to_bin
            ? prov_editor_open_binary(s->a, e->path, &info, interp)
            : prov_editor_open(s->a, e->path, NULL, &info, NULL, s->cfg.fallback_encoding);
        if (!PROVEN_IS_OK(er.err)) { FMT_INTO(s->message, "reload failed"); return; }
        prov_editor_set_undo_limit(er.value, s->cfg.undo_limit);
        prov_editor_destroy(e->ed);
        e->ed = er.value;
        e->info = info;
        e->modified = false; e->overwrite = false; e->top = 0;
    } else {                                           /* unnamed: flip the flag, keep current bytes */
        e->info.binary = to_bin;
        if (to_bin && s->open_enc[0]) prov_cstr_set(e->info.enc_name, sizeof e->info.enc_name, prov_cstr_view(s->open_enc));
    }
    focused_win(s)->top = 0;
    s->hex.ascii = s->hex.pend = s->hex.sel = false;
    s->zx_visual = false;
    s->modified = false;
    buf_load_active(s);                                /* refresh s->ed / s->top from the (possibly new) editor */
    prov_editor_clear_selection(s->ed);
    FMT_INTO(s->message, to_bin ? "hex editor — Tab pane, v select, r edit, ^S save, Esc/^Q leave"
                                : "text mode");
}

/* wx (and Esc/^Q in the hex editor): switch the focused buffer text<->hex. Hex
 * always means raw bytes, so this reloads the file; if there are unsaved changes
 * it first asks whether to save (the reload would otherwise lose them). */
static void pane_toggle_hex(prov_session_t *s) {
    bool to_bin = !buf_is_binary(s, active_buf(s));
    if (s->modified) {                                 /* ask before the reload (request B/C) */
        s->hex_pending = to_bin ? HEX_PEND_TOHEX : HEX_PEND_TOTEXT;
        FMT_INTO(s->message, "unsaved changes — s save · d discard · c cancel");
        return;
    }
    hex_reload(s, to_bin);
}

/* Resolve the save question raised by pane_toggle_hex / leaving the hex editor. */
static void handle_hex_pending_key(prov_session_t *s, prov_key_t k) {
    bool to_bin = (s->hex_pending == HEX_PEND_TOHEX);
    bool save = false, go = false;
    if (k.kind == PROV_KEY_ESC) { s->hex_pending = HEX_PEND_NONE; FMT_INTO(s->message, "cancelled"); return; }
    if (k.kind == PROV_KEY_CHAR && k.nbytes == 1) {
        char c = k.bytes[0];
        if (c == 'c') { s->hex_pending = HEX_PEND_NONE; FMT_INTO(s->message, "cancelled"); return; }
        if (c == 's' || c == 'y') { save = true; go = true; }
        else if (c == 'd' || c == 'n') { go = true; }
    }
    if (!go) return;                                   /* ignore other keys, keep asking */
    s->hex_pending = HEX_PEND_NONE;
    if (save) {                                        /* save in the CURRENT mode, then reload */
        int b = active_buf(s);
        prov_buf_t *e = &s->bufs.entries[b];
        if (e->path[0]) {
            if (PROVEN_IS_OK(prov_save_buffer(s->a, prov_editor_buffer(s->ed), e->path, &e->info))) {
                s->modified = false; e->modified = false;
            } else { FMT_INTO(s->message, "save failed"); return; }
        }   /* unnamed: nothing on disk to save; the bytes are kept by hex_reload */
    }
    hex_reload(s, to_bin);
}

/* Overwrite the byte at `off` (or append when off == len), then advance. One undo
 * step. Used by both hex-pane nibble entry and ASCII-pane overtype. */
static void hex_overwrite(prov_session_t *s, proven_size_t off, proven_u8 byte) {
    proven_size_t len = prov_buffer_byte_len(prov_editor_buffer(s->ed));
    (void)prov_editor_replace_range(s->ed, off, off < len ? 1 : 0, &byte, 1);
    prov_editor_move_to(s->ed, off + 1);
    s->modified = true;
}

static int hex_digit_val(proven_u32 cp) {
    if (cp >= '0' && cp <= '9') return (int)(cp - '0');
    if (cp >= 'a' && cp <= 'f') return (int)(cp - 'a' + 10);
    if (cp >= 'A' && cp <= 'F') return (int)(cp - 'A' + 10);
    return -1;
}

/* The selected byte range [lo, hi) when a hex visual selection is active (P3);
 * anchor and cursor bytes are both inclusive. */
static void hex_sel_range(const prov_session_t *s, proven_size_t cur,
                          proven_size_t *lo, proven_size_t *hi) {
    proven_size_t a = s->hex.sel_anchor;
    if (a <= cur) { *lo = a; *hi = cur + 1; } else { *lo = cur; *hi = a + 1; }
}

/* Delete the active hex selection [lo, hi) as one undo step; cursor to lo. */
static void hex_delete_sel(prov_session_t *s, proven_size_t off) {
    proven_size_t lo, hi; hex_sel_range(s, off, &lo, &hi);
    proven_u8 d = 0;
    (void)prov_editor_replace_range(s->ed, lo, hi - lo, &d, 0);
    prov_editor_move_to(s->ed, lo);
    s->hex.sel = false;
    s->modified = true;
}

/* Destination byte offset for a movement key in the hex editor (BPR rows). */
static proven_size_t hex_move_dst(proven_size_t off, proven_size_t len, int align, prov_key_kind_t kind) {
    proven_size_t bpr = PROV_HEX_BPR, page = bpr * 16;
    switch (kind) {
        case PROV_KEY_LEFT:     return off ? off - 1 : 0;
        case PROV_KEY_RIGHT:    return off < len ? off + 1 : len;
        case PROV_KEY_UP:       return off >= bpr ? off - bpr : 0;
        case PROV_KEY_DOWN:     return off + bpr <= len ? off + bpr : len;
        case PROV_KEY_PAGEUP:   return off >= page ? off - page : 0;
        case PROV_KEY_PAGEDOWN: return off + page <= len ? off + page : len;
        case PROV_KEY_HOME:   { int slot = (int)((off + (proven_size_t)align) % bpr); return off - (proven_size_t)slot; }
        case PROV_KEY_END:    { int slot = (int)((off + (proven_size_t)align) % bpr);
                                proven_size_t e = off + (bpr - 1 - (proven_size_t)slot); return e < len ? e : len; }
        default: return off;
    }
}

/* Apply a hex movement, extending the selection when shift is held / it is active. */
static void hex_do_move(prov_session_t *s, proven_size_t off, prov_key_kind_t kind, bool shift) {
    s->hex.pend = false;
    if (shift && !s->hex.sel) { s->hex.sel = true; s->hex.sel_anchor = off; }
    proven_size_t len = prov_buffer_byte_len(prov_editor_buffer(s->ed));
    prov_editor_move_to(s->ed, hex_move_dst(off, len, focused_win(s)->hex_align, kind));
}

/* y: copy the selected bytes into the hex register (or the byte under the cursor). */
static void hex_yank(prov_session_t *s, proven_size_t off) {
    proven_size_t len = prov_buffer_byte_len(prov_editor_buffer(s->ed));
    proven_size_t lo = off, hi = (off < len) ? off + 1 : off;
    if (s->hex.sel) hex_sel_range(s, off, &lo, &hi);
    if (hi <= lo) return;
    proven_size_t n = hi - lo;
    if (n > s->hex.clip_cap) {
        proven_result_mem_mut_t rm = s->hex.clip
            ? s->a.realloc_fn(s->a.ctx, s->hex.clip, s->hex.clip_cap, n, 16)
            : s->a.alloc_fn(s->a.ctx, n, 16);
        if (!PROVEN_IS_OK(rm.err)) return;
        s->hex.clip = (proven_u8 *)rm.value.ptr; s->hex.clip_cap = n;
    }
    prov_buffer_copy_range(prov_editor_buffer(s->ed), lo, hi, s->hex.clip, n);
    s->hex.clip_len = n;
    s->hex.sel = false;
    FMT_INTO(s->message, "yanked {} bytes", PROVEN_ARG((proven_u32)n));
}

/* p: insert the hex register's bytes at the cursor (replacing a selection first). */
static void hex_paste(prov_session_t *s, proven_size_t off) {
    if (ro_guard(s)) return;
    if (!s->hex.clip_len) { FMT_INTO(s->message, "nothing yanked"); return; }
    proven_size_t lo = off, hi = off;
    if (s->hex.sel) hex_sel_range(s, off, &lo, &hi);
    (void)prov_editor_replace_range(s->ed, lo, hi - lo, s->hex.clip, s->hex.clip_len);
    prov_editor_move_to(s->ed, lo + s->hex.clip_len);
    s->hex.sel = false; s->modified = true;
}

/* RFC-0019 P3: `r` opens a full multi-line ed-mode editing panel on the selected
 * bytes (or the byte at the cursor), decoded with the buffer's interpretation
 * charset. The text is edited in a temporary editor; ^S re-encodes it and writes
 * it back over the range (one undo step), Esc cancels. */
static void hex_replace_open(prov_session_t *s) {
    if (ro_guard(s)) return;
    proven_size_t off = prov_editor_cursor_byte(s->ed);
    proven_size_t lo = off, hi = (off < prov_buffer_byte_len(prov_editor_buffer(s->ed))) ? off : off;
    if (s->hex.sel) hex_sel_range(s, off, &lo, &hi);
    proven_size_t n = hi - lo;
    proven_result_mem_mut_t rm = s->a.alloc_fn(s->a.ctx, n ? n : 1, 16);
    if (!PROVEN_IS_OK(rm.err)) return;
    proven_u8 *raw = (proven_u8 *)rm.value.ptr;
    if (n) prov_buffer_copy_range(prov_editor_buffer(s->ed), lo, hi, raw, n);

    const char *enc = s->bufs.entries[active_buf(s)].info.enc_name;
    const proven_u8 *txt = raw; proven_size_t txtn = n; proven_u8 *owned = NULL;
    if (enc && enc[0]) {                               /* decode the charset to UTF-8 for editing */
        proven_size_t un = 0;
        owned = prov_charset_to_utf8(s->a, enc, raw, n, &un);
        if (owned) { txt = owned; txtn = un; }
    }
    prov_result_editor_t er = prov_editor_create(s->a);
    if (!PROVEN_IS_OK(er.err)) { s->a.free_fn(s->a.ctx, raw); if (owned) s->a.free_fn(s->a.ctx, owned); return; }
    if (txtn) (void)prov_editor_insert(er.value, txt, txtn);
    prov_editor_move_to(er.value, 0);
    s->a.free_fn(s->a.ctx, raw); if (owned) s->a.free_fn(s->a.ctx, owned);

    s->hexedit.ed = er.value;
    s->hexedit.lo = lo; s->hexedit.hi = hi;
    s->hexedit.top = 0; s->hexedit.overwrite = false;
    s->hex.sel = false; s->hex.pend = false;
    panel_close(s);
    prov_panel_init(&s->panel, s->a, "Edit string (re-encoded on ^S)", NULL, 0, NULL);
    s->panel_kind = PANEL_K_HEXEDIT;
    s->panel_open = true; s->panel_filter = false; s->panel_help = false;
    s->panel.pos = PANEL_FULL;
    FMT_INTO(s->message, "edit string — ^S writes it back, Esc cancels");
}

/* ^S in the hex-edit panel: re-encode the edited text to the interpretation
 * charset and write it over [lo, hi) in the main buffer as one undo step. */
static void hexedit_commit(prov_session_t *s) {
    prov_editor_t *te = s->hexedit.ed;
    if (!te) { panel_close(s); return; }
    const prov_buffer_t *tb = prov_editor_buffer(te);
    proven_size_t tn = prov_buffer_byte_len(tb);
    proven_result_mem_mut_t rm = s->a.alloc_fn(s->a.ctx, tn ? tn : 1, 16);
    if (!PROVEN_IS_OK(rm.err)) return;
    proven_u8 *txt = (proven_u8 *)rm.value.ptr;
    if (tn) prov_buffer_copy_range(tb, 0, tn, txt, tn);

    const char *enc = s->bufs.entries[active_buf(s)].info.enc_name;
    const proven_u8 *bytes = txt; proven_size_t blen = tn; proven_u8 *owned = NULL;
    if (enc && enc[0]) {
        proven_size_t en = 0;
        owned = prov_charset_from_utf8(s->a, enc, txt, tn, &en);
        if (!owned) { FMT_INTO(s->message, "cannot encode to {}", PROVEN_ARG(prov_cstr_view(enc)));
                      s->a.free_fn(s->a.ctx, txt); return; }
        bytes = owned; blen = en;
    }
    if (!ro_guard(s)) {
        (void)prov_editor_replace_range(s->ed, s->hexedit.lo, s->hexedit.hi - s->hexedit.lo, bytes, blen);
        prov_editor_move_to(s->ed, s->hexedit.lo + blen);
        s->modified = true;
        FMT_INTO(s->message, "wrote {} bytes", PROVEN_ARG((proven_u32)blen));
    }
    s->a.free_fn(s->a.ctx, txt); if (owned) s->a.free_fn(s->a.ctx, owned);
    prov_editor_destroy(te); s->hexedit.ed = NULL;
    panel_close(s);
}

static void hexedit_cancel(prov_session_t *s) {
    if (s->hexedit.ed) { prov_editor_destroy(s->hexedit.ed); s->hexedit.ed = NULL; }
    panel_close(s);
    FMT_INTO(s->message, "edit cancelled");
}

/* RFC-0018/0019 hex-mode input: movement (+ visual selection), nibble / ASCII
 * overtype, byte insert/delete, range string-replace (Enter), byte-window nudge,
 * save/undo, and Esc to leave. The editor byte cursor is the single position. */
static void handle_hex_key(prov_session_t *s, prov_key_t k) {
    prov_pane_node_t *w = focused_win(s);
    proven_size_t len = prov_buffer_byte_len(prov_editor_buffer(s->ed));
    proven_size_t off = prov_editor_cursor_byte(s->ed);
    bool ro = s->readonly;

    /* Esc: cancel a pending nibble, then a selection, then leave hex (reload to text). */
    if (k.kind == PROV_KEY_ESC) {
        if (s->hex.pend) { s->hex.pend = false; return; }
        if (s->hex.sel)  { s->hex.sel = false; return; }
        pane_toggle_hex(s);                 /* leave hex → text (asks to save if modified) */
        return;
    }
    if (k.kind == PROV_KEY_TAB) { s->hex.pend = false; s->hex.ascii = !s->hex.ascii; return; }

    /* movement via the arrow / Page / Home / End keys (both panes; any move discards a nibble) */
    switch (k.kind) {
        case PROV_KEY_LEFT: case PROV_KEY_RIGHT: case PROV_KEY_UP: case PROV_KEY_DOWN:
        case PROV_KEY_PAGEUP: case PROV_KEY_PAGEDOWN: case PROV_KEY_HOME: case PROV_KEY_END:
            hex_do_move(s, off, k.kind, k.shift); return;
        case PROV_KEY_INSERT:                     /* Insert key: insert a 0x00 byte (both panes) */
            s->hex.pend = false;
            if (ro_guard(s)) return;
            { proven_u8 zero = 0; (void)prov_editor_replace_range(s->ed, off, 0, &zero, 1); }
            prov_editor_move_to(s->ed, off); s->modified = true; return;
        default: break;
    }

    if (k.kind == PROV_KEY_CTRL) {                /* ^S save+leave · ^Q leave · ^Z/^Y undo · ^G goto offset */
        s->hex.pend = false;
        if (k.cp == 's') {                        /* save the raw bytes (verbatim), then leave to text */
            int b = active_buf(s);
            prov_buf_t *e = &s->bufs.entries[b];
            if (!e->path[0]) { FMT_INTO(s->message, "no file name — Esc leaves; save-as in text mode"); return; }
            if (PROVEN_IS_OK(prov_save_buffer(s->a, prov_editor_buffer(s->ed), e->path, &e->info))) {
                s->modified = false; e->modified = false;
                hex_reload(s, false);             /* saved → leave the hex editor */
            } else FMT_INTO(s->message, "save failed");
            return;
        }
        if (k.cp == 'q') { pane_toggle_hex(s); return; }   /* leave hex (asks to save if modified) */
        if (k.cp == 'z') { if (prov_editor_undo(s->ed)) s->modified = true; return; }
        if (k.cp == 'y') { if (prov_editor_redo(s->ed)) s->modified = true; return; }
        if (k.cp == 'g') { prompt_open(s, PROMPT_HEXGOTO, "goto hex offset"); return; }   /* jump to a byte */
        return;
    }

    /* Delete / Backspace: the selection if any, else one byte (one undo step). */
    if (k.kind == PROV_KEY_DELETE) {
        s->hex.pend = false;
        if (ro) { (void)ro_guard(s); return; }
        if (s->hex.sel) { hex_delete_sel(s, off); return; }
        if (off < len) { proven_u8 d = 0; (void)prov_editor_replace_range(s->ed, off, 1, &d, 0); s->modified = true; }
        return;
    }
    if (k.kind == PROV_KEY_BACKSPACE) {
        s->hex.pend = false;
        if (ro) { (void)ro_guard(s); return; }
        if (s->hex.sel) { hex_delete_sel(s, off); return; }
        if (off > 0) { proven_u8 d = 0; (void)prov_editor_replace_range(s->ed, off - 1, 1, &d, 0);
                       prov_editor_move_to(s->ed, off - 1); s->modified = true; }
        return;
    }

    if (k.kind != PROV_KEY_CHAR || k.nbytes != 1) return;

    if (s->hex.ascii) {                           /* ASCII pane: every printable byte overtypes */
        if (k.cp >= 0x20 && k.cp < 0x7F) {
            if (ro_guard(s)) return;
            hex_overwrite(s, off, (proven_u8)k.cp);
        }
        return;
    }

    /* hex pane: hex digits assemble a byte; the rest are commands (a..f are nibbles). */
    int v = hex_digit_val(k.cp);
    if (v >= 0) {
        if (ro) { (void)ro_guard(s); return; }
        if (!s->hex.pend) { s->hex.hi = (proven_u8)v; s->hex.pend = true; return; }  /* high nibble */
        s->hex.pend = false;
        hex_overwrite(s, off, (proven_u8)((s->hex.hi << 4) | v));                    /* low nibble: commit */
        return;
    }
    char c = (char)k.cp;
    s->hex.pend = false;
    switch (c) {
        /* movement: i/k/j/l = ↑/↓/←/→ ; uppercase I/K = PgUp/PgDn ; J/L = Home/End */
        case 'i': hex_do_move(s, off, PROV_KEY_UP,    false); break;
        case 'k': hex_do_move(s, off, PROV_KEY_DOWN,  false); break;
        case 'j': hex_do_move(s, off, PROV_KEY_LEFT,  false); break;
        case 'l': hex_do_move(s, off, PROV_KEY_RIGHT, false); break;
        case 'I': hex_do_move(s, off, PROV_KEY_PAGEUP,   false); break;
        case 'K': hex_do_move(s, off, PROV_KEY_PAGEDOWN, false); break;
        case 'J': hex_do_move(s, off, PROV_KEY_HOME, false); break;
        case 'L': hex_do_move(s, off, PROV_KEY_END,  false); break;
        case 'g': prov_editor_move_to(s->ed, 0);   break;                                     /* doc start */
        case 'G': prov_editor_move_to(s->ed, len); break;                                     /* doc end */
        case 'v': if (s->hex.sel) s->hex.sel = false;                                          /* toggle byte selection */
                  else { s->hex.sel = true; s->hex.sel_anchor = off; } break;
        case 'r': hex_replace_open(s); break;                                                 /* string-replace panel */
        case 'y': hex_yank(s, off);  break;                                                   /* copy bytes */
        case 'p': hex_paste(s, off); break;                                                   /* paste bytes */
        case 'o':                                                                              /* insert a 0x00 byte */
            if (ro_guard(s)) break;
            { proven_u8 zero = 0; (void)prov_editor_replace_range(s->ed, off, 0, &zero, 1); }
            prov_editor_move_to(s->ed, off); s->modified = true; break;
        case '[': w->hex_align = (w->hex_align + PROV_HEX_BPR - 1) % PROV_HEX_BPR; break;      /* nudge left  */
        case ']': w->hex_align = (w->hex_align + 1) % PROV_HEX_BPR; break;                     /* nudge right */
        case 'h': panel_open_help(s, 'w'); break;                                             /* hex/window help page */
        case 'x':                                                                              /* delete: selection or one byte */
            if (ro_guard(s)) break;
            if (s->hex.sel) hex_delete_sel(s, off);
            else if (off < len) { proven_u8 d = 0; (void)prov_editor_replace_range(s->ed, off, 1, &d, 0); s->modified = true; }
            break;
        default: break;
    }
}

/* PANEL_K_HEXEDIT input (RFC-0019 P3): full ed-mode editing of the temp editor —
 * typing, Enter (multi-line), arrows + Shift (select) + Ctrl (word), Home/End,
 * Backspace/Delete, ^A select-all, ^C/^X/^V clipboard, ^Z/^Y undo. ^S commits
 * (re-encode + write back), Esc cancels. Reuses the editor primitives by pointing
 * s->ed at the temp editor for the duration of the key. */
static void handle_hexedit_key(prov_session_t *s, prov_key_t k) {
    if (k.kind == PROV_KEY_CTRL && k.cp == 's') { hexedit_commit(s); return; }
    if (k.kind == PROV_KEY_ESC) { hexedit_cancel(s); return; }
    prov_editor_t *te = s->hexedit.ed;
    if (!te) { panel_close(s); return; }

    prov_editor_t *save_ed = s->ed; bool save_mod = s->modified;
    s->ed = te;                                   /* move_by / clip helpers act on the temp editor */
    proven_size_t page = 8;

    if (k.kind == PROV_KEY_CHAR) {
        (void)prov_editor_insert(te, k.bytes, k.nbytes);
    } else if (is_movement(k.kind) || k.kind == PROV_KEY_PAGEUP || k.kind == PROV_KEY_PAGEDOWN) {
        move_by(s, k.kind, k.shift, k.ctrl, page);
    } else switch (k.kind) {
        case PROV_KEY_ENTER:     (void)prov_editor_insert(te, (const proven_u8 *)"\n", 1); break;
        case PROV_KEY_TAB:       (void)prov_editor_insert(te, (const proven_u8 *)"\t", 1); break;
        case PROV_KEY_BACKSPACE: (void)prov_editor_backspace(te); break;
        case PROV_KEY_DELETE:    (void)prov_editor_delete(te);    break;
        case PROV_KEY_INSERT:    s->hexedit.overwrite = !s->hexedit.overwrite; break;
        case PROV_KEY_CTRL:
            if      (k.cp == 'a') prov_editor_select_all(te);
            else if (k.cp == 'c') { (void)prov_editor_copy_selection(te); clip_push(s); }
            else if (k.cp == 'x') { (void)prov_editor_cut_selection(te);  clip_push(s); }
            else if (k.cp == 'v') { clip_pull(s); (void)prov_editor_paste(te); }
            else if (k.cp == 'z') (void)prov_editor_undo(te);
            else if (k.cp == 'y') (void)prov_editor_redo(te);
            break;
        default: break;
    }
    s->ed = save_ed; s->modified = save_mod;
}

/* Refuse a mutation in a read-only window; returns true (and posts a message)
 * when the edit must be blocked. */
static bool ro_guard(prov_session_t *s) {
    if (!s->readonly) return false;
    FMT_INTO(s->message, "read-only window — wr to unset");
    return true;
}


/* wi/wk/wj/wl: focus the window in a direction within the current tab. */
static void pane_move_dir(prov_session_t *s, prov_dir_t dir) {
    prov_rect_t area = { 0, 0, s->area_h, s->area_w };
    buf_save_active(s);
    if (prov_layout_move_focus(cur_layout(s), area, dir)) {
        buf_load_active(s);
        buf_reset_idle(s);
    }
}

/* wp: focus the previous window in left-to-right / top-to-bottom order (wraps). */
static void pane_focus_prev(prov_session_t *s) {
    prov_layout_t *L = cur_layout(s);
    int leaves[PROV_MAX_PANE_NODES];
    int n = prov_layout_leaves(L, leaves, PROV_MAX_PANE_NODES);
    if (n <= 1) return;
    int pos = 0;
    for (int i = 0; i < n; i++) if (leaves[i] == L->focus) pos = i;
    buf_save_active(s);
    L->focus = leaves[(pos - 1 + n) % n];
    buf_load_active(s);
    buf_reset_idle(s);
}

/* ww: open the window-list popup for the current tab. */

/* Enter the ws resize submode; a no-op with one window. */
static void pane_enter_resize(prov_session_t *s) {
    if (prov_layout_leaf_count(cur_layout(s)) <= 1) {
        FMT_INTO(s->message, "only one window");
        return;
    }
    s->pane_mode = 2;
}

/* Handle one key while in the ws resize submode (continuous). Up/down (i/k or
 * arrows) change the window's height, left/right (j/l or arrows) its width —
 * down/right grow, up/left shrink. `c` closes the window (its area merges into a
 * neighbour); `q` (or Esc) leaves. Other keys are ignored (stays in mode). */
static bool pane_mode_key(prov_session_t *s, prov_key_t k) {
    if (s->pane_mode == 0) return false;
    char c = (k.kind == PROV_KEY_CHAR && k.nbytes == 1) ? (char)k.bytes[0] : 0;
    if (k.kind == PROV_KEY_ESC || c == 'q') { s->pane_mode = 0; return true; }
    if (c == 'c') {                                /* close this window; its area joins a neighbour */
        pane_close(s);
        if (prov_layout_leaf_count(cur_layout(s)) <= 1) s->pane_mode = 0;   /* nothing left to resize */
        return true;
    }
    if (c == 'k' || k.kind == PROV_KEY_DOWN)       prov_layout_resize_axis(cur_layout(s), true,  +5);
    else if (c == 'i' || k.kind == PROV_KEY_UP)    prov_layout_resize_axis(cur_layout(s), true,  -5);
    else if (c == 'l' || k.kind == PROV_KEY_RIGHT) prov_layout_resize_axis(cur_layout(s), false, +5);
    else if (c == 'j' || k.kind == PROV_KEY_LEFT)  prov_layout_resize_axis(cur_layout(s), false, -5);
    return true;
}

/* Insert bytes at the cursor; in OVR mode replace the (non-newline) code point
 * under the cursor first. */
static void insert_text(prov_session_t *s, const proven_u8 *b, proven_size_t n) {
    if (ro_guard(s)) return;
    if (s->overwrite && !prov_editor_has_selection(s->ed)) {
        proven_u32 ch = prov_editor_char_at_cursor(s->ed);
        if (ch != 0 && ch != '\n') prov_editor_delete(s->ed);
    }
    if (PROVEN_IS_OK(prov_editor_insert(s->ed, b, n))) s->modified = true;
}

/* Move the cursor to the start of `line` (clamped to the last line). */
static void set_cursor_line(prov_session_t *s, proven_size_t line) {
    const prov_buffer_t *b = prov_editor_buffer(s->ed);
    proven_size_t lc = prov_buffer_line_count(b);
    if (lc == 0) return;
    if (line >= lc) line = lc - 1;
    proven_size_t x = prov_buffer_line_start(b, line);
    prov_editor_select_range(s->ed, x, x);
}

/* Page keys scroll the viewport (s->top) by a page and carry the cursor along.
 * The viewport may advance until only the ~ EOF rows remain below the last line
 * (top up to line_count-1), so the end of the file is reachable. The cursor is
 * kept inside the resulting viewport. */
static void page_scroll(prov_session_t *s, bool down, bool extend,
                        proven_size_t page) {
    const prov_buffer_t *b = prov_editor_buffer(s->ed);
    proven_size_t lc = prov_buffer_line_count(b);
    proven_size_t max_top = lc ? lc - 1 : 0;

    if (down) s->top = (s->top + page > max_top) ? max_top : s->top + page;
    else      s->top = (s->top > page) ? s->top - page : 0;

    prov_editor_set_extending(s->ed, extend);
    for (proven_size_t i = 0; i < page; i++) {
        if (down) prov_editor_move_down(s->ed); else prov_editor_move_up(s->ed);
    }

    /* Clamp the cursor into the (possibly scrolled-past-end) viewport. */
    proven_size_t line = prov_editor_cursor_line(s->ed);
    proven_size_t lo = s->top;
    proven_size_t hi = s->top + (page ? page - 1 : 0);
    if (hi > max_top) hi = max_top;
    proven_size_t want = (line < lo) ? lo : (line > hi ? hi : line);
    if (want != line) set_cursor_line(s, want);
}

static void move_by(prov_session_t *s, prov_key_kind_t kind, bool extend, bool word,
                    proven_size_t page) {
    prov_editor_t *ed = s->ed;
    prov_editor_set_extending(ed, extend);
    const prov_buffer_t *b = prov_editor_buffer(ed);   /* for word motions (Ctrl+arrow) */
    switch (kind) {
        case PROV_KEY_LEFT:
            if (word) prov_editor_move_to(ed, prov_motion_word_prev(b, prov_editor_cursor_byte(ed)));
            else      prov_editor_move_left(ed);
            break;
        case PROV_KEY_RIGHT:
            if (word) prov_editor_move_to(ed, prov_motion_word_next(b, prov_editor_cursor_byte(ed)));
            else      prov_editor_move_right(ed);
            break;
        case PROV_KEY_UP:    prov_editor_move_up(ed);    break;
        case PROV_KEY_DOWN:  prov_editor_move_down(ed);  break;
        case PROV_KEY_HOME:  prov_editor_move_home(ed);  break;
        case PROV_KEY_END:   prov_editor_move_end(ed);   break;
        case PROV_KEY_PAGEUP:
            for (proven_size_t i = 0; i < page; i++) prov_editor_move_up(ed);
            break;
        case PROV_KEY_PAGEDOWN:
            for (proven_size_t i = 0; i < page; i++) prov_editor_move_down(ed);
            break;
        default: break;
    }
}

/* Apply an operator over the byte range [start, end). */
static void apply_op(prov_session_t *s, prov_op_kind_t op,
                     proven_size_t start, proven_size_t end) {
    if (start >= end) return;                /* empty range: no-op */
    if (op == PROV_OP_CHANGE) {              /* c: field mode over the target (pre-selected) */
        field_begin(s, start, 1, end - start);
        return;
    }
    prov_editor_select_range(s->ed, start, end);
    if (op == PROV_OP_YANK) {
        prov_editor_copy_selection(s->ed);
        prov_editor_select_range(s->ed, start, start);   /* collapse */
    } else {
        if (PROVEN_IS_OK(prov_editor_cut_selection(s->ed))) s->modified = true;
    }
    yank_synced(s);
}

/* Move the cursor via an f/t/;/, find on the current line (cnt times). */
static void do_findc(prov_session_t *s, proven_u32 ch, bool till, bool backward,
                     proven_u32 cnt) {
    const prov_buffer_t *b = prov_editor_buffer(s->ed);
    proven_size_t cur = prov_editor_cursor_byte(s->ed);
    for (proven_u32 i = 0; i < cnt; i++) {
        proven_size_t t = prov_motion_findc(b, cur, ch, till, backward);
        if (t == cur) break;               /* not found / no further match */
        cur = t;
    }
    prov_editor_set_extending(s->ed, s->zx_visual);   /* extend in visual mode */
    prov_editor_move_to(s->ed, cur);
}

/* Execute a fully-parsed zx command. */
/* ---- zx command execution -------------------------------------------------
 * zx_execute (bottom) dispatches a fully-parsed command to one of these arm
 * helpers by cmd.kind. Each helper re-derives ed/b locally; `cnt` is the
 * resolved repeat count and `half` a half-page. Pure decomposition of what was
 * one switch — behavior is identical. */

static void zx_exec_move(prov_session_t *s, prov_command_t cmd, proven_u32 cnt) {
    prov_editor_t *ed = s->ed;
    prov_editor_set_extending(ed, s->zx_visual);
    for (proven_u32 i = 0; i < cnt; i++) {
        switch (cmd.move) {
            case PROV_MOVE_UP:    prov_editor_move_up(ed);    break;
            case PROV_MOVE_DOWN:  prov_editor_move_down(ed);  break;
            case PROV_MOVE_LEFT:  prov_editor_move_left(ed);  break;
            case PROV_MOVE_RIGHT: prov_editor_move_right(ed); break;
        }
    }
}

static void zx_exec_edit(prov_session_t *s, prov_command_t cmd, proven_u32 cnt) {
    prov_editor_t *ed = s->ed;
            switch (cmd.edit) {
                case PROV_EDIT_UNDO:
                    if (ro_guard(s)) break;
                    for (proven_u32 i = 0; i < cnt; i++) prov_editor_undo(ed);
                    s->modified = true;
                    break;
                case PROV_EDIT_PASTE:        /* p: char at cursor / line below; [N] copies */
                    if (ro_guard(s)) break;
                    clip_pull(s);
                    if (PROVEN_IS_OK(prov_editor_paste_lines(ed, true, cnt))) s->modified = true;
                    break;
                case PROV_EDIT_PASTE_ABOVE:  /* P: char at cursor / line above */
                    if (ro_guard(s)) break;
                    clip_pull(s);
                    if (PROVEN_IS_OK(prov_editor_paste_lines(ed, false, cnt))) s->modified = true;
                    break;
                case PROV_EDIT_CUT:
                    if (ro_guard(s)) break;
                    if (prov_editor_has_selection(ed)) {
                        if (PROVEN_IS_OK(prov_editor_cut_selection(ed))) s->modified = true;
                    } else if (PROVEN_IS_OK(prov_editor_delete(ed))) {
                        s->modified = true;
                    }
                    yank_synced(s);
                    break;
                case PROV_EDIT_TOGGLE_SELECT:
                    s->zx_visual = !s->zx_visual;
                    prov_editor_set_extending(ed, s->zx_visual);
                    if (!s->zx_visual) prov_editor_clear_selection(ed);
                    break;
                case PROV_EDIT_APPEND:        /* a / [N]a: field mode (empty region, stamp xN) */
                    if (ro_guard(s)) break;
                    field_begin(s, prov_editor_cursor_byte(ed), cnt, 0);
                    break;
                case PROV_EDIT_REPEAT: break;  /* n: handled by the dispatcher (replays last cmd) */
                default: break;   /* r / q: not yet executed */
            }
}

static void zx_exec_operation(prov_session_t *s, prov_command_t cmd, proven_u32 cnt) {
    prov_editor_t *ed = s->ed;
    const prov_buffer_t *b = prov_editor_buffer(ed);
            if (cmd.op != PROV_OP_YANK && ro_guard(s)) return;   /* yank is read-only-safe */
            if (cmd.target == PROV_TARGET_LINEWISE) {
                proven_size_t lc = prov_buffer_line_count(b);
                proven_size_t total = prov_buffer_byte_len(b);
                proven_size_t L = prov_editor_cursor_line(ed);
                proven_size_t start = prov_buffer_line_start(b, L);
                if (cmd.op == PROV_OP_CHANGE) {                 /* cc: change line(s) in field mode */
                    proven_size_t lastL = L + (cnt - 1);
                    proven_size_t lend = (lastL + 1 < lc)
                                       ? prov_buffer_line_start(b, lastL + 1) - 1 : total;
                    field_begin(s, start, 1, lend - start);     /* pre-fill + pre-select the line(s) */
                } else {                                        /* dd / yy */
                    proven_size_t endL = L + cnt;
                    proven_size_t end = (endL < lc) ? prov_buffer_line_start(b, endL) : total;
                    prov_editor_select_range(ed, start, end);
                    if (cmd.op == PROV_OP_DELETE) {
                        if (PROVEN_IS_OK(prov_editor_cut_selection(ed))) s->modified = true;
                    } else {
                        prov_editor_copy_selection(ed);
                        prov_editor_select_range(ed, start, start);  /* collapse */
                    }
                    /* a linewise yank/cut always carries a whole line, even if the
                     * last line had no trailing newline (item: yy then p) */
                    prov_editor_reg_ensure_trailing_newline(ed);
                    yank_synced(s);
                }
                return;
            }
            {   /* motion-based operator targets */
                proven_size_t cur = prov_editor_cursor_byte(ed);
                proven_size_t start = cur, end = cur;
                switch (cmd.target) {
                    case PROV_TARGET_WORD:
                        for (proven_u32 i = 0; i < cnt; i++) end = prov_motion_word_next(b, end);
                        break;
                    case PROV_TARGET_BACK:
                        for (proven_u32 i = 0; i < cnt; i++) start = prov_motion_word_prev(b, start);
                        break;
                    case PROV_TARGET_END:
                        for (proven_u32 i = 0; i < cnt; i++) end = prov_motion_word_end(b, end);
                        break;
                    case PROV_TARGET_LINE_END: {
                        proven_size_t lc = prov_buffer_line_count(b);
                        proven_size_t L = prov_editor_cursor_line(ed);
                        end = (L + 1 < lc) ? prov_buffer_line_start(b, L + 1) - 1
                                           : prov_buffer_byte_len(b);
                        break;
                    }
                    case PROV_TARGET_MATCH: {
                        proven_size_t m = prov_motion_match(b, cur);
                        if (m != cur) { start = m < cur ? m : cur; end = (m < cur ? cur : m) + 1; }
                        break;
                    }
                    case PROV_TARGET_FIND: end = prov_motion_find(b, cur, cmd.param, false); break;
                    case PROV_TARGET_TILL: end = prov_motion_find(b, cur, cmd.param, true);  break;
                    case PROV_TARGET_TEXTOBJ: {
                        prov_range_t r = prov_motion_textobj(b, cur, cmd.textobj, cmd.inner);
                        if (r.ok) { start = r.start; end = r.end; }
                        break;
                    }
                    default: break;
                }
                apply_op(s, cmd.op, start, end);
            }
}

static void zx_exec_action(prov_session_t *s, prov_command_t cmd, proven_u32 cnt,
                           proven_size_t half) {
    prov_editor_t *ed = s->ed;
    const prov_buffer_t *b = prov_editor_buffer(ed);
            switch (cmd.action) {
                case PROV_ACT_RETURN_ED:        /* zx: plain return to Ed mode (no insertion) */
                    s->mode = MODE_ED; s->zx_visual = false;
                    prov_editor_set_extending(ed, false);
                    prov_editor_clear_selection(ed);
                    break;
                case PROV_ACT_REDO:
                    for (proven_u32 i = 0; i < cnt; i++) prov_editor_redo(ed);
                    s->modified = true;
                    break;
                case PROV_ACT_WRITE_FILE:
                    if (s->path && s->path[0]) {                /* named: write straight to its path */
                        if (PROVEN_IS_OK(prov_save_buffer(s->a, b, s->path, &s->bufs.entries[active_buf(s)].info))) {
                            prov_editor_compact(ed);   /* reclaim orphaned add-store memory */
                            s->modified = false;
                            maybe_reapply_config(s, ed, s->path);   /* zc live-apply (RFC) */
                        }
                    } else {
                        panel_open_saveas(s, active_buf(s));    /* unnamed: ask for a path */
                    }
                    break;
                case PROV_ACT_WRITE_AS:                         /* za: always prompt for a path */
                    panel_open_saveas(s, active_buf(s));
                    break;
                case PROV_ACT_QUIT:        arm_or_quit(s); break;
                case PROV_ACT_DOC_START:   prov_editor_set_extending(ed, s->zx_visual); prov_editor_move_doc_start(ed); break;
                case PROV_ACT_DOC_END:     prov_editor_set_extending(ed, s->zx_visual); prov_editor_move_doc_end(ed);   break;
                case PROV_ACT_LINE_START:  prov_editor_set_extending(ed, s->zx_visual); prov_editor_move_home(ed);      break;
                case PROV_ACT_LINE_END:    prov_editor_set_extending(ed, s->zx_visual); prov_editor_move_end(ed);       break;
                case PROV_ACT_HALF_PAGE_UP:
                    prov_editor_set_extending(ed, s->zx_visual);
                    for (proven_size_t i = 0; i < half; i++) prov_editor_move_up(ed);
                    break;
                case PROV_ACT_HALF_PAGE_DOWN:
                    prov_editor_set_extending(ed, s->zx_visual);
                    for (proven_size_t i = 0; i < half; i++) prov_editor_move_down(ed);
                    break;
                case PROV_ACT_GOTO_LINE: {
                    proven_size_t lc = prov_buffer_line_count(b);
                    proven_size_t tgt = cnt ? cnt - 1 : 0;
                    if (tgt >= lc) tgt = lc - 1;
                    jump_push(s);
                    prov_editor_set_extending(ed, s->zx_visual);
                    prov_editor_move_to(ed, prov_buffer_line_start(b, tgt));
                    break;
                }
                case PROV_ACT_FILE_LAST_LINE: {
                    proven_size_t lc = prov_buffer_line_count(b);
                    jump_push(s);
                    prov_editor_set_extending(ed, s->zx_visual);
                    prov_editor_move_to(ed, prov_buffer_line_start(b, lc - 1));
                    break;
                }
                case PROV_ACT_PREV_WORD: {
                    proven_size_t t = prov_editor_cursor_byte(ed);
                    for (proven_u32 i = 0; i < cnt; i++) t = prov_motion_word_prev(b, t);
                    prov_editor_set_extending(ed, s->zx_visual);
                    prov_editor_move_to(ed, t);
                    break;
                }
                case PROV_ACT_NEXT_WORD: {
                    proven_size_t t = prov_editor_cursor_byte(ed);
                    for (proven_u32 i = 0; i < cnt; i++) t = prov_motion_word_next(b, t);
                    prov_editor_set_extending(ed, s->zx_visual);
                    prov_editor_move_to(ed, t);
                    break;
                }
                case PROV_ACT_OPEN_BELOW: {        /* on / [N]on: field mode, leading \n -> N lines below */
                    if (ro_guard(s)) break;
                    prov_editor_move_end(ed);
                    proven_size_t origin = prov_editor_cursor_byte(ed);   /* eol */
                    field_begin(s, origin, cnt, 0);
                    (void)prov_editor_insert(ed, (const proven_u8 *)"\n", 1);  /* region seed; cursor on the new line */
                    s->modified = true;
                    break;
                }
                case PROV_ACT_OPEN_ABOVE: {        /* op / [N]op: field mode, trailing \n -> N lines above */
                    if (ro_guard(s)) break;
                    prov_editor_move_home(ed);
                    proven_size_t origin = prov_editor_cursor_byte(ed);   /* bol */
                    field_begin(s, origin, cnt, 0);
                    (void)prov_editor_insert(ed, (const proven_u8 *)"\n", 1);  /* region seed */
                    prov_editor_move_to(ed, origin);                      /* type before the \n */
                    s->modified = true;
                    break;
                }
                case PROV_ACT_FIND_NEXT:
                    if (s->last_find_valid)
                        do_findc(s, s->last_find_char, s->last_find_till, false, cnt);
                    break;
                case PROV_ACT_FIND_PREV:
                    if (s->last_find_valid)
                        do_findc(s, s->last_find_char, s->last_find_till, true, cnt);
                    break;
                case PROV_ACT_BUFFER_LIST:
                    s->buf_select = true;   /* zb: digit switches, n = new buffer */
                    break;
                case PROV_ACT_TAB_NEW:   tab_new(s);       break;   /* tn: new tab (desk) */
                case PROV_ACT_TAB_CLOSE: tab_close(s);     break;   /* tq: close tab */
                case PROV_ACT_TAB_PREV:  tab_step(s, -1);  break;   /* tj */
                case PROV_ACT_TAB_NEXT:  tab_step(s, +1);  break;   /* tl */
                case PROV_ACT_GOTO_TAB:                            /* [N]t */
                    if (cnt >= 1) tab_goto(s, (int)cnt - 1);
                    break;
                case PROV_ACT_OPEN_FILE:  panel_open_browser(s); break;   /* zo: file browser panel */
                case PROV_ACT_CONFIG:     open_config(s);         break;   /* zc: edit the config file */
                case PROV_ACT_WIN_OVERVIEW: panel_open_windows(s); break;   /* 0w: windows panel */
                case PROV_ACT_TAB_OVERVIEW: panel_open_tabs(s); break;      /* 0t: tabs panel */
                case PROV_ACT_REG_BROWSER: panel_open_regs(s); break;       /* 0b: registers panel */
                case PROV_ACT_MACRO_OVERVIEW: panel_open_macros(s); break;  /* 0e: macros panel */
                case PROV_ACT_BOOKMARK_OVERVIEW: panel_open_bookmarks(s); break; /* 0m: bookmarks panel */
                case PROV_ACT_SEARCH_BROWSER: panel_open_search(s); break;  /* 0s: search history panel */
                case PROV_ACT_CMD_BROWSER: panel_open_cmds(s); break;       /* 0z: command cheat-sheet */
                case PROV_ACT_MOVE_HISTORY: panel_open_moves(s); break;     /* 0n: jumplist */
                case PROV_ACT_UNDO_BROWSER: panel_open_undo(s); break;      /* 0u: undo history */
                case PROV_ACT_CMD_PROMPT: prompt_open(s, PROMPT_CMD, "cmd");   break;   /* zp */
                case PROV_ACT_SEARCH_PROMPT:                                            /* ss */
                    s->search.origin = prov_editor_cursor_byte(ed);
                    prompt_open(s, PROMPT_SEARCH, "search");
                    search_cache_begin(s);     /* materialize once for the whole prompt */
                    break;
                case PROV_ACT_SEARCH_PANEL: panel_open_find(s); break;                  /* / : find dialog */
                case PROV_ACT_SEARCH_WORD: search_word(s); break;                       /* sw */
                case PROV_ACT_SEARCH_NEXT: do_search(s, true, true); break;             /* sn */
                case PROV_ACT_SEARCH_PREV: do_search(s, false, true); break;            /* sp */
                case PROV_ACT_SEARCH_CASE:                                              /* soc */
                    s->search.icase = !s->search.icase;
                    search_recompile(s);
                    FMT_INTO(s->message, "search: case-{}", PROVEN_ARG(prov_cstr_view(s->search.icase ? "insensitive" : "sensitive")));
                    if (s->search.valid) do_search(s, true, false);
                    break;
                case PROV_ACT_SEARCH_REGEX:                                             /* sox */
                    s->search.regex = !s->search.regex;
                    search_recompile(s);
                    if (s->search.regex && s->search.valid && !s->search.re)
                        FMT_INTO(s->message, "regex on (invalid: {})", PROVEN_ARG(prov_cstr_view(s->search.term)));
                    else
                        FMT_INTO(s->message, "regex {}", PROVEN_ARG(prov_cstr_view(s->search.regex ? "on" : "off")));
                    if (s->search.valid) do_search(s, true, false);
                    break;
                case PROV_ACT_SEARCH_WORDB:                                             /* sow */
                    s->search.word = !s->search.word;
                    if (s->search.word) s->search.regex = true;        /* whole-word uses the engine */
                    search_recompile(s);
                    FMT_INTO(s->message, "whole-word {}", PROVEN_ARG(prov_cstr_view(s->search.word ? "on" : "off")));
                    if (s->search.valid) do_search(s, true, false);
                    break;
                case PROV_ACT_SEARCH_HL:                                                /* soh */
                    s->search.hl = !s->search.hl;
                    FMT_INTO(s->message, "highlight {}", PROVEN_ARG(prov_cstr_view(s->search.hl ? "on" : "off")));
                    break;
                case PROV_ACT_REPLACE:                                                  /* sr */
                    if (!s->search.valid || !s->search.term[0]) { FMT_INTO(s->message, "search first (ss)"); break; }
                    if (ro_guard(s)) break;
                    prompt_open(s, PROMPT_REPLACE, "replace");
                    break;
                case PROV_ACT_PANE_HSPLIT: pane_split(s, false); break;   /* wh */
                case PROV_ACT_PANE_VSPLIT: pane_split(s, true);  break;   /* wv */
                case PROV_ACT_WIN_PREV:    pane_focus_prev(s);   break;   /* wp */
                case PROV_ACT_WIN_NEXT:    pane_focus_next(s);   break;   /* wn */
                case PROV_ACT_WIN_UP:      pane_move_dir(s, PROV_DIR_UP);    break;  /* wi */
                case PROV_ACT_WIN_DOWN:    pane_move_dir(s, PROV_DIR_DOWN);  break;  /* wk */
                case PROV_ACT_WIN_LEFT:    pane_move_dir(s, PROV_DIR_LEFT);  break;  /* wj */
                case PROV_ACT_WIN_RIGHT:   pane_move_dir(s, PROV_DIR_RIGHT); break;  /* wl */
                case PROV_ACT_PANE_CLOSE:  pane_close(s);        break;   /* wq */
                case PROV_ACT_PANE_RESIZE: pane_enter_resize(s); break;   /* ws */
                case PROV_ACT_PANE_READONLY: pane_toggle_readonly(s); break; /* wr */
                case PROV_ACT_PANE_HEX: pane_toggle_hex(s); break;            /* wx */
                default: {        /* za/zh/zi/zd/zs and the 0-browsers: later milestones */
                    char lbl[40];
                    prov_cmd_label(&cmd, lbl, sizeof lbl);
                    FMT_INTO(s->message, "{}: not implemented yet", PROVEN_ARG(prov_cstr_view(lbl)));
                    break;
                }
            }
}

static void zx_exec_findchar(prov_session_t *s, prov_command_t cmd, proven_u32 cnt) {
    bool till = (cmd.target == PROV_TARGET_TILL);
    do_findc(s, cmd.param, till, false, cnt);
    s->last_find_char = cmd.param;
    s->last_find_till = till;
    s->last_find_valid = true;
}

static void zx_exec_bookmark_jump(prov_session_t *s, prov_command_t cmd) {  /* m<letter> */
    prov_editor_t *ed = s->ed;
    char lc[2] = { (char)cmd.param, 0 };
    proven_size_t pos;
    if (prov_buffer_get_mark(prov_editor_buffer(ed), (int)cmd.param - 'a', &pos)) {
        jump_push(s);
        prov_editor_set_extending(ed, s->zx_visual);
        prov_editor_move_to(ed, pos);
    } else {
        FMT_INTO(s->message, "mark {} not set", PROVEN_ARG(prov_cstr_view(lc)));
    }
}

static void zx_exec_register(prov_session_t *s, prov_command_t cmd, proven_u32 cnt) {  /* b<reg><op> */
    prov_editor_t *ed = s->ed;
    int idx = reg_index((int)cmd.param);
    if (idx < 0) return;
    char op = (char)cmd.param2;
    char lc[2] = { (char)cmd.param, 0 };
    if (op == 'c') {                           /* copy selection -> register */
        if (!prov_editor_has_selection(ed)) { FMT_INTO(s->message, "no selection"); return; }
        prov_editor_copy_selection(ed);
        reg_store(s, idx);
        clip_push(s);
        FMT_INTO(s->message, "yanked to register {}", PROVEN_ARG(prov_cstr_view(lc)));
    } else if (op == 'x') {                    /* cut selection -> register */
        if (ro_guard(s)) return;
        if (!prov_editor_has_selection(ed)) { FMT_INTO(s->message, "no selection"); return; }
        if (PROVEN_IS_OK(prov_editor_cut_selection(ed))) { reg_store(s, idx); s->modified = true; clip_push(s); }
    } else if (op == 'v') {                    /* paste register */
        if (ro_guard(s)) return;
        if (!reg_load(s, idx)) { FMT_INTO(s->message, "register empty"); return; }
        if (PROVEN_IS_OK(prov_editor_paste_lines(ed, true, cnt))) s->modified = true;
    }
}

static void zx_exec_macro(prov_session_t *s, prov_command_t cmd, proven_u32 cnt) {  /* e<letter> */
    int idx = reg_index((int)cmd.param);
    if (idx < 0 || s->macro.len[idx] == 0) { FMT_INTO(s->message, "macro empty"); return; }
    s->macro.last = idx;
    for (proven_u32 i = 0; i < cnt; i++) feed_push(s, s->macro.slot[idx], s->macro.len[idx]);
}

static void zx_exec_macro_last(prov_session_t *s, proven_u32 cnt) {  /* E: stop rec, else replay last */
    if (s->macro.rec) { macro_stop(s, 1); return; }   /* drop the trailing `E` itself */
    if (s->macro.last < 0 || s->macro.len[s->macro.last] == 0) { FMT_INTO(s->message, "no macro to replay"); return; }
    for (proven_u32 i = 0; i < cnt; i++) feed_push(s, s->macro.slot[s->macro.last], s->macro.len[s->macro.last]);
}

static void zx_execute(prov_session_t *s, prov_command_t cmd, proven_size_t page) {
    proven_u32 cnt = cmd.count ? cmd.count : 1;
    proven_size_t half = page / 2 ? page / 2 : 1;
    switch (cmd.kind) {
        case PROV_CMD_MOVE:          zx_exec_move(s, cmd, cnt);          break;
        case PROV_CMD_EDIT:          zx_exec_edit(s, cmd, cnt);          break;
        case PROV_CMD_OPERATION:     zx_exec_operation(s, cmd, cnt);     break;
        case PROV_CMD_ACTION:        zx_exec_action(s, cmd, cnt, half);  break;
        case PROV_CMD_FINDCHAR:      zx_exec_findchar(s, cmd, cnt);      break;
        case PROV_CMD_BOOKMARK_JUMP: zx_exec_bookmark_jump(s, cmd);      break;
        case PROV_CMD_REGISTER:      zx_exec_register(s, cmd, cnt);      break;
        case PROV_CMD_MACRO_EXEC:    zx_exec_macro(s, cmd, cnt);         break;
        case PROV_CMD_MACRO_LAST:    zx_exec_macro_last(s, cnt);         break;
        default: break;   /* INCOMPLETE / INVALID handled by caller */
    }
}

static bool is_movement(prov_key_kind_t k) {
    return k == PROV_KEY_LEFT || k == PROV_KEY_RIGHT || k == PROV_KEY_UP ||
           k == PROV_KEY_DOWN || k == PROV_KEY_HOME || k == PROV_KEY_END ||
           k == PROV_KEY_PAGEUP || k == PROV_KEY_PAGEDOWN;
}

/* True when the active buffer (live state) or any other buffer is modified. */
static bool any_dirty(const prov_session_t *s) {
    if (s->modified) return true;
    for (int i = 0; i < s->bufs.count; i++)
        if (i != s->bufs.active && s->bufs.entries[i].modified) return true;
    return false;
}

/* Save every modified buffer that has a path. Returns how many remain unsaved
 * because they have no file name. */
static int save_all_with_path(prov_session_t *s) {
    int unsaved = 0;
    if (s->modified) {
        if (s->path && PROVEN_IS_OK(prov_save_buffer(s->a, prov_editor_buffer(s->ed), s->path, &s->bufs.entries[active_buf(s)].info)))
            s->modified = false;
        else if (!s->path) unsaved++;
    }
    for (int i = 0; i < s->bufs.count; i++) {
        if (i == s->bufs.active) continue;
        prov_buf_t *e = &s->bufs.entries[i];
        if (!e->modified) continue;
        if (e->path[0] && PROVEN_IS_OK(prov_save_buffer(s->a, prov_editor_buffer(e->ed), e->path, &e->info)))
            e->modified = false;
        else unsaved++;
    }
    return unsaved;
}

/* Quit request (SPEC §13.1). No dirty buffers -> quit immediately; otherwise
 * open the quit wizard (w write-all / d discard&quit / c cancel) on the command
 * line. `zqq` and a repeated `^Q` reach the wizard's discard path. */
static void arm_or_quit(prov_session_t *s) {
    if (!any_dirty(s)) { s->running = false; return; }
    s->quit_wizard = true;
}

/* Quit-wizard key (active when s->quit_wizard). Returns true if it consumed k. */
static bool quit_wizard_key(prov_session_t *s, prov_key_t k) {
    if (!s->quit_wizard) return false;
    if (k.kind == PROV_KEY_CHAR) {
        if (k.cp == 'w') {
            int u = save_all_with_path(s);
            if (u == 0) { s->running = false; s->quit_wizard = false; }
            else FMT_INTO(s->message,
                          "{} unnamed buffer(s) remain — d to discard, c to cancel", PROVEN_ARG(u));
            return true;                       /* wizard stays armed when u > 0 */
        }
        if (k.cp == 'd' || k.cp == 'q') { s->running = false; return true; }   /* discard all & quit */
        if (k.cp == 'c') { s->quit_wizard = false; return true; }
    }
    if (k.kind == PROV_KEY_CTRL && k.cp == 'q') { s->running = false; return true; }
    s->quit_wizard = false;                    /* any other key cancels */
    return true;
}

/* ---- command-line prompt (zo open file, zp command) ---- */

/* Drop length bytes off an owning string (truncate to empty when len == size). */
static void pstr_clear(proven_u8str_t *s) {
    proven_size_t n = proven_u8str_as_view(s).size;
    if (n) (void)proven_u8str_remove(s, 0, n);
}

static void prompt_open(prov_session_t *s, int kind, const char *label) {
    s->prompt_kind = kind;
    prov_le_clear(&s->prompt_le);
    s->prompt_hist.pos = -1;                       /* not navigating history */
    prov_cstr_set(s->prompt_label, sizeof s->prompt_label, prov_cstr_view(label));
}

/* Open `path` (already trimmed) into a new buffer, or switch if already open. */
static void open_path(prov_session_t *s, const char *path) {
    if (!*path) return;
    int existing = prov_bufset_find(&s->bufs, path);
    if (existing >= 0) { buf_switch(s, existing); return; }
    bool sanitized = false;                      /* lossy load: drop invalid bytes, warn */
    prov_fileinfo_t info;                         /* original encoding / EOL / BOM (for save) */
    prov_result_editor_t er = s->open_binary       /* RFC-0019: raw load, no conversion */
        ? prov_editor_open_binary(s->a, path, &info, s->open_enc[0] ? s->open_enc : NULL)
        : prov_editor_open(s->a, path, &sanitized, &info,
                           s->open_enc[0] ? s->open_enc : NULL, s->cfg.fallback_encoding);
    if (!PROVEN_IS_OK(er.err)) { FMT_INTO(s->message, "cannot open '{}'", PROVEN_ARG(prov_cstr_view(path))); return; }
    buf_save_active(s);
    int i = prov_bufset_add(&s->bufs, er.value, path);
    if (i < 0) { prov_editor_destroy(er.value); FMT_INTO(s->message, "too many buffers"); return; }
    s->bufs.entries[i].info = info;
    if (s->open_bom && !info.binary) s->bufs.entries[i].info.bom = true;   /* open-panel: force a BOM on save */
    if (s->open_eol > 0 && !info.binary)                                   /* open-panel: force the EOL (request 1) */
        s->bufs.entries[i].info.eol = (prov_eol_t)(s->open_eol - 1);
    focused_win(s)->buf = i;                     /* show it in the focused window */
    if (s->open_ro) focused_win(s)->readonly = true;   /* open-panel: read-only (buf_load mirrors to s->readonly) */
    buf_load_active(s);
    buf_reset_idle(s);
    if (sanitized)
        FMT_INTO(s->message, "warning: dropped invalid (non-UTF-8) bytes while reading this file");
}

/* Resolve the config file path (SPEC §17.1). Portable mode: if `provconf.ini`
 * sits next to the executable, that file wins and the home copy is ignored.
 * Otherwise the path is ~/.prov/provconf.ini, and `home_dir` (when non-NULL) is
 * filled with ~/.prov so the caller can create it. Returns false only when no
 * location is available (no executable dir and no HOME). */
static bool config_path(proven_allocator_t a, char *out, proven_size_t cap,
                        char *home_dir, proven_size_t hcap) {
    if (home_dir && hcap) home_dir[0] = '\0';
    char exedir[1024];
    if (prov_platform_exe_dir(exedir, sizeof exedir)) {
        char p[1100];
        FMT_INTO(p, "{}/provconf.ini", PROVEN_ARG(prov_cstr_view(exedir)));
        proven_fs_stat_t st;
        if (PROVEN_IS_OK(proven_fs_stat(a, prov_cstr_view(p), &st))) {   /* portable: exe-side wins */
            prov_cstr_set(out, cap, prov_cstr_view(p));
            return true;
        }
    }
    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    char hp[1100];
    FMT_INTO(hp, "{}/.prov/provconf.ini", PROVEN_ARG(prov_cstr_view(home)));
    prov_cstr_set(out, cap, prov_cstr_view(hp));
    if (home_dir && hcap) {              /* home_dir is a pointer: build then copy with its cap */
        char hd[1100];
        FMT_INTO(hd, "{}/.prov", PROVEN_ARG(prov_cstr_view(home)));
        prov_cstr_set(home_dir, hcap, prov_cstr_view(hd));
    }
    return true;
}

/* The session-state file (find/replace + type-filter history) lives next to the
 * resolved config file, named `prov_state.ini`. Reuses config_path's portable/
 * home resolution so it follows the config: exe-side in portable mode, else
 * ~/.prov. `home_dir` (when non-NULL) is filled with the dir to create. */
static bool state_path(proven_allocator_t a, char *out, proven_size_t cap,
                       char *home_dir, proven_size_t hcap) {
    char cfg[1100];
    if (!config_path(a, cfg, sizeof cfg, home_dir, hcap)) return false;
    proven_size_t slash = 0; bool has = false;          /* truncate to the config's directory */
    for (proven_size_t i = 0; cfg[i]; i++)
        if (cfg[i] == '/' || cfg[i] == '\\') { slash = i; has = true; }
    if (has) cfg[slash + 1] = '\0'; else cfg[0] = '\0';
    char p[1100];
    FMT_INTO(p, "{}prov_state.ini", PROVEN_ARG(prov_cstr_view(cfg)));
    prov_cstr_set(out, cap, prov_cstr_view(p));
    return true;
}

/* Serialize one history ring under a `[section]` header, oldest entry first.
 * Each entry is written verbatim after an `e=` prefix (so a value may contain
 * any byte, including `[`); an entry holding a newline is skipped to keep the
 * line-based format well-formed (line editors are single-line, so this only
 * guards a pasted-multiline edge case). */
static void state_write_hist(FILE *f, const char *section, const prov_lehist_t *h) {
    fprintf(f, "[%s]\n", section);
    int n = prov_lehist_len(h);
    for (int i = 0; i < n; i++) {
        const char *e = prov_lehist_get(h, i);
        bool nl = false;
        for (const char *p = e; *p; p++) if (*p == '\n' || *p == '\r') { nl = true; break; }
        if (!nl) fprintf(f, "e=%s\n", e);
    }
}

/* Persist the session history rings to prov_state.ini (best-effort; a failure is
 * silent — history is convenience state, never essential). Called at clean exit. */
static void save_state(prov_session_t *s) {
    char path[1100], dir[1100];
    if (!state_path(s->a, path, sizeof path, dir, sizeof dir)) return;
    if (dir[0]) (void)proven_fs_mkdir(s->a, prov_cstr_view(dir));   /* ensure ~/.prov exists */
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs("# prov editor session state (auto-generated; safe to delete)\n", f);
    state_write_hist(f, "find", &s->find.pathist);
    state_write_hist(f, "replace", &s->find.replhist);
    state_write_hist(f, "browse_types", &s->browse.pfhist);
    fclose(f);
}

/* Load prov_state.ini into the history rings at startup (missing file = empty
 * rings, never fatal). Entries replay through prov_lehist_push so the ring's
 * newest-first order is reproduced. */
static void load_state(prov_session_t *s) {
    char path[1100];
    if (!state_path(s->a, path, sizeof path, NULL, 0)) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    static char buf[131072];     /* 3 rings × 32 × ≤1 KB worst case; typical is tiny */
    size_t got = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[got] = '\0';
    prov_lehist_t *cur = NULL;
    for (size_t i = 0; i < got; ) {
        size_t j = i;
        while (j < got && buf[j] != '\n') j++;
        size_t end = j;
        if (end > i && buf[end - 1] == '\r') end--;   /* tolerate CRLF */
        buf[end] = '\0';
        char *line = &buf[i];
        if (line[0] == '[') {
            if      (proven_u8str_view_eq(prov_cstr_view(line), PROVEN_LIT("[find]")))         cur = &s->find.pathist;
            else if (proven_u8str_view_eq(prov_cstr_view(line), PROVEN_LIT("[replace]")))      cur = &s->find.replhist;
            else if (proven_u8str_view_eq(prov_cstr_view(line), PROVEN_LIT("[browse_types]"))) cur = &s->browse.pfhist;
            else cur = NULL;   /* unknown section: ignore until the next header */
        } else if (cur && line[0] == 'e' && line[1] == '=') {
            prov_lehist_push(cur, line + 2);
        }
        i = j + 1;
    }
}

/* zc: open the config file for editing. When it is missing (and not the
 * portable exe-side copy) create ~/.prov and write a commented starter file (the
 * built-in defaults). The editor reads this same file at startup. */
static void open_config(prov_session_t *s) {
    char path[1100], dir[1100];
    if (!config_path(s->a, path, sizeof path, dir, sizeof dir)) {
        FMT_INTO(s->message, "no HOME and no executable dir: cannot locate the config file");
        return;
    }
    proven_fs_stat_t st;
    if (!PROVEN_IS_OK(proven_fs_stat(s->a, prov_cstr_view(path), &st))) {   /* missing -> write starter */
        if (dir[0]) (void)proven_fs_mkdir(s->a, prov_cstr_view(dir));        /* only for the home copy */
        proven_result_file_t of = proven_fs_open(s->a, prov_cstr_view(path),
            PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
        if (!PROVEN_IS_OK(of.err)) {
            FMT_INTO(s->message, "cannot create {}", PROVEN_ARG(prov_cstr_view(path)));
            return;
        }
        const char *t = prov_config_default_text();
        proven_size_t tn = proven_cstr_len(t);
        (void)proven_fs_write_all(of.value, (proven_mem_view_t){ (const proven_byte_t *)t, tn });
        proven_fs_close(of.value);
        FMT_INTO(s->message, "created {} — edit and restart to apply", PROVEN_ARG(prov_cstr_view(path)));
    }
    open_path(s, path);   /* open (or switch to) the config buffer */
}

/* True when `path` is the config file the editor reads at startup (RFC: zc live
 * apply + the "config" status tag). */
static bool is_config_path(prov_session_t *s, const char *path) {
    if (!path || !path[0]) return false;
    char cpath[1100];
    if (!config_path(s->a, cpath, sizeof cpath, NULL, 0)) return false;
    return proven_u8str_view_eq(prov_cstr_view(path), prov_cstr_view(cpath));
}

/* Live-apply a freshly parsed config: replace `s->cfg` and run the few settings
 * that need a side effect on change (every other field is read straight from
 * `s->cfg` at its point of use, so it takes effect on the next frame). No setting
 * currently requires a restart; if one is added, gate it here and tell the user. */
static void config_apply(prov_session_t *s, const prov_config_t *nc) {
    bool mouse_changed = (s->cfg.mouse != nc->mouse);
    s->cfg = *nc;
    if (mouse_changed) prov_term_enable_mouse(nc->mouse);          /* terminal mouse reporting */
    prov_charset_configure(nc->charset_backend);                   /* re-resolve the charset backend */
    prov_charset_set_iconv_path(nc->charset_iconv_path);
    for (int i = 0; i < s->bufs.count; i++)                        /* undo depth, every open buffer */
        prov_editor_set_undo_limit(s->bufs.entries[i].ed, nc->undo_limit);
}

/* After a successful save of `ed` to `path`: if that file is the config, re-parse
 * the just-saved buffer text and live-apply it. A parse error leaves the running
 * config untouched and reports the offending line. */
static void maybe_reapply_config(prov_session_t *s, prov_editor_t *ed, const char *path) {
    if (!is_config_path(s, path)) return;
    const prov_buffer_t *b = prov_editor_buffer(ed);
    proven_size_t n = prov_buffer_byte_len(b);
    proven_result_mem_mut_t m = s->a.alloc_fn(s->a.ctx, n + 1, 16);
    if (!PROVEN_IS_OK(m.err)) return;
    char *buf = (char *)m.value.ptr;
    if (n) prov_buffer_copy_range(b, 0, n, (proven_u8 *)buf, n);
    buf[n] = '\0';
    prov_config_t nc = prov_config_default();
    prov_config_result_t r = prov_config_parse(&nc, s->a, buf, n);
    s->a.free_fn(s->a.ctx, buf);
    if (!r.ok) {
        FMT_INTO(s->message, "config saved \xe2\x80\x94 NOT applied (line {}: {})",
                 PROVEN_ARG((unsigned long)r.line), PROVEN_ARG(prov_cstr_view(r.message ? r.message : "syntax error")));
        return;
    }
    if (proven_cstr_len(nc.trigger) != 2) { nc.trigger[0] = 'z'; nc.trigger[1] = 'x'; nc.trigger[2] = '\0'; }
    config_apply(s, &nc);
    FMT_INTO(s->message, "config applied (live)");
}

/* ---- file-open browser (goal 4) ------------------------------------------ */

/* Derive the directory to start browsing in from `path` (a file path or NULL):
 * its parent directory, or "." when it has no directory part. */
static void browse_dir_of(const char *path, char *out, proven_size_t cap) {
    proven_size_t n = 0;
    if (path) for (; path[n] && n + 1 < cap; n++) out[n] = path[n];
    out[n] = '\0';
    while (n > 0 && out[n - 1] != '/') n--;          /* strip the file name */
    if (n == 0) { out[0] = '.'; out[1] = '\0'; return; }
    if (n > 1) n--;                                  /* drop the separator (keep root "/") */
    out[n] = '\0';
}

/* Resolve a relative start directory to an absolute path (via the cwd) so the
 * parent (".." ) navigation can ascend past it. Leaves absolute paths as-is;
 * on any failure the original relative path is kept. */
static void browse_resolve(char *dir, proven_size_t cap) {
    if (proven_fs_is_absolute(proven_u8str_view_from_cstr(dir))) return;
    char cwd[1024];
    if (!prov_getcwd(cwd, sizeof cwd)) return;
    proven_size_t cn = proven_cstr_len(cwd);
    bool dot = dir[0] == '.' && dir[1] == '\0';
    proven_u8str_t s = proven_u8str_borrow((proven_byte_t *)dir, cap);   /* starts empty */
    if (dot) (void)proven_u8str_append_fmt_trunc(&s, "{}", PROVEN_ARG(prov_cstr_view(cwd)));
    else (void)proven_u8str_append_fmt_trunc(&s, "{}{}{}", PROVEN_ARG(prov_cstr_view(cwd)),
        PROVEN_ARG(prov_cstr_view(cn && cwd[cn - 1] == '/' ? "" : "/")), PROVEN_ARG(prov_cstr_view(dir)));
    (void)proven_u8str_as_cstr(&s);
}

/* case-insensitive substring test (ASCII fold), libc-free. */
static bool browse_contains_ci(const char *hay, const char *needle) {
    if (!needle[0]) return true;
    for (proven_size_t i = 0; hay[i]; i++) {
        proven_size_t j = 0;
        while (needle[j]) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            j++;
        }
        if (!needle[j]) return true;
    }
    return false;
}


/* ---- common modal panel (RFC-0010) --------------------------------------- */
/* PANEL_K_* (the panel-kind enum) lives in panel.h. */

/* 0z command cheat-sheet: static rows (key + description); `id` = the help topic
 * key that activating jumps to. Filterable with `ss`. */
static const prov_panel_row_t CMD_ROWS[] = {
    { .text = "ikjl / arrows     move the cursor",                       .id = 'i' },
    { .text = "[N]<motion>       repeat a motion N times",               .id = '#' },
    { .text = "g / [N]g / 0g     goto first / line N / last line",       .id = 'g' },
    { .text = "f<ch> ; ,         find char, repeat / reverse",           .id = 'f' },
    { .text = "w b e / iw i( i\" text objects & word motions",          .id = 'i' },
    { .text = "a                 append: enter Ed text entry",          .id = 'a' },
    { .text = "c / d / y <obj>   change / delete / yank an object",      .id = 'c' },
    { .text = "x  r  p  P        delete char, replace, paste below/above", .id = 'x' },
    { .text = "v / V             visual selection / visual block",        .id = 'v' },
    { .text = "u  z y            undo / redo",                           .id = 'u' },
    { .text = "n  [N]n           repeat the last command",               .id = 'n' },
    { .text = "/                find/replace panel (pattern+opts+replace)", .id = 's' },
    { .text = "ss               search (incremental); sn sp next/prev",  .id = 's' },
    { .text = "sw  sr           search word / replace",                  .id = 's' },
    { .text = "sox sow soc soh  regex / word / case / highlight",        .id = 's' },
    { .text = "b<reg>c/x/v       yank / cut / paste a named register",   .id = 'b' },
    { .text = "m<a-z>            jump to a bookmark",                     .id = 'b' },
    { .text = "e<slot>  E        run a macro / replay last (E stops rec)", .id = 'e' },
    { .text = "wh wv wq          split horizontally / vertically / close", .id = 'w' },
    { .text = "wi/wk/wj/wl       focus window by direction (0w = panel)", .id = 'w' },
    { .text = "ws wr wp wn       resize / read-only / prev / next window", .id = 'w' },
    { .text = "tn tq tj tl [N]t  new / close / prev / next / Nth tab",   .id = 't' },
    { .text = "zo zw za zb       open / write / write-as / buffer list", .id = 'z' },
    { .text = "zq zqq            quit (wizard if dirty) / force quit",   .id = 'z' },
    { .text = "zx  Esc           leave Ed to zx / cancel a command",     .id = 'z' },
    { .text = "0w 0t windows / tabs panel",                              .id = '0' },
    { .text = "0b 0e 0m  registers / macros / bookmarks panel",          .id = '0' },
    { .text = "0s 0z  search-history / this command panel",              .id = '0' },
    { .text = "h                 keyboard help overlay",                 .id = 0   },
};
#define CMD_ROWS_N ((int)(sizeof CMD_ROWS / sizeof CMD_ROWS[0]))

/* Panel verb actions (the keymap `action` ids). Each fires a "press verb, then a
 * slot key (a-z / 0-9)" two-step (RFC-0010 §8 P1). */
enum { PV_RECORD = 1, PV_SET, PV_DELETE };

static int reg_index(int ch);                         /* a-z -> 0..25, 0-9 -> 26..35 */
static proven_size_t buf_line_of(const prov_buffer_t *buf, proven_size_t pos);

/* Prompt shown while awaiting the slot key for a pending verb. */
static const char *panel_verb_prompt(int verb) {
    switch (verb) {
        case PV_RECORD: return "record into slot:";
        case PV_SET:    return "set bookmark slot:";
        case PV_DELETE: return "delete slot:";
        default:        return "slot:";
    }
}

static void panel_open_regs(prov_session_t *s);       /* (forward: rebuild after delete) */

/* Apply a verb to a chosen slot (0..35). Per-kind; macro/bookmark verbs land in
 * P2. Empty/invalid slots are a quiet no-op. */
static void panel_verb_apply(prov_session_t *s, int verb, int slot) {
    if (slot < 0 || slot >= 36) return;
    switch (s->panel_kind) {
        case PANEL_K_REGS:
            if (verb == PV_DELETE) {
                if (s->regs[slot].bytes) { s->a.free_fn(s->a.ctx, s->regs[slot].bytes); s->regs[slot].bytes = NULL; }
                s->regs[slot].len = 0;
                panel_open_regs(s);
            }
            break;
        case PANEL_K_MACROS:
            if (verb == PV_RECORD) { macro_start(s, slot); panel_close(s); }   /* keys now go to the editor */
            else if (verb == PV_DELETE) { s->macro.len[slot] = 0; panel_open_macros(s); }
            break;
        case PANEL_K_BOOKMARKS:
            if (slot >= 26) { FMT_INTO(s->message, "bookmarks are a-z"); break; }  /* no digit slots */
            if (verb == PV_SET) { prov_editor_set_mark(s->ed, slot); panel_open_bookmarks(s); }
            else if (verb == PV_DELETE) { prov_editor_clear_mark(s->ed, slot); panel_open_bookmarks(s); }
            break;
        default: break;
    }
}

static const prov_keymap_entry_t REG_VERBS[]  = { { 'x', PV_DELETE, "delete" } };
static const prov_keymap_t       REG_KEYMAP   = { REG_VERBS, 1 };
static const prov_keymap_entry_t MAC_VERBS[]  = { { 'r', PV_RECORD, "record" }, { 'x', PV_DELETE, "delete" } };
static const prov_keymap_t       MAC_KEYMAP   = { MAC_VERBS, 2 };
static const prov_keymap_entry_t BMK_VERBS[]  = { { 'n', PV_SET, "set" }, { 'x', PV_DELETE, "delete" } };
static const prov_keymap_t       BMK_KEYMAP   = { BMK_VERBS, 2 };
/* 0t tab panel: immediate verbs (uppercase so lowercase i/k/j/l stay navigation). */
enum { TV_UP = 20, TV_DOWN, TV_TOP, TV_BOTTOM, TV_CLOSE, TV_FOLD, TV_NEW };
static const prov_keymap_entry_t TAB_VERBS[]  = {
    { 'I', TV_UP, "move up" }, { 'K', TV_DOWN, "move down" },
    { 'J', TV_TOP, "to top" }, { 'L', TV_BOTTOM, "to bottom" },
    { 'x', TV_CLOSE, "close" }, { 'f', TV_FOLD, "fold" }, { 'n', TV_NEW, "new tab" },
};
static const prov_keymap_t       TAB_KEYMAP   = { TAB_VERBS, 7 };
/* 0w windows panel: S marks a window then swaps it with the next pick.
 * (Uppercase: lowercase s starts the `ss` filter in the keymap engine.) */
enum { WV_SWAP = 40 };
static const prov_keymap_entry_t WIN_VERBS[]  = { { 'S', WV_SWAP, "swap" } };
static const prov_keymap_t       WIN_KEYMAP   = { WIN_VERBS, 1 };

static void panel_close(prov_session_t *s) {
    if (s->panel_open) prov_panel_free(&s->panel);
    s->panel_open = false; s->panel_filter = false; s->panel_help = false; s->panel_verb = 0;
    s->help_await = false;
    s->panel_pick = NULL;                          /* a closed panel is no longer a picker */
    if (s->browse.pv_buf) { s->a.free_fn(s->a.ctx, s->browse.pv_buf); s->browse.pv_buf = NULL; }
    s->browse.pv_len = 0; s->browse.pv_top = 0; s->browse.focus = BF_LIST;
    s->browse.pv_path[0] = '\0'; s->browse.pv_enc[0] = '\0';   /* force re-decode next open */
    s->browse.subscreen = BSUB_NONE; prov_le_clear(&s->browse.pathedit);
}

/* ---- save-as dialog (za): a full panel that owns input ---- */
enum { SA_PATH = 0, SA_CONFIRM = 1 };

/* za / save of an unnamed buffer: open the full-panel save dialog, seeding the
 * path with the buffer's current name (empty for a new buffer). */
static void panel_open_saveas(prov_session_t *s, int bufidx) {
    panel_close(s);
    prov_panel_init(&s->panel, s->a, "Save As", NULL, 0, NULL);
    s->panel.pos = PANEL_FULL;
    s->panel_kind = PANEL_K_SAVEAS;
    s->panel_open = true; s->panel_filter = false; s->panel_scroll = 0;
    s->saveas_buf = bufidx;
    s->saveas_state = SA_PATH;
    s->saveas_msg[0] = '\0';
    prov_le_set(&s->prompt_le, s->bufs.entries[bufidx].path);   /* seed with the current name */
}

/* Write the dialog's buffer to the typed path. On failure, stay in the dialog
 * with an error notice so the user can fix the path and retry; on success, name
 * the buffer, refresh its file info, clear modified, and close. */
static void saveas_do(prov_session_t *s) {
    int b = s->saveas_buf;
    if (b < 0 || b >= s->bufs.count) { panel_close(s); return; }
    char path[1024];
    prov_cstr_set(path, sizeof path, prov_cstr_view(s->prompt_le.buf));
    proven_err_t err = prov_save_buffer(s->a, prov_editor_buffer(s->bufs.entries[b].ed), path, &s->bufs.entries[b].info);
    if (!PROVEN_IS_OK(err)) {
        s->saveas_state = SA_PATH;
        FMT_INTO(s->saveas_msg, "could not write \"{}\" — check the directory and permissions, then retry",
                 PROVEN_ARG(prov_cstr_view(path)));
        return;
    }
    prov_cstr_set(s->bufs.entries[b].path, sizeof s->bufs.entries[b].path, prov_cstr_view(path));   /* now named */
    /* keep the buffer's encoding/EOL/BOM (the save reproduced them); the
     * normalized buffer would always re-detect as UTF-8/LF, so don't re-detect. */
    if (b == active_buf(s)) {
        prov_editor_compact(s->ed); s->modified = false;
        s->path = s->bufs.entries[b].path;     /* refresh the mirror: the buffer is no longer unnamed */
    } else s->bufs.entries[b].modified = false;
    FMT_INTO(s->message, "saved {}", PROVEN_ARG(prov_cstr_view(path)));
    panel_close(s);
    maybe_reapply_config(s, s->bufs.entries[b].ed, path);   /* zc live-apply if saved as the config (RFC) */
}

/* Build the 0w rows into panel_rowbuf/panel_txt (id = leaf node index). The
 * focused window is marked `>`; a window marked for swap is marked `*`. */
static int build_windows_rows(prov_session_t *s) {
    prov_layout_t *L = cur_layout(s);
    int leaves[PROV_MAX_PANE_NODES];
    int n = prov_layout_leaves(L, leaves, PROV_MAX_PANE_NODES);
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) {
        prov_pane_node_t *nd = &L->nodes[leaves[i]];
        prov_buf_t *e = &s->bufs.entries[nd->buf];
        const char *nm = e->path[0] ? prov_basename(e->path) : "[No Name]";
        const char *mk = leaves[i] == s->win_swap ? "*" : (leaves[i] == L->focus ? ">" : " ");
        FMT_INTO(s->panel_txt[i], "{}{} {}  Ln{} Co{}{}",
                 PROVEN_ARG(i + 1), PROVEN_ARG(prov_cstr_view(mk)),
                 PROVEN_ARG(prov_cstr_view(nm)),
                 PROVEN_ARG((unsigned long)(prov_editor_cursor_line(e->ed) + 1)),
                 PROVEN_ARG((unsigned long)(prov_editor_cursor_col(e->ed) + 1)),
                 PROVEN_ARG(prov_cstr_view(nd->readonly ? "  RO" : "")));
        s->panel_rowbuf[i] = (prov_panel_row_t){ .text = s->panel_txt[i], .id = leaves[i] };
    }
    return n;
}

/* Swap the contents (buffer, viewport, read-only) of two leaf windows in the
 * current tab; the layout tree (geometry) is unchanged. */
static void win_swap_nodes(prov_session_t *s, int a, int b) {
    if (a == b) return;
    prov_layout_t *L = cur_layout(s);
    buf_save_active(s);                        /* flush the focused window's live state first */
    prov_pane_node_t *na = &L->nodes[a], *nb = &L->nodes[b];
    int tb = na->buf;            na->buf = nb->buf;           nb->buf = tb;
    proven_size_t tt = na->top;  na->top = nb->top;           nb->top = tt;
    bool tr = na->readonly;      na->readonly = nb->readonly; nb->readonly = tr;
    buf_load_active(s);                        /* the focused window may now show a different buffer */
    buf_reset_idle(s);
}

/* 0w: open the windows overview as a panel. */
static void panel_open_windows(prov_session_t *s) {
    s->win_swap = -1;
    panel_close(s);
    int n = build_windows_rows(s);
    prov_panel_init(&s->panel, s->a, "Windows", s->panel_rowbuf, (proven_size_t)n, &WIN_KEYMAP);
    s->panel.pos = PANEL_FULL;
    s->panel_kind = PANEL_K_WINDOWS;
    s->panel_open = true; s->panel_filter = false; s->panel_scroll = 0;
}

/* 0t: open the tabs overview as a panel — same layout as the windows panel
 * (index, focus marker, focused buffer, its Ln/Co, then the window count). */
/* Build the 0t panel rows into panel_rowbuf/panel_txt: one row per tab, plus
 * indented window rows under each tab whose fold bit is set. Returns the count
 * (capped at 64). */
static int build_tabs_rows(prov_session_t *s) {
    int n = 0;
    for (int t = 0; t < s->tab_count && n < 64; t++) {
        prov_layout_t *Lt = &s->tabs[t];
        prov_pane_node_t *nd = &Lt->nodes[Lt->focus];
        prov_buf_t *e = &s->bufs.entries[nd->buf];
        const char *nm = e->path[0] ? prov_basename(e->path) : "[No Name]";
        int lc = (int)prov_layout_leaf_count(Lt);
        char winc[24];
        const char *fold = lc > 1 ? ((s->tab_fold & (1u << t)) ? "\xe2\x96\xbe" : "\xe2\x96\xb8") : " ";
        if (lc > 1) FMT_INTO(winc, "  {}  {} win", PROVEN_ARG(prov_cstr_view(fold)), PROVEN_ARG(lc));
        else        FMT_INTO(winc, "  {}", PROVEN_ARG(prov_cstr_view(fold)));
        FMT_INTO(s->panel_txt[n], "{}{} {}  Ln{} Co{}{}",
                 PROVEN_ARG(t + 1), PROVEN_ARG(prov_cstr_view(t == s->tab ? ">" : " ")),
                 PROVEN_ARG(prov_cstr_view(nm)),
                 PROVEN_ARG((unsigned long)(prov_editor_cursor_line(e->ed) + 1)),
                 PROVEN_ARG((unsigned long)(prov_editor_cursor_col(e->ed) + 1)),
                 PROVEN_ARG(prov_cstr_view(winc)));
        s->panel_rowbuf[n] = (prov_panel_row_t){ .text = s->panel_txt[n], .id = t };
        n++;

        if ((s->tab_fold & (1u << t)) && lc > 1) {       /* expand: indented window rows */
            int leaves[PROV_MAX_PANE_NODES];
            int m = prov_layout_leaves(Lt, leaves, PROV_MAX_PANE_NODES);
            for (int j = 0; j < m && n < 64; j++) {
                prov_pane_node_t *wn = &Lt->nodes[leaves[j]];
                prov_buf_t *we = &s->bufs.entries[wn->buf];
                const char *wnm = we->path[0] ? prov_basename(we->path) : "[No Name]";
                FMT_INTO(s->panel_txt[n], "    {} {}  Ln{} Co{}",
                         PROVEN_ARG(prov_cstr_view(leaves[j] == Lt->focus ? "\xe2\x80\xa2" : "-")),
                         PROVEN_ARG(prov_cstr_view(wnm)),
                         PROVEN_ARG((unsigned long)(prov_editor_cursor_line(we->ed) + 1)),
                         PROVEN_ARG((unsigned long)(prov_editor_cursor_col(we->ed) + 1)));
                s->panel_rowbuf[n] = (prov_panel_row_t){ .text = s->panel_txt[n],
                                                         .id = TAB_CHILD + t * 64 + leaves[j] };
                n++;
            }
        }
    }
    return n;
}

/* Re-render the 0t rows in place (after reorder / close / fold). */
static void rebuild_tabs_rows(prov_session_t *s) {
    int n = build_tabs_rows(s);
    prov_panel_set_rows(&s->panel, s->panel_rowbuf, (proven_size_t)n);
}

/* Move the panel selection onto the row that shows tab `t`. */
static void tabs_select_tab(prov_session_t *s, int t) {
    for (proven_size_t i = 0; i < s->panel.nview; i++)
        if (s->panel_rowbuf[s->panel.view[i]].id == t) { s->panel.sel = i; return; }
}

static void panel_open_tabs(prov_session_t *s) {
    s->tab_fold = 0;
    panel_close(s);
    int n = build_tabs_rows(s);
    prov_panel_init(&s->panel, s->a, "Tabs", s->panel_rowbuf, (proven_size_t)n, &TAB_KEYMAP);
    s->panel.pos = PANEL_FULL;
    s->panel_kind = PANEL_K_TABS;
    s->panel_open = true; s->panel_filter = false; s->panel_scroll = 0;
    tabs_select_tab(s, s->tab);                          /* start on the active tab */
}

/* 0b: open the named registers (a-z 0-9) that hold content, with a one-line preview. */
static void panel_open_regs(prov_session_t *s) {
    int n = 0;
    for (int idx = 0; idx < 36 && n < 64; idx++) {
        if (s->regs[idx].len == 0) continue;
        char nm[2] = { idx < 26 ? (char)('a' + idx) : (char)('0' + idx - 26), '\0' };
        char prev[56]; proven_size_t pl = 0; bool more = false;
        for (proven_size_t i = 0; i < s->regs[idx].len; i++) {
            proven_u8 c = s->regs[idx].bytes[i];
            if (c == '\n') { more = (i + 1 < s->regs[idx].len); break; }
            if (pl + 1 >= sizeof prev) { more = true; break; }
            prev[pl++] = (c < 0x20 || c == 0x7f) ? '.' : (char)c;   /* control -> '.' */
        }
        prev[pl] = '\0';
        FMT_INTO(s->panel_txt[n], "{}  {}B  {}{}", PROVEN_ARG(prov_cstr_view(nm)),
                 PROVEN_ARG((proven_u32)s->regs[idx].len), PROVEN_ARG(prov_cstr_view(prev)),
                 PROVEN_ARG(prov_cstr_view(more ? "\xe2\x80\xa6" : "")));
        s->panel_rowbuf[n] = (prov_panel_row_t){ .text = s->panel_txt[n], .id = idx };
        n++;
    }
    panel_close(s);
    prov_panel_init(&s->panel, s->a, "Registers", s->panel_rowbuf, (proven_size_t)n, &REG_KEYMAP);
    s->panel.pos = PANEL_FULL;
    s->panel_kind = PANEL_K_REGS;
    s->panel_open = true; s->panel_filter = false; s->panel_scroll = 0;
}

/* ---- find/replace panel (RFC-0016) ---------------------------------------- */

#define FIND_COUNT_CAP 100000   /* stop counting matches past this (UI-stall guard) */

/* Count matches in the cached hay and set find.index to the 1-based position of the
 * match starting at the current cursor (0 if the cursor isn't on a match start). */
static void find_count(prov_session_t *s) {
    s->find.matches = 0; s->find.index = 0;
    if (!s->search.valid || !s->search.term[0]) return;
    const proven_u8 *hay = s->search.hay; proven_size_t len = s->search.haylen;
    proven_size_t cur = prov_editor_cursor_byte(s->ed);
    if (s->search.regex) {
        if (!s->search.re) return;
        prov_regex_match_t m; proven_size_t pos = 0;
        while (pos <= len && s->find.matches < FIND_COUNT_CAP &&
               prov_regex_search(s->search.re, hay, len, pos, &m)) {
            s->find.matches++;
            if (m.start == cur) s->find.index = s->find.matches;
            pos = m.end > m.start ? m.end : m.start + 1;
        }
    } else {
        proven_size_t nlen = proven_cstr_len(s->search.term);
        if (!nlen || nlen > len) return;
        proven_size_t from = 0; bool found;
        for (;;) {
            proven_size_t p = prov_search_bytes(hay, len, (const proven_u8 *)s->search.term,
                                                nlen, from, true, false, s->search.icase, &found);
            if (!found || s->find.matches >= FIND_COUNT_CAP) break;
            s->find.matches++;
            if (p == cur) s->find.index = s->find.matches;
            from = p + 1;
            if (from > len) break;
        }
    }
}

/* Re-run the search live from the saved origin (incremental / option-toggle), then
 * recompute the match count. Leaves the cursor on the first match at/after origin. */
static void find_rerun(prov_session_t *s) {
    prov_cstr_set(s->search.term, sizeof s->search.term, prov_cstr_view(s->find.pat.buf));
    s->search.valid = s->search.term[0] != '\0';
    search_recompile(s);
    prov_editor_move_to(s->ed, s->search.origin);
    if (s->search.valid && s->search.hay) search_run(s, s->search.hay, s->search.haylen, true, false);
    find_count(s);
}

/* `/`: open the find/replace dialog, pattern focused, seeded from the live term. */
static void panel_open_find(prov_session_t *s) {
    panel_close(s);
    prov_panel_init(&s->panel, s->a, "Find", NULL, 0, NULL);
    s->panel.pos = PANEL_FULL;
    s->panel_kind = PANEL_K_FIND;
    s->panel_open = true; s->panel_filter = false; s->panel_help = false;
    s->find.focus = FF_PAT;
    prov_le_set(&s->find.pat, s->search.term);
    s->find.pat.anchor = 0;                          /* whole seed pre-selected (overtype-friendly) */
    /* Capture any active selection as the replace-all scope before the incremental
     * search below moves the cursor (which would collapse it). Default scoped ON
     * when a selection exists; `s` toggles it off (whole buffer). */
    prov_selection_t sel = prov_editor_selection(s->ed);
    s->find.has_scope = s->find.scoped = sel.active;
    s->find.scope_lo = sel.start;
    s->find.scope_hi = sel.end;
    s->search.origin = prov_editor_cursor_byte(s->ed);
    search_cache_begin(s);
    find_rerun(s);
}

/* Close the find panel; on cancel restore the pre-search cursor. */
static void find_close(prov_session_t *s, bool cancel) {
    if (cancel) prov_editor_move_to(s->ed, s->search.origin);
    search_cache_end(s);
    panel_close(s);
}

/* 0s: open the recent search terms (newest first); Enter searches with it. */
static void panel_open_search(prov_session_t *s) {
    int n = s->search.hist_n;
    for (int i = 0; i < n; i++) {
        int ri = (s->search.hist_head - 1 - i + 32 * 2) % 32;   /* ring index, newest first */
        prov_cstr_set(s->panel_txt[i], sizeof s->panel_txt[i], prov_cstr_view(s->search.hist[ri]));
        s->panel_rowbuf[i] = (prov_panel_row_t){ .text = s->panel_txt[i], .id = ri };  /* id = ring slot */
    }
    panel_close(s);
    prov_panel_init(&s->panel, s->a, "Search history", s->panel_rowbuf, (proven_size_t)n, NULL);
    s->panel.pos = PANEL_FULL;
    s->panel_kind = PANEL_K_SEARCH;
    s->panel_open = true; s->panel_filter = false; s->panel_scroll = 0;
}

/* 0u: open the undo history (newest action first); Enter undoes back to there. */
static void panel_open_undo(prov_session_t *s) {
    const prov_buffer_t *b = prov_editor_buffer(s->ed);
    proven_size_t depth = prov_buffer_undo_depth(b);
    int n = 0;
    for (proven_size_t k = 0; k < depth && n < 64; k++) {
        prov_undo_view_t v;
        if (!prov_buffer_undo_peek(b, k, &v)) break;
        const char *sign = v.is_replace ? "~" : v.is_insert ? "+" : "-";
        char prev[40]; proven_size_t pl = 0;
        for (proven_size_t i = 0; i < v.bytes_len && pl + 1 < sizeof prev; i++) {
            proven_u8 c = v.bytes[i];
            prev[pl++] = (c < 0x20 || c == 0x7f) ? '.' : (char)c;   /* incl. '\n' -> '.' */
        }
        prev[pl] = 0;
        FMT_INTO(s->panel_txt[n], "{}{}B  L{}  {}", PROVEN_ARG(prov_cstr_view(sign)),
                 PROVEN_ARG((proven_u32)v.len), PROVEN_ARG((proven_u32)(buf_line_of(b, v.at) + 1)),
                 PROVEN_ARG(prov_cstr_view(prev)));
        s->panel_rowbuf[n] = (prov_panel_row_t){ .text = s->panel_txt[n], .id = (int)k };  /* id = undos-1 */
        n++;
    }
    panel_close(s);
    prov_panel_init(&s->panel, s->a, "Undo history", s->panel_rowbuf, (proven_size_t)n, NULL);
    s->panel.pos = PANEL_FULL;
    s->panel_kind = PANEL_K_UNDO;
    s->panel_open = true; s->panel_filter = false; s->panel_scroll = 0;
}

/* 0n: open the jumplist for the current buffer (newest first); Enter jumps back. */
static void panel_open_moves(prov_session_t *s) {
    int n = 0;
    for (int i = 0; i < s->jump.n && n < 64; i++) {
        int ri = (s->jump.head - 1 - i + 32 * 2) % 32;   /* ring index, newest first */
        if (s->jump.hist[ri].ed != s->ed) continue;      /* current buffer only */
        FMT_INTO(s->panel_txt[n], "L{}  {}", PROVEN_ARG((proven_u32)(s->jump.hist[ri].line + 1)),
                 PROVEN_ARG(prov_cstr_view(s->jump.hist[ri].preview)));
        s->panel_rowbuf[n] = (prov_panel_row_t){ .text = s->panel_txt[n], .id = ri };  /* id = ring slot */
        n++;
    }
    panel_close(s);
    prov_panel_init(&s->panel, s->a, "Jumps", s->panel_rowbuf, (proven_size_t)n, NULL);
    s->panel.pos = PANEL_FULL;
    s->panel_kind = PANEL_K_MOVES;
    s->panel_open = true; s->panel_filter = false; s->panel_scroll = 0;
}

/* h: open the keyboard-help overlay as a panel (paged text from help.c; `topic`
 * is the page key, 0 = QWERTY overview). One box/lifecycle, like every panel. */
static void panel_open_help(prov_session_t *s, int topic) {
    panel_close(s);
    prov_panel_init(&s->panel, s->a, "keyboard help", NULL, 0, NULL);
    s->panel.pos = PANEL_FULL;
    s->panel_kind = PANEL_K_HELP;
    s->help_topic = topic;
    s->panel_open = true; s->panel_filter = false; s->panel_scroll = 0;
}

/* 0z: open the command cheat-sheet (static rows; ss filters; Enter -> help page). */
static void panel_open_cmds(prov_session_t *s) {
    panel_close(s);
    prov_panel_init(&s->panel, s->a, "Commands", CMD_ROWS, (proven_size_t)CMD_ROWS_N, NULL);
    s->panel.pos = PANEL_FULL;
    s->panel_kind = PANEL_K_CMDS;
    s->panel_open = true; s->panel_filter = false; s->panel_scroll = 0;
}

/* 0e: open the macro slots that hold a recording. `r`<slot> records, `x`<slot>
 * clears, Enter replays the selected one. */
static void panel_open_macros(prov_session_t *s) {
    int n = 0;
    for (int idx = 0; idx < 36 && n < 64; idx++) {
        if (s->macro.len[idx] == 0) continue;
        char nm[2] = { idx < 26 ? (char)('a' + idx) : (char)('0' + idx - 26), '\0' };
        FMT_INTO(s->panel_txt[n], "{}  {} keys", PROVEN_ARG(prov_cstr_view(nm)),
                 PROVEN_ARG((proven_u32)s->macro.len[idx]));
        s->panel_rowbuf[n] = (prov_panel_row_t){ .text = s->panel_txt[n], .id = idx };
        n++;
    }
    panel_close(s);
    prov_panel_init(&s->panel, s->a, "Macros", s->panel_rowbuf, (proven_size_t)n, &MAC_KEYMAP);
    s->panel.pos = PANEL_FULL;
    s->panel_kind = PANEL_K_MACROS;
    s->panel_open = true; s->panel_filter = false; s->panel_scroll = 0;
}

/* Largest line whose start byte <= pos (0-based), via the monotonic line index. */
static proven_size_t buf_line_of(const prov_buffer_t *buf, proven_size_t pos) {
    proven_size_t lo = 0, hi = prov_buffer_line_count(buf);
    while (lo + 1 < hi) {
        proven_size_t mid = lo + (hi - lo) / 2;
        if (prov_buffer_line_start(buf, mid) <= pos) lo = mid; else hi = mid;
    }
    return lo;
}

/* Record where the cursor is *now* (before a jump) in the 0n jumplist, newest
 * first, deduped by line, cap 32. The buffer (editor) is tagged so 0n only lists
 * the current buffer's jumps. */
static void jump_push(prov_session_t *s) {
    const prov_buffer_t *b = prov_editor_buffer(s->ed);
    proven_size_t pos = prov_editor_cursor_byte(s->ed);
    proven_size_t line = buf_line_of(b, pos);
    if (s->jump.n > 0) {                              /* skip if the newest is the same line */
        int newest = (s->jump.head - 1 + 32) % 32;
        if (s->jump.hist[newest].ed == s->ed && s->jump.hist[newest].line == line) return;
    }
    int w = s->jump.head;                             /* ring: write at head, no struct shift */
    s->jump.head = (s->jump.head + 1) % 32;
    if (s->jump.n < 32) s->jump.n++;
    s->jump.hist[w].ed = s->ed; s->jump.hist[w].pos = pos; s->jump.hist[w].line = line;
    proven_size_t ls = prov_buffer_line_start(b, line);
    proven_u8 tmp[80]; proven_size_t got = prov_buffer_copy_range(b, ls, sizeof tmp - 1, tmp, sizeof tmp);
    proven_size_t pl = 0;
    for (proven_size_t i = 0; i < got; i++) {
        proven_u8 c = tmp[i];
        if (c == '\n') break;
        s->jump.hist[w].preview[pl++] = (c < 0x20 || c == 0x7f) ? '.' : (char)c;
    }
    s->jump.hist[w].preview[pl] = 0;
}

/* 0m: open the current buffer's set bookmarks (a-z). `n`<slot> sets at the cursor,
 * `x`<slot> clears, Enter jumps to the selected one. */
static void panel_open_bookmarks(prov_session_t *s) {
    const prov_buffer_t *buf = prov_editor_buffer(s->ed);
    int n = 0;
    for (int idx = 0; idx < 26; idx++) {
        proven_size_t pos;
        if (!prov_buffer_get_mark(buf, idx, &pos)) continue;
        char nm[2] = { (char)('a' + idx), '\0' };
        proven_size_t ls = prov_buffer_line_start(buf, buf_line_of(buf, pos));
        proven_u8 line[56]; proven_size_t got = prov_buffer_copy_range(buf, ls, sizeof line - 1, line, sizeof line);
        char prev[56]; proven_size_t pl = 0;
        for (proven_size_t i = 0; i < got; i++) {
            proven_u8 c = line[i];
            if (c == '\n') break;
            prev[pl++] = (c < 0x20 || c == 0x7f) ? '.' : (char)c;
        }
        prev[pl] = '\0';
        FMT_INTO(s->panel_txt[n], "{}  L{}  {}", PROVEN_ARG(prov_cstr_view(nm)),
                 PROVEN_ARG((proven_u32)(buf_line_of(buf, pos) + 1)), PROVEN_ARG(prov_cstr_view(prev)));
        s->panel_rowbuf[n] = (prov_panel_row_t){ .text = s->panel_txt[n], .id = idx };
        n++;
    }
    panel_close(s);
    prov_panel_init(&s->panel, s->a, "Bookmarks", s->panel_rowbuf, (proven_size_t)n, &BMK_KEYMAP);
    s->panel.pos = PANEL_FULL;
    s->panel_kind = PANEL_K_BOOKMARKS;
    s->panel_open = true; s->panel_filter = false; s->panel_scroll = 0;
}

/* ---- browser as a heavy/virtual panel (RFC-0010 P6) ----
 * The session's prov_browser_t is the data; browse_view holds the filtered entry
 * indices. The four vsource callbacks read it; rows are formatted (with lazy
 * per-row stat) only as they scroll into view. The session pointer is the ctx. */

enum { BSORT_NAME = 0, BSORT_MTIME, BSORT_EXT };
/* Default postfix filter: the text / syntax-highlightable document formats. The
 * user edits this; an empty box shows every file. Suffixes are matched exactly
 * (a leading '.' is part of the suffix), not as globs or regex. */
#define BROWSE_POSTFIX_DEFAULT \
    ".txt;.md;.markdown;.rst;.html;.htm;.css;.js;.ts;.json;.xml;.yml;.yaml;.toml;" \
    ".ini;.cfg;.conf;.c;.h;.cpp;.hpp;.cc;.hh;.py;.sh;.rs;.go;.java;.lua;.sql;.log;.csv;.tex"

static char br_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Does `name` end with the `len`-byte suffix `suf` (ASCII case-insensitive)? */
static bool name_ends_ci(const char *name, const char *suf, proven_size_t len) {
    proven_size_t nl = 0; while (name[nl]) nl++;
    if (len == 0 || nl < len) return false;
    const char *t = name + nl - len;
    for (proven_size_t i = 0; i < len; i++) if (br_lower(t[i]) != br_lower(suf[i])) return false;
    return true;
}
/* A file passes the postfix filter when the box is empty or its name ends with
 * one of the `;`-separated suffixes. */
static bool browse_postfix_match(const prov_session_t *s, const char *name) {
    const char *p = s->browse.postfix;
    if (!p[0]) return true;
    while (*p) {
        const char *start = p;
        while (*p && *p != ';') p++;
        if (p > start && name_ends_ci(name, start, (proven_size_t)(p - start))) return true;
        if (*p == ';') p++;
    }
    return false;
}

static void browser_grow_view(prov_session_t *s) {
    if (s->browse.view_n < s->browse.view_cap) return;
    proven_size_t cap = s->browse.view_cap ? s->browse.view_cap * 2 : 128;
    proven_result_mem_mut_t rm = s->browse.view
        ? s->a.realloc_fn(s->a.ctx, s->browse.view, s->browse.view_cap * sizeof(proven_size_t), cap * sizeof(proven_size_t), 16)
        : s->a.alloc_fn(s->a.ctx, cap * sizeof(proven_size_t), 16);
    if (!PROVEN_IS_OK(rm.err)) return;
    s->browse.view = (proven_size_t *)rm.value.ptr; s->browse.view_cap = cap;
}
/* The last component after the final '.' (the extension), or "" if none. */
static const char *br_ext(const char *name) {
    const char *dot = NULL;
    for (const char *p = name; *p; p++) if (*p == '.' && p != name) dot = p;
    return dot ? dot + 1 : "";
}
static int br_ci_cmp(const char *a, const char *b) {
    for (;; a++, b++) {
        char ca = br_lower(*a), cb = br_lower(*b);
        if (ca != cb) return (ca < cb) ? -1 : 1;
        if (!ca) return 0;
    }
}
static prov_session_t *g_bsort;                  /* qsort context (single-threaded) */
static int browse_cmp(const void *pa, const void *pb) {
    prov_session_t *s = g_bsort;
    prov_dirent_t *a = &s->browse.model.entries[*(const proven_size_t *)pa];
    prov_dirent_t *b = &s->browse.model.entries[*(const proven_size_t *)pb];
    bool a_up = a->name[0] == '.' && a->name[1] == '.' && a->name[2] == '\0';
    bool b_up = b->name[0] == '.' && b->name[1] == '.' && b->name[2] == '\0';
    if (a_up != b_up) return a_up ? -1 : 1;       /* ".." always first, regardless of direction */
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;   /* folders before files */
    int c;
    if (s->browse.sort == BSORT_MTIME) {
        c = a->mtime == b->mtime ? 0 : (a->mtime > b->mtime ? -1 : 1);   /* newest first (ascending sense) */
        if (c == 0) c = br_ci_cmp(a->name, b->name);
    } else if (s->browse.sort == BSORT_EXT) {
        c = br_ci_cmp(br_ext(a->name), br_ext(b->name));
        if (c == 0) c = br_ci_cmp(a->name, b->name);
    } else {
        c = br_ci_cmp(a->name, b->name);          /* BSORT_NAME */
    }
    return s->browse.sort_desc ? -c : c;          /* reverse only the field order */
}

static void browser_vfilter(void *ctx, const char *f) {
    prov_session_t *s = ctx;
    s->browse.view_n = 0;
    for (proven_size_t i = 0; i < s->browse.model.count; i++) {
        bool keep;
        if (i == 0) keep = (f[0] == '\0');         /* ".." only with no name filter */
        else {
            prov_dirent_t *e = &s->browse.model.entries[i];
            bool ss_ok = f[0] == '\0' || browse_contains_ci(e->name, f);
            bool pf_ok = e->is_dir || browse_postfix_match(s, e->name);   /* postfix: files only */
            keep = ss_ok && pf_ok;
        }
        if (!keep) continue;
        browser_grow_view(s);
        if (s->browse.view_n < s->browse.view_cap) s->browse.view[s->browse.view_n++] = i;
    }
    if (s->browse.sort == BSORT_MTIME)             /* mtime sort needs every shown row stat'd */
        for (proven_size_t i = 0; i < s->browse.view_n; i++)
            prov_browser_ensure_stat(&s->browse.model, s->a, s->browse.view[i]);
    if (s->browse.view_n > 1) {
        g_bsort = s;
        qsort(s->browse.view, s->browse.view_n, sizeof(proven_size_t), browse_cmp);
    }
    /* column widths derived from the shown set: row-number digits, longest extension
     * (default ≤3, capped at 10 so a `.torrent`-class suffix still fits). */
    int nw = 1; for (proven_size_t v = s->browse.view_n; v >= 10; v /= 10) nw++;
    s->browse.num_w = nw;
    int ew = 3;
    for (proven_size_t i = 0; i < s->browse.view_n; i++) {
        prov_dirent_t *e = &s->browse.model.entries[s->browse.view[i]];
        if (e->is_dir) continue;
        const char *x = br_ext(e->name);
        int xl = 0; while (x[xl]) xl++;
        if (xl > ew) ew = xl;
    }
    s->browse.ext_w = ew > 10 ? 10 : ew;
}
static proven_size_t browser_vcount(void *ctx) { return ((prov_session_t *)ctx)->browse.view_n; }
static int browser_vid(void *ctx, proven_size_t i) {
    prov_session_t *s = ctx;
    return i < s->browse.view_n ? (int)s->browse.view[i] : -1;
}
static void browser_vrow(void *ctx, proven_size_t i, char *buf, proven_size_t cap) {
    prov_session_t *s = ctx;
    if (cap == 0) return;
    buf[0] = 0;
    if (i >= s->browse.view_n) return;
    proven_size_t ei = s->browse.view[i];
    prov_dirent_t *e = &s->browse.model.entries[ei];
    if (s->browse.cols & (BCOL_PERMS | BCOL_MTIME | BCOL_OWNER | BCOL_GROUP))
        prov_browser_ensure_stat(&s->browse.model, s->a, ei);
    char nm[300];                                /* name + optional '/' (display fields can be many bytes/cell) */
    int j = 0;
    for (; e->name[j] && j < (int)sizeof nm - 2; j++) nm[j] = e->name[j];
    if (e->is_dir && ei != 0) nm[j++] = '/';
    nm[j] = 0;
    /* Each field is width-aware and code-point-safe; `o` tracks the byte cursor so
     * a multibyte name can never overflow `buf` or split a glyph. */
    proven_size_t o = 0;
    #define APPEND_CELL(c) do { if (o + 1 < cap) buf[o] = (char)(c); o++; } while (0)
    /* row number (1-based; ".." is row 1): right-aligned in the digit-width column */
    char ns[24]; int nl = 0;
    {
        char tmp[24]; int t = 0; proven_size_t v = i + 1;
        if (!v) tmp[t++] = '0';
        while (v) { tmp[t++] = (char)('0' + (int)(v % 10)); v /= 10; }
        while (t) ns[nl++] = tmp[--t];
        ns[nl] = 0;
    }
    int numw = s->browse.num_w < 1 ? 1 : s->browse.num_w;
    o += prov_fit_field(buf, cap, ns, numw, false);   /* right-aligned */
    APPEND_CELL(' ');
    /* name: head…tail abbreviation keeps the start + trailing extension visible */
    char nmab[96];
    prov_abbreviate_filename(nm, 26, nmab, sizeof nmab);
    if (o < cap) o += prov_fit_field(buf + o, cap - o, nmab, 26, true);
    /* extension column (RFC-0015): suffix after the last interior '.', files only.
     * Width tracks the longest shown extension (≤10); a longer one is middle-elided. */
    char ext[64] = "";
    if (!e->is_dir) {
        proven_size_t nlen = 0; while (e->name[nlen]) nlen++;
        proven_size_t dot = 0;
        for (proven_size_t k = 1; k < nlen; k++) if (e->name[k] == '.') dot = k;
        if (dot > 0 && dot + 1 < nlen) {
            proven_size_t x = 0;
            for (proven_size_t k = dot + 1; k < nlen && x + 1 < sizeof ext; k++) ext[x++] = e->name[k];
            ext[x] = '\0';
        }
    }
    int extw = s->browse.ext_w < 3 ? 3 : s->browse.ext_w;
    char extd[80];
    int exl = 0; while (ext[exl]) exl++;
    if (exl <= extw) {
        int p = 0;
        while (ext[p]) { extd[p] = ext[p]; p++; }
        extd[p] = 0;
    } else {                                          /* head…tail middle elision (U+2026) */
        int lead = (extw - 1) / 2, trail = extw - 1 - lead, p = 0;
        for (int k = 0; k < lead; k++) extd[p++] = ext[k];
        extd[p++] = '\xe2'; extd[p++] = '\x80'; extd[p++] = '\xa6';
        for (int k = exl - trail; k < exl; k++) extd[p++] = ext[k];
        extd[p] = 0;
    }
    APPEND_CELL(' ');
    if (o < cap) o += prov_fit_field(buf + o, cap - o, extd, extw, true);
    if (s->browse.cols & BCOL_SIZE)  { APPEND_CELL(' '); char t[16]; prov_fmt_size(t, sizeof t, e->size, e->is_dir);
                                       if (o < cap) o += prov_fit_field(buf + o, cap - o, t, 8, false); }
    if (s->browse.cols & BCOL_PERMS) { APPEND_CELL(' '); char t[12]; prov_fmt_perms(t, e->perms, e->is_dir);
                                       if (o < cap) o += prov_fit_field(buf + o, cap - o, t, 10, true); }
    if (s->browse.cols & BCOL_OWNER) { APPEND_CELL(' ');
                                       if (o < cap) o += prov_fit_field(buf + o, cap - o, e->owner, 12, true); }
    if (s->browse.cols & BCOL_GROUP) { APPEND_CELL(' ');
                                       if (o < cap) o += prov_fit_field(buf + o, cap - o, e->group, 12, true); }
    if (s->browse.cols & BCOL_MTIME) { APPEND_CELL(' '); char t[24]; prov_fmt_mtime(t, sizeof t, e->mtime);
                                       if (o < cap) o += prov_fit_field(buf + o, cap - o, t, 16, true); }
    #undef APPEND_CELL
    if (o >= cap) o = cap - 1;
    buf[o] = 0;
}
enum { PV_OPTS = 10, PV_PARENT, PV_BACK, PV_ENTER, PV_FWD, PV_SORT, PV_PFEDIT,
       PV_ENC, PV_BOM, PV_PREVIEW, PV_BACKEND, PV_RO, PV_PATH, PV_BINARY, PV_EOL };  /* browser immediate verbs */
static const prov_keymap_entry_t BR_VERBS[] = {
    { 'I', PV_PARENT, "up" }, { 'K', PV_ENTER, "open dir" },
    { 'J', PV_BACK, "back" }, { 'L', PV_FWD, "fwd" },
    { 't', PV_SORT, "sort" }, { 'f', PV_PFEDIT, "types" }, { 'C', PV_OPTS, "column" },
    { 'e', PV_ENC, "encoding" }, { 'm', PV_BACKEND, "backend" }, { 'b', PV_BOM, "bom" },
    { 'R', PV_RO, "readonly" }, { 'v', PV_PREVIEW, "preview" }, { 'p', PV_PATH, "path" },
    { 'x', PV_BINARY, "hex" }, { 'r', PV_EOL, "eol" }
};
static const prov_keymap_t       BR_KEYMAP  = { BR_VERBS, 15 };

/* Open-as encoding cycle: UTF-8 (auto, the empty string) then the common legacy
 * code pages, which are converted through the charset PAL. UTF-16/32 stay
 * BOM-auto-detected, so they are not in the forced cycle. */
static const char *const ENC_CYCLE[] = { "", "CP949", "SHIFT_JIS", "GBK", "BIG5", "EUC-JP" };

/* (the open-as encoding is now chosen on the `e` sub-screen, RFC-0015 Phase C) */

/* (Re)load `dir` and refilter, keeping the panel open. Returns false (leaving the
 * current listing intact — the load is atomic) if `dir` can't be opened. The panel
 * title points at the browser's own dir buffer, so it tracks the directory. */
static bool browser_reload(prov_session_t *s, const char *dir) {
    if (!prov_browser_load(&s->browse.model, s->a, dir)) {
        FMT_INTO(s->message, "cannot open '{}'", PROVEN_ARG(prov_cstr_view(dir)));
        return false;
    }
    s->panel.filter[0] = '\0'; s->panel.flen = 0;
    prov_panel_refilter(&s->panel);                   /* rebuild browse_view via the vsource */
    s->panel.sel = 0; s->panel_scroll = 0;
    return true;
}

/* Forward navigation (enter a folder / go to parent / open a directory). On
 * success, append the new dir to the visit history at the cursor, dropping any
 * forward entries (a fresh branch — like a web/file-explorer history). */
static void browser_goto(prov_session_t *s, const char *dir) {
    if (!browser_reload(s, dir)) return;
    int cur = s->browse.hist_cur;
    if (cur + 1 < s->browse.hist_n) s->browse.hist_n = cur + 1;   /* drop the forward branch */
    if (s->browse.hist_n >= 16) {                                 /* full: drop the oldest */
        for (int k = 1; k < 16; k++) prov_cstr_set(s->browse.hist[k - 1], 1024, prov_cstr_view(s->browse.hist[k]));
        s->browse.hist_n = 15;
    }
    prov_cstr_set(s->browse.hist[s->browse.hist_n], 1024, prov_cstr_view(s->browse.model.dir));
    s->browse.hist_cur = s->browse.hist_n;
    s->browse.hist_n++;
}

/* `J` back / `L` forward through the visit history (Windows-Explorer style); the
 * cursor moves but the history list is unchanged. Reverts on a load failure. */
static void browser_history(prov_session_t *s, int dir) {       /* dir: -1 back, +1 forward */
    int want = s->browse.hist_cur + dir;
    if (want < 0 || want >= s->browse.hist_n) {
        FMT_INTO(s->message, dir < 0 ? "no previous folder" : "no next folder");
        return;
    }
    char path[1024];
    prov_cstr_set(path, sizeof path, prov_cstr_view(s->browse.hist[want]));
    if (browser_reload(s, path)) s->browse.hist_cur = want;
}

/* zo: open the file browser as a heavy panel rooted at the current file's dir. */
static void panel_open_browser(prov_session_t *s) {
    char dir[1024];
    browse_dir_of(s->path, dir, sizeof dir);
    browse_resolve(dir, sizeof dir);
    if (!s->browse.cols) s->browse.cols = BCOL_SIZE | BCOL_MTIME;
    if (!prov_browser_load(&s->browse.model, s->a, dir)) (void)prov_browser_load(&s->browse.model, s->a, ".");
    s->browse.vs = (prov_panel_vsource_t){ .ctx = s, .count = browser_vcount,
        .row = browser_vrow, .id = browser_vid, .filter = browser_vfilter };
    panel_close(s);
    prov_panel_init_dynamic(&s->panel, s->a, s->browse.model.dir, &s->browse.vs, &BR_KEYMAP);
    s->panel.pos = PANEL_FULL;
    s->panel_kind = PANEL_K_BROWSER;
    s->panel_open = true; s->panel_filter = false; s->panel_scroll = 0;
    prov_cstr_set(s->browse.hist[0], 1024, prov_cstr_view(s->browse.model.dir));   /* history root */
    s->browse.hist_n = 1; s->browse.hist_cur = 0;
    if (!s->browse.postfix[0])                    /* first open: seed the postfix filter */
        prov_cstr_set(s->browse.postfix, sizeof s->browse.postfix, prov_cstr_view(BROWSE_POSTFIX_DEFAULT));
    s->browse.pf_edit = false;
    s->browse.preview = true;                     /* RFC-0015: the preview box is part of the layout */
    s->browse.focus = BF_LIST;
    s->browse.pathhist.pos = -1;                  /* not navigating history (entries persist) */
    prov_panel_refilter(&s->panel);               /* apply sort + postfix now */
}

/* `I`: ascend to the parent directory (entry 0 = ".."). */
static void browser_parent(prov_session_t *s) {
    char up[1024];
    prov_browser_path_at(&s->browse.model, 0, up, sizeof up);
    browser_goto(s, up);
}

/* `K`: descend into the selected entry if it is a folder; ignore a plain file. */
static void browser_enter(prov_session_t *s) {
    int id = prov_panel_selected_id(&s->panel);
    if (id < 0 || !s->browse.model.entries[id].is_dir) return;
    char path[1024];
    prov_browser_path_at(&s->browse.model, (proven_size_t)id, path, sizeof path);
    browser_goto(s, path);
}

/* ---- file-browser preview pane (RFC-0013) ---- */
#define PV_READ_CAP 65536u   /* bytes read for a preview (bounded; never the whole file) */

static void pv_set(prov_session_t *s, proven_u8 *buf, proven_size_t len, bool hex) {
    if (s->browse.pv_buf) s->a.free_fn(s->a.ctx, s->browse.pv_buf);
    s->browse.pv_buf = buf; s->browse.pv_len = len; s->browse.pv_hex = hex; s->browse.pv_top = 0;
}

/* A capped byte span looks binary if it has a NUL or many control bytes. */
static bool pv_looks_binary(const proven_u8 *b, proven_size_t n) {
    proven_size_t ctrl = 0;
    for (proven_size_t i = 0; i < n; i++) {
        proven_u8 c = b[i];
        if (c == 0) return true;
        if (c < 0x09 || (c > 0x0D && c < 0x20)) ctrl++;
    }
    return n > 0 && ctrl * 100 > n * 30;   /* >30% control bytes */
}

/* Refresh the preview for the selected entry, decoding with the chosen encoding
 * (charset PAL when `open_enc` is set, else raw UTF-8 with invalid bytes skipped),
 * or a hex view for a binary file. Cached by (path, enc) so it runs only when the
 * selection or encoding changes. Directories / ".." clear the preview. */
static void browser_refresh_preview(prov_session_t *s) {
    if (!s->browse.preview) return;
    proven_size_t sel = s->panel.sel;
    if (sel >= s->browse.view_n) { s->browse.pv_path[0] = '\0'; pv_set(s, NULL, 0, false); return; }
    proven_size_t ei = s->browse.view[sel];
    prov_dirent_t *e = &s->browse.model.entries[ei];
    if (e->is_dir) {                                   /* dir / ".." : nothing to preview */
        if (s->browse.pv_path[0]) { s->browse.pv_path[0] = '\0'; pv_set(s, NULL, 0, false); }
        return;
    }
    char full[1280];
    prov_browser_path_at(&s->browse.model, ei, full, sizeof full);
    const char *enc = s->open_enc[0] ? s->open_enc : "";
    if (proven_u8str_view_eq(prov_cstr_view(full), prov_cstr_view(s->browse.pv_path)) &&
        proven_u8str_view_eq(prov_cstr_view(enc), prov_cstr_view(s->browse.pv_enc)))
        return;                                        /* cache hit */
    prov_cstr_set(s->browse.pv_path, sizeof s->browse.pv_path, prov_cstr_view(full));
    prov_cstr_set(s->browse.pv_enc, sizeof s->browse.pv_enc, prov_cstr_view(enc));

    proven_result_file_t of = proven_fs_open(s->a, prov_cstr_view(full), PROVEN_FS_READ);
    if (!PROVEN_IS_OK(of.err)) { pv_set(s, NULL, 0, false); return; }
    proven_result_mem_mut_t rb = s->a.alloc_fn(s->a.ctx, PV_READ_CAP, 16);
    if (!PROVEN_IS_OK(rb.err)) { proven_fs_close(of.value); pv_set(s, NULL, 0, false); return; }
    proven_u8 *raw = (proven_u8 *)rb.value.ptr;
    proven_result_size_t rr = proven_fs_read(of.value, (proven_mem_mut_t){ rb.value.ptr, PV_READ_CAP });
    proven_fs_close(of.value);
    proven_size_t n = PROVEN_IS_OK(rr.err) ? rr.value : 0;

    bool bin = s->open_binary || pv_looks_binary(raw, n);   /* RFC-0019: binary toggle forces hex */
    if (!bin && enc[0] && prov_charset_supports(enc)) {     /* encoding-aware decode */
        proven_size_t outn = 0;
        proven_u8 *dec = prov_charset_to_utf8(s->a, enc, raw, n, &outn);
        s->a.free_fn(s->a.ctx, raw);
        if (dec) pv_set(s, dec, outn, false);
        else pv_set(s, NULL, 0, false);
    } else {
        pv_set(s, raw, n, bin);     /* hex for binary, else raw UTF-8 (textbox skips invalid) */
    }
}

/* `o`: cycle the visible info columns through a few presets. */
static void browser_cycle_cols(prov_session_t *s) {
    static const unsigned presets[] = {
        BCOL_SIZE | BCOL_MTIME, 0u, BCOL_SIZE, BCOL_SIZE | BCOL_PERMS | BCOL_MTIME,
        BCOL_PERMS | BCOL_OWNER | BCOL_GROUP | BCOL_SIZE | BCOL_MTIME,   /* `ls -l`-style */
    };
    int n = (int)(sizeof presets / sizeof presets[0]);
    int cur = 0;
    for (int i = 0; i < n; i++) if (presets[i] == s->browse.cols) { cur = i; break; }
    s->browse.cols = presets[(cur + 1) % n];
}

/* Per-panel Enter/pick action. `id` is the selected item (or <0 = none). The
 * return value says whether panel_activate should close the panel afterward:
 * true = close; false = the handler already closed/replaced it, or wants it to
 * stay open (e.g. descending into a browser folder). */
typedef bool (*panel_activate_fn)(prov_session_t *s, int id);

static bool pa_windows(prov_session_t *s, int id) {
    if (id < 0) return true;
    prov_layout_t *L = cur_layout(s);
    if (id != L->focus) { buf_save_active(s); L->focus = id; buf_load_active(s); buf_reset_idle(s); }
    return true;
}
static bool pa_tabs(prov_session_t *s, int id) {
    if (id < 0) return true;
    if (id < TAB_CHILD) { tab_goto(s, id); return true; }   /* a tab row: switch to it */
    int t = (id - TAB_CHILD) / 64, leaf = (id - TAB_CHILD) % 64;   /* a window row: go there */
    tab_goto(s, t);
    prov_layout_t *L = &s->tabs[t];
    if (leaf != L->focus) { buf_save_active(s); L->focus = leaf; buf_load_active(s); buf_reset_idle(s); }
    return true;
}
static bool pa_regs(prov_session_t *s, int id) {
    if (id >= 0 && !ro_guard(s) && reg_load(s, id)
        && PROVEN_IS_OK(prov_editor_paste_lines(s->ed, true, 1))) s->modified = true;
    return true;
}
static bool pa_macros(prov_session_t *s, int id) {
    if (id < 0) return true;
    panel_close(s);                                /* close before replaying into the key queue */
    s->macro.last = id;
    feed_push(s, s->macro.slot[id], s->macro.len[id]);
    return false;
}
static bool pa_bookmarks(prov_session_t *s, int id) {
    proven_size_t pos;
    if (id >= 0 && prov_buffer_get_mark(prov_editor_buffer(s->ed), id, &pos)) {
        prov_editor_set_extending(s->ed, s->zx_visual);
        prov_editor_move_to(s->ed, pos);
    }
    return true;
}
static bool pa_search(prov_session_t *s, int id) {
    if (id < 0) return true;
    prov_cstr_set(s->search.term, sizeof s->search.term, prov_cstr_view(s->search.hist[id]));
    s->search.valid = true;
    search_recompile(s);
    panel_close(s);
    do_search(s, true, false);                     /* jump to the next match from the cursor */
    return false;
}
static bool pa_cmds(prov_session_t *s, int id) {
    if (id < 0) return true;
    panel_open_help(s, id);                        /* open that key's help page (as a panel) */
    return false;
}
static bool pa_moves(prov_session_t *s, int id) {
    if (id < 0) return true;
    proven_size_t pos = s->jump.hist[id].pos;
    proven_size_t total = prov_buffer_byte_len(prov_editor_buffer(s->ed));
    if (pos > total) pos = total;                   /* clamp: edits may have shifted it */
    prov_editor_set_extending(s->ed, s->zx_visual);
    prov_editor_move_to(s->ed, pos);
    return true;
}
static bool pa_undo(prov_session_t *s, int id) {
    if (id >= 0 && !ro_guard(s)) {                  /* undo (id+1) actions back to that point */
        for (int i = 0; i <= id; i++) if (!prov_editor_undo(s->ed)) break;
        s->modified = true;
    }
    return true;
}
/* RFC-0012 P2 picker callback: a path chosen in the browser (or NULL on cancel)
 * is delivered back to the save-as dialog, which reopens with that path in its
 * input (or the text the user had typed before browsing, on cancel). */
static void saveas_pick(prov_session_t *s, const char *value) {
    int buf = s->saveas_buf;
    panel_open_saveas(s, buf);                     /* re-show the dialog (reseeds prompt_le) */
    prov_le_set(&s->prompt_le, value ? value : s->picker_stash);
}

static bool pa_browser(prov_session_t *s, int id) {
    if (id < 0) return true;
    char path[1024];
    prov_browser_path_at(&s->browse.model, (proven_size_t)id, path, sizeof path);
    if (s->browse.model.entries[id].is_dir) { browser_goto(s, path); return false; }  /* descend, stay open */
    if (s->panel_pick) {                           /* picker mode: return the file path to the host */
        void (*cb)(prov_session_t *, const char *) = s->panel_pick;
        s->panel_pick = NULL;
        cb(s, path);                               /* cb reopens the host; don't close it */
        return false;
    }
    open_path(s, path);                                                          /* file: open + close */
    return true;
}

/* The Enter/pick dispatch, indexed by PANEL_K_*. Adding a panel = add a row. */
static const panel_activate_fn PANEL_ACTIVATE[] = {
    [PANEL_K_WINDOWS]   = pa_windows,
    [PANEL_K_TABS]      = pa_tabs,
    [PANEL_K_REGS]      = pa_regs,
    [PANEL_K_MACROS]    = pa_macros,
    [PANEL_K_BOOKMARKS] = pa_bookmarks,
    [PANEL_K_BROWSER]   = pa_browser,
    [PANEL_K_SEARCH]    = pa_search,
    [PANEL_K_CMDS]      = pa_cmds,
    [PANEL_K_MOVES]     = pa_moves,
    [PANEL_K_UNDO]      = pa_undo,
};

static void panel_activate(prov_session_t *s) {
    int id = prov_panel_selected_id(&s->panel);
    int kind = s->panel_kind;
    panel_activate_fn fn = (kind > 0 && kind < (int)(sizeof PANEL_ACTIVATE / sizeof *PANEL_ACTIVATE))
                         ? PANEL_ACTIVATE[kind] : NULL;
    if (!fn || fn(s, id)) panel_close(s);
}

/* Apply one key to a line-edit field (RFC-0015 shared input): arrows (Ctrl = word,
 * Shift = extend), Home/End, Backspace/Delete, Ctrl+C/X/V via the OS clipboard,
 * and printable insertion. Returns true if it was an editing key (Enter/Esc/Up/
 * Down and history are left to the caller). */
static bool le_handle_key(prov_lineedit_t *le, prov_key_t k) {
    switch (k.kind) {
        case PROV_KEY_LEFT:  prov_le_move(le, k.ctrl ? PROV_LE_WORD_LEFT  : PROV_LE_LEFT,  k.shift); return true;
        case PROV_KEY_RIGHT: prov_le_move(le, k.ctrl ? PROV_LE_WORD_RIGHT : PROV_LE_RIGHT, k.shift); return true;
        case PROV_KEY_HOME:  prov_le_move(le, PROV_LE_HOME, k.shift); return true;
        case PROV_KEY_END:   prov_le_move(le, PROV_LE_END,  k.shift); return true;
        case PROV_KEY_BACKSPACE: prov_le_backspace(le); return true;
        case PROV_KEY_DELETE:    prov_le_delete(le);    return true;
        case PROV_KEY_CTRL:
            if (k.cp == 'a') { le->anchor = 0; le->cur = le->len; return true; }  /* select all, cursor to end */
            if (k.cp == 'c' || k.cp == 'x') {                  /* copy/cut: selection else whole field */
                proven_size_t a = 0, b = le->len;
                if (prov_le_has_sel(le)) prov_le_sel_range(le, &a, &b);
                if (b > a) prov_os_clip_set((const proven_u8 *)le->buf + a, b - a);
                if (k.cp == 'x') { if (prov_le_has_sel(le)) prov_le_delete_sel(le); else prov_le_clear(le); }
                return true;
            }
            if (k.cp == 'v') {                                 /* paste at the cursor (control bytes filtered) */
                proven_u8 cb[PROV_LE_CAP];
                proven_size_t n = prov_os_clip_get(cb, sizeof cb);
                for (proven_size_t i = 0; i < n; i++) {
                    proven_u8 c = cb[i];
                    if (c == '\n' || c == '\r' || c == '\t' || c >= 0x20) {
                        char one = (c < 0x20) ? ' ' : (char)c;
                        prov_le_insert(le, &one, 1);
                    }
                }
                return true;
            }
            return false;
        case PROV_KEY_CHAR:
            if (k.cp >= 0x20 && k.cp != 0x7f) { prov_le_insert(le, (const char *)k.bytes, k.nbytes); return true; }
            return false;
        default: return false;
    }
}

/* The four open-dialog option actions (RFC-0015), shared by the verb keys
 * (e/B/b/r) and the Tab-focus + Enter path. */
static void browse_opt_enc(prov_session_t *s) {
    int n = (int)(sizeof ENC_CYCLE / sizeof ENC_CYCLE[0]), cur = 0;
    for (int i = 0; i < n; i++)
        if (proven_u8str_view_eq(prov_cstr_view(ENC_CYCLE[i]), prov_cstr_view(s->open_enc))) { cur = i; break; }
    s->browse.subscreen = BSUB_ENC; s->browse.sub_sel = cur;
}
static void browse_opt_backend(prov_session_t *s) {
    const char *bl[BR_BACKEND_MAX]; int n = br_backends(bl); int cur = 0;
    const char *cb = s->cfg.charset_backend[0] ? s->cfg.charset_backend : "auto";
    for (int i = 0; i < n; i++)
        if (proven_u8str_view_eq(prov_cstr_view(bl[i]), prov_cstr_view(cb))) { cur = i; break; }
    s->browse.subscreen = BSUB_BACKEND; s->browse.sub_sel = cur;
}
static void browse_opt_bom(prov_session_t *s) {
    s->open_bom = !s->open_bom;
    FMT_INTO(s->message, "BOM on save: {}", PROVEN_ARG(prov_cstr_view(s->open_bom ? "on" : "off")));
}
static void browse_opt_ro(prov_session_t *s) {
    s->open_ro = !s->open_ro;
    FMT_INTO(s->message, "open read-only: {}", PROVEN_ARG(prov_cstr_view(s->open_ro ? "on" : "off")));
}
static void browse_opt_binary(prov_session_t *s) {
    s->open_binary = !s->open_binary;
    s->browse.pv_path[0] = '\0';   /* invalidate the preview cache so it re-decodes as hex/text */
    FMT_INTO(s->message, "open as binary (hex): {}", PROVEN_ARG(prov_cstr_view(s->open_binary ? "on" : "off")));
}
static const char *eol_choice_name(int e) {   /* 0=auto,1=LF,2=CRLF,3=CR */
    return e == 1 ? "LF" : e == 2 ? "CRLF" : e == 3 ? "CR" : "auto";
}
static void browse_opt_eol(prov_session_t *s) {   /* cycle Auto -> LF -> CRLF -> CR (request 1) */
    s->open_eol = (s->open_eol + 1) % 4;
    FMT_INTO(s->message, "read line endings as: {}", PROVEN_ARG(prov_cstr_view(eol_choice_name(s->open_eol))));
}

/* Enter on the path field (RFC-0015): resolve the typed path against the current
 * dir, then navigate into a directory or open a file (honoring open_enc/bom). */
static void browser_path_submit(prov_session_t *s) {
    if (!s->browse.pathedit.len) { s->browse.focus = BF_LIST; return; }
    prov_lehist_push(&s->browse.pathhist, s->browse.pathedit.buf);   /* remember the typed path */
    char resolved[1280];
    prov_browser_resolve_path(s->browse.model.dir, s->browse.pathedit.buf, resolved, sizeof resolved);
    proven_fs_stat_t st;
    if (!PROVEN_IS_OK(proven_fs_stat(s->a, prov_cstr_view(resolved), &st))) {
        FMT_INTO(s->message, "no such path: {}", PROVEN_ARG(prov_cstr_view(s->browse.pathedit.buf)));
        return;
    }
    prov_le_clear(&s->browse.pathedit);
    if (st.type == PROVEN_FS_TYPE_DIR) { s->browse.focus = BF_LIST; browser_goto(s, resolved); return; }
    if (s->panel_pick) {                               /* picker mode: hand the path to the host */
        void (*cb)(prov_session_t *, const char *) = s->panel_pick;
        s->panel_pick = NULL;
        cb(s, resolved);
        return;
    }
    open_path(s, resolved);                            /* file: open (encoding/BOM) + close */
    panel_close(s);
}

static void panel_key(prov_session_t *s, prov_key_t k) {
    if (s->panel_kind == PANEL_K_HEXEDIT) { handle_hexedit_key(s, k); return; }   /* RFC-0019 P3 */
    if (s->panel_kind == PANEL_K_FIND && !s->panel_help) {   /* RFC-0016 find/replace dialog */
        bool editing = (s->find.focus == FF_PAT || s->find.focus == FF_REPL);
        if (k.kind == PROV_KEY_ESC) { find_close(s, true); return; }   /* cancel: restore cursor */
        if (k.kind == PROV_KEY_TAB) {
            int step = k.shift ? (FF_COUNT - 1) : 1;
            s->find.focus = (s->find.focus + step) % FF_COUNT;
            return;
        }
        if (editing) {
            prov_lineedit_t *le = (s->find.focus == FF_PAT) ? &s->find.pat : &s->find.repl;
            prov_lehist_t   *h  = (s->find.focus == FF_PAT) ? &s->find.pathist : &s->find.replhist;
            if (k.kind == PROV_KEY_ENTER) {            /* commit the field */
                if (s->find.focus == FF_PAT) {
                    prov_lehist_push(&s->find.pathist, s->find.pat.buf);
                    if (s->search.valid) {             /* jump to the next match from the cursor */
                        search_hist_push(s, s->search.term);
                        search_run(s, s->search.hay, s->search.haylen, true, true);
                        find_count(s);
                    }
                } else {
                    prov_lehist_push(&s->find.replhist, s->find.repl.buf);
                    s->find.focus = FF_PAT;
                }
                return;
            }
            if (k.kind == PROV_KEY_UP || k.kind == PROV_KEY_DOWN) {
                prov_le_history(le, h, k.kind == PROV_KEY_UP);
                if (s->find.focus == FF_PAT) find_rerun(s);
                return;
            }
            bool ed = le_handle_key(le, k);
            if (ed && s->find.focus == FF_PAT) find_rerun(s);   /* incremental live preview */
            return;
        }
        /* not editing: option toggles (focus + Enter/Space, or x/w/c) and verbs */
        int tog = -1;
        if (k.kind == PROV_KEY_ENTER || (k.kind == PROV_KEY_CHAR && k.cp == ' ')) {
            if (s->find.focus >= FF_REGEX) tog = s->find.focus;
        } else if (k.kind == PROV_KEY_CHAR && k.nbytes == 1) {
            char c = k.bytes[0];
            if (c == 'x') tog = FF_REGEX;
            else if (c == 'w') tog = FF_WORD;
            else if (c == 'c') tog = FF_CASE;
            else if (c == 's') { if (s->find.has_scope) s->find.scoped = !s->find.scoped; return; }  /* scope replace to selection */
            else if (c == 'n') { if (s->search.valid) { search_hist_push(s, s->search.term);
                                   search_run(s, s->search.hay, s->search.haylen, true, true); find_count(s); } return; }
            else if (c == 'N') { if (s->search.valid) { search_run(s, s->search.hay, s->search.haylen, false, true); find_count(s); } return; }
            else if (c == 'r') { find_replace_one(s); return; }
            else if (c == 'a') {                                   /* replace all (within scope when set) */
                proven_size_t total = prov_buffer_byte_len(prov_editor_buffer(s->ed));
                proven_size_t lo = s->find.scoped ? s->find.scope_lo : 0;
                proven_size_t hi = s->find.scoped ? s->find.scope_hi : total;
                if (hi > total) hi = total;
                do_replace_scoped(s, s->find.repl.buf, lo, hi);
                s->find.scoped = s->find.has_scope = false;        /* scope consumed; bounds now stale */
                search_cache_begin(s);
                s->search.origin = prov_editor_cursor_byte(s->ed); find_count(s); return; }
            else if (c == 'q') { find_close(s, false); return; }   /* close, keep the match */
            else if (c == 'h') { s->panel_help = true; s->panel_help_scroll = 0; return; }
        }
        if (tog >= 0) {
            switch (tog) {
                case FF_REGEX: s->search.regex = !s->search.regex; if (!s->search.regex) s->search.word = false; break;
                case FF_WORD:  s->search.word  = !s->search.word;  if (s->search.word) s->search.regex = true; break;
                case FF_CASE:  s->search.icase = !s->search.icase; break;
                case FF_HL:    s->cfg.search_highlight = !s->cfg.search_highlight; break;
            }
            search_recompile(s);
            find_rerun(s);
        }
        return;                                        /* swallow any other key in the dialog */
    }
    if (s->panel_kind == PANEL_K_SAVEAS) {            /* save dialog: edit path / confirm overwrite */
        if (s->saveas_state == SA_CONFIRM) {          /* the file exists: y overwrite / n rename */
            if (k.kind == PROV_KEY_ESC) { panel_close(s); return; }
            if (k.kind == PROV_KEY_CHAR && k.nbytes == 1) {
                if (k.bytes[0] == 'y')      saveas_do(s);
                else if (k.bytes[0] == 'n') { s->saveas_state = SA_PATH; s->saveas_msg[0] = '\0'; }
            }
            return;
        }
        if (k.kind == PROV_KEY_ESC) { panel_close(s); return; }
        if (k.kind == PROV_KEY_TAB) {                 /* browse for a path; pick returns it here */
            prov_cstr_set(s->picker_stash, sizeof s->picker_stash, prov_cstr_view(s->prompt_le.buf));
            char dir[1024]; browse_dir_of(s->picker_stash, dir, sizeof dir);
            panel_open_browser(s);                    /* panel_close inside clears panel_pick */
            if (dir[0]) browser_goto(s, dir);         /* start where the typed path points */
            s->panel_pick = saveas_pick;              /* now a sub-picker returning to save-as */
            return;
        }
        if (k.kind == PROV_KEY_ENTER) {
            if (s->prompt_le.len == 0) { FMT_INTO(s->saveas_msg, "type a file name first"); return; }
            char path[1024]; prov_cstr_set(path, sizeof path, prov_cstr_view(s->prompt_le.buf));
            proven_fs_stat_t st;
            if (PROVEN_IS_OK(proven_fs_stat(s->a, prov_cstr_view(path), &st))) {   /* exists: confirm */
                s->saveas_state = SA_CONFIRM;
                FMT_INTO(s->saveas_msg, "\"{}\" already exists.", PROVEN_ARG(prov_cstr_view(path)));
            } else saveas_do(s);
            return;
        }
        if (le_handle_key(&s->prompt_le, k)) { s->saveas_msg[0] = '\0'; return; }   /* line editor */
        return;
    }
    if (s->panel_kind == PANEL_K_HELP) {
        /* `h` is a prefix: the NEXT key picks that key's help page (so the help
         * panel uses the same common keys as every other panel — w move, ikjl
         * scroll, q/x close — and a bare key never silently drills). */
        if (s->help_await) {
            s->help_await = false;
            if (k.kind == PROV_KEY_CHAR && k.nbytes == 1) {
                char c = (char)k.bytes[0];
                s->help_topic = (c >= '1' && c <= '9') ? '#' : prov_help_topic_for_key(c);
                s->panel_scroll = 0;
            }
            return;
        }
        /* ESC / Space / Enter -> back to the key-arrangement overview (q/x exits) */
        if (k.kind == PROV_KEY_ESC || k.kind == PROV_KEY_ENTER) { s->help_topic = 0; s->panel_scroll = 0; return; }
        if (k.kind == PROV_KEY_UP)       { if (s->panel_scroll > 0) s->panel_scroll--; return; }
        if (k.kind == PROV_KEY_DOWN)     { s->panel_scroll++; return; }
        if (k.kind == PROV_KEY_PAGEUP)   { s->panel_scroll = s->panel_scroll > 10 ? s->panel_scroll - 10 : 0; return; }
        if (k.kind == PROV_KEY_PAGEDOWN) { s->panel_scroll += 10; return; }
        if (k.kind == PROV_KEY_CHAR && k.nbytes == 1) {
            char c = (char)k.bytes[0];
            if      (c == 'q' || c == 'x') panel_close(s);                                  /* exit the help panel */
            else if (c == ' ')           { s->help_topic = 0; s->panel_scroll = 0; }        /* overview */
            else if (c == 'w')           { s->panel.pos = (prov_panel_pos_t)((s->panel.pos + 1) % 5); s->panel_scroll = 0; }  /* move */
            else if (c == 'h')             s->help_await = true;                            /* prefix: h<key> = look up */
            else if (c == 'i')           { if (s->panel_scroll > 0) s->panel_scroll--; }    /* scroll (narrow panels) */
            else if (c == 'k')             s->panel_scroll++;
            else if (c == 'j')           { s->panel_scroll = s->panel_scroll > 10 ? s->panel_scroll - 10 : 0; }  /* page */
            else if (c == 'l')             s->panel_scroll += 10;
            /* any other key is inert: only the help panel's own keys act */
        }
        return;
    }
    if (s->browse.pf_edit) {                          /* editing the type filter (line editor) */
        if (k.kind == PROV_KEY_ENTER) {               /* commit: remember this filter for ↑/↓ recall */
            prov_lehist_push(&s->browse.pfhist, s->browse.pfedit.buf);
            s->browse.pf_edit = false; return;
        }
        if (k.kind == PROV_KEY_ESC) { s->browse.pf_edit = false; return; }
        if (k.kind == PROV_KEY_UP || k.kind == PROV_KEY_DOWN) {   /* session history (config-persisted later) */
            prov_le_history(&s->browse.pfedit, &s->browse.pfhist, k.kind == PROV_KEY_UP);
            prov_cstr_set(s->browse.postfix, sizeof s->browse.postfix, prov_cstr_view(s->browse.pfedit.buf));
            prov_panel_refilter(&s->panel); s->panel.sel = 0; s->panel_scroll = 0; return;
        }
        le_handle_key(&s->browse.pfedit, k);
        prov_cstr_set(s->browse.postfix, sizeof s->browse.postfix, prov_cstr_view(s->browse.pfedit.buf));
        prov_panel_refilter(&s->panel);               /* live re-filter as the box changes */
        s->panel.sel = 0;
        return;
    }
    if (s->panel_help) {                              /* `h` intent-help overlay over a panel */
        if (k.kind == PROV_KEY_ESC || k.kind == PROV_KEY_ENTER) { s->panel_help = false; return; }
        if (k.kind == PROV_KEY_UP || (k.kind == PROV_KEY_CHAR && k.cp == 'i')) {
            if (s->panel_help_scroll > 0) s->panel_help_scroll--;
            return;
        }
        if (k.kind == PROV_KEY_DOWN || (k.kind == PROV_KEY_CHAR && k.cp == 'k')) { s->panel_help_scroll++; return; }
        if (k.kind == PROV_KEY_PAGEUP || (k.kind == PROV_KEY_CHAR && k.cp == 'j')) {
            s->panel_help_scroll = s->panel_help_scroll > 5 ? s->panel_help_scroll - 5 : 0;
            return;
        }
        if (k.kind == PROV_KEY_PAGEDOWN || (k.kind == PROV_KEY_CHAR && k.cp == 'l')) { s->panel_help_scroll += 5; return; }
        if (k.kind == PROV_KEY_CHAR && k.nbytes == 1) {
            char c = (char)k.bytes[0];
            if (c == 'q' || c == 'x' || c == 'h' || c == ' ') s->panel_help = false;   /* exit the help */
            else if (c == 'w') s->panel.pos = (prov_panel_pos_t)((s->panel.pos + 1) % 5);  /* reposition */
        }
        return;
    }
    if (s->panel_verb) {                              /* awaiting the slot key for a verb */
        int verb = s->panel_verb;
        s->panel_verb = 0;
        if (k.kind == PROV_KEY_CHAR && k.nbytes == 1) {
            int slot = reg_index(k.bytes[0]);
            if (slot >= 0) panel_verb_apply(s, verb, slot);
        }
        return;                                       /* Esc / any other key cancels */
    }
    if (s->panel_filter) {                            /* ss text-filter sub-mode */
        if (k.kind == PROV_KEY_ENTER) s->panel_filter = false;
        else if (k.kind == PROV_KEY_ESC) { s->panel_filter = false; prov_panel_filter_clear(&s->panel); }
        else if (k.kind == PROV_KEY_BACKSPACE) prov_panel_filter_pop(&s->panel);
        else if (k.kind == PROV_KEY_CHAR && k.cp >= 0x20)
            for (proven_size_t i = 0; i < k.nbytes; i++) prov_panel_filter_push(&s->panel, (char)k.bytes[i]);
        return;
    }
    if (s->panel_kind == PANEL_K_BROWSER && k.kind == PROV_KEY_TAB) {
        int f = s->browse.focus;                          /* Tab forward / Shift+Tab reverse */
        int step = k.shift ? (BF_COUNT - 1) : 1;          /* +(N-1) ≡ -1 (mod N) */
        do { f = (f + step) % BF_COUNT; } while (f == BF_PREVIEW && !s->browse.preview);
        s->browse.focus = f;
        return;
    }
    if (s->panel_kind == PANEL_K_BROWSER && s->browse.focus == BF_PATH) {
        /* editable path/name field — a full line editor (RFC-0015): arrows/Home/End
         * (Shift extends), Ctrl+C/X/V via the OS clipboard, control bytes rejected. */
        prov_lineedit_t *le = &s->browse.pathedit;
        if (k.kind == PROV_KEY_ESC)   { s->browse.focus = BF_LIST; prov_le_clear(le); return; }
        if (k.kind == PROV_KEY_ENTER) { browser_path_submit(s); return; }
        if (k.kind == PROV_KEY_UP)    { prov_le_history(le, &s->browse.pathhist, true);  return; }
        if (k.kind == PROV_KEY_DOWN)  { prov_le_history(le, &s->browse.pathhist, false); return; }
        le_handle_key(le, k);
        return;                                           /* other keys inert while editing */
    }
    if (s->panel_kind == PANEL_K_BROWSER && s->browse.focus >= BF_ENC
        && (k.kind == PROV_KEY_ENTER || (k.kind == PROV_KEY_CHAR && k.cp == ' '))) {
        switch (s->browse.focus) {                        /* Enter/Space activates the Tab-focused option */
            case BF_ENC:     browse_opt_enc(s);     break;
            case BF_BACKEND: browse_opt_backend(s); break;
            case BF_BOM:     browse_opt_bom(s);     break;
            case BF_RO:      browse_opt_ro(s);      break;
        }
        return;
    }
    if (s->panel_kind == PANEL_K_BROWSER && s->browse.subscreen != BSUB_NONE) {
        /* encoding / backend chooser sub-screen: i/k move, Enter picks, Esc cancels */
        bool enc = s->browse.subscreen == BSUB_ENC;
        const char *bl[BR_BACKEND_MAX]; int bn = br_backends(bl);
        int n = enc ? (int)(sizeof ENC_CYCLE / sizeof ENC_CYCLE[0]) : bn;
        if (k.kind == PROV_KEY_ESC) { s->browse.subscreen = BSUB_NONE; return; }
        if (k.kind == PROV_KEY_UP || (k.kind == PROV_KEY_CHAR && k.cp == 'i')) {
            if (s->browse.sub_sel > 0) s->browse.sub_sel--;
            return;
        }
        if (k.kind == PROV_KEY_DOWN || (k.kind == PROV_KEY_CHAR && k.cp == 'k')) {
            if (s->browse.sub_sel < n - 1) s->browse.sub_sel++;
            return;
        }
        if (k.kind == PROV_KEY_ENTER) {
            if (enc) {
                const char *e = ENC_CYCLE[s->browse.sub_sel];
                prov_cstr_set(s->open_enc, sizeof s->open_enc, prov_cstr_view(e));
                if (e[0] && !prov_charset_supports(e))
                    FMT_INTO(s->message, "open as: {} (unsupported by backend)", PROVEN_ARG(prov_cstr_view(e)));
                else
                    FMT_INTO(s->message, "open as: {}", PROVEN_ARG(prov_cstr_view(e[0] ? e : "UTF-8 (auto)")));
            } else {
                const char *bk = bl[s->browse.sub_sel];
                prov_cstr_set(s->cfg.charset_backend, sizeof s->cfg.charset_backend, prov_cstr_view(bk));
                prov_charset_configure(s->cfg.charset_backend);
                FMT_INTO(s->message, "charset backend: {}", PROVEN_ARG(prov_cstr_view(bk)));
            }
            s->browse.subscreen = BSUB_NONE;
            return;
        }
        return;                                           /* other keys inert in the chooser */
    }
    prov_pk_t r = prov_keymap_feed(&s->panel.parser, k, s->panel.keys);
    switch (r.kind) {
        case PK_MOVE:
            if (s->panel_kind == PANEL_K_BROWSER && s->browse.preview && s->browse.focus == BF_PREVIEW) {
                if (r.dir == NAV_UP)        s->browse.pv_top = s->browse.pv_top > r.count ? s->browse.pv_top - r.count : 0;
                else if (r.dir == NAV_DOWN) s->browse.pv_top += r.count;   /* clamped in draw vs row count */
                break;
            }
            if (r.dir != NAV_LEFT && r.dir != NAV_RIGHT)       /* arrows mirror ikjl now */
                prov_panel_move(&s->panel, r.dir, r.count, s->panel_page ? s->panel_page : 1);
            break;
        case PK_GOTO:
            prov_panel_goto(&s->panel, r.index);
            if (s->panel_kind == PANEL_K_WINDOWS || s->panel_kind == PANEL_K_TABS)
                panel_activate(s);                  /* Ng focuses window / switches tab directly */
            break;
        case PK_SEARCH: s->panel_filter = true; break;
        case PK_VERB:
            if (s->panel_kind == PANEL_K_BROWSER) {
                switch (r.action) {                        /* browser verbs are immediate (no slot) */
                    case PV_OPTS:   browser_cycle_cols(s); break;   /* C: cycle info columns */
                    case PV_PARENT: browser_parent(s);     break;   /* I */
                    case PV_ENTER:  browser_enter(s);      break;   /* K */
                    case PV_BACK:   browser_history(s, -1); break;  /* J */
                    case PV_FWD:    browser_history(s, +1); break;  /* L */
                    case PV_SORT:                                   /* t: cycle field × direction (asc→desc→next field) */
                        if (!s->browse.sort_desc) s->browse.sort_desc = true;
                        else { s->browse.sort_desc = false; s->browse.sort = (s->browse.sort + 1) % 3; }
                        prov_panel_refilter(&s->panel); s->panel.sel = 0; s->panel_scroll = 0;
                        FMT_INTO(s->message, "sort: {} {}", PROVEN_ARG(prov_cstr_view(
                            s->browse.sort == BSORT_NAME ? "name" : s->browse.sort == BSORT_MTIME ? "date" : "extension")),
                            PROVEN_ARG(prov_cstr_view(s->browse.sort_desc ? "\xe2\x86\x93 desc" : "\xe2\x86\x91 asc")));
                        break;
                    case PV_PFEDIT:                                   /* f: edit the type filter */
                        prov_le_set(&s->browse.pfedit, s->browse.postfix);
                        s->browse.pf_edit = true; break;
                    case PV_ENC:     browse_opt_enc(s);     break;     /* e: encoding sub-screen */
                    case PV_BACKEND: browse_opt_backend(s); break;     /* m: charset-backend sub-screen */
                    case PV_BOM:     browse_opt_bom(s);     break;     /* b: toggle save-BOM */
                    case PV_RO:      browse_opt_ro(s);      break;     /* R: toggle read-only open */
                    case PV_BINARY:  browse_opt_binary(s);  break;     /* x: toggle binary/hex open */
                    case PV_EOL:     browse_opt_eol(s);     break;     /* r: cycle read-EOL Auto/LF/CRLF/CR */
                    case PV_PATH:    s->browse.focus = BF_PATH; break; /* p: jump to the path field */
                    case PV_PREVIEW:                                   /* v: toggle the preview pane */
                                    s->browse.preview = !s->browse.preview;
                                    if (!s->browse.preview && s->browse.focus == BF_PREVIEW) s->browse.focus = BF_LIST;
                                    FMT_INTO(s->message, "preview {}", PROVEN_ARG(prov_cstr_view(s->browse.preview ? "on (Tab=focus)" : "off"))); break;
                    default: break;
                }
            } else if (s->panel_kind == PANEL_K_TABS) {    /* tab verbs: immediate, act on the selected tab */
                int t = tabs_sel_tab(s);
                if (t < 0) break;
                switch (r.action) {
                    case TV_UP:   { int d = t > 0 ? t - 1 : 0;
                                    tab_move(s, t, d); s->tab_fold = 0; rebuild_tabs_rows(s); tabs_select_tab(s, d); break; }
                    case TV_DOWN: { int d = t + 1 < s->tab_count ? t + 1 : s->tab_count - 1;
                                    tab_move(s, t, d); s->tab_fold = 0; rebuild_tabs_rows(s); tabs_select_tab(s, d); break; }
                    case TV_TOP:    tab_move(s, t, 0); s->tab_fold = 0; rebuild_tabs_rows(s); tabs_select_tab(s, 0); break;
                    case TV_BOTTOM: { int d = s->tab_count - 1;
                                      tab_move(s, t, d); s->tab_fold = 0; rebuild_tabs_rows(s); tabs_select_tab(s, d); break; }
                    case TV_CLOSE:
                        tab_close_at(s, t);
                        if (s->orphan_buf >= 0 || s->quit_wizard || !s->running) panel_close(s);  /* yield to the save/quit prompt */
                        else { s->tab_fold = 0; rebuild_tabs_rows(s); }
                        break;
                    case TV_FOLD:
                        if (prov_layout_leaf_count(&s->tabs[t]) > 1) s->tab_fold ^= (1u << t);
                        rebuild_tabs_rows(s); tabs_select_tab(s, t);
                        break;
                    case TV_NEW:
                        tab_new(s);                         /* opens a new empty tab, now active */
                        s->tab_fold = 0; rebuild_tabs_rows(s); tabs_select_tab(s, s->tab);
                        break;
                    default: break;
                }
            } else if (s->panel_kind == PANEL_K_WINDOWS && r.action == WV_SWAP) {
                int sel = prov_panel_selected_id(&s->panel);   /* leaf node index */
                if (sel >= 0) {
                    if (s->win_swap < 0) s->win_swap = sel;            /* first pick: mark it */
                    else if (s->win_swap == sel) s->win_swap = -1;     /* same again: unmark */
                    else { win_swap_nodes(s, s->win_swap, sel); s->win_swap = -1; }
                    prov_panel_set_rows(&s->panel, s->panel_rowbuf, (proven_size_t)build_windows_rows(s));
                }
            } else s->panel_verb = r.action;               /* else await a slot key */
            break;
        case PK_HELP:   s->panel_help = true; s->panel_help_scroll = 0; break;
        case PK_ACTIVATE:
        case PK_OK:     panel_activate(s); break;
        case PK_DISCARD:
            if (s->panel_kind == PANEL_K_BROWSER) {   /* d: drop the per-open read options + typed path */
                s->open_enc[0] = '\0';
                s->open_bom = s->open_ro = s->open_binary = false;
                s->open_eol = 0;
                prov_le_clear(&s->browse.pathedit);
                FMT_INTO(s->message, "open options reset (encoding/BOM/EOL/hex/read-only, path)");
            }
            break;
        case PK_CYCLE:  s->panel.pos = (prov_panel_pos_t)((s->panel.pos + 1) % 5); s->panel_scroll = 0; break;
        case PK_CLOSE:
        case PK_CANCEL:
            if (s->panel_pick) {                      /* picker: cancel returns to the host */
                void (*cb)(prov_session_t *, const char *) = s->panel_pick;
                s->panel_pick = NULL;
                cb(s, NULL);
            } else panel_close(s);
            break;
        default: break;                               /* verbs: per-panel, later */
    }
}

static void run_command(prov_session_t *s, const char *cmd) {
    if (!*cmd) return;
    if (proven_u8str_view_eq(prov_cstr_view(cmd), PROVEN_LIT("w"))) {
        if (s->path && PROVEN_IS_OK(prov_save_buffer(s->a, prov_editor_buffer(s->ed), s->path, &s->bufs.entries[active_buf(s)].info))) {
            prov_editor_compact(s->ed); s->modified = false;
            maybe_reapply_config(s, s->ed, s->path);   /* zc live-apply (RFC) */
        } else if (!s->path) FMT_INTO(s->message, "no file name (use zo or 'e <path>')");
    } else if (proven_u8str_view_eq(prov_cstr_view(cmd), PROVEN_LIT("q"))) {
        arm_or_quit(s);
    } else if ((cmd[0] == 'e' || cmd[0] == 'o') && cmd[1] == ' ') {
        const char *pp = cmd + 2;
        while (*pp == ' ' || *pp == '\t') pp++;     /* skip spaces after the verb */
        open_path(s, pp);
    } else {
        FMT_INTO(s->message, "unknown command: {}", PROVEN_ARG(prov_cstr_view(cmd)));
    }
}

/* Push a committed term onto the search history (newest first, deduped, cap 32). */
static void search_hist_push(prov_session_t *s, const char *term) {
    if (!term[0]) return;
    if (s->search.hist_n > 0) {                        /* skip a repeat of the newest */
        int newest = (s->search.hist_head - 1 + 32) % 32;
        if (proven_u8str_view_eq(prov_cstr_view(s->search.hist[newest]), prov_cstr_view(term))) return;
    }
    prov_cstr_set(s->search.hist[s->search.hist_head], 256, prov_cstr_view(term));   /* ring: O(1), no shift */
    s->search.hist_head = (s->search.hist_head + 1) % 32;
    if (s->search.hist_n < 32) s->search.hist_n++;
}

/* Search `hay` for the stored term and jump. `advance` skips the current
 * position (sn/sp); the initial / incremental search does not. Wraps around. */
/* (Re)compile the regex from the current term/flags; NULL on invalid pattern
 * or when regex mode is off. Called whenever the term or its flags change. */
static void search_recompile(prov_session_t *s) {
    if (s->search.re) { prov_regex_destroy(s->a, s->search.re); s->search.re = NULL; }
    if (!s->search.regex || !s->search.valid || !s->search.term[0]) return;
    /* MULTILINE so ^/$ mean line bounds (editor-natural, and consistent with the
     * per-visible-line highlight). */
    unsigned flags = (s->search.icase ? PROV_RX_ICASE : 0) | PROV_RX_MULTILINE;
    proven_u8str_view_t pat;
    char wb[300];                                     /* \b(?:term)\b for whole-word */
    if (s->search.word) {
        const char *pre = "\\b(?:", *suf = ")\\b";
        proven_size_t w = 0;
        for (const char *p = pre; *p && w + 1 < sizeof wb; p++) wb[w++] = *p;
        for (const char *p = s->search.term; *p && w + 1 < sizeof wb; p++) wb[w++] = *p;
        for (const char *p = suf; *p && w + 1 < sizeof wb; p++) wb[w++] = *p;
        wb[w] = '\0';
        pat = (proven_u8str_view_t){ .ptr = (const proven_byte_t *)wb, .size = w };
    } else pat = prov_cstr_view(s->search.term);
    prov_result_regex_t r = prov_regex_compile(s->a, pat, flags);
    if (PROVEN_IS_OK(r.err)) s->search.re = r.re;     /* else NULL: an invalid regex */
}

/* forward: leftmost match with start >= from; wraps to 0 if none (when wrap). */
static bool rx_find_fwd(prov_regex_t *re, const proven_u8 *hay, proven_size_t len,
                        proven_size_t from, bool wrap, prov_regex_match_t *out) {
    if (from <= len && prov_regex_search(re, hay, len, from, out)) return true;
    return wrap && from > 0 && prov_regex_search(re, hay, len, 0, out);
}
/* backward: the match with the largest start <= from; wraps to the last (when wrap). */
static bool rx_find_bwd(prov_regex_t *re, const proven_u8 *hay, proven_size_t len,
                        proven_size_t from, bool wrap, prov_regex_match_t *out) {
    prov_regex_match_t m, best, last; bool have = false, any = false;
    proven_size_t pos = 0;
    while (pos <= len && prov_regex_search(re, hay, len, pos, &m)) {
        any = true; last = m;
        if (m.start <= from) { best = m; have = true; }
        pos = m.end > m.start ? m.end : m.start + 1;
    }
    if (have) { *out = best; return true; }
    if (wrap && any) { *out = last; return true; }
    return false;
}

static void search_run(prov_session_t *s, const proven_u8 *hay, proven_size_t haylen,
                       bool forward, bool advance) {
    if (!s->search.valid || !s->search.term[0]) {
        FMT_INTO(s->message, "not found: {}", PROVEN_ARG(prov_cstr_view(s->search.term)));
        return;
    }
    proven_size_t cur = prov_editor_cursor_byte(s->ed);
    proven_size_t from = forward ? (advance ? cur + 1 : cur)
                                 : (advance && cur > 0 ? cur - 1 : cur);
    s->search.hl = s->cfg.search_highlight;           /* config gates match highlighting (soh toggles) */
    bool wrap = s->cfg.search_wrapscan;

    if (s->search.regex) {                            /* regex path (RFC-0009) */
        if (!s->search.re) { FMT_INTO(s->message, "invalid regex: {}", PROVEN_ARG(prov_cstr_view(s->search.term))); return; }
        if (from > haylen) from = haylen;
        prov_regex_match_t m;
        bool found = forward ? rx_find_fwd(s->search.re, hay, haylen, from, wrap, &m)
                             : rx_find_bwd(s->search.re, hay, haylen, from, wrap, &m);
        if (found) {
            prov_editor_move_to(s->ed, m.start);
            FMT_INTO(s->message, "{}{}", PROVEN_ARG(prov_cstr_view(forward ? "/" : "?")),
                     PROVEN_ARG(prov_cstr_view(s->search.term)));
        } else FMT_INTO(s->message, "not found: {}", PROVEN_ARG(prov_cstr_view(s->search.term)));
        return;
    }

    proven_size_t nlen = proven_cstr_len(s->search.term);
    if (nlen > haylen) { FMT_INTO(s->message, "not found: {}", PROVEN_ARG(prov_cstr_view(s->search.term))); return; }
    bool found;
    proven_size_t pos = prov_search_bytes(hay, haylen, (const proven_u8 *)s->search.term,
                                          nlen, from, forward, wrap, s->search.icase, &found);
    if (found) {
        prov_editor_move_to(s->ed, pos);
        FMT_INTO(s->message, "{}{}", PROVEN_ARG(prov_cstr_view(forward ? "/" : "?")),
                 PROVEN_ARG(prov_cstr_view(s->search.term)));
    } else {
        FMT_INTO(s->message, "not found: {}", PROVEN_ARG(prov_cstr_view(s->search.term)));
    }
}

/* Materialize the document, then search (used by sn/sp/sw, outside the prompt). */
static void do_search(prov_session_t *s, bool forward, bool advance) {
    if (!s->search.valid || !s->search.term[0]) return;
    jump_push(s);                              /* record the jump-from spot for 0n */
    const prov_buffer_t *b = prov_editor_buffer(s->ed);
    proven_size_t total = prov_buffer_byte_len(b);
    proven_result_mem_mut_t rm = s->a.alloc_fn(s->a.ctx, total ? total : 1, 16);
    if (!PROVEN_IS_OK(rm.err)) return;
    proven_u8 *hay = (proven_u8 *)rm.value.ptr;
    if (total) prov_buffer_copy_range(b, 0, total, hay, total);
    search_run(s, hay, total, forward, advance);
    s->a.free_fn(s->a.ctx, hay);
}

/* Materialize the document once for the search prompt (incremental keystrokes
 * reuse it instead of re-copying the whole buffer). */
static void search_cache_begin(prov_session_t *s) {
    search_cache_end(s);
    const prov_buffer_t *b = prov_editor_buffer(s->ed);
    proven_size_t total = prov_buffer_byte_len(b);
    proven_result_mem_mut_t rm = s->a.alloc_fn(s->a.ctx, total ? total : 1, 16);
    if (!PROVEN_IS_OK(rm.err)) return;
    s->search.hay = (proven_u8 *)rm.value.ptr;
    if (total) prov_buffer_copy_range(b, 0, total, s->search.hay, total);
    s->search.haylen = total;
}
static void search_cache_end(prov_session_t *s) {
    if (s->search.hay) s->a.free_fn(s->a.ctx, s->search.hay);
    s->search.hay = NULL;
    s->search.haylen = 0;
}

/* growable byte buffer for regex replacement assembly */
typedef struct { proven_u8 *p; proven_size_t n, cap; proven_allocator_t a; bool oom; } gbuf_t;
static void gb_app(gbuf_t *g, const proven_u8 *src, proven_size_t n) {
    if (g->oom || n == 0) return;
    if (g->n + n > g->cap) {
        proven_size_t cap = g->cap ? g->cap * 2 : 256;
        while (cap < g->n + n) cap *= 2;
        proven_result_mem_mut_t m = g->p ? g->a.realloc_fn(g->a.ctx, g->p, g->cap, cap, 16)
                                         : g->a.alloc_fn(g->a.ctx, cap, 16);
        if (!PROVEN_IS_OK(m.err)) { g->oom = true; return; }
        g->p = (proven_u8 *)m.value.ptr; g->cap = cap;
    }
    for (proven_size_t i = 0; i < n; i++) g->p[g->n + i] = src[i];
    g->n += n;
}
static void gb_byte(gbuf_t *g, proven_u8 b) { gb_app(g, &b, 1); }
/* Append `n` bytes of buffer `b` starting at `off` into the growable buffer. */
static void gb_copy_buf(gbuf_t *g, const prov_buffer_t *b, proven_size_t off, proven_size_t n) {
    proven_u8 chunk[256];
    while (n && !g->oom) {
        proven_size_t k = n < sizeof chunk ? n : sizeof chunk;
        prov_buffer_copy_range(b, off, k, chunk, k);
        gb_app(g, chunk, k);
        off += k; n -= k;
    }
}

/* expand `repl` for one match: \1..\9 / \0 / & = captures, \n \t \\ escapes. */
static void rx_expand(gbuf_t *g, const char *repl, const proven_u8 *hay, const prov_regex_match_t *m) {
    for (proven_size_t i = 0; repl[i]; i++) {
        char c = repl[i];
        if (c == '\\' && repl[i + 1]) {
            char e = repl[++i];
            if (e >= '0' && e <= '9') {
                int gnum = e - '0';
                if (gnum == 0) gb_app(g, hay + m->start, m->end - m->start);
                else if (gnum <= m->ngroups && m->groups[gnum].set)
                    gb_app(g, hay + m->groups[gnum].start, m->groups[gnum].end - m->groups[gnum].start);
            } else if (e == 'n') gb_byte(g, '\n');
            else if (e == 't') gb_byte(g, '\t');
            else gb_byte(g, (proven_u8)e);            /* \\ \& \. … literal */
        } else if (c == '&') {
            gb_app(g, hay + m->start, m->end - m->start);
        } else gb_byte(g, (proven_u8)c);
    }
}

/* A `g/re/` or `v/re/` prefix on `repl` is a line guard (ed/vi/sam lineage):
 * substitute only on lines that do (`g`) / don't (`v`) match the guard regex.
 * Compiles the guard, advances `*repl` past the prefix, sets `*inv`. */
static prov_regex_t *parse_guard(prov_session_t *s, const char **repl, bool *inv) {
    const char *r = *repl;
    if ((r[0] != 'g' && r[0] != 'v') || r[1] != '/') return NULL;
    proven_size_t i = 2;
    while (r[i] && r[i] != '/') i++;
    if (r[i] != '/') return NULL;                     /* no closing '/': not a guard */
    char gp[256];
    proven_size_t gl = i - 2;
    if (gl >= sizeof gp) return NULL;
    for (proven_size_t k = 0; k < gl; k++) gp[k] = r[2 + k];
    gp[gl] = '\0';
    prov_result_regex_t gr = prov_regex_compile(s->a, prov_cstr_view(gp), s->search.icase ? PROV_RX_ICASE : 0);
    if (!PROVEN_IS_OK(gr.err)) return NULL;
    *inv = (r[0] == 'v');
    *repl = r + i + 1;                                 /* the rest is the replacement */
    return gr.re;
}

/* True if the line of hay containing byte `at` matches the guard (xor inv). */
static bool guard_ok(prov_regex_t *guard, bool inv, const proven_u8 *hay, proven_size_t total, proven_size_t at) {
    proven_size_t ls = at, le = at;
    while (ls > 0 && hay[ls - 1] != '\n') ls--;
    while (le < total && hay[le] != '\n') le++;
    prov_regex_match_t gm;
    bool m = prov_regex_search(guard, hay + ls, le - ls, 0, &gm);
    return m != inv;
}

/* sr in regex mode: replace every match, expanding captures, as one undo step.
 * The replacement may carry a `g/re/` or `v/re/` line guard prefix (S8). */
/* Replace within the byte scope [lo, hi); matches straddling or outside the scope
 * are left verbatim. The regex still sees the whole document (so ^/$/\b keep their
 * real context); only matches fully inside the scope are rewritten. */
static void do_replace_regex(prov_session_t *s, const char *repl,
                             proven_size_t lo, proven_size_t hi) {
    if (!s->search.re) { FMT_INTO(s->message, "invalid regex: {}", PROVEN_ARG(prov_cstr_view(s->search.term))); return; }
    bool inv = false;
    prov_regex_t *guard = parse_guard(s, &repl, &inv);

    const prov_buffer_t *b = prov_editor_buffer(s->ed);
    proven_size_t total = prov_buffer_byte_len(b);
    proven_result_mem_mut_t hm = s->a.alloc_fn(s->a.ctx, total ? total : 1, 16);
    if (!PROVEN_IS_OK(hm.err)) { if (guard) prov_regex_destroy(s->a, guard); return; }
    proven_u8 *hay = (proven_u8 *)hm.value.ptr;
    if (total) prov_buffer_copy_range(b, 0, total, hay, total);

    gbuf_t g = { .a = s->a };
    proven_size_t pos = 0, count = 0;
    while (pos <= total) {
        prov_regex_match_t m;
        if (!prov_regex_search(s->search.re, hay, total, pos, &m)) break;
        gb_app(&g, hay + pos, m.start - pos);          /* unmatched prefix */
        bool in_scope = (m.start >= lo && m.end <= hi);
        bool apply = in_scope && (!guard || guard_ok(guard, inv, hay, total, m.start));
        if (apply) { rx_expand(&g, repl, hay, &m); count++; }
        else gb_app(&g, hay + m.start, m.end - m.start);   /* out of scope / guarded out: verbatim */
        if (m.end > m.start) pos = m.end;
        else { if (m.start < total) gb_byte(&g, hay[m.start]); pos = m.start + 1; }
    }
    if (guard) prov_regex_destroy(s->a, guard);
    if (count == 0 || g.oom) {
        s->a.free_fn(s->a.ctx, hay);
        if (g.p) s->a.free_fn(s->a.ctx, g.p);
        FMT_INTO(s->message, count == 0 ? "no match replaced: {}" : "out of memory", PROVEN_ARG(prov_cstr_view(s->search.term)));
        return;
    }
    if (pos < total) gb_app(&g, hay + pos, total - pos);   /* trailing unmatched */
    (void)prov_editor_replace_range(s->ed, 0, total, g.p, g.n);
    prov_editor_move_to(s->ed, 0);
    s->modified = true;
    s->a.free_fn(s->a.ctx, hay);
    if (g.p) s->a.free_fn(s->a.ctx, g.p);
    FMT_INTO(s->message, "replaced {} of /{}/", PROVEN_ARG((proven_u32)count), PROVEN_ARG(prov_cstr_view(s->search.term)));
}

/* sr: replace every occurrence of the current search term with `repl` within the
 * byte scope [lo, hi), as one undo step (rebuild the document and apply a single
 * replace). lo=0/hi=total replaces the whole buffer. */
static void do_replace_scoped(prov_session_t *s, const char *repl,
                              proven_size_t lo, proven_size_t hi) {
    if (!s->search.valid || !s->search.term[0] || ro_guard(s)) return;
    if (s->search.regex) { do_replace_regex(s, repl, lo, hi); return; }
    const prov_buffer_t *b = prov_editor_buffer(s->ed);
    proven_size_t total = prov_buffer_byte_len(b);
    proven_size_t plen = proven_cstr_len(s->search.term);
    proven_size_t rlen = proven_cstr_len(repl);
    if (plen == 0 || plen > total) {
        FMT_INTO(s->message, "not found: {}", PROVEN_ARG(prov_cstr_view(s->search.term)));
        return;
    }
    proven_result_mem_mut_t hm = s->a.alloc_fn(s->a.ctx, total, 16);
    if (!PROVEN_IS_OK(hm.err)) return;
    proven_u8 *hay = (proven_u8 *)hm.value.ptr;
    prov_buffer_copy_range(b, 0, total, hay, total);
    const proven_u8 *pat = (const proven_u8 *)s->search.term;

    /* count non-overlapping matches fully inside the scope */
    proven_size_t count = 0;
    for (proven_size_t i = lo; i + plen <= hi; ) {
        if (prov_match_at(hay, pat, plen, i, s->search.icase)) { count++; i += plen; } else i++;
    }
    if (count == 0) {
        s->a.free_fn(s->a.ctx, hay);
        FMT_INTO(s->message, "not found: {}", PROVEN_ARG(prov_cstr_view(s->search.term)));
        return;
    }
    /* newlen = total - count*plen + count*rlen (checked) */
    proven_size_t add = count * rlen, sub = count * plen;
    if (rlen && add / count != rlen) { s->a.free_fn(s->a.ctx, hay); return; }   /* overflow */
    proven_size_t newlen = total - sub + add;
    proven_result_mem_mut_t om = s->a.alloc_fn(s->a.ctx, newlen ? newlen : 1, 16);
    if (!PROVEN_IS_OK(om.err)) { s->a.free_fn(s->a.ctx, hay); return; }
    proven_u8 *out = (proven_u8 *)om.value.ptr;

    proven_size_t op = 0;
    for (proven_size_t i = 0; i < total; ) {
        if (i >= lo && i + plen <= hi && prov_match_at(hay, pat, plen, i, s->search.icase)) {
            for (proven_size_t j = 0; j < rlen; j++) out[op++] = (proven_u8)repl[j];
            i += plen;
        } else out[op++] = hay[i++];
    }
    (void)prov_editor_replace_range(s->ed, 0, total, out, newlen);   /* one undo step */
    prov_editor_move_to(s->ed, 0);
    s->modified = true;
    s->a.free_fn(s->a.ctx, hay);
    s->a.free_fn(s->a.ctx, out);
    FMT_INTO(s->message, "replaced {} of {}", PROVEN_ARG((proven_u32)count),
             PROVEN_ARG(prov_cstr_view(s->search.term)));
}

/* Whole-buffer replace (the sr command / non-scoped find-panel `a`). */
static void do_replace(prov_session_t *s, const char *repl) {
    proven_size_t total = prov_buffer_byte_len(prov_editor_buffer(s->ed));
    do_replace_scoped(s, repl, 0, total);
}

/* RFC-0016: replace the single match starting at the cursor with the find panel's
 * replacement (regex captures expanded), then re-cache the document and advance to
 * the next match. No-op (just advance) if the cursor isn't on a match. One undo step. */
static void find_replace_one(prov_session_t *s) {
    if (!s->search.valid || !s->search.term[0] || ro_guard(s)) return;
    const char *repl = s->find.repl.buf;
    const prov_buffer_t *b = prov_editor_buffer(s->ed);
    proven_size_t total = prov_buffer_byte_len(b);
    proven_size_t cur = prov_editor_cursor_byte(s->ed);
    proven_size_t mstart = cur, mend = cur;          /* the match span at the cursor */
    gbuf_t g = { .a = s->a };                         /* assembled replacement bytes */
    if (s->search.regex) {
        if (!s->search.re || !s->search.hay) return;
        prov_regex_match_t m;
        if (!prov_regex_search(s->search.re, s->search.hay, s->search.haylen, cur, &m) || m.start != cur) {
            search_run(s, s->search.hay, s->search.haylen, true, true); find_count(s); return;  /* not on a match: just advance */
        }
        mstart = m.start; mend = m.end;
        rx_expand(&g, repl, s->search.hay, &m);
    } else {
        proven_size_t plen = proven_cstr_len(s->search.term);
        if (!plen || cur + plen > total ||
            !prov_match_at(s->search.hay, (const proven_u8 *)s->search.term, plen, cur, s->search.icase)) {
            search_run(s, s->search.hay, s->search.haylen, true, true); find_count(s); return;
        }
        mend = cur + plen;
        gb_app(&g, (const proven_u8 *)repl, proven_cstr_len(repl));
    }
    if (g.oom) { if (g.p) s->a.free_fn(s->a.ctx, g.p); return; }
    (void)prov_editor_replace_range(s->ed, mstart, mend, g.p, g.n);   /* one undo step */
    if (g.p) s->a.free_fn(s->a.ctx, g.p);
    s->modified = true;
    prov_editor_move_to(s->ed, mstart);              /* land where the replacement begins */
    search_cache_begin(s);                            /* document changed: refresh the hay */
    search_run(s, s->search.hay, s->search.haylen, true, true);   /* advance to the next match */
    find_count(s);
    FMT_INTO(s->message, "replaced 1 of {}", PROVEN_ARG(prov_cstr_view(s->search.term)));
}

/* sw: take the word under the cursor as the search term and jump to its next. */
static void search_word(prov_session_t *s) {
    const prov_buffer_t *b = prov_editor_buffer(s->ed);
    prov_range_t r = prov_motion_textobj(b, prov_editor_cursor_byte(s->ed), PROV_TOBJ_WORD, true);
    if (!r.ok || r.end <= r.start || r.end - r.start >= sizeof s->search.term) return;
    proven_size_t n = r.end - r.start;
    prov_buffer_copy_range(b, r.start, n, (proven_u8 *)s->search.term, n);
    s->search.term[n] = '\0';
    s->search.valid = true;
    search_recompile(s);
    prov_editor_move_to(s->ed, r.start);
    do_search(s, true, true);          /* move to the NEXT occurrence of the word */
}

/* ---- named registers (M4.2): a-z -> 0..25, 0-9 -> 26..35 ---- */
static int reg_index(int ch) {
    if (ch >= 'a' && ch <= 'z') return ch - 'a';
    if (ch >= '0' && ch <= '9') return 26 + (ch - '0');
    return -1;
}
/* Snapshot the unnamed register into named slot `idx`. */
static void reg_store(prov_session_t *s, int idx) {
    if (idx < 0 || idx >= 36) return;
    proven_size_t len = prov_editor_reg_len(s->ed);
    if (s->regs[idx].bytes) { s->a.free_fn(s->a.ctx, s->regs[idx].bytes); s->regs[idx].bytes = NULL; }
    s->regs[idx].len = 0;
    if (len) {
        proven_result_mem_mut_t rm = s->a.alloc_fn(s->a.ctx, len, 16);
        if (!PROVEN_IS_OK(rm.err)) return;
        s->regs[idx].bytes = (proven_u8 *)rm.value.ptr;
        prov_editor_reg_copy(s->ed, s->regs[idx].bytes, len);
        s->regs[idx].len = len;
    }
    s->regs[idx].shape = prov_editor_reg_shape(s->ed);
}
/* Restore named slot `idx` into the unnamed register; false if the slot is empty. */
static bool reg_load(prov_session_t *s, int idx) {
    if (idx < 0 || idx >= 36 || s->regs[idx].len == 0) return false;
    return PROVEN_IS_OK(prov_editor_reg_set(s->ed, s->regs[idx].bytes,
                                            s->regs[idx].len, s->regs[idx].shape));
}

/* ---- OS clipboard bridge (best-effort; no-op when no tool is present) ----
 * The OS clipboard mirrors the unnamed register: a user yank/cut pushes it out,
 * and a paste pulls an external change back in (keeping the internal linewise
 * shape when the clipboard is unchanged from what we last pushed). */
static bool clip_last_ensure(prov_session_t *s, proven_size_t n) {
    if (s->clip_last_cap >= n) return true;
    proven_size_t cap = s->clip_last_cap ? s->clip_last_cap : 256;
    while (cap < n) cap *= 2;
    proven_result_mem_mut_t rm = s->clip_last
        ? s->a.realloc_fn(s->a.ctx, s->clip_last, s->clip_last_cap, cap, 16)
        : s->a.alloc_fn(s->a.ctx, cap, 16);
    if (!PROVEN_IS_OK(rm.err)) return false;
    s->clip_last = (proven_u8 *)rm.value.ptr; s->clip_last_cap = cap;
    return true;
}
/* push the unnamed register to the OS clipboard (after a yank/cut). */
static void clip_push(prov_session_t *s) {
    if (!s->cfg.clipboard_sync) return;             /* [clipboard] sync = false: stay in-editor */
    proven_size_t n = prov_editor_reg_len(s->ed);
    if (n == 0 || !clip_last_ensure(s, n)) return;
    prov_editor_reg_copy(s->ed, s->clip_last, s->clip_last_cap);
    s->clip_last_len = n;
    prov_os_clip_set(s->clip_last, n);
}
/* pull an externally-changed OS clipboard into the unnamed register (before paste). */
static void clip_pull(prov_session_t *s) {
    if (!s->cfg.clipboard_sync) return;             /* [clipboard] sync = false: stay in-editor */
    enum { CLIP_CAP = 1u << 20 };               /* 1 MiB cap on a pulled clipboard */
    proven_result_mem_mut_t rm = s->a.alloc_fn(s->a.ctx, CLIP_CAP, 16);
    if (!PROVEN_IS_OK(rm.err)) return;
    proven_u8 *buf = (proven_u8 *)rm.value.ptr;
    proven_size_t n = prov_os_clip_get(buf, CLIP_CAP);
    /* Compare ignoring a trailing newline run on either side: clipboard tools
     * and managers routinely add or strip a final newline on round-trip, which
     * must NOT be mistaken for an external edit — doing so would clobber a
     * linewise yank's LINE shape with CHAR and make `p` paste mid-line instead
     * of as whole lines (the "unstable paste" symptom). */
    proven_size_t na = n;                while (na > 0 && (buf[na - 1] == '\n' || buf[na - 1] == '\r')) na--;
    proven_size_t la = s->clip_last_len; while (la > 0 && (s->clip_last[la - 1] == '\n' || s->clip_last[la - 1] == '\r')) la--;
    bool same = na == la;
    for (proven_size_t i = 0; same && i < na; i++) same = buf[i] == s->clip_last[i];
    if (n > 0 && !same) {                       /* external change: adopt it (charwise) */
        if (PROVEN_IS_OK(prov_editor_reg_set(s->ed, buf, n, PROV_REG_CHAR)) && clip_last_ensure(s, n)) {
            proven_mem_copy(s->clip_last, s->clip_last_cap, (proven_mem_view_t){ buf, n });
            s->clip_last_len = n;
        }
    }
    s->a.free_fn(s->a.ctx, buf);
}

/* Push the unnamed register onto the numbered ring 0..9 (newest in '0'); the
 * oldest ('9') falls off. So b0v..b9v walk back through recent yanks/cuts. */
static void reg_history_push(prov_session_t *s) {
    if (prov_editor_reg_len(s->ed) == 0) return;
    int first = reg_index('0'), last = reg_index('9');   /* 26 .. 35 */
    if (s->regs[last].bytes) s->a.free_fn(s->a.ctx, s->regs[last].bytes);
    for (int i = last; i > first; i--) s->regs[i] = s->regs[i - 1];
    s->regs[first].bytes = NULL; s->regs[first].len = 0; s->regs[first].shape = PROV_REG_CHAR;
    reg_store(s, first);                                 /* snapshot the unnamed reg into '0' */
}
/* The post-yank/cut sync for the unnamed register (OS clipboard + numbered ring). */
static void yank_synced(prov_session_t *s) { clip_push(s); reg_history_push(s); }

/* ---- macros (M4.6): record into a slot, replay via the feed queue ---- */
static void macro_append(prov_session_t *s, int slot, prov_key_t k) {
    if (slot < 0 || slot >= 36 || s->macro.len[slot] >= 10000) return;   /* sane cap */
    if (s->macro.len[slot] >= s->macro.cap[slot]) {
        proven_size_t cap = s->macro.cap[slot] ? s->macro.cap[slot] * 2 : 32;
        proven_result_mem_mut_t rm = s->macro.slot[slot]
            ? s->a.realloc_fn(s->a.ctx, s->macro.slot[slot], s->macro.cap[slot] * sizeof(prov_key_t), cap * sizeof(prov_key_t), 16)
            : s->a.alloc_fn(s->a.ctx, cap * sizeof(prov_key_t), 16);
        if (!PROVEN_IS_OK(rm.err)) return;
        s->macro.slot[slot] = (prov_key_t *)rm.value.ptr;
        s->macro.cap[slot] = cap;
    }
    s->macro.slot[slot][s->macro.len[slot]++] = k;
}
/* Begin recording into `slot` (a-z/0-9 → 0..35); live keys append until macro_stop. */
static void macro_start(prov_session_t *s, int slot) {
    if (slot < 0 || slot >= 36) return;
    s->macro.rec = true; s->macro.rec_slot = slot; s->macro.len[slot] = 0;
    FMT_INTO(s->message, "recording macro... (E to stop)");
}
/* Stop recording, dropping the last `trailing` keys (the stop keystroke itself). */
static void macro_stop(prov_session_t *s, proven_size_t trailing) {
    if (!s->macro.rec) return;
    s->macro.rec = false;
    int sl = s->macro.rec_slot;
    s->macro.len[sl] = s->macro.len[sl] >= trailing ? s->macro.len[sl] - trailing : 0;
    if (s->macro.len[sl]) s->macro.last = sl;          /* E can replay the just-recorded one */
    FMT_INTO(s->message, "recorded macro ({} keys)", PROVEN_ARG((proven_u32)s->macro.len[sl]));
}
static void feed_push(prov_session_t *s, const prov_key_t *keys, proven_size_t n) {
    if (n == 0 || s->feed.len + n > 200000) return;          /* runaway guard (recursive macro) */
    if (s->feed.len + n > s->feed.cap) {
        proven_size_t cap = s->feed.cap ? s->feed.cap : 256;
        while (cap < s->feed.len + n) cap *= 2;
        proven_result_mem_mut_t rm = s->feed.keys
            ? s->a.realloc_fn(s->a.ctx, s->feed.keys, s->feed.cap * sizeof(prov_key_t), cap * sizeof(prov_key_t), 16)
            : s->a.alloc_fn(s->a.ctx, cap * sizeof(prov_key_t), 16);
        if (!PROVEN_IS_OK(rm.err)) return;
        s->feed.keys = (prov_key_t *)rm.value.ptr;
        s->feed.cap = cap;
    }
    for (proven_size_t i = 0; i < n; i++) s->feed.keys[s->feed.len++] = keys[i];
}

static void handle_prompt_key(prov_session_t *s, prov_key_t k) {
    if (k.kind == PROV_KEY_ESC) {
        int kind = s->prompt_kind;
        s->prompt_kind = PROMPT_NONE;
        if (kind == PROMPT_SEARCH) {           /* cancel: restore the cursor, drop the cache */
            prov_editor_move_to(s->ed, s->search.origin);
            search_cache_end(s);
        }
        return;
    }
    if (k.kind == PROV_KEY_ENTER) {
        int kind = s->prompt_kind;
        s->prompt_kind = PROMPT_NONE;
        const char *raw = s->prompt_le.buf;                /* untrimmed: search/replace spaces matter */
        prov_lehist_push(&s->prompt_hist, raw);            /* recent-prompt history (Up/Down) */
        if (kind == PROMPT_SEARCH) {
            prov_cstr_set(s->search.term, sizeof s->search.term, prov_cstr_view(raw));
            s->search.valid = s->search.term[0] != '\0';
            if (s->search.valid) search_hist_push(s, s->search.term);
            search_recompile(s);
            prov_editor_move_to(s->ed, s->search.origin);
            if (s->search.valid) search_run(s, s->search.hay, s->search.haylen, true, false);
            search_cache_end(s);
            return;
        }
        if (kind == PROMPT_REPLACE) { do_replace(s, raw); return; }
        if (kind == PROMPT_HEXGOTO) {                                        /* hex editor: jump to a byte offset */
            proven_size_t v = 0; bool any = false;
            for (const char *q = raw; *q; q++) {
                int d = hex_digit_val((proven_u8)*q);
                if (d < 0) { if (*q == ' ' || *q == '\t' || *q == 'x' || *q == 'X') continue; break; }
                v = v * 16 + (proven_size_t)d; any = true;
            }
            if (any) {
                proven_size_t len = prov_buffer_byte_len(prov_editor_buffer(s->ed));
                prov_editor_move_to(s->ed, v < len ? v : len);
            }
            return;
        }
        char p[1024];                                      /* trim leading/trailing ws for open/cmd */
        { const char *r = raw; while (*r == ' ' || *r == '\t') r++;
          proven_size_t n = 0; for (; r[n] && n + 1 < sizeof p; n++) p[n] = r[n];
          while (n > 0 && (p[n-1]==' '||p[n-1]=='\t'||p[n-1]=='\n'||p[n-1]=='\r')) n--;
          p[n] = '\0'; }
        if (kind == PROMPT_OPEN) open_path(s, p);
        else if (kind == PROMPT_CMD) run_command(s, p);
        else if (kind == PROMPT_SAVEAS) {          /* save the orphan, then keep sweeping */
            int b = s->orphan_buf;
            if (*p && b >= 0)
                prov_save_buffer(s->a, prov_editor_buffer(s->bufs.entries[b].ed), p, &s->bufs.entries[b].info);
            if (b >= 0) { remove_buffer(s, b); resolve_close(s); }
        }
        return;
    }
    bool edited;                                            /* a full line editor + Up/Down history */
    if (k.kind == PROV_KEY_UP)        { prov_le_history(&s->prompt_le, &s->prompt_hist, true);  edited = true; }
    else if (k.kind == PROV_KEY_DOWN) { prov_le_history(&s->prompt_le, &s->prompt_hist, false); edited = true; }
    else                              { edited = le_handle_key(&s->prompt_le, k); }
    if (edited && s->prompt_kind == PROMPT_SEARCH) {        /* incremental: jump live from origin */
        prov_cstr_set(s->search.term, sizeof s->search.term, prov_cstr_view(s->prompt_le.buf));
        s->search.valid = s->search.term[0] != '\0';
        search_recompile(s);
        prov_editor_move_to(s->ed, s->search.origin);
        if (s->search.valid) search_run(s, s->search.hay, s->search.haylen, true, false);
    }
}

/* ---- field mode (RFC-0007): bounded fragment input over [origin, end) ---- */

static proven_size_t field_region_end(prov_session_t *s) {
    return prov_buffer_byte_len(prov_editor_buffer(s->ed)) - s->field_after;
}
static const prov_buffer_t *field_buf(prov_session_t *s) { return prov_editor_buffer(s->ed); }

/* Keep the cursor inside the region after a move (vertical moves land outside as
 * a byte < origin or > end, so a byte clamp covers every motion). */
static void field_clamp(prov_session_t *s) {
    proven_size_t end = field_region_end(s);
    proven_size_t cur = prov_editor_cursor_byte(s->ed);
    if (cur < s->field_origin) prov_editor_move_to(s->ed, s->field_origin);
    else if (cur > end)        prov_editor_move_to(s->ed, end);
}

/* Insert-only: a selection (always within the region) is replaced. */
static void field_insert(prov_session_t *s, const proven_u8 *b, proven_size_t n) {
    if (prov_editor_has_selection(s->ed)) prov_editor_delete_selection(s->ed);
    if (PROVEN_IS_OK(prov_editor_insert(s->ed, b, n))) s->modified = true;
}

/* Enter field mode: begin an undo scope, place the region [origin, origin+tgt].
 * tgt > 0 (c) pre-fills+selects the target; tgt == 0 (a/o) starts empty. */
static void field_begin(prov_session_t *s, proven_size_t origin,
                        proven_u32 count, proven_size_t tgt_len) {
    s->field_origin = origin;
    s->field_after  = prov_buffer_byte_len(field_buf(s)) - (origin + tgt_len);
    s->field_count  = count ? count : 1;
    s->field_tgt_len = tgt_len;
    s->overwrite = false;                       /* insert-only */
    prov_editor_undo_scope_begin(s->ed);
    if (tgt_len) {                              /* c: select the target to overtype */
        prov_editor_select_range(s->ed, origin, origin + tgt_len);
    } else {
        prov_editor_clear_selection(s->ed);
        prov_editor_move_to(s->ed, origin);
    }
    s->mode = MODE_FIELD;
}

/* Esc: collapse the whole session into one global undo step, then act:
 * a/o stamp the region xN at origin; c replaces the target with the region. */
static void field_commit(prov_session_t *s) {
    prov_editor_t *ed = s->ed;
    proven_size_t origin = s->field_origin;
    proven_size_t end = field_region_end(s);
    proven_size_t rlen = end - origin;
    proven_u32 count = s->field_tgt_len ? 1 : s->field_count;   /* c does not repeat */

    /* capture the region bytes (R) before reverting, into the head of an R*count buffer */
    proven_u8 *bufp = NULL;
    proven_size_t total = 0;
    if (rlen && count) {
        total = rlen * (proven_size_t)count;
        if (total / count == rlen) {            /* no overflow */
            proven_result_mem_mut_t rm = s->a.alloc_fn(s->a.ctx, total, 16);
            if (PROVEN_IS_OK(rm.err)) {
                bufp = (proven_u8 *)rm.value.ptr;
                prov_buffer_copy_range(field_buf(s), origin, rlen, bufp, rlen);
                for (proven_size_t i = rlen; i < total; i++) bufp[i] = bufp[i - rlen];
            }
        }
    }

    while (prov_editor_undo(ed)) { }            /* revert the scope -> pre-field state */
    prov_editor_undo_scope_end(ed);             /* restore the global undo stacks */

    if (bufp) {                                 /* one global edit = one undo step */
        if (s->field_tgt_len)                   /* c: replace target with R (count==1) */
            (void)prov_editor_replace_range(ed, origin, s->field_tgt_len, bufp, total);
        else {                                  /* a/o: insert R*count at origin */
            prov_editor_move_to(ed, origin);
            (void)prov_editor_insert(ed, bufp, total);
        }
        s->modified = true;
        s->a.free_fn(s->a.ctx, bufp);
    } else if (s->field_tgt_len && (rlen == 0)) {
        (void)prov_editor_replace_range(ed, origin, s->field_tgt_len, NULL, 0);  /* c, empty R = delete target */
        s->modified = true;
    }
    prov_editor_move_to(ed, origin);
    prov_editor_clear_selection(ed);
    prov_editor_set_extending(ed, false);
    s->mode = MODE_ZX;
    s->zx_visual = false;
}

static void handle_field_key(prov_session_t *s, prov_key_t k, proven_size_t page) {
    prov_editor_t *ed = s->ed;
    if (k.kind == PROV_KEY_ESC) { field_commit(s); return; }

    if (k.kind == PROV_KEY_CHAR) { field_insert(s, k.bytes, k.nbytes); return; }
    if (k.kind == PROV_KEY_ENTER) { field_insert(s, (const proven_u8 *)"\n", 1); return; }
    if (k.kind == PROV_KEY_TAB) {
        if (s->cfg.expandtab) {
            proven_u32 ts = s->cfg.tabstop ? s->cfg.tabstop : 4;
            proven_size_t vcol = prov_cursor_screen_pos(ed, 0, ts).col;
            proven_u32 nsp = ts - (proven_u32)(vcol % ts);
            proven_u8 sp[64];
            if (nsp > sizeof sp) nsp = sizeof sp;
            for (proven_u32 i = 0; i < nsp; i++) sp[i] = ' ';
            field_insert(s, sp, nsp);
        } else field_insert(s, (const proven_u8 *)"\t", 1);
        return;
    }
    if (k.kind == PROV_KEY_BACKSPACE) {
        if (prov_editor_has_selection(ed)) { prov_editor_delete_selection(ed); s->modified = true; }
        else if (prov_editor_cursor_byte(ed) > s->field_origin) { prov_editor_backspace(ed); s->modified = true; }
        return;
    }
    if (k.kind == PROV_KEY_DELETE) {
        if (prov_editor_has_selection(ed)) { prov_editor_delete_selection(ed); s->modified = true; }
        else if (prov_editor_cursor_byte(ed) < field_region_end(s)) { prov_editor_delete(ed); s->modified = true; }
        return;
    }
    if (k.kind == PROV_KEY_PAGEUP || k.kind == PROV_KEY_PAGEDOWN) {
        page_scroll(s, k.kind == PROV_KEY_PAGEDOWN, k.shift, page); field_clamp(s); return;
    }
    if (is_movement(k.kind)) { move_by(s, k.kind, k.shift, k.ctrl, page); field_clamp(s); return; }
    if (k.kind == PROV_KEY_CTRL) {
        switch (k.cp) {
            case 'z': prov_editor_undo(ed); field_clamp(s); break;   /* scoped undo */
            case 'y': prov_editor_redo(ed); field_clamp(s); break;   /* scoped redo */
            case 'a': prov_editor_select_range(ed, s->field_origin, field_region_end(s)); break;
            case 'c': prov_editor_copy_selection(ed); yank_synced(s); break;
            case 'x': if (prov_editor_has_selection(ed) && PROVEN_IS_OK(prov_editor_cut_selection(ed))) { s->modified = true; yank_synced(s); } break;
            case 'v':
                clip_pull(s);
                if (prov_editor_has_selection(ed)) prov_editor_delete_selection(ed);
                if (PROVEN_IS_OK(prov_editor_paste(ed))) s->modified = true;
                break;
            default: break;   /* other Ctrl keys are inert in field mode */
        }
        return;
    }
    /* PROV_KEY_INSERT and anything else: inert (field mode is insert-only) */
}

static void handle_ed_key(prov_session_t *s, prov_key_t k, proven_size_t page) {
    prov_editor_t *ed = s->ed;
    if (quit_wizard_key(s, k)) return;

    if (k.kind == PROV_KEY_CHAR) {
        bool single = (k.nbytes == 1);
        proven_u8 t0 = (proven_u8)s->cfg.trigger[0], t1 = (proven_u8)s->cfg.trigger[1];
        if (s->trig_pending && single && k.bytes[0] == t1) {   /* trigger complete -> zx mode */
            s->trig_pending = 0;
            s->mode = MODE_ZX;
            s->parser = (prov_cmd_parser_t){0};
            s->zx_pending[0] = '\0';
            s->zx_last[0] = '\0';
            s->zx_from_trigger = true;   /* an immediate Enter now types the literal "zx" instead */
            return;
        }
        if (s->trig_pending) { insert_text(s, &t0, 1); s->trig_pending = 0; }
        if (single && k.bytes[0] == t0) { s->trig_pending = t0; return; }  /* hold first char */
        insert_text(s, k.bytes, k.nbytes);
        return;
    }

    if (s->trig_pending) { proven_u8 t0 = (proven_u8)s->cfg.trigger[0]; insert_text(s, &t0, 1); s->trig_pending = 0; }

    if (k.kind == PROV_KEY_PAGEUP || k.kind == PROV_KEY_PAGEDOWN) {
        page_scroll(s, k.kind == PROV_KEY_PAGEDOWN, k.shift, page); return;
    }
    if (is_movement(k.kind)) { move_by(s, k.kind, k.shift, k.ctrl, page); return; }

    switch (k.kind) {
        case PROV_KEY_ENTER:     insert_text(s, (const proven_u8 *)"\n", 1); break;
        case PROV_KEY_TAB:
            if (s->cfg.expandtab) {           /* spaces to the next tab stop */
                proven_u32 ts = s->cfg.tabstop ? s->cfg.tabstop : 4;
                proven_size_t vcol = prov_cursor_screen_pos(ed, 0, ts).col;
                proven_u32 nsp = ts - (proven_u32)(vcol % ts);
                proven_u8 sp[64];
                if (nsp > sizeof sp) nsp = sizeof sp;
                for (proven_u32 i = 0; i < nsp; i++) sp[i] = ' ';
                insert_text(s, sp, nsp);
            } else {
                insert_text(s, (const proven_u8 *)"\t", 1);
            }
            break;
        case PROV_KEY_ESC:                          /* Esc in Ed mode switches to zx */
            if (s->block_insert) block_insert_commit(s);   /* replicate the block insert first */
            s->mode = MODE_ZX;
            s->parser = (prov_cmd_parser_t){0};
            s->zx_pending[0] = '\0';
            s->zx_last[0] = '\0';
            s->zx_visual = false;
            break;
        case PROV_KEY_BACKSPACE: if (!ro_guard(s)) { prov_editor_backspace(ed); s->modified = true; } break;
        case PROV_KEY_DELETE:    if (!ro_guard(s)) { prov_editor_delete(ed);    s->modified = true; } break;
        case PROV_KEY_INSERT:    s->overwrite = !s->overwrite; break;
        case PROV_KEY_CTRL:
            if (k.cp == 'q') arm_or_quit(s);
            else if (k.cp == 's') {
                if (s->path && PROVEN_IS_OK(prov_save_buffer(s->a, prov_editor_buffer(ed), s->path, &s->bufs.entries[active_buf(s)].info))) {
                    prov_editor_compact(ed); s->modified = false;
                    maybe_reapply_config(s, ed, s->path);   /* zc live-apply (RFC) */
                }
            } else if (k.cp == 'z') { if (!ro_guard(s)) { prov_editor_undo(ed); s->modified = true; } }
            else if (k.cp == 'y') { if (!ro_guard(s)) { prov_editor_redo(ed); s->modified = true; } }
            else if (k.cp == 'a') { prov_editor_select_all(ed); }
            else if (k.cp == 'c') { prov_editor_copy_selection(ed); yank_synced(s); }
            else if (k.cp == 'x') { if (!ro_guard(s) && PROVEN_IS_OK(prov_editor_cut_selection(ed))) { s->modified = true; yank_synced(s); } }
            else if (k.cp == 'v') { clip_pull(s); if (!ro_guard(s) && PROVEN_IS_OK(prov_editor_paste(ed))) s->modified = true; }
            else if (k.cp == 'f') {                          /* Ctrl+F: find (same prompt as zx `ss`) */
                s->search.origin = prov_editor_cursor_byte(ed);
                prompt_open(s, PROMPT_SEARCH, "search");
                search_cache_begin(s);
            }
            else if (k.cp == 'r') {                          /* Ctrl+R: replace (zx `sr`) */
                if (!s->search.valid || !s->search.term[0]) FMT_INTO(s->message, "search first (Ctrl+F)");
                else if (!ro_guard(s)) prompt_open(s, PROMPT_REPLACE, "replace");
            }
            break;
        default: break;   /* ESC, NONE */
    }
}

/* ---- visual-block (V): a rectangular [r0,r1]x[c0,c1) region; yank/cut into a
 * BLOCK register, paste inserts each row's segment column-wise (RFC: SPEC §11). */
/* Compute the visual-block rectangle from the anchor + cursor corners: rows
 * [r0,r1] and visual columns [v0,v1). The right edge spans the full glyph under
 * whichever corner is rightmost (so a wide char / tab at the edge is included). */
static void block_rect(prov_session_t *s, proven_size_t *pr0, proven_size_t *pr1,
                       proven_size_t *pv0, proven_size_t *pv1) {
    prov_editor_t *ed = s->ed;
    proven_size_t ts = s->cfg.tabstop;
    proven_size_t cl = prov_editor_cursor_line(ed), cb = prov_editor_cursor_byte(ed);
    proven_size_t vc = prov_editor_vcol_at(ed, cl, cb, ts);
    proven_size_t wc = prov_editor_vwidth_at(ed, cl, cb, ts);
    proven_size_t al = s->block_anchor_line, va = s->block_anchor_col;
    proven_size_t ab = prov_editor_byte_at_vcol(ed, al, va, ts, NULL);
    proven_size_t wa = prov_editor_vwidth_at(ed, al, ab, ts);
    proven_size_t r0 = al < cl ? al : cl, r1 = al < cl ? cl : al;
    proven_size_t v0 = va < vc ? va : vc;
    proven_size_t ea = va + wa, ec = vc + wc;
    proven_size_t v1 = ea > ec ? ea : ec;
    proven_size_t lc = prov_buffer_line_count(prov_editor_buffer(ed));
    if (r1 >= lc) r1 = lc - 1;
    *pr0 = r0; *pr1 = r1; *pv0 = v0; *pv1 = v1;
}

/* For block row `r` over visual columns [v0,v1): the byte range [*a,*e) of the
 * fully-contained chars (a wide glyph / tab straddling either boundary is kept
 * OUT of it so it is not cut), and the leading/trailing blank-cell counts that
 * stand in for a straddling glyph's in-region cells in the yanked rectangle. */
static void block_row_seg(prov_editor_t *ed, const prov_buffer_t *b, proven_size_t r,
                          proven_size_t v0, proven_size_t v1, proven_size_t ts,
                          proven_size_t *pa, proven_size_t *pe,
                          proven_size_t *plead, proven_size_t *ptrail) {
    proven_size_t lc = prov_buffer_line_count(b);
    proven_size_t le = (r + 1 < lc) ? prov_buffer_line_start(b, r + 1) - 1 : prov_buffer_byte_len(b);
    proven_size_t a = prov_editor_byte_at_vcol(ed, r, v0, ts, NULL);
    proven_size_t lead = 0;
    if (a < le) {
        proven_size_t av = prov_editor_vcol_at(ed, r, a, ts);
        if (av < v0) {                          /* a glyph straddles the left edge: keep it */
            proven_size_t w = prov_editor_vwidth_at(ed, r, a, ts);
            lead = (av + w > v0) ? (av + w) - v0 : 0;
            a = prov_editor_byte_at_vcol(ed, r, av + w, ts, NULL);   /* the cut starts after it */
        }
    }
    proven_size_t e = prov_editor_byte_at_vcol(ed, r, v1, ts, NULL);
    proven_size_t trail = 0;
    if (e < le) {
        proven_size_t ev = prov_editor_vcol_at(ed, r, e, ts);
        if (ev < v1) trail = v1 - ev;           /* a glyph straddles the right edge: keep it */
    }
    if (a > e) a = e;                            /* one glyph straddled both edges: nothing contained */
    proven_size_t width = v1 - v0;
    if (lead > width) lead = width;
    if (lead + trail > width) trail = width - lead;
    *pa = a; *pe = e; *plead = lead; *ptrail = trail;
}

static void block_op(prov_session_t *s, bool yank) {
    prov_editor_t *ed = s->ed;
    if (!yank && ro_guard(s)) { s->block_visual = false; return; }
    const prov_buffer_t *b = prov_editor_buffer(ed);
    proven_size_t ts = s->cfg.tabstop;
    proven_size_t r0, r1, v0, v1;
    block_rect(s, &r0, &r1, &v0, &v1);
    proven_size_t lc = prov_buffer_line_count(b);

    /* the block register: each row's [v0,v1) slice, with spaces standing in for a
     * boundary-straddling glyph's in-region cells, joined by newlines */
    gbuf_t reg = { .a = s->a };
    for (proven_size_t r = r0; r <= r1; r++) {
        proven_size_t a, e, lead, trail;
        block_row_seg(ed, b, r, v0, v1, ts, &a, &e, &lead, &trail);
        for (proven_size_t k = 0; k < lead; k++) gb_byte(&reg, ' ');
        if (e > a) gb_copy_buf(&reg, b, a, e - a);
        for (proven_size_t k = 0; k < trail; k++) gb_byte(&reg, ' ');
        if (r < r1) gb_byte(&reg, '\n');
    }
    if (!reg.oom) prov_editor_reg_set(ed, reg.p, reg.n, PROV_REG_BLOCK);
    if (reg.p) s->a.free_fn(s->a.ctx, reg.p);

    if (!yank) {                                /* cut: rebuild the whole block span in one edit */
        proven_size_t span0 = prov_buffer_line_start(b, r0);
        proven_size_t span1 = (r1 + 1 < lc) ? prov_buffer_line_start(b, r1 + 1) - 1
                                            : prov_buffer_byte_len(b);
        gbuf_t out = { .a = s->a };
        for (proven_size_t r = r0; r <= r1; r++) {
            proven_size_t ls = prov_buffer_line_start(b, r);
            proven_size_t le = (r + 1 < lc) ? prov_buffer_line_start(b, r + 1) - 1
                                            : prov_buffer_byte_len(b);
            proven_size_t a, e, lead, trail;
            block_row_seg(ed, b, r, v0, v1, ts, &a, &e, &lead, &trail);
            gb_copy_buf(&out, b, ls, a - ls);       /* before the cut (keeps a left-straddling glyph) */
            gb_copy_buf(&out, b, e, le - e);        /* after the cut (keeps a right-straddling glyph) */
            if (r < r1) gb_byte(&out, '\n');
        }
        if (!out.oom) {
            (void)prov_editor_replace_range(ed, span0, span1 - span0, out.p, out.n);
            s->modified = true;
        }
        if (out.p) s->a.free_fn(s->a.ctx, out.p);
    }
    prov_editor_clear_selection(ed);
    prov_editor_move_to(ed, prov_editor_byte_at_vcol(ed, r0, v0, ts, NULL));
    yank_synced(s);
    s->block_visual = false;
}

/* p with a BLOCK register: insert each row's segment at the same visual column
 * on successive lines, padding short lines with spaces so the block stays a true
 * rectangle. One edit = one undo step. */
static void block_paste(prov_session_t *s) {
    prov_editor_t *ed = s->ed;
    proven_size_t n = prov_editor_reg_len(ed);
    if (n == 0) return;
    proven_result_mem_mut_t rm = s->a.alloc_fn(s->a.ctx, n, 16);
    if (!PROVEN_IS_OK(rm.err)) return;
    proven_u8 *reg = (proven_u8 *)rm.value.ptr;
    prov_editor_reg_copy(ed, reg, n);
    const prov_buffer_t *b = prov_editor_buffer(ed);
    proven_size_t ts = s->cfg.tabstop;
    proven_size_t lc = prov_buffer_line_count(b);
    proven_size_t line0 = prov_editor_cursor_line(ed);
    proven_size_t tv = prov_editor_vcol_at(ed, line0, prov_editor_cursor_byte(ed), ts) + 1;  /* after the cursor */

    proven_size_t segs = 1;                               /* register rows = newlines + 1 */
    for (proven_size_t i = 0; i < n; i++) if (reg[i] == '\n') segs++;
    proven_size_t lastrow = line0 + segs - 1;             /* only fill existing lines */
    if (lastrow >= lc) lastrow = lc - 1;

    proven_size_t span0 = prov_buffer_line_start(b, line0);
    proven_size_t span1 = (lastrow + 1 < lc) ? prov_buffer_line_start(b, lastrow + 1) - 1
                                             : prov_buffer_byte_len(b);
    gbuf_t out = { .a = s->a };
    proven_size_t segstart = 0;
    for (proven_size_t r = line0; r <= lastrow; r++) {
        proven_size_t segend = segstart;
        while (segend < n && reg[segend] != '\n') segend++;
        proven_size_t ls = prov_buffer_line_start(b, r);
        proven_size_t le = (r + 1 < lc) ? prov_buffer_line_start(b, r + 1) - 1 : prov_buffer_byte_len(b);
        proven_size_t reached = 0;
        proven_size_t ins = prov_editor_byte_at_vcol(ed, r, tv, ts, &reached);
        gb_copy_buf(&out, b, ls, ins - ls);               /* the line up to the insert column */
        if (ins == le && reached < tv)                    /* short line: pad to the column */
            for (proven_size_t k = reached; k < tv; k++) gb_byte(&out, ' ');
        gb_app(&out, reg + segstart, segend - segstart);  /* the block segment */
        gb_copy_buf(&out, b, ins, le - ins);              /* the rest of the line */
        if (r < lastrow) gb_byte(&out, '\n');
        segstart = segend < n ? segend + 1 : n;
    }
    if (!out.oom) {
        (void)prov_editor_replace_range(ed, span0, span1 - span0, out.p, out.n);
        s->modified = true;
    }
    if (out.p) s->a.free_fn(s->a.ctx, out.p);
    s->a.free_fn(s->a.ctx, reg);
    prov_editor_move_to(ed, prov_editor_byte_at_vcol(ed, line0, tv, ts, NULL));
}

/* I/A: enter Ed insert at the block's left (I) or right (A) column on the top
 * row; on Esc the typed text replicates onto every other row of the block. */
static void block_insert_begin(prov_session_t *s, bool append) {
    if (ro_guard(s)) { s->block_visual = false; return; }
    prov_editor_t *ed = s->ed;
    proven_size_t ts = s->cfg.tabstop;
    proven_size_t r0, r1, v0, v1;
    block_rect(s, &r0, &r1, &v0, &v1);
    proven_size_t col = append ? v1 : v0;             /* visual column to type at */
    s->block_ins_r0 = r0; s->block_ins_r1 = r1; s->block_ins_col = col;
    prov_editor_clear_selection(ed);
    proven_size_t reached = 0;
    proven_size_t ins = prov_editor_byte_at_vcol(ed, r0, col, ts, &reached);
    prov_editor_move_to(ed, ins);
    if (reached < col) {                              /* short top line: pad to the column */
        for (proven_size_t k = reached; k < col; k++)
            (void)prov_editor_insert(ed, (const proven_u8 *)" ", 1);
        s->modified = true;
    }
    s->block_ins_anchor = prov_editor_cursor_byte(ed);
    s->block_insert = true;
    s->block_visual = false;
    s->mode = MODE_ED; s->overwrite = false;          /* type on the top row, live */
}
/* On Esc: copy what was typed on the top row and insert it at the same visual
 * column on every other row of the block (padding short lines), as one edit. */
static void block_insert_commit(prov_session_t *s) {
    prov_editor_t *ed = s->ed;
    s->block_insert = false;
    proven_size_t cur = prov_editor_cursor_byte(ed);
    if (cur <= s->block_ins_anchor) return;           /* nothing typed */
    proven_size_t n = cur - s->block_ins_anchor;
    proven_result_mem_mut_t rm = s->a.alloc_fn(s->a.ctx, n, 16);
    if (!PROVEN_IS_OK(rm.err)) return;
    proven_u8 *typed = (proven_u8 *)rm.value.ptr;
    const prov_buffer_t *b = prov_editor_buffer(ed);
    prov_buffer_copy_range(b, s->block_ins_anchor, n, typed, n);
    proven_size_t ts = s->cfg.tabstop;
    proven_size_t lc = prov_buffer_line_count(b);
    proven_size_t r0 = s->block_ins_r0 + 1, r1 = s->block_ins_r1, col = s->block_ins_col;
    if (r0 <= r1 && r0 < lc) {                         /* replicate to the lower rows in one edit */
        if (r1 >= lc) r1 = lc - 1;
        proven_size_t span0 = prov_buffer_line_start(b, r0);
        proven_size_t span1 = (r1 + 1 < lc) ? prov_buffer_line_start(b, r1 + 1) - 1 : prov_buffer_byte_len(b);
        gbuf_t out = { .a = s->a };
        for (proven_size_t r = r0; r <= r1; r++) {
            proven_size_t ls = prov_buffer_line_start(b, r);
            proven_size_t le = (r + 1 < lc) ? prov_buffer_line_start(b, r + 1) - 1 : prov_buffer_byte_len(b);
            proven_size_t reached = 0;
            proven_size_t ins = prov_editor_byte_at_vcol(ed, r, col, ts, &reached);
            gb_copy_buf(&out, b, ls, ins - ls);
            if (ins == le && reached < col)
                for (proven_size_t k = reached; k < col; k++) gb_byte(&out, ' ');
            gb_app(&out, typed, n);
            gb_copy_buf(&out, b, ins, le - ins);
            if (r < r1) gb_byte(&out, '\n');
        }
        if (!out.oom) (void)prov_editor_replace_range(ed, span0, span1 - span0, out.p, out.n);
        if (out.p) s->a.free_fn(s->a.ctx, out.p);
    }
    s->a.free_fn(s->a.ctx, typed);
    prov_editor_move_to(ed, prov_editor_byte_at_vcol(ed, s->block_ins_r0, col, ts, NULL));
    s->modified = true;
}

static void handle_zx_key(prov_session_t *s, prov_key_t k, proven_size_t page) {
    /* Just arrived in zx mode by typing the "zx" trigger in Ed mode. If the very
     * next key is Enter (main or keypad — both decode to PROV_KEY_ENTER), the user
     * wanted the literal trigger text: return to Ed and insert "zx". Any other key
     * clears the one-shot and is handled as a normal zx command. */
    if (s->zx_from_trigger) {
        s->zx_from_trigger = false;
        if (k.kind == PROV_KEY_ENTER) {
            s->mode = MODE_ED;
            insert_text(s, (const proven_u8 *)s->cfg.trigger, proven_cstr_len(s->cfg.trigger));
            return;
        }
    }
    if (orphan_prompt_key(s, k)) return;       /* save-before-close prompt */
    if (quit_wizard_key(s, k)) return;
    if (pane_mode_key(s, k)) return;           /* ws resize submode owns the key */
    if (s->buf_select) {                       /* zb: a digit picks a buffer, n = new */
        s->buf_select = false;
        if (k.kind == PROV_KEY_CHAR && k.cp >= '1' && k.cp <= '9') {
            int idx = (int)(k.cp - '1');
            if (idx < s->bufs.count) buf_switch(s, idx);
        } else if (k.kind == PROV_KEY_CHAR && k.cp == 'n') {
            buffer_new(s);
        } else if (k.kind == PROV_KEY_CHAR && k.cp == 'q') {
            buf_close_active(s);               /* zbq: close the active buffer */
        }
        return;                                /* any other key cancels */
    }
    if (k.kind == PROV_KEY_ESC) {
        /* Cancel: drop any in-progress command and any visual selection, and
         * stay in zx mode at its idle base state (SPEC §9.2). It does NOT
         * return to Ed mode. */
        s->parser = (prov_cmd_parser_t){0};
        s->zx_pending[0] = '\0';
        s->zx_last[0] = '\0';            /* return to the idle base feedback */
        s->zx_visual = false;
        s->block_visual = false;        /* leave visual-block too */
        s->search.hl = false;           /* also dismiss search-match highlighting */
        prov_editor_set_extending(s->ed, false);
        prov_editor_clear_selection(s->ed);
        return;
    }
    if (k.kind == PROV_KEY_CTRL && k.cp == 'q') { arm_or_quit(s); return; }
    if (k.kind == PROV_KEY_CHAR && k.cp == 'V') {        /* toggle visual-block */
        s->block_visual = !s->block_visual;
        if (s->block_visual) {
            s->zx_visual = false;
            prov_editor_set_extending(s->ed, false);
            prov_editor_clear_selection(s->ed);
            s->block_anchor_line = prov_editor_cursor_line(s->ed);
            s->block_anchor_col  = prov_editor_vcol_at(s->ed, prov_editor_cursor_line(s->ed),
                                                       prov_editor_cursor_byte(s->ed), s->cfg.tabstop);
        }
        return;
    }
    if (s->block_visual && k.kind == PROV_KEY_CHAR &&
        (k.cp == 'y' || k.cp == 'd' || k.cp == 'x' || k.cp == 'c')) {
        block_op(s, k.cp == 'y');                        /* y = yank; d/x/c = cut */
        return;
    }
    if (s->block_visual && k.kind == PROV_KEY_CHAR && (k.cp == 'I' || k.cp == 'A')) {
        block_insert_begin(s, k.cp == 'A');              /* I = left edge, A = right edge */
        return;
    }
    if (k.kind == PROV_KEY_CHAR && k.cp == 'p' && prov_editor_reg_shape(s->ed) == PROV_REG_BLOCK) {
        if (!ro_guard(s)) block_paste(s);                /* block-shaped register: column-wise paste */
        return;
    }
    if (k.kind == PROV_KEY_CHAR && k.nbytes == 1) {      /* uppercase IK/JL = page / line-edge nav */
        switch (k.bytes[0]) {                            /* (lowercase ikjl stay i=up k=down j=left l=right) */
            case 'I': k.kind = PROV_KEY_PAGEUP;   break;
            case 'K': k.kind = PROV_KEY_PAGEDOWN; break;
            case 'J': k.kind = PROV_KEY_HOME;     break;
            case 'L': k.kind = PROV_KEY_END;      break;
            default: break;
        }
    }
    if (k.kind == PROV_KEY_PAGEUP || k.kind == PROV_KEY_PAGEDOWN ||
        is_movement(k.kind)) {
        /* Arrow/page keys bypass the parser, so apply any pending bare count
         * here (e.g. zx `5<Down>` moves five lines), then end the command. */
        proven_u32 rep = prov_cmd_pending_count(&s->parser);
        if (rep == 0) rep = 1;
        bool ext = k.shift || s->zx_visual;
        for (proven_u32 i = 0; i < rep; i++) {
            if (k.kind == PROV_KEY_PAGEUP || k.kind == PROV_KEY_PAGEDOWN)
                page_scroll(s, k.kind == PROV_KEY_PAGEDOWN, ext, page);
            else
                move_by(s, k.kind, ext, k.ctrl, page);
        }
        s->parser = (prov_cmd_parser_t){0};
        s->zx_pending[0] = '\0';
        return;
    }
    if (k.kind == PROV_KEY_CHAR && k.nbytes == 1 && k.bytes[0] == 'h' &&
        prov_cmd_parser_idle(&s->parser)) {
        panel_open_help(s, 0);                          /* open keyboard help (as a panel) */
        return;
    }
    if (k.kind == PROV_KEY_CHAR) {
        /* Echo the printable key into the in-progress command buffer so the
         * status bar can show what is being typed (e.g. "123g"). */
        if (k.cp >= 0x20 && k.cp < 0x7f) {
            size_t L = proven_cstr_len(s->zx_pending);
            if (L + 1 < sizeof s->zx_pending) {
                s->zx_pending[L] = (char)k.cp;
                s->zx_pending[L + 1] = '\0';
            }
        }
        prov_command_t cmd = prov_cmd_feed(&s->parser, k.cp);
        if (cmd.kind == PROV_CMD_INVALID) {
            s->zx_pending[0] = '\0';            /* not a command; drop the echo */
        } else if (cmd.kind != PROV_CMD_INCOMPLETE) {
            /* Completed: remember "keys  function" for the post-run readout. */
            char lbl[40];
            prov_cmd_label(&cmd, lbl, sizeof lbl);
            FMT_INTO(s->zx_last, "{}  {}", PROVEN_ARG(prov_cstr_view(s->zx_pending)), PROVEN_ARG(prov_cstr_view(lbl)));
            s->zx_pending[0] = '\0';
            if (cmd.kind == PROV_CMD_EDIT && cmd.edit == PROV_EDIT_REPEAT) {
                /* n: replay the last non-`n` command. [N]n replays it N times.
                 * `n` itself is never recorded, so a run of n's always replays
                 * the nearest preceding non-n command (the chain rule). */
                proven_u32 reps = cmd.count ? cmd.count : 1;
                if (s->has_repeatable)
                    for (proven_u32 i = 0; i < reps; i++)
                        zx_execute(s, s->last_repeatable, page);
            } else {
                s->last_repeatable = cmd;     /* record for a later `n` */
                s->has_repeatable = true;
                zx_execute(s, cmd, page);
            }
        }
    }
}

/* Load ~/.prov/config.toml into `cfg` (missing file = defaults, never fatal).
 * A syntax error is reported to stderr; recognized keys parsed before it stay. */
static void load_config(prov_config_t *cfg, proven_allocator_t alloc) {
    *cfg = prov_config_default();
    char path[1100];
    if (config_path(alloc, path, sizeof path, NULL, 0)) {
        FILE *f = fopen(path, "rb");
        if (f) {
            static char buf[16384];      /* a config is a few keys; 16 KB is ample */
            size_t got = fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
            buf[got] = '\0';
            prov_config_result_t r = prov_config_parse(cfg, alloc, buf, (proven_size_t)got);
            if (!r.ok)
                fprintf(stderr, "prov: config %s line %zu: %s\n",
                        path, (size_t)r.line, r.message ? r.message : "syntax error");
        }
    }
    /* the two-key trigger machine needs exactly two characters */
    if (proven_cstr_len(cfg->trigger) != 2) { cfg->trigger[0] = 'z'; cfg->trigger[1] = 'x'; cfg->trigger[2] = '\0'; }
}

/* Format a count as a plain integer with thousands separators (e.g. 12341 ->
 * "12,341"). */
/* === window status line === per-window, the bottom row of each window, drawn as
 * a box bottom border (panel style, normal video; dim when unfocused). Left to
 * right: a `─` rule that carries the status label `─ L<cur>/<lines>
 * C<col>/<linelen> <name>[*][ RO] ` (the name placeholder is `NoName`; `*` marks
 * modified, `RO` read-only), then a prepared horizontal scrollbar over the space
 * that remains (kept to at least HSCROLL_MIN cells), then a `┘` corner in the
 * rightmost cell that meets the right vertical scrollbar above it. The column and
 * line length are visual (a 2-cell wide char counts as 2), matching the screen. */
static void draw_window_status(prov_session_t *s, int bufidx, int lr, int col, int w,
                               prov_cell_t *grid, int gridcols, bool focused, bool readonly,
                               proven_size_t hleft, proven_size_t htotal) {
    prov_buf_t *e = &s->bufs.entries[bufidx];
    const prov_buffer_t *b = prov_editor_buffer(e->ed);
    bool mod = (bufidx == active_buf(s)) ? s->modified : e->modified;
    proven_u8 attr = focused ? 0 : PROV_ATTR_DIM;

    if (w <= 0) return;
    /* bottom-left: a reverse `X` mouse close-button cue (a future click closes the
     * window, saving first if modified); the box bottom border follows it. */
    grid[lr * gridcols + col] = (prov_cell_t){ .cp = 'X', .attr = (proven_u8)(attr | PROV_ATTR_REVERSE) };
    int bc = col + 1, bw = w - 1;                        /* the border starts after the X */
    if (bw <= 0) return;

    for (int c = 0; c < bw; c++)                         /* `─` rule fill (the bottom border) */
        grid[lr * gridcols + bc + c] = (prov_cell_t){ .cp = 0x2500, .attr = attr };
    grid[lr * gridcols + bc + bw - 1] = (prov_cell_t){ .cp = 0x2518, .attr = attr };  /* ┘ corner */

    int interior = bw - 1;                               /* cells left of the corner */
    if (interior <= 0) return;

    proven_size_t cline = prov_editor_cursor_line(e->ed);
    char pos[64];
    FMT_INTO(pos, "\xe2\x94\x80 L{}/{} C{}/{} ",
             PROVEN_ARG((unsigned long)(cline + 1)),
             PROVEN_ARG((unsigned long)prov_buffer_line_count(b)),
             PROVEN_ARG((unsigned long)(prov_cursor_screen_pos(e->ed, cline, s->cfg.tabstop).col + 1)),
             PROVEN_ARG((unsigned long)prov_line_visual_width(e->ed, cline, s->cfg.tabstop)));
    const char *base = e->path[0] ? prov_basename(e->path) : "NoName";
    const char *star = mod ? "*" : "";
    const char *ro = readonly ? " RO" : "";
    const char *cfgtag = is_config_path(s, e->path) ? " [config]" : "";   /* zc live-apply marker */

    /* The horizontal scrollbar always keeps at least HSCROLL_MIN cells; the label
     * (with the file name abbreviated to fit) takes whatever is left of that. */
    enum { HSCROLL_MIN = 4 };
    int label_max = interior - HSCROLL_MIN; if (label_max < 0) label_max = 0;
    int fixed = (int)prov_str_disp_width(pos) + (int)prov_str_disp_width(star)
              + (int)prov_str_disp_width(ro) + (int)prov_str_disp_width(cfgtag) + 1;   /* +1 = trailing space */
    int namebudget = label_max - fixed; if (namebudget < 0) namebudget = 0;
    char name[256];
    prov_abbreviate_filename(base, (proven_size_t)namebudget, name, sizeof name);
    char label[400];
    FMT_INTO(label, "{}{}{}{}{} ", PROVEN_ARG(prov_cstr_view(pos)), PROVEN_ARG(prov_cstr_view(name)),
             PROVEN_ARG(prov_cstr_view(star)), PROVEN_ARG(prov_cstr_view(ro)), PROVEN_ARG(prov_cstr_view(cfgtag)));
    int lw = (int)prov_str_disp_width(label); if (lw > label_max) lw = label_max;
    prov_blit_utf8_clip(grid, gridcols, lr, bc, label_max, attr, label);

    /* Horizontal scrollbar over the remaining interior. With wrap=off the caller
     * passes the real origin + line width (htotal>0) so a thumb shows; otherwise
     * it stays a plain `─` track (total = region width => no thumb). */
    int hx = bc + lw, hw = interior - lw;
    if (hw > 0) {
        proven_size_t btotal = htotal ? htotal : (proven_size_t)hw;
        proven_size_t bleft  = htotal ? hleft  : 0;
        prov_draw_hscroll(grid, gridcols, lr, hx, hw, bleft, btotal, attr);
    }
}

/* Composite the modal panel (RFC-0010) over the already-rendered editor grid:
 * FULL covers the whole text area; a half covers half (the other half keeps
 * showing the editor). Reverse title bar (title + tabs/filter) on top, reverse
 * footer legend on the bottom, a scrollbar + `+` corner on the right. */
/* The common modal panel, drawn as a box: the title sits on the top border line,
 * a `│`-track / `█`-thumb scrollbar (editing-window style, with `▲`/`▼` buttons;
 * just the `│` box border when it all fits) is the right border, the bottom
 * border carries a (currently unused) horizontal scrollbar, and the status/legend
 * row sits just inside the bottom border. The box corners are left intact. */
static void draw_panel(prov_cell_t *grid, int cols, int rows, prov_session_t *s) {
    prov_panel_t *p = &s->panel;
    int r0 = 0, c0 = 0, h = rows, w = cols;
    switch (p->pos) {
        case PANEL_FULL: break;
        case PANEL_TOP:    h = rows / 2; if (h < 5) h = rows; break;
        case PANEL_BOTTOM: { int hh = rows / 2; if (hh < 5) hh = rows; r0 = rows - hh; h = hh; } break;
        case PANEL_LEFT:   w = cols / 2; if (w < 16) w = cols; break;
        case PANEL_RIGHT:  { int ww = cols / 2; if (ww < 16) ww = cols; c0 = cols - ww; w = ww; } break;
    }
    if (h < 4 || w < 6) return;

    for (int r = 0; r < h; r++)                        /* clear the panel rect */
        for (int c = 0; c < w; c++) grid[(r0 + r) * cols + c0 + c] = (prov_cell_t){ .cp = 0x20 };

    int br = r0 + h - 1;                                /* bottom border row */
    int fr = r0 + h - 2;                               /* status/legend row (inside) */
    int ix = c0 + 1, iw = w - 2;                       /* interior left col + width */
    int ctop = r0 + 1, ch = (fr - 1) - ctop + 1;       /* content rows: ctop .. fr-1 */
    if (ch < 1) ch = 1;

    /* ---- box outline: corners + horizontal lines + left border ---- */
    grid[r0 * cols + c0]              = (prov_cell_t){ .cp = 0x250C };  /* ┌ */
    grid[r0 * cols + c0 + w - 1]      = (prov_cell_t){ .cp = 0x2510 };  /* ┐ */
    grid[br * cols + c0]              = (prov_cell_t){ .cp = 0x2514 };  /* └ */
    grid[br * cols + c0 + w - 1]      = (prov_cell_t){ .cp = 0x2518 };  /* ┘ */
    for (int c = 1; c < w - 1; c++)   grid[r0 * cols + c0 + c] = (prov_cell_t){ .cp = 0x2500 };  /* top ─ */
    for (int r = ctop; r <= fr; r++)  grid[r * cols + c0]      = (prov_cell_t){ .cp = 0x2502 };  /* left │ */
    grid[fr * cols + c0 + w - 1]      = (prov_cell_t){ .cp = 0x2502 };  /* right │ at the legend row */

    bool is_saveas = (s->panel_kind == PANEL_K_SAVEAS);
    /* keyboard-help panel: page lines come from help.c, scrolled by panel_scroll */
    bool is_help = (s->panel_kind == PANEL_K_HELP);
    const char *hpage_title = NULL; const char *hlines[96]; int hn = 0;
    if (is_help) {
        hn = prov_help_page(s->help_topic, &hpage_title, hlines, 96);
        int ms = hn > ch ? hn - ch : 0;
        if ((int)s->panel_scroll > ms) s->panel_scroll = (proven_size_t)ms;
    }

    /* ---- title on the top border: left = title + state, right = info ---- */
    const char *ttl = p->title ? p->title : "";
    char right[96];
    if (is_help) { int ms = hn > ch ? hn - ch : 0;
                   FMT_INTO(right, "{}%", PROVEN_ARG(ms > 0 ? (int)(s->panel_scroll * 100) / ms : 100)); }
    else if (s->panel_verb)   FMT_INTO(right, "{} _", PROVEN_ARG(prov_cstr_view(panel_verb_prompt(s->panel_verb))));
    else if (s->panel_help)   FMT_INTO(right, "help");
    else if (s->panel_filter) FMT_INTO(right, "find: {}_", PROVEN_ARG(prov_cstr_view(p->filter)));
    else if (s->panel_kind == PANEL_K_BROWSER && s->open_binary)
        FMT_INTO(right, "as binary/hex \xc2\xb7 {} items", PROVEN_ARG((unsigned long)p->nview));
    else if (s->panel_kind == PANEL_K_BROWSER && (s->open_enc[0] || s->open_bom || s->open_eol))
        FMT_INTO(right, "as {}{}{} \xc2\xb7 {} items",
            PROVEN_ARG(prov_cstr_view(s->open_enc[0] ? s->open_enc : "UTF-8")),
            PROVEN_ARG(prov_cstr_view(s->open_bom ? "+BOM" : "")),
            PROVEN_ARG(prov_cstr_view(s->open_eol ? (s->open_eol == 1 ? " LF" : s->open_eol == 2 ? " CRLF" : " CR") : "")),
            PROVEN_ARG((unsigned long)p->nview));
    else                      FMT_INTO(right, "{} items", PROVEN_ARG((unsigned long)p->nview));
    if (is_saveas) FMT_INTO(right, "{}", PROVEN_ARG(prov_cstr_view(s->saveas_state == SA_CONFIRM ? "confirm" : "new file")));
    char lt[160];
    if (is_help && s->help_topic != 0) FMT_INTO(lt, "\xe2\x94\x80 prov \xe2\x80\xba {} ", PROVEN_ARG(prov_cstr_view(hpage_title ? hpage_title : "")));
    else if (is_help)                  FMT_INTO(lt, "\xe2\x94\x80 prov \xe2\x94\x82 keyboard help ");
    else                               FMT_INTO(lt, "\xe2\x94\x80 {} ", PROVEN_ARG(prov_cstr_view(ttl)));
    prov_blit_utf8_clip(grid, cols, r0, c0 + 1, iw, 0, lt);
    char rt[120]; FMT_INTO(rt, " {} \xe2\x94\x80", PROVEN_ARG(prov_cstr_view(right)));   /*  info ─ */
    int rw = (int)prov_str_disp_width(rt);
    if (rw < iw - 4) prov_blit_utf8_clip(grid, cols, r0, c0 + w - 1 - rw, rw, 0, rt);

    /* ---- content: save dialog, keyboard-help page, per-panel help, or list rows ---- */
    if (s->panel_kind == PANEL_K_HEXEDIT && s->hexedit.ed) {   /* RFC-0019 P3: multi-line text edit */
        prov_editor_t *te = s->hexedit.ed;
        int hch = (fr - 1) - ctop + 1; if (hch < 1) hch = 1;
        int hcw = iw;                  if (hcw < 1) hcw = 1;
        proven_size_t cl = prov_editor_cursor_line(te);        /* keep the cursor line visible */
        proven_size_t top = s->hexedit.top;
        if (cl < top) top = cl;
        if (cl >= top + (proven_size_t)hch) top = cl - (proven_size_t)hch + 1;
        s->hexedit.top = top;
        proven_size_t need = (proven_size_t)hch * (proven_size_t)hcw;
        proven_result_mem_mut_t gm = s->a.alloc_fn(s->a.ctx, need * sizeof(prov_cell_t), 16);
        if (PROVEN_IS_OK(gm.err)) {
            prov_cell_t *tg = (prov_cell_t *)gm.value.ptr;
            prov_render_into_full(te, top, (proven_size_t)hch, (proven_size_t)hcw, s->cfg.tabstop, tg,
                                  0, 0, NULL, 0, false, NULL, NULL, false, 0, false);
            for (int r = 0; r < hch; r++)
                for (int c = 0; c < hcw; c++)
                    grid[(ctop + r) * cols + ix + c] = tg[r * hcw + c];
            prov_screen_pos_t cp = prov_cursor_wrap_pos(te, top, (proven_size_t)hcw, s->cfg.tabstop, false, 0, false);
            if (cp.row < (proven_size_t)hch && (int)cp.col < hcw) {   /* cursor block (panel is opaque) */
                prov_cell_t *cell = &grid[(ctop + (int)cp.row) * cols + ix + (int)cp.col];
                if (cell->cp == 0) cell->cp = 0x20;
                cell->attr |= PROV_ATTR_REVERSE;
            }
            s->a.free_fn(s->a.ctx, tg);
        }
    } else if (is_saveas) {
        const char *cur = s->bufs.entries[s->saveas_buf >= 0 && s->saveas_buf < s->bufs.count ? s->saveas_buf : 0].path;
        char l0[200], l1[1100], l2[200];
        FMT_INTO(l0, "Save this buffer to a file{}{}:",
                 PROVEN_ARG(prov_cstr_view(cur[0] ? " (was " : "")),
                 PROVEN_ARG(prov_cstr_view(cur[0] ? "named)" : "")));
        char pv[256]; int pcur, psa, psb;            /* line editor: scrolled slice + real cursor */
        prov_le_render(&s->prompt_le, iw > 10 ? iw - 6 : 8, pv, sizeof pv, &pcur, &psa, &psb);
        char ph[256] = "", pt[256] = ""; int pbi = pcur; { int vl = 0; while (pv[vl]) vl++; if (pbi > vl) pbi = vl; }
        for (int i = 0; i < pbi; i++) ph[i] = pv[i];
        { int j = 0; for (int i = pbi; pv[i]; i++) pt[j++] = pv[i]; }
        FMT_INTO(l1, "  {}{}{}", PROVEN_ARG(prov_cstr_view(ph)),
                 PROVEN_ARG(prov_cstr_view(s->saveas_state == SA_PATH ? "\xe2\x80\xb8" : "")),
                 PROVEN_ARG(prov_cstr_view(pt)));
        if (ch > 0) prov_blit_utf8_clip(grid, cols, ctop, ix + 1, iw - 2, 0, l0);
        if (ch > 1) prov_blit_utf8_clip(grid, cols, ctop + 2, ix + 1, iw - 2, PROV_ATTR_REVERSE, l1);
        if (s->saveas_msg[0] && ch > 3)
            prov_blit_utf8_clip(grid, cols, ctop + 4, ix + 1, iw - 2, 0, s->saveas_msg);
        if (s->saveas_state == SA_CONFIRM && ch > 4) {
            FMT_INTO(l2, "Overwrite it?  press  y  to overwrite,  n  to choose another name,  Esc to cancel.");
            prov_blit_utf8_clip(grid, cols, ctop + 5, ix + 1, iw - 2, 0, l2);
        }
    } else if (is_help) {
        for (int r = 0; r < ch; r++) {
            int li = (int)s->panel_scroll + r;
            if (li < 0 || li >= hn) continue;
            const char *ln = hlines[li];
            proven_u8 attr = 0;
            if (*ln == '\x01') { attr = PROV_ATTR_DIM; ln++; }
            prov_blit_utf8_clip(grid, cols, ctop + r, ix + 1, iw - 2, attr, ln);
        }
        prov_draw_vscroll(grid, cols, ctop, c0 + w - 1, ch, s->panel_scroll, (proven_size_t)hn, 0);
    } else if (s->panel_help) {
        const char *hl[16]; int nh = prov_panel_help_lines(s->panel_kind, hl);
        if (s->panel_help_scroll > (proven_size_t)nh) s->panel_help_scroll = 0;
        for (int r = 0; r < ch; r++) {
            int li = (int)s->panel_help_scroll + r;
            if (li >= nh) break;
            prov_blit_utf8_clip(grid, cols, ctop + r, ix + 1, iw - 2, 0, hl[li]);
        }
        prov_draw_vscroll(grid, cols, ctop, c0 + w - 1, ch, s->panel_help_scroll, (proven_size_t)nh, 0);
    } else if (s->panel_kind == PANEL_K_FIND) {
        /* find/replace dialog (RFC-0016): pattern / replace fields, options, count */
        for (int r = ctop; r <= fr - 1; r++)
            grid[r * cols + c0 + w - 1] = (prov_cell_t){ .cp = 0x2502 };
        struct { const char *label; prov_lineedit_t *le; int foc; } fld[2] = {
            { "find:    ", &s->find.pat,  FF_PAT },
            { "replace: ", &s->find.repl, FF_REPL },
        };
        for (int fi = 0; fi < 2; fi++) {
            int row = ctop + fi;
            if (row > fr - 1) break;
            int base = (int)prov_str_disp_width(fld[fi].label);
            int fieldw = iw - 1 - base; if (fieldw < 1) fieldw = 1;
            char vis[440]; int curc, sa, sb;
            prov_le_render(fld[fi].le, fieldw, vis, sizeof vis, &curc, &sa, &sb);
            char line[480];
            FMT_INTO(line, "{}{}", PROVEN_ARG(prov_cstr_view(fld[fi].label)), PROVEN_ARG(prov_cstr_view(vis)));
            prov_blit_utf8_clip(grid, cols, row, ix + 1, iw - 1, 0, line);
            int fx = ix + 1 + base;
            for (int c = 0; c < fieldw; c++) {            /* underline the editable region */
                prov_cell_t *cell = &grid[row * cols + fx + c];
                if (cell->cp == 0) cell->cp = 0x20;
                cell->attr |= PROV_ATTR_UNDERLINE;
            }
            bool focused = (s->find.focus == fld[fi].foc);
            for (int c = sa; c < sb; c++) grid[row * cols + fx + c].attr |= PROV_ATTR_REVERSE;
            if (focused && curc < fieldw) {               /* cursor block (only when this field has focus) */
                prov_cell_t *cell = &grid[row * cols + fx + curc];
                if (cell->cp == 0) cell->cp = 0x20;
                cell->attr |= PROV_ATTR_REVERSE;
            }
        }
        {   /* options row: x/w/c keyed; highlight toggled by focus (h = help) */
            int row = ctop + 2;
            struct { const char *key, *label; bool on; int foc; } opt[5] = {
                { "x", "regex",  s->search.regex,           FF_REGEX },
                { "w", "word",   s->search.word,            FF_WORD  },
                { "c", "case",   s->search.icase,           FF_CASE  },
                { "",  "hilite", s->cfg.search_highlight,   FF_HL    },
            };
            int nopt = 4;
            if (s->find.has_scope) {   /* scope toggle only when a selection was captured (foc=-1: not in Tab cycle) */
                opt[4].key = "s"; opt[4].label = "selection"; opt[4].on = s->find.scoped; opt[4].foc = -1;
                nopt = 5;
            }
            int ocol = 0;
            for (int oi = 0; oi < nopt && row <= fr - 1; oi++) {
                char seg[40];
                if (opt[oi].key[0]) FMT_INTO(seg, "{}:{}", PROVEN_ARG(prov_cstr_view(opt[oi].key)), PROVEN_ARG(prov_cstr_view(opt[oi].label)));
                else                FMT_INTO(seg, "{}", PROVEN_ARG(prov_cstr_view(opt[oi].label)));
                int segw = (int)prov_str_disp_width(seg);
                if (ocol + segw > iw - 1) break;
                bool foc = (s->find.focus == opt[oi].foc);
                proven_u8 at = foc ? PROV_ATTR_REVERSE : (opt[oi].on ? 0 : PROV_ATTR_DIM);
                prov_blit_utf8_clip(grid, cols, row, ix + 1 + ocol, iw - 1 - ocol, at, seg);
                if (opt[oi].key[0]) {                      /* emphasize the shortcut letter */
                    prov_cell_t *kc = &grid[row * cols + ix + 1 + ocol];
                    kc->attr = (proven_u8)((foc ? PROV_ATTR_REVERSE : 0) | PROV_ATTR_BOLD | PROV_ATTR_UNDERLINE);
                }
                ocol += segw + 2;
            }
        }
        {   /* status / match count */
            int row = ctop + 4;
            if (row <= fr - 1) {
                char st[120];
                if (!s->search.valid || !s->search.term[0]) FMT_INTO(st, "type a pattern \xc2\xb7 Tab moves \xc2\xb7 n/N next/prev \xc2\xb7 r/a replace");
                else if (s->search.regex && !s->search.re)  FMT_INTO(st, "invalid regex: {}", PROVEN_ARG(prov_cstr_view(s->search.term)));
                else if (s->find.matches == 0)               FMT_INTO(st, "no match");
                else if (s->find.index > 0)                  FMT_INTO(st, "match {} / {}", PROVEN_ARG((proven_u32)s->find.index), PROVEN_ARG((proven_u32)s->find.matches));
                else                                         FMT_INTO(st, "{} matches", PROVEN_ARG((proven_u32)s->find.matches));
                prov_blit_utf8_clip(grid, cols, row, ix + 1, iw - 2, PROV_ATTR_DIM, st);
            }
        }
    } else if (s->panel_kind == PANEL_K_BROWSER && s->browse.subscreen != BSUB_NONE) {
        /* encoding / backend chooser sub-screen (RFC-0015): takes over the body */
        for (int r = ctop; r <= fr - 1; r++)
            grid[r * cols + c0 + w - 1] = (prov_cell_t){ .cp = 0x2502 };
        bool enc = s->browse.subscreen == BSUB_ENC;
        prov_blit_utf8_clip(grid, cols, ctop, ix + 1, iw - 1, PROV_ATTR_DIM,
            enc ? "Select an encoding to open as  (Enter pick \xc2\xb7 Esc cancel):"
                : "Select the charset backend  (Enter pick \xc2\xb7 Esc cancel):");
        const char *bl[BR_BACKEND_MAX]; int bn = br_backends(bl);
        int n = enc ? (int)(sizeof ENC_CYCLE / sizeof ENC_CYCLE[0]) : bn;
        for (int i = 0; i < n; i++) {
            int row = ctop + 2 + i;
            if (row > fr - 1) break;
            const char *label = enc ? (ENC_CYCLE[i][0] ? ENC_CYCLE[i] : "UTF-8 (auto)") : bl[i];
            bool seld = (i == s->browse.sub_sel);
            if (seld) for (int c = 0; c < iw; c++)
                grid[row * cols + ix + c] = (prov_cell_t){ .cp = 0x20, .attr = PROV_ATTR_REVERSE };
            prov_blit_utf8_clip(grid, cols, row, ix + 2, iw - 2, seld ? PROV_ATTR_REVERSE : 0, label);
        }
    } else if (s->panel_kind == PANEL_K_BROWSER) {
        /* file-open dialog (RFC-0015): vertical stack — list / preview / path / opts */
        browser_refresh_preview(s);
        for (int r = ctop; r <= fr - 1; r++)                          /* right border (scrollbars overwrite) */
            grid[r * cols + c0 + w - 1] = (prov_cell_t){ .cp = 0x2502 };
        int opt_row = fr - 1, path_row = fr - 2, sep1_row = fr - 3;   /* fixed bottom block */
        int bodyh = sep1_row - ctop;                                  /* list (+ preview) area */
        if (bodyh < 1) bodyh = 1;
        bool pv = s->browse.preview && bodyh >= 6;
        int list_h = bodyh, pvsep = 0, pvtop = 0, pvh = 0;
        if (pv) {
            list_h = bodyh * 3 / 5; if (list_h < 2) list_h = 2;
            pvsep = ctop + list_h; pvtop = pvsep + 1; pvh = (ctop + bodyh) - pvtop;
            if (pvh < 2) { pv = false; list_h = bodyh; }
        }
        /* (1) file list */
        s->panel_page = (proven_size_t)list_h;
        if (p->sel < s->panel_scroll) s->panel_scroll = p->sel;
        if (p->sel >= s->panel_scroll + (proven_size_t)list_h) s->panel_scroll = p->sel - (proven_size_t)list_h + 1;
        if (s->panel_scroll > p->nview) s->panel_scroll = 0;
        for (int r = 0; r < list_h; r++) {
            proven_size_t vi = s->panel_scroll + (proven_size_t)r;
            if (vi >= p->nview) break;
            char rbuf[256]; p->src->row(p->src->ctx, vi, rbuf, sizeof rbuf);
            bool seld = vi == p->sel;
            if (seld) for (int c = 0; c < iw; c++)
                grid[(ctop + r) * cols + ix + c] = (prov_cell_t){ .cp = 0x20, .attr = PROV_ATTR_REVERSE };
            prov_blit_utf8_clip(grid, cols, ctop + r, ix + 1, iw - 1, seld ? PROV_ATTR_REVERSE : 0, rbuf);
            /* per-cell accent: reverse-color the middle-elision marker (U+2026) so a
             * truncated name/extension reads as "more here". XOR keeps it distinct on
             * a selected (already-reverse) row too. */
            for (int c = 1; c < iw; c++) {
                prov_cell_t *cell = &grid[(ctop + r) * cols + ix + c];
                if (cell->cp == 0x2026) cell->attr ^= PROV_ATTR_REVERSE;
            }
        }
        prov_draw_vscroll(grid, cols, ctop, c0 + w - 1, list_h, s->panel_scroll, p->nview, 0);
        /* (2) preview box (stacked below the list) */
        if (pv) {
            for (int c = 0; c < iw; c++) grid[pvsep * cols + ix + c] = (prov_cell_t){ .cp = 0x2500 };
            prov_textbox_t tb = { .bytes = s->browse.pv_buf, .len = s->browse.pv_len,
                                  .hex = s->browse.pv_hex, .flow = !s->browse.pv_hex };  /* text: char-wrap, ignore newlines */
            proven_size_t prows = prov_textbox_rows(&tb, iw - 1);
            if (s->browse.pv_top >= prows) s->browse.pv_top = prows ? prows - 1 : 0;
            for (int r = 0; r < pvh; r++) {
                char prb[512];
                prov_textbox_render(&tb, iw - 1, s->browse.pv_top + (proven_size_t)r, prb, sizeof prb);
                prov_blit_utf8_clip(grid, cols, pvtop + r, ix + 1, iw - 1, 0, prb);
            }
            prov_draw_vscroll(grid, cols, pvtop, c0 + w - 1, pvh, s->browse.pv_top, prows, 0);
        }
        /* (3) path / name input + (4) options */
        for (int c = 0; c < iw; c++) grid[sep1_row * cols + ix + c] = (prov_cell_t){ .cp = 0x2500 };
        char line[440];
        bool editing = (s->browse.focus == BF_PATH);
        if (editing) {
            const int base = 6;                            /* "path: " prefix */
            int fieldw = iw - 1 - base; if (fieldw < 1) fieldw = 1;
            char vis[440]; int curc, sa, sb;
            prov_le_render(&s->browse.pathedit, fieldw, vis, sizeof vis, &curc, &sa, &sb);
            FMT_INTO(line, "path: {}", PROVEN_ARG(prov_cstr_view(vis)));
            prov_blit_utf8_clip(grid, cols, path_row, ix + 1, iw - 1, 0, line);
            int fx = ix + 1 + base;                         /* underline the editable region */
            for (int c = 0; c < fieldw; c++) {
                prov_cell_t *cell = &grid[path_row * cols + fx + c];
                if (cell->cp == 0) cell->cp = 0x20;
                cell->attr |= PROV_ATTR_UNDERLINE;
            }
            for (int c = sa; c < sb; c++) grid[path_row * cols + fx + c].attr |= PROV_ATTR_REVERSE;  /* selection */
            if (curc < fieldw) {                            /* cursor block */
                prov_cell_t *cell = &grid[path_row * cols + fx + curc];
                if (cell->cp == 0) cell->cp = 0x20;
                cell->attr |= PROV_ATTR_REVERSE;
            }
        } else {                                          /* show the selected entry's full name */
            char nm[300] = "";
            int sid = prov_panel_selected_id(&s->panel);
            if (sid >= 0) prov_cstr_set(nm, sizeof nm, prov_cstr_view(s->browse.model.entries[sid].name));
            FMT_INTO(line, "path: {}", PROVEN_ARG(prov_cstr_view(nm)));
            prov_blit_utf8_clip(grid, cols, path_row, ix + 1, iw - 1, 0, line);
        }
        {   /* options row: per-segment, mnemonic key letter emphasized, focused one reversed */
            struct { const char *label; const char *val; int foc; } opt[] = {
                { "enc",     s->open_enc[0] ? s->open_enc : "UTF-8(auto)", BF_ENC },
                { "backend", s->cfg.charset_backend[0] ? s->cfg.charset_backend : "auto", BF_BACKEND },
                { "BOM",     s->open_bom ? "on" : "off", BF_BOM },
                { "RO",      s->open_ro ? "on" : "off", BF_RO },
            };
            int ocol = 0;
            for (int oi = 0; oi < 4; oi++) {
                char seg[80];
                FMT_INTO(seg, "{}:{}", PROVEN_ARG(prov_cstr_view(opt[oi].label)), PROVEN_ARG(prov_cstr_view(opt[oi].val)));
                int segw = (int)prov_str_disp_width(seg);
                if (ocol + segw > iw - 1) break;
                bool foc = (s->browse.focus == opt[oi].foc);
                prov_blit_utf8_clip(grid, cols, opt_row, ix + 1 + ocol, iw - 1 - ocol,
                                    foc ? PROV_ATTR_REVERSE : PROV_ATTR_DIM, seg);
                /* emphasize the shortcut letter (the label's first char = the key) */
                prov_cell_t *kc = &grid[opt_row * cols + ix + 1 + ocol];
                kc->attr = (proven_u8)((foc ? PROV_ATTR_REVERSE : 0) | PROV_ATTR_BOLD | PROV_ATTR_UNDERLINE);
                ocol += segw + 2;
            }
        }
    } else {
        s->panel_page = (proven_size_t)ch;
        if (p->sel < s->panel_scroll) s->panel_scroll = p->sel;
        if (p->sel >= s->panel_scroll + (proven_size_t)ch) s->panel_scroll = p->sel - (proven_size_t)ch + 1;
        if (s->panel_scroll > p->nview) s->panel_scroll = 0;
        for (int r = 0; r < ch; r++) {
            proven_size_t vi = s->panel_scroll + (proven_size_t)r;
            if (vi >= p->nview) break;
            char rbuf[256];
            const char *text;
            if (p->src) { p->src->row(p->src->ctx, vi, rbuf, sizeof rbuf); text = rbuf; }
            else { const char *t = p->rows[p->view[vi]].text; text = t ? t : ""; }
            bool seld = vi == p->sel;
            proven_u8 at = seld ? PROV_ATTR_REVERSE : 0;
            if (seld) for (int c = 0; c < iw; c++)
                grid[(ctop + r) * cols + ix + c] = (prov_cell_t){ .cp = 0x20, .attr = PROV_ATTR_REVERSE };
            prov_blit_utf8_clip(grid, cols, ctop + r, ix + 1, iw - 1, at, text);
        }
        prov_draw_vscroll(grid, cols, ctop, c0 + w - 1, ch, s->panel_scroll, p->nview, 0);
    }

    /* ---- bottom border: the (prepared, currently unused) horizontal scrollbar ---- */
    prov_draw_hscroll(grid, cols, br, ix, iw, 0, (proven_size_t)iw, 0);

    /* ---- status/legend row: footer buttons first, then the key legend ---- */
    char foot[400];
    if (is_saveas)
        FMT_INTO(foot, "{}", PROVEN_ARG(prov_cstr_view(s->saveas_state == SA_CONFIRM
            ? "y:Overwrite" "\xe2\x94\x82" "n:Rename" "\xe2\x94\x82" "Esc:Cancel" "\xe2\x94\x82"
            : "Enter:Save" "\xe2\x94\x82" "Esc:Cancel" "\xe2\x94\x82" "type the file path")));
    else if (is_help)
        FMT_INTO(foot, "{}",
            PROVEN_ARG(prov_cstr_view(s->help_await
                ? "press a key for its help \xe2\x80\xa6 \xe2\x94\x82 Esc cancels"
                : "h+key:help" "\xe2\x94\x82" "w:move" "\xe2\x94\x82" "ikjl:scroll" "\xe2\x94\x82"
                  "Space/Enter:overview" "\xe2\x94\x82" "q/x:close" "\xe2\x94\x82")));
    else if (s->panel_help)
        FMT_INTO(foot, "q/x:back" "\xe2\x94\x82" "w:move" "\xe2\x94\x82" "ikjl:scroll" "\xe2\x94\x82" "Esc/Enter:back" "\xe2\x94\x82");
    else if (s->panel_kind == PANEL_K_HEXEDIT)
        FMT_INTO(foot, "ed-mode edit" "\xe2\x94\x82" "Shift+arrows:select" "\xe2\x94\x82" "^A/^C/^X/^V" "\xe2\x94\x82"
                       "^Z/^Y:undo" "\xe2\x94\x82" "^S:write back" "\xe2\x94\x82" "Esc:cancel" "\xe2\x94\x82");
    else if (s->panel_kind == PANEL_K_FIND)
        FMT_INTO(foot, "n:next" "\xe2\x94\x82" "N:prev" "\xe2\x94\x82" "r:replace" "\xe2\x94\x82" "a:all" "\xe2\x94\x82"
                       "x:rgx" "\xe2\x94\x82" "w:word" "\xe2\x94\x82" "c:case" "\xe2\x94\x82" "{}" "Tab:field" "\xe2\x94\x82" "h:help" "\xe2\x94\x82" "q/Esc:close" "\xe2\x94\x82",
                 PROVEN_ARG(prov_cstr_view(s->find.has_scope ? "s:sel\xe2\x94\x82" : "")));
    else
        FMT_INTO(foot, "o:OK" "\xe2\x94\x82" "c:Cancel" "\xe2\x94\x82" "d:Discard" "\xe2\x94\x82" "{}",
                 PROVEN_ARG(prov_cstr_view(p->legend)));
    prov_blit_utf8_clip(grid, cols, fr, ix, iw, 0, foot);
}

/* Draw a per-window line-number gutter into columns [col0, col0+gw) over `ch`
 * screen rows, aligned to the wrapped layout: a logical line's number sits on its
 * first screen row (right-aligned, with the last gutter column a separator
 * space), continuation rows and past-EOF rows are blank. `absolute` shows
 * <line+1>; `relative` shows the distance from the cursor line, except the cursor
 * line itself which shows its absolute number. `content_cols` is the wrapped
 * content width (must match what was rendered) so row spans line up. Dim. */
static void draw_gutter(prov_cell_t *grid, int gridcols, int row0, int col0, int gw,
                        int ch, const prov_editor_t *ed, proven_size_t top,
                        int content_cols, prov_linenum_t mode, proven_size_t tabstop,
                        bool wrap_off, bool word_wrap) {
    if (gw <= 0 || content_cols <= 0) return;
    const prov_buffer_t *b = prov_editor_buffer(ed);
    proven_size_t lc = prov_buffer_line_count(b);
    proven_size_t curline = prov_editor_cursor_line(ed);
    proven_u8 attr = PROV_ATTR_DIM;
    int sr = 0;
    for (proven_size_t line = top; sr < ch; line++) {
        int wraprows = 1;
        bool past_eof = line >= lc;
        if (!past_eof && !wrap_off)              /* wrap=off → one screen row per line */
            wraprows = (int)prov_wrap_rows(ed, line, (proven_size_t)content_cols, tabstop, word_wrap);
        if (wraprows < 1) wraprows = 1;
        for (int wr = 0; wr < wraprows && sr < ch; wr++, sr++) {
            for (int g = 0; g < gw; g++)            /* blank the whole gutter row first */
                grid[(row0 + sr) * gridcols + col0 + g] = (prov_cell_t){ .cp = 0x20, .attr = attr };
            if (wr != 0 || past_eof) continue;      /* number only on the line's first row */
            unsigned long v = (mode == PROV_LINENUM_RELATIVE && line != curline)
                ? (unsigned long)(line > curline ? line - curline : curline - line)
                : (unsigned long)(line + 1);
            char num[24]; int nl = 0;               /* decimal digits, MSB-first */
            char rev[24]; int t = 0;
            if (v == 0) rev[t++] = '0';
            else for (; v; v /= 10) rev[t++] = (char)('0' + (int)(v % 10));
            while (t) num[nl++] = rev[--t];
            int start = (gw - 1) - nl; if (start < 0) start = 0;   /* right-align; col gw-1 = space */
            for (int g = 0; g < nl && start + g < gw; g++)
                grid[(row0 + sr) * gridcols + col0 + start + g] =
                    (prov_cell_t){ .cp = (proven_u32)(proven_u8)num[g], .attr = attr };
        }
        if (past_eof) { /* one ~ row per past-EOF line; keep stepping */ }
    }
}

/* Recursively paint the pane tree into `grid` (gridcols wide). A leaf renders
 * its buffer into the scratch grid `tmp`, blits it, then draws the window's own
 * borders: a status row along the bottom and a scrollbar down the right edge
 * (so siblings sit directly adjacent — no separate border cell). Unfocused
 * windows are dimmed; the focused leaf's cursor lands in cur_row/cur_col. */
/* Decoded-string line for the hex row at byte offset `base` (RFC-0019 P2): the
 * row's 16 bytes rendered with the interpretation charset `enc` ("" = UTF-8). For
 * UTF-8 a character that straddles the 16-byte boundary is shown whole on its
 * starting row and the next row skips its tail (exact boundaries); control /
 * invalid bytes show as '.'. For any other charset the row's bytes are decoded
 * independently via the PAL (boundary handling is best-effort — use [ ] to nudge). */
static void hex_decoded_line(prov_session_t *s, const proven_u8 *bytes, proven_size_t len,
                             long base, const char *enc, char *out, proven_size_t cap) {
    proven_size_t o = 0;
    if (cap) out[0] = '\0';
    proven_size_t lo = base < 0 ? 0 : (proven_size_t)base;
    proven_size_t hi = (proven_size_t)(base + PROV_HEX_BPR);
    if (hi > len) hi = len;
    if (lo >= hi) return;
    if (!enc || !enc[0]) {                         /* UTF-8 interpretation (exact boundaries) */
        proven_size_t p = lo;
        while (p < hi && (bytes[p] & 0xC0) == 0x80) p++;   /* skip a char continuing from the row above */
        while (p < hi) {
            prov_decode_t d = prov_utf8_decode(bytes + p, len - p);   /* full buffer: complete a straddler */
            proven_size_t cl = d.len ? d.len : 1;
            if (d.valid && d.cp >= 0x20 && d.cp != 0x7F) {
                for (proven_size_t k = 0; k < cl && o + 1 < cap; k++) out[o++] = (char)bytes[p + k];
            } else if (o + 1 < cap) out[o++] = '.';
            p += cl;
        }
    } else {                                       /* other charset: decode the row via the PAL */
        proven_size_t un = 0;
        proven_u8 *u = prov_charset_to_utf8(s->a, enc, bytes + lo, hi - lo, &un);
        if (u) {
            for (proven_size_t k = 0; k < un && o + 1 < cap; k++) {
                proven_u8 c = u[k];
                out[o++] = (c == '\n' || c == '\r' || c == '\t') ? '.' : (char)c;   /* keep one line */
            }
            s->a.free_fn(s->a.ctx, u);
        }
    }
    if (o < cap) out[o] = '\0';
}

/* Render a leaf window as a hex dump (RFC-0018/0019). Reuses the pure hexview
 * geometry over a per-frame materialization of the buffer bytes. Each logical row
 * is two screen rows: the offset/hex/ascii dump and, below it, the charset-decoded
 * string (P2). Highlights the byte under the cursor in both panes and (when
 * focused) lands the hardware cursor on the active nibble / ascii char. */
static void render_hex_pane(prov_session_t *s, prov_pane_node_t *nd, prov_rect_t a,
                            prov_cell_t *grid, int gridcols, bool focused,
                            int *cur_row, int *cur_col) {
    prov_editor_t *ed = s->bufs.entries[nd->buf].ed;
    const prov_buffer_t *b = prov_editor_buffer(ed);
    proven_size_t len = prov_buffer_byte_len(b);
    int ch = a.h >= 2 ? a.h - 1 : a.h;       /* content rows (bottom = status) */
    int cw = a.w >= 2 ? a.w - 1 : a.w;       /* content cols (right = scrollbar) */
    if (ch < 1) ch = 1;
    if (cw < 1) cw = 1;

    if (len > s->hex.cap) {                  /* grow the reused materialization buffer */
        proven_size_t nc = len;
        proven_result_mem_mut_t rm = s->hex.buf
            ? s->a.realloc_fn(s->a.ctx, s->hex.buf, s->hex.cap, nc, 16)
            : s->a.alloc_fn(s->a.ctx, nc, 16);
        if (PROVEN_IS_OK(rm.err)) { s->hex.buf = (proven_u8 *)rm.value.ptr; s->hex.cap = nc; }
    }
    if (len && len <= s->hex.cap) prov_buffer_copy_range(b, 0, len, s->hex.buf, len);
    proven_size_t haylen = (len <= s->hex.cap) ? len : 0;
    prov_hexview_t hv = { .bytes = s->hex.buf, .len = haylen, .align = nd->hex_align };

    proven_size_t total = prov_hexview_rows(haylen, nd->hex_align);
    proven_size_t off = prov_editor_cursor_byte(ed);
    proven_size_t crow; int cslot;
    prov_hexview_locate(off, nd->hex_align, &crow, &cslot);

    proven_size_t vis = (proven_size_t)((ch + 1) / 2);   /* hex lines that fit (each = 2 screen rows) */
    if (vis < 1) vis = 1;
    proven_size_t top = nd->top;             /* keep the cursor row visible */
    if (top >= total) top = total ? total - 1 : 0;   /* heal a stale top (text line vs hex row) */
    if (focused) {
        if (crow < top) top = crow;
        if (crow >= top + vis) top = crow - vis + 1;
        nd->top = top;
    }
    const char *enc = s->bufs.entries[nd->buf].info.enc_name;
    proven_size_t sello = 0, selhi = 0;          /* visual byte-selection range [lo,hi) (P3) */
    if (focused && s->hex.sel) {
        proven_size_t an = s->hex.sel_anchor;
        if (an <= off) { sello = an; selhi = off + 1; } else { sello = off; selhi = an + 1; }
    }

    for (int lr = 0; lr * 2 < ch; lr++) {
        proven_size_t row = top + (proven_size_t)lr;
        int sr = lr * 2;                              /* screen row of the hex line */
        char line[PROV_HEX_ROW_W + 1];
        bool draw = (row < total) || (focused && row == crow);
        if (draw) prov_hexview_render(&hv, row, line, sizeof line);
        else      line[0] = '\0';
        prov_blit_utf8_clip(grid, gridcols, a.row + sr, a.col, cw, focused ? 0 : PROV_ATTR_DIM, line);
        if (selhi > sello) {                         /* highlight selected bytes in both panes */
            for (int slot = 0; slot < PROV_HEX_BPR; slot++) {
                long bi = (long)row * PROV_HEX_BPR - nd->hex_align + slot;
                if (bi < (long)sello || bi >= (long)selhi) continue;
                int hc = prov_hexview_hexcol(slot), ac = prov_hexview_asciicol(slot);
                for (int k = 0; k < 2; k++)
                    if (hc + k < cw) grid[(a.row + sr) * gridcols + a.col + hc + k].attr |= PROV_ATTR_REVERSE;
                if (ac < cw) grid[(a.row + sr) * gridcols + a.col + ac].attr |= PROV_ATTR_REVERSE;
            }
        }
        if (focused && row == crow) {                /* highlight the cursor byte in both panes */
            int hc = prov_hexview_hexcol(cslot), ac = prov_hexview_asciicol(cslot);
            for (int k = 0; k < 2; k++)
                if (hc + k < cw) grid[(a.row + sr) * gridcols + a.col + hc + k].attr |= PROV_ATTR_REVERSE;
            if (ac < cw) grid[(a.row + sr) * gridcols + a.col + ac].attr |= PROV_ATTR_REVERSE;
        }
        if (sr + 1 < ch && draw) {                   /* decoded-string line under the hex row (P2) */
            char dec[PROV_HEX_ROW_W * 4 + 1];
            long dbase = (long)row * PROV_HEX_BPR - nd->hex_align;
            hex_decoded_line(s, hv.bytes, hv.len, dbase, enc, dec, sizeof dec);
            char ind[PROV_HEX_ROW_W * 4 + 16];
            FMT_INTO(ind, "          {}", PROVEN_ARG(prov_cstr_view(dec)));   /* indent under the hex bytes */
            prov_blit_utf8_clip(grid, gridcols, a.row + sr + 1, a.col, cw, PROV_ATTR_DIM, ind);
        }
    }

    if (focused && crow >= top && crow < top + vis) {
        int cr = (int)(crow - top) * 2;              /* hex line of the logical row */
        int base = s->hex.ascii ? prov_hexview_asciicol(cslot)
                                : prov_hexview_hexcol(cslot) + (s->hex.pend ? 1 : 0);
        if (base >= cw) base = cw - 1;
        *cur_row = a.row + cr;
        *cur_col = a.col + base;
    }

    if (a.w >= 2)
        prov_draw_vscroll(grid, gridcols, a.row, a.col + a.w - 1, ch, top, total,
                          focused ? 0 : PROV_ATTR_DIM);
    if (a.h >= 2)
        draw_window_status(s, nd->buf, a.row + a.h - 1, a.col, a.w, grid, gridcols,
                           focused, nd->readonly, 0, 0);
}

static void render_panes(prov_session_t *s, int node, prov_rect_t a,
                         prov_cell_t *grid, int gridcols,
                         prov_cell_t *tmp, int tmpcap,
                         int *cur_row, int *cur_col) {
    prov_pane_node_t *nd = &cur_layout(s)->nodes[node];
    if (a.h <= 0 || a.w <= 0) return;

    if (nd->kind == PROV_PANE_LEAF) {
        bool focused = (node == cur_layout(s)->focus);
        if (buf_is_binary(s, nd->buf)) { render_hex_pane(s, nd, a, grid, gridcols, focused, cur_row, cur_col); return; }
        int ch = a.h >= 2 ? a.h - 1 : a.h;     /* content rows (bottom = status) */
        int cw = a.w >= 2 ? a.w - 1 : a.w;     /* content cols (right = scrollbar) */
        prov_editor_t *ed = s->bufs.entries[nd->buf].ed;
        /* optional line-number gutter on the left; the wrapped content shrinks to fit it */
        int gw = (s->cfg.line_numbers != PROV_LINENUM_OFF)
                 ? prov_gutter_width(prov_buffer_line_count(prov_editor_buffer(ed))) : 0;
        if (gw > cw - 1) gw = 0;               /* not enough room: drop the gutter, keep content */
        int cwc = cw - gw;                     /* content width (cols right of the gutter) */
        bool wrap_off = (s->cfg.wrap == PROV_WRAP_OFF);          /* horizontal scroll vs soft-wrap */
        bool word_wrap = (s->cfg.wrap == PROV_WRAP_WORD);        /* word vs char soft-wrap */
        proven_size_t leftcol = wrap_off ? nd->leftcol : 0;
        if (ch > 0 && cwc > 0 && (proven_size_t)ch * (proven_size_t)cwc <= (proven_size_t)tmpcap) {
            bool fld = focused && s->mode == MODE_FIELD && ed == s->ed;
            bool hl = s->search.hl && s->search.term[0];
            prov_block_sel_t blk, *blkp = NULL;       /* V visual-block highlight on the focused pane */
            if (focused && s->block_visual && ed == s->ed) {
                proven_size_t br0, br1, bv0, bv1;
                block_rect(s, &br0, &br1, &bv0, &bv1);     /* visual-column rectangle */
                blk.r0 = br0; blk.r1 = br1; blk.c0 = bv0; blk.c1 = bv1;
                blkp = &blk;
            }
            prov_render_into_full(ed, nd->top, (proven_size_t)ch, (proven_size_t)cwc,
                                  s->cfg.tabstop, tmp,
                                  fld ? s->field_origin : 0,
                                  fld ? field_region_end(s) : 0,
                                  (hl && !s->search.regex) ? (const proven_u8 *)s->search.term : NULL,
                                  (hl && !s->search.regex) ? proven_cstr_len(s->search.term) : 0,
                                  s->search.icase,
                                  (hl && s->search.regex) ? s->search.re : NULL, blkp,
                                  wrap_off, leftcol, word_wrap);
            for (int r = 0; r < ch; r++)
                for (int c = 0; c < cwc; c++) {
                    prov_cell_t cell = tmp[r * cwc + c];
                    if (!focused) cell.attr |= PROV_ATTR_DIM;
                    grid[(a.row + r) * gridcols + a.col + gw + c] = cell;
                }
            if (gw > 0)                          /* line-number gutter, aligned to the rendered rows */
                draw_gutter(grid, gridcols, a.row, a.col, gw, ch, ed, nd->top,
                            cwc, s->cfg.line_numbers, s->cfg.tabstop, wrap_off, word_wrap);
            if (focused) {
                prov_screen_pos_t cp = prov_cursor_wrap_pos(ed, nd->top, (proven_size_t)cwc,
                                                            s->cfg.tabstop, wrap_off, leftcol, word_wrap);
                int cr = (int)cp.row; if (cr >= ch) cr = ch - 1;
                *cur_row = a.row + cr;
                int cc = (int)cp.col; if (cc >= cwc) cc = cwc - 1;
                *cur_col = a.col + gw + cc;
                /* field cue: an empty region has no cells to underline, so mark the
                 * insertion-point cell itself — the bounded field stays visible. */
                if (fld && field_region_end(s) == s->field_origin)
                    grid[(*cur_row) * gridcols + *cur_col].attr |= PROV_ATTR_UNDERLINE;
            }
        }
        if (a.w >= 2)                          /* right edge = panel-style scrollbar (`│` when it fits) */
            prov_draw_vscroll(grid, gridcols, a.row, a.col + a.w - 1, ch, nd->top,
                           prov_buffer_line_count(prov_editor_buffer(ed)),
                           focused ? 0 : PROV_ATTR_DIM);
        if (a.h >= 2) {                        /* bottom edge = status line (+ h-scroll thumb when wrap=off) */
            proven_size_t htotal = wrap_off
                ? prov_line_visual_width(ed, prov_editor_cursor_line(ed), s->cfg.tabstop) : 0;
            draw_window_status(s, nd->buf, a.row + a.h - 1, a.col, a.w, grid, gridcols,
                               focused, nd->readonly, leftcol, htotal);
        }
        return;
    }

    if (nd->kind == PROV_PANE_HSPLIT) {        /* stacked, adjacent (no border) */
        int h0 = prov_layout_split_span(a.h, nd->ratio);
        prov_rect_t top = { a.row, a.col, h0, a.w };
        prov_rect_t bot = { a.row + h0, a.col, a.h - h0, a.w };
        render_panes(s, nd->child0, top, grid, gridcols, tmp, tmpcap, cur_row, cur_col);
        render_panes(s, nd->child1, bot, grid, gridcols, tmp, tmpcap, cur_row, cur_col);
    } else {                                   /* side by side, adjacent */
        int w0 = prov_layout_split_span(a.w, nd->ratio);
        prov_rect_t left = { a.row, a.col, a.h, w0 };
        prov_rect_t right = { a.row, a.col + w0, a.h, a.w - w0 };
        render_panes(s, nd->child0, left, grid, gridcols, tmp, tmpcap, cur_row, cur_col);
        render_panes(s, nd->child1, right, grid, gridcols, tmp, tmpcap, cur_row, cur_col);
    }
}

/* === global status line === second row from the bottom, document-wide,
 * single-space separated: X, mode, code page, encoding, BOM, EOL, then the
 * cursor's byte and char position (current/total). Built into the session's
 * reused scratch string (allocation-free in steady state); the returned pointer
 * is valid until the next frame clears the scratch. */
static const char *build_status_line(prov_session_t *s) {
    /* Two-letter mode code: Ei Ed-insert, Eo Ed-overwrite, Zx zx command,
     * Zv zx visual, Zb zx visual-block, Fi field, Hx/Ha hex (hex/ascii pane). */
    const char *modestr;
    if (buf_is_binary(s, active_buf(s)))
        modestr = s->hex.ascii ? "Ha" : "Hx";
    else if (s->mode == MODE_ZX)
        modestr = s->block_visual ? "Zb" : s->zx_visual ? "Zv" : "Zx";
    else if (s->mode == MODE_FIELD)
        modestr = "Fi";
    else
        modestr = s->overwrite ? "Eo" : "Ei";

    prov_fileinfo_t fi = s->bufs.entries[active_buf(s)].info;
    char cc[8] = "";
    if (fi.country) FMT_INTO(cc, "({})", PROVEN_ARG(prov_cstr_view(fi.country)));
    pstr_clear(&s->scratch);
    if (s->panel_open) {
        (void)proven_u8str_append_fmt_grow(s->a, &s->scratch, "X PANEL {}",
            PROVEN_ARG(prov_cstr_view(s->panel.title ? s->panel.title : "")));
    } else {
        const prov_buffer_t *sb = prov_editor_buffer(s->ed);
        char cb[32], tb[32], chc[32], tc[32];
        prov_fmt_count(cb,  sizeof cb,  prov_editor_cursor_byte(s->ed));
        prov_fmt_count(tb,  sizeof tb,  prov_buffer_byte_len(sb));
        prov_fmt_count(chc, sizeof chc, prov_cursor_char_offset(s->ed));
        prov_fmt_count(tc,  sizeof tc,  prov_buffer_char_count(sb));
        (void)proven_u8str_append_fmt_grow(s->a, &s->scratch,
            "X {} {} {}{} {} {} {}/{}b {}/{}c Tab:{}",
            PROVEN_ARG(prov_cstr_view(modestr)),
            PROVEN_ARG((unsigned)fi.codepage),
            PROVEN_ARG(prov_cstr_view(fi.enc_name[0] ? fi.enc_name : fi.encoding)),
            PROVEN_ARG(prov_cstr_view(cc)),
            PROVEN_ARG(prov_cstr_view(fi.bom ? "BOM" : "noBOM")),
            PROVEN_ARG(prov_cstr_view(prov_eol_name(fi.eol))),
            PROVEN_ARG(prov_cstr_view(cb)), PROVEN_ARG(prov_cstr_view(tb)),
            PROVEN_ARG(prov_cstr_view(chc)), PROVEN_ARG(prov_cstr_view(tc)),
            PROVEN_ARG(s->tab_count));   /* tab count only; 0t lists/manages them */
    }
    return proven_u8str_as_cstr(&s->scratch);
}

/* === command line === the very bottom row: input prompts and contextual key
 * legends. Written into the caller's `cmdline` buffer (CL = cap-aware FMT_INTO,
 * since a pointer parameter loses array sizeof). */
static void build_cmdline(prov_session_t *s, char *cmdline, proven_size_t cap) {
    #define CL(...) do { proven_u8str_t cl_ = proven_u8str_borrow((proven_byte_t *)cmdline, cap); \
                         (void)proven_u8str_append_fmt_trunc(&cl_, __VA_ARGS__); } while (0)
    #define BAR "\xe2\x94\x82"   /* packed separator; kept as its own literal so a following hex-digit key (a–f) is not absorbed into the \x82 escape */
    if (s->panel_open) {                    /* panel: filter box or key legend */
        if (s->panel_kind == PANEL_K_HELP)
            CL("  key:page" BAR "ik/\xe2\x86\x91\xe2\x86\x93:scroll" BAR "Space:keymap" BAR "h:move" BAR "Enter:close");
        else if (s->panel_verb)
            CL("  {} press a slot key a-z / 0-9  (Esc cancel)",
               PROVEN_ARG(prov_cstr_view(panel_verb_prompt(s->panel_verb))));
        else if (s->panel_help)
            CL("  ik/\xe2\x86\x91\xe2\x86\x93:scroll" BAR "h/Esc:back");
        else if (s->panel_kind == PANEL_K_HEXEDIT)   /* RFC-0019 P3: ed-mode text editor */
            CL("  edit text" BAR "type/Enter:multi-line" BAR "Shift+arrows:select" BAR "^A/^C/^X/^V" BAR "^Z/^Y:undo" BAR "^S:write back" BAR "Esc:cancel");
        else if (s->panel_filter)
            CL("  find: {}_   (Enter keep · Esc clear)", PROVEN_ARG(prov_cstr_view(s->panel.filter)));
        else if (s->panel_kind == PANEL_K_FIND) {
            const char *f = s->find.focus == FF_PAT ? "pattern" : s->find.focus == FF_REPL ? "replace"
                          : s->find.focus == FF_REGEX ? "regex" : s->find.focus == FF_WORD ? "word"
                          : s->find.focus == FF_CASE ? "case" : "highlight";
            CL("  find[{}]" BAR "Tab:field" BAR "n/N:next/prev" BAR "r:replace" BAR "a:all" BAR "x/w/c:opts" BAR "h:help" BAR "q/Esc:close",
               PROVEN_ARG(prov_cstr_view(f)));
        }
        else if (s->panel_kind == PANEL_K_BROWSER && s->browse.pf_edit) {
            char vis[80]; int curc, sa, sb;            /* line-edit: scrolled slice + real cursor */
            prov_le_render(&s->browse.pfedit, 56, vis, sizeof vis, &curc, &sa, &sb);
            char head[80] = "", tail[80] = "";          /* split at the cursor (extensions are ASCII) */
            int bi = curc; { int vl = 0; while (vis[vl]) vl++; if (bi > vl) bi = vl; }
            for (int i = 0; i < bi; i++) head[i] = vis[i];
            { int j = 0; for (int i = bi; vis[i]; i++) tail[j++] = vis[i]; }
            CL("  types: {}\xe2\x80\xb8{}   (Enter done \xe2\x94\x82 ; separates suffixes like .txt)",
               PROVEN_ARG(prov_cstr_view(head)), PROVEN_ARG(prov_cstr_view(tail)));
        } else if (s->panel_kind == PANEL_K_BROWSER) {
            const char *srt = s->browse.sort == BSORT_NAME ? "name" : s->browse.sort == BSORT_MTIME ? "date" : "ext";
            CL("  ik/jl:move" BAR "o:open" BAR "K:enter" BAR "I:up" BAR "J/L:back/fwd" BAR
               "t:sort:{}" BAR "f:types" BAR "e:enc" BAR "n:eol:{}" BAR "x:hex{}" BAR "p:path" BAR "v:preview" BAR "h:help",
               PROVEN_ARG(prov_cstr_view(srt)), PROVEN_ARG(prov_cstr_view(eol_choice_name(s->open_eol))),
               PROVEN_ARG(prov_cstr_view(s->open_binary ? "*" : "")));
        } else if (s->panel_kind == PANEL_K_TABS) {
            CL("  ik:move" BAR "[N]g:switch" BAR "I/K:reorder" BAR "J/L:top/bottom" BAR "f:fold" BAR "n:new" BAR "x:close" BAR "Esc");
        } else if (s->panel_kind == PANEL_K_WINDOWS) {
            CL("  ik:move" BAR "[N]g:focus" BAR "S:{}" BAR "Esc",
               PROVEN_ARG(prov_cstr_view(s->win_swap >= 0 ? "swap with this one" : "mark to swap")));
        } else if (s->panel_kind == PANEL_K_SAVEAS) {
            if (s->saveas_state == SA_CONFIRM) CL("  file exists — y overwrite · n rename · Esc cancel");
            else                               CL("  type the file path, then Enter to save (Esc cancels)");
        }
        else
            CL("  ik/\xe2\x86\x91\xe2\x86\x93:select" BAR "[N]g:go" BAR "ss:find" BAR "Space/Enter:pick" BAR "h:help" BAR "w:pos" BAR "Esc:close");
    } else if (s->pane_mode) {              /* ws resize submode legend */
        CL("  resize" BAR "ik:height" BAR "jl:width" BAR "c:close" BAR "q:done");
    } else if (s->prompt_kind != PROMPT_NONE) {    /* text input prompt — line editor (cursor shown) */
        char pv[160]; int pcur, psa, psb;
        prov_le_render(&s->prompt_le, 60, pv, sizeof pv, &pcur, &psa, &psb);
        char ph[160] = "", pt[160] = ""; int pbi = pcur; { int vl = 0; while (pv[vl]) vl++; if (pbi > vl) pbi = vl; }
        for (int i = 0; i < pbi; i++) ph[i] = pv[i];
        { int j = 0; for (int i = pbi; pv[i]; i++) pt[j++] = pv[i]; }
        CL("  {}: {}\xe2\x80\xb8{}", PROVEN_ARG(prov_cstr_view(s->prompt_label)),
           PROVEN_ARG(prov_cstr_view(ph)), PROVEN_ARG(prov_cstr_view(pt)));
    } else if (s->orphan_buf >= 0) {        /* save-before-close prompt */
        const char *nm = s->bufs.entries[s->orphan_buf].path[0]
                       ? s->bufs.entries[s->orphan_buf].path : "[No Name]";
        const char *base = prov_basename(nm);
        CL("  {} has unsaved changes — save? (y {} · n discard)",
           PROVEN_ARG(prov_cstr_view(base)),
           PROVEN_ARG(prov_cstr_view(s->bufs.entries[s->orphan_buf].path[0] ? "save" : "save-as")));
    } else if (s->hex_pending != HEX_PEND_NONE) {   /* save-before-reload prompt (text<->hex) */
        CL("  unsaved changes before {} — s save · d discard · c cancel",
           PROVEN_ARG(prov_cstr_view(s->hex_pending == HEX_PEND_TOHEX ? "reading as hex" : "leaving hex")));
    } else if (s->quit_wizard) {            /* zq with dirty buffers */
        CL("  unsaved {}— w write-all  d discard & quit  c cancel{}",
           PROVEN_ARG(prov_cstr_view(s->message[0] ? "" : "changes ")),
           PROVEN_ARG(prov_cstr_view(s->message)));
    } else if (s->buf_select) {             /* zb: buffer list, digit to switch */
        proven_u8str_t cl = proven_u8str_borrow((proven_byte_t *)cmdline, cap);
        (void)proven_u8str_append_fmt_trunc(&cl, "  buffers:");
        for (int i = 0; i < s->bufs.count; i++) {
            const char *nm = s->bufs.entries[i].path[0] ? s->bufs.entries[i].path : "[No Name]";
            bool mod = (i == s->bufs.active) ? s->modified : s->bufs.entries[i].modified;
            (void)proven_u8str_append_fmt_trunc(&cl, "  {}{} {}{}",
                PROVEN_ARG(prov_cstr_view(i == s->bufs.active ? ">" : "")), PROVEN_ARG(i + 1),
                PROVEN_ARG(prov_cstr_view(prov_basename(nm))), PROVEN_ARG(prov_cstr_view(mod ? "*" : "")));
        }
        (void)proven_u8str_append_fmt_trunc(&cl, "   — digit switch · n new");
    } else if (s->message[0]) {             /* transient message */
        CL("  {}", PROVEN_ARG(prov_cstr_view(s->message)));
    } else if (buf_is_binary(s, active_buf(s))) {   /* RFC-0019 hex editor legend */
        CL("  HEX:{}{}" BAR "Tab:pane" BAR "ikjl/IK:PgU/D/JL:Home/End" BAR "0-9a-f:byte" BAR "v:sel" BAR
           "r:str" BAR "y/p:copy/paste" BAR "o/Ins:ins" BAR "x:del" BAR "[ ]:nudge" BAR "^G:goto" BAR "^S:save+exit" BAR "^Q/Esc:exit" BAR "h:help",
           PROVEN_ARG(prov_cstr_view(s->hex.ascii ? "ascii" : "hex")),
           PROVEN_ARG(prov_cstr_view(s->hex.sel ? " SEL" : "")));
    } else if (s->mode == MODE_ZX) {
        char hint[120];
        if (s->zx_pending[0]) {
            prov_cmd_describe(&s->parser, hint, sizeof hint);
            CL("  {}   {}", PROVEN_ARG(prov_cstr_view(s->zx_pending)),
               PROVEN_ARG(prov_cstr_view(hint)));
        } else if (s->zx_last[0]) {
            CL("  {}", PROVEN_ARG(prov_cstr_view(s->zx_last)));
        } else {
            prov_cmd_describe(&s->parser, hint, sizeof hint);  /* idle legend */
            CL("  {}", PROVEN_ARG(prov_cstr_view(hint)));
        }
    } else if (s->mode == MODE_FIELD) {     /* field mode: bounded input region */
        CL("  field x{}" BAR "type the fragment (region underlined)" BAR
           "move/select/edit" BAR "^X^C^V:clip" BAR "^Z/^Y:undo" BAR "Esc:commits",
           PROVEN_ARG(s->field_count));
    } else {                               /* Ed-mode key legend (room permitting) */
        CL("  type:insert" BAR "Shift+arrows:select" BAR
           "^A:all" BAR "^C:copy" BAR "^X:cut" BAR "^V:paste" BAR "^Z/^Y:undo/redo" BAR "Ins:ins/ovr" BAR
           "^S:save" BAR "Esc/zx:cmds" BAR "^Q:quit");
    }
    #undef CL
    #undef BAR
}

/* The leaf pane (window) whose rect contains screen cell (row,col), or -1 if the
 * point is outside the text area (e.g. on the global status / command lines). The
 * pane's rect is written to *out_rect when found (and out_rect is non-NULL). */
static int layout_leaf_at(prov_session_t *s, int row, int col, prov_rect_t *out_rect) {
    if (row < 0 || col < 0 || row >= s->area_h || col >= s->area_w) return -1;
    prov_layout_t *L = cur_layout(s);
    prov_rect_t area = { 0, 0, s->area_h, s->area_w };
    prov_rect_t rects[PROV_MAX_PANE_NODES];
    prov_layout_rects(L, area, rects);
    int leaves[PROV_MAX_PANE_NODES];
    int n = prov_layout_leaves(L, leaves, PROV_MAX_PANE_NODES);
    for (int i = 0; i < n; i++) {
        prov_rect_t r = rects[leaves[i]];
        if (row >= r.row && row < r.row + r.h && col >= r.col && col < r.col + r.w) {
            if (out_rect) *out_rect = r;
            return leaves[i];
        }
    }
    return -1;
}

/* Map a screen cell inside pane `nd` (rect `r`) to a buffer byte offset, inverting
 * the render geometry (viewport top, gutter, soft-wrap or horizontal scroll). Sets
 * *inside to whether the point lands in the content area (vs. status/scrollbar). */
static proven_size_t pane_click_byte(prov_session_t *s, prov_pane_node_t *nd,
                                     prov_rect_t r, int mrow, int mcol, bool *inside) {
    prov_editor_t *ed = s->bufs.entries[nd->buf].ed;
    const prov_buffer_t *b = prov_editor_buffer(ed);
    proven_size_t lc = prov_buffer_line_count(b);
    int ch = r.h >= 2 ? r.h - 1 : r.h;             /* content rows (bottom = status) */
    int cw = r.w >= 2 ? r.w - 1 : r.w;             /* content cols (right = scrollbar) */
    int gw = (s->cfg.line_numbers != PROV_LINENUM_OFF) ? prov_gutter_width(lc) : 0;
    if (gw > cw - 1) gw = 0;
    int cwc = cw - gw; if (cwc < 1) cwc = 1;
    int crow = mrow - r.row;
    int ccol = mcol - (r.col + gw);
    if (inside) *inside = (crow >= 0 && crow < ch && ccol >= 0 && ccol < cwc);
    if (crow < 0) crow = 0;
    if (ccol < 0) ccol = 0;
    bool wrap_off = (s->cfg.wrap == PROV_WRAP_OFF);
    bool word_wrap = (s->cfg.wrap == PROV_WRAP_WORD);
    if (wrap_off) {
        proven_size_t line = nd->top + (proven_size_t)crow;
        if (line >= lc) line = lc ? lc - 1 : 0;
        return prov_editor_byte_at_vcol(ed, line, nd->leftcol + (proven_size_t)ccol,
                                        s->cfg.tabstop, NULL);
    }
    proven_size_t line = nd->top, acc = 0;          /* soft-wrap: find the line owning crow */
    while (line < lc) {
        proven_size_t wr = prov_wrap_rows(ed, line, (proven_size_t)cwc, s->cfg.tabstop, word_wrap);
        if (acc + wr > (proven_size_t)crow) break;
        acc += wr; line++;
    }
    if (line >= lc) line = lc ? lc - 1 : 0;
    proven_size_t wrow = (proven_size_t)crow - acc;
    return prov_editor_byte_at_vcol(ed, line, wrow * (proven_size_t)cwc + (proven_size_t)ccol,
                                    s->cfg.tabstop, NULL);
}

/* Map a click/drag on a pane's vertical scrollbar (row `mrow`) to a viewport top,
 * proportional to the track position. The focused pane also moves its cursor to
 * the new top line so the cursor-follow logic doesn't snap it back. */
static void mouse_scroll_to(prov_session_t *s, int hit, prov_rect_t r, int mrow) {
    prov_layout_t *L = cur_layout(s);
    prov_pane_node_t *nd = &L->nodes[hit];
    const prov_buffer_t *b = prov_editor_buffer(s->bufs.entries[nd->buf].ed);
    proven_size_t maxt = prov_buffer_line_count(b);
    maxt = maxt ? maxt - 1 : 0;
    int ch = r.h >= 2 ? r.h - 1 : r.h;
    int t = mrow - r.row; if (t < 0) t = 0; if (ch > 1 && t > ch - 1) t = ch - 1;
    proven_size_t newtop = (ch > 1) ? (proven_size_t)((long long)t * (long long)maxt / (ch - 1)) : 0;
    if (newtop > maxt) newtop = maxt;
    nd->top = newtop;
    if (hit == L->focus) {
        s->top = newtop;
        prov_editor_move_to(s->ed, prov_buffer_line_start(b, newtop));
    }
}

enum { WHEEL_LINES = 3 };
enum { MDRAG_NONE = 0, MDRAG_SELECT, MDRAG_SCROLL };

/* The modal panel's screen rect, mirroring draw_panel's PANEL_* geometry. */
static void panel_geom(prov_session_t *s, int *pr0, int *pc0, int *ph, int *pw) {
    int r0 = 0, c0 = 0, h = s->area_h, w = s->area_w;
    switch (s->panel.pos) {
        case PANEL_TOP:    h = s->area_h / 2; if (h < 5) h = s->area_h; break;
        case PANEL_BOTTOM: { int hh = s->area_h / 2; if (hh < 5) hh = s->area_h; r0 = s->area_h - hh; h = hh; } break;
        case PANEL_LEFT:   w = s->area_w / 2; if (w < 16) w = s->area_w; break;
        case PANEL_RIGHT:  { int ww = s->area_w / 2; if (ww < 16) ww = s->area_w; c0 = s->area_w - ww; w = ww; } break;
        default: break;
    }
    *pr0 = r0; *pc0 = c0; *ph = h; *pw = w;
}

/* RFC-0014 Phase 3d: mouse over an open modal panel — wheel scrolls the list (or
 * the browser preview when it has focus), a left click selects the row under the
 * pointer, and clicking the already-selected row activates it. Sub-modes that own
 * input (save-as, help, verb/filter/postfix entry) take only the wheel. */
static void handle_panel_mouse(prov_session_t *s, prov_key_t k) {
    int r0, c0, h, w; panel_geom(s, &r0, &c0, &h, &w);
    int ctop = r0 + 1, fr = r0 + h - 2, ch = (fr - 1) - ctop + 1; if (ch < 1) ch = 1;
    bool list_panel = !(s->panel_kind == PANEL_K_SAVEAS || s->panel_kind == PANEL_K_HELP
                        || s->panel_kind == PANEL_K_FIND)
                      && !s->panel_verb && !s->panel_filter && !s->browse.pf_edit;

    if (k.mbtn == PROV_MB_WHEEL_UP || k.mbtn == PROV_MB_WHEEL_DOWN) {
        bool down = (k.mbtn == PROV_MB_WHEEL_DOWN);
        if (s->panel_kind == PANEL_K_BROWSER && s->browse.preview && s->browse.focus == BF_PREVIEW) {
            if (down) s->browse.pv_top += WHEEL_LINES;
            else      s->browse.pv_top = s->browse.pv_top > WHEEL_LINES ? s->browse.pv_top - WHEEL_LINES : 0;
        } else if (s->panel_kind == PANEL_K_HELP) {
            s->panel_scroll = down ? s->panel_scroll + WHEEL_LINES
                                   : (s->panel_scroll > WHEEL_LINES ? s->panel_scroll - WHEEL_LINES : 0);
        } else if (list_panel) {
            prov_panel_move(&s->panel, down ? NAV_DOWN : NAV_UP, WHEEL_LINES, (proven_size_t)ch);
        }
        return;
    }
    if (k.mbtn == PROV_MB_LEFT && k.mact == PROV_ME_PRESS && list_panel) {
        int last = fr - 1;
        if (s->panel_kind == PANEL_K_BROWSER) {
            if (s->browse.subscreen != BSUB_NONE) return;   /* chooser sub-screen: clicks inert */
            int list_h = (fr - 3) - ctop;                   /* the list occupies only the top section */
            if (s->browse.preview && list_h >= 6) { int lh = list_h * 3 / 5; if (lh < 2) lh = 2; list_h = lh; }
            if (list_h < 1) list_h = 1;
            last = ctop + list_h - 1;
        }
        if (k.mrow < ctop || k.mrow > last || k.mcol < c0 || k.mcol >= c0 + w) return;
        proven_size_t vi = s->panel_scroll + (proven_size_t)(k.mrow - ctop);
        if (vi >= s->panel.nview) return;
        if (vi == s->panel.sel) panel_activate(s);          /* click the selected row -> open */
        else s->panel.sel = vi;                             /* else select it (sel is 0-based) */
    }
}

/* RFC-0014 Phase 3a/3b/3c: wheel scrolls the window under the pointer; a left press
 * focuses the clicked window and positions the cursor (or hits the close-`X` /
 * scrollbar); a left drag extends the selection or drag-scrolls. Field mode and
 * text prompts ignore mouse; an open panel routes to handle_panel_mouse. */
static void handle_mouse(prov_session_t *s, prov_key_t k) {
    /* Modal states own all input — don't let a click/scroll slip past them. */
    if (s->prompt_kind != PROMPT_NONE || s->mode == MODE_FIELD ||
        s->orphan_buf >= 0 || s->quit_wizard || s->pane_mode) return;
    if (s->panel_open) { handle_panel_mouse(s, k); return; }
    prov_layout_t *L = cur_layout(s);

    if (k.mbtn == PROV_MB_WHEEL_UP || k.mbtn == PROV_MB_WHEEL_DOWN) {
        bool down = (k.mbtn == PROV_MB_WHEEL_DOWN);
        int hit = layout_leaf_at(s, k.mrow, k.mcol, NULL);
        if (hit < 0) return;
        if (hit == L->focus) {
            page_scroll(s, down, false, WHEEL_LINES);   /* focused: viewport + cursor */
        } else {                                        /* peek-scroll another pane */
            prov_pane_node_t *nd = &L->nodes[hit];
            proven_size_t lc = prov_buffer_line_count(prov_editor_buffer(s->bufs.entries[nd->buf].ed));
            proven_size_t maxt = lc ? lc - 1 : 0;
            if (down) nd->top = nd->top + WHEEL_LINES > maxt ? maxt : nd->top + WHEEL_LINES;
            else      nd->top = nd->top > WHEEL_LINES ? nd->top - WHEEL_LINES : 0;
        }
        return;
    }

    if (k.mbtn == PROV_MB_LEFT && k.mact == PROV_ME_RELEASE) { s->mouse_drag = MDRAG_NONE; return; }

    if (k.mbtn == PROV_MB_LEFT && k.mact == PROV_ME_PRESS) {
        prov_rect_t r;
        int hit = layout_leaf_at(s, k.mrow, k.mcol, &r);
        if (hit < 0) { s->mouse_drag = MDRAG_NONE; return; }
        int xrow = r.row + r.h - 1, sx = r.col + r.w - 1;         /* status row, scrollbar col */
        if (r.h >= 2 && k.mrow == xrow && k.mcol == r.col) {      /* close-`X` on the status line */
            if (hit != L->focus) { buf_save_active(s); L->focus = hit; buf_load_active(s); }
            pane_close(s); s->mouse_drag = MDRAG_NONE; return;
        }
        if (r.w >= 2 && k.mcol == sx && k.mrow < xrow) {          /* vertical scrollbar */
            if (hit != L->focus) { buf_save_active(s); L->focus = hit; buf_load_active(s); buf_reset_idle(s); }
            mouse_scroll_to(s, hit, r, k.mrow);
            s->mouse_drag = MDRAG_SCROLL; return;
        }
        /* content: focus + position the cursor */
        if (hit != L->focus) { buf_save_active(s); L->focus = hit; buf_load_active(s); buf_reset_idle(s); }
        bool inside = false;
        proven_size_t byte = pane_click_byte(s, &L->nodes[hit], r, k.mrow, k.mcol, &inside);
        if (inside) {
            prov_editor_set_extending(s->ed, false);
            prov_editor_clear_selection(s->ed);
            prov_editor_move_to(s->ed, byte);
            s->zx_visual = false; s->block_visual = false;
            s->mouse_drag = MDRAG_SELECT;
        } else s->mouse_drag = MDRAG_NONE;
        return;
    }

    if (k.mbtn == PROV_MB_LEFT && k.mact == PROV_ME_DRAG) {
        prov_rect_t r;
        int hit = layout_leaf_at(s, k.mrow, k.mcol, &r);
        if (s->mouse_drag == MDRAG_SCROLL) {                      /* drag the scrollbar */
            if (hit == L->focus) mouse_scroll_to(s, hit, r, k.mrow);
            return;
        }
        if (s->mouse_drag != MDRAG_SELECT || hit != L->focus) return;
        bool inside = false;
        proven_size_t byte = pane_click_byte(s, &L->nodes[hit], r, k.mrow, k.mcol, &inside);
        prov_editor_set_extending(s->ed, true);
        prov_editor_move_to(s->ed, byte);
        return;
    }
}

int main(int argc, char **argv) {
    proven_allocator_t a = proven_heap_allocator();
    prov_config_t cfg;
    load_config(&cfg, a);
    prov_charset_configure(cfg.charset_backend);   /* record preference; backends are probed lazily */
    prov_charset_set_iconv_path(cfg.charset_iconv_path);   /* external-iconv exe (PATH name or full path) */

    prov_session_t s = {
        .a = a, .mode = MODE_ZX, .running = true, .cfg = cfg, .orphan_buf = -1, .macro = { .last = -1 },
    };
    prov_bufset_init(&s.bufs);

    /* One buffer per file argument; no args = a single unnamed buffer. */
    for (int i = 1; i < argc; i++) {
        bool san = false;
        prov_fileinfo_t info;
        prov_result_editor_t er = prov_editor_open(a, argv[i], &san, &info, NULL, cfg.fallback_encoding);
        if (!PROVEN_IS_OK(er.err)) { fprintf(stderr, "prov: cannot open '%s'\n", argv[i]); continue; }
        if (san) fprintf(stderr, "prov: '%s': dropped invalid (non-UTF-8) bytes\n", argv[i]);
        int idx = prov_bufset_add(&s.bufs, er.value, argv[i]);
        if (idx < 0) { prov_editor_destroy(er.value); break; }
        s.bufs.entries[idx].info = info;
    }
    if (s.bufs.count == 0) {
        prov_result_editor_t er = prov_editor_create(a);
        if (!PROVEN_IS_OK(er.err)) { fprintf(stderr, "prov: cannot create a buffer\n"); return 1; }
        prov_bufset_add(&s.bufs, er.value, NULL);
    }
    s.bufs.active = 0;
    s.search.icase = s.cfg.search_ignorecase;   /* config seeds the search defaults; soc/soh toggle */
    for (int i = 0; i < s.bufs.count; i++)
        prov_editor_set_undo_limit(s.bufs.entries[i].ed, s.cfg.undo_limit);
    prov_layout_init(&s.tabs[0], 0);   /* one tab, one window showing buffer 0 */
    s.tab = 0;
    s.tab_count = 1;
    buf_load_active(&s);

    if (!prov_term_is_tty()) {
        fprintf(stderr, "prov: stdin/stdout is not a terminal\n");
        for (int i = 0; i < s.bufs.count; i++) prov_editor_destroy(s.bufs.entries[i].ed);
        return 1;
    }
    if (!prov_term_init()) {
        fprintf(stderr, "prov: failed to enter raw terminal mode\n");
        for (int i = 0; i < s.bufs.count; i++) prov_editor_destroy(s.bufs.entries[i].ed);
        return 1;
    }
    prov_term_enable_mouse(s.cfg.mouse);   /* RFC-0014: SGR mouse reporting (config-gated) */

    proven_result_u8str_t scr = proven_u8str_create(a, 128);  /* per-frame status scratch */
    if (!PROVEN_IS_OK(scr.err)) {
        prov_term_shutdown();
        fprintf(stderr, "prov: out of memory\n");
        for (int i = 0; i < s.bufs.count; i++) prov_editor_destroy(s.bufs.entries[i].ed);
        return 1;
    }
    s.scratch = scr.value;
    s.prompt_hist.pos = -1;                                   /* prompt history: not navigating */
    load_state(&s);              /* restore find/replace + type-filter history rings */

    prov_cell_t  *grid = NULL;   /* the composited screen */
    prov_cell_t  *tmp  = NULL;   /* scratch for rendering one pane before blit */
    proven_size_t gridcap = 0;

    while (s.running) {
        prov_term_size_t sz = prov_term_size();
        proven_size_t cols = sz.cols ? sz.cols : 80;
        /* Reserved rows: the global status line + the command line. There is no
         * top tab bar — the tab count shows in the status line, and 0t lists and
         * manages the tabs. */
        proven_size_t reserved = 2;
        proven_size_t text_rows = sz.rows > reserved ? sz.rows - reserved : 1;

        proven_size_t need = text_rows * cols;
        if (need > gridcap) {
            proven_result_mem_mut_t rg =
                grid ? a.realloc_fn(a.ctx, grid, gridcap * sizeof(prov_cell_t),
                                    need * sizeof(prov_cell_t), 16)
                     : a.alloc_fn(a.ctx, need * sizeof(prov_cell_t), 16);
            if (!PROVEN_IS_OK(rg.err)) break;
            grid = (prov_cell_t *)rg.value.ptr;
            proven_result_mem_mut_t rt =
                tmp ? a.realloc_fn(a.ctx, tmp, gridcap * sizeof(prov_cell_t),
                                   need * sizeof(prov_cell_t), 16)
                    : a.alloc_fn(a.ctx, need * sizeof(prov_cell_t), 16);
            if (!PROVEN_IS_OK(rt.err)) break;
            tmp = (prov_cell_t *)rt.value.ptr;
            gridcap = need;
        }

        /* Lay the panes out over the text area; the focused pane drives the
         * cursor-visible scroll and the page size. When split, each pane keeps
         * its bottom row for a label, so the usable text height is one less. */
        prov_rect_t area = { 0, 0, (int)text_rows, (int)cols };
        s.area_h = (int)text_rows;
        s.area_w = (int)cols;
        prov_layout_t *L = cur_layout(&s);
        bool multi = prov_layout_leaf_count(L) > 1;
        if (!multi) s.pane_mode = 0;            /* no panes left to move/resize */
        prov_rect_t rects[PROV_MAX_PANE_NODES];
        prov_layout_rects(L, area, rects);
        int fh = rects[L->focus].h;
        if (fh >= 2) fh -= 1;                          /* per-window status row */
        proven_size_t vis = fh > 0 ? (proven_size_t)fh : 1;
        int fcw = rects[L->focus].w >= 2 ? rects[L->focus].w - 1 : 1;  /* content width (minus scrollbar) */

        /* Keep the cursor visible within the focused pane, honoring `scrolloff`.
         * Page keys may have scrolled s.top further (even past the last line,
         * exposing the ~ EOF rows); prov_scroll_top only nudges when the cursor
         * actually fell outside the margin. */
        proven_size_t line = prov_editor_cursor_line(s.ed);
        proven_size_t lc = prov_buffer_line_count(prov_editor_buffer(s.ed));
        bool wrap_off = s.cfg.wrap == PROV_WRAP_OFF;
        bool word_wrap = s.cfg.wrap == PROV_WRAP_WORD;
        s.top = prov_scroll_top(s.top, line, vis, s.cfg.scrolloff, lc);
        /* Soft-wrap correction: a wrapped line can push the cursor below the
         * viewport even with its logical line on screen — scroll down until the
         * cursor's wrapped row fits. (No-op when nothing wraps / wrap=off.) */
        while (!wrap_off && s.top < line) {
            prov_screen_pos_t wp = prov_cursor_wrap_pos(s.ed, s.top, (proven_size_t)fcw,
                                                        s.cfg.tabstop, false, 0, word_wrap);
            if (wp.row < vis) break;
            s.top++;
        }
        if (wrap_off) {                   /* keep the cursor's column visible (horizontal scroll) */
            int gw = (s.cfg.line_numbers != PROV_LINENUM_OFF)
                     ? prov_gutter_width(lc) : 0;
            if (gw > fcw - 1) gw = 0;
            proven_size_t hview = (proven_size_t)(fcw - gw > 0 ? fcw - gw : 1);
            proven_size_t cvx = prov_cursor_screen_pos(s.ed, s.top, s.cfg.tabstop).col;
            s.leftcol = prov_hscroll_left(s.leftcol, cvx, hview, s.cfg.scrolloff);
        } else {
            s.leftcol = 0;
        }
        L->nodes[L->focus].top = s.top;          /* publish to the focused window */
        L->nodes[L->focus].leftcol = s.leftcol;

        int cur_row = 0, cur_col = 0;
        if (s.panel_open) {                            /* modal panel composited over the editor */
            for (proven_size_t i = 0; i < need; i++)
                grid[i] = (prov_cell_t){ .cp = 0x20 };
            render_panes(&s, L->root, area, grid, (int)cols,
                         tmp, (int)gridcap, &cur_row, &cur_col);
            draw_panel(grid, (int)cols, (int)text_rows, &s);
        } else {                                       /* every window: content + status + scrollbar */
            for (proven_size_t i = 0; i < need; i++)
                grid[i] = (prov_cell_t){ .cp = 0x20 };
            render_panes(&s, L->root, area, grid, (int)cols,
                         tmp, (int)gridcap, &cur_row, &cur_col);
            /* welcome splash on the fresh empty unnamed start buffer */
            if (s.prompt_kind == PROMPT_NONE && !s.panel_open && s.tab_count == 1
                && prov_layout_leaf_count(L) == 1
                && !s.bufs.entries[s.bufs.active].path[0]
                && prov_buffer_byte_len(prov_editor_buffer(s.ed)) == 0)
                prov_draw_splash(grid, (int)cols, (int)text_rows);
        }

        proven_size_t ccol = (proven_size_t)cur_col < cols ? (proven_size_t)cur_col : (cols ? cols - 1 : 0);

        const char *status = build_status_line(&s);

        char cmdline[256];
        build_cmdline(&s, cmdline, sizeof cmdline);

        prov_term_present(grid, text_rows, cols, (proven_size_t)cur_row, ccol,
                          status, cmdline, NULL);

        prov_key_t k;
        bool from_feed = s.feed.pos < s.feed.len;   /* replay queued macro keys first */
        if (from_feed) {
            k = s.feed.keys[s.feed.pos++];
            if (s.feed.pos >= s.feed.len) s.feed.len = s.feed.pos = 0;
        } else {
            k = prov_term_read_key();
        }
        if (k.kind == PROV_KEY_NONE) continue;  /* e.g. a terminal resize: just re-render */
        if (k.kind == PROV_KEY_MOUSE) { handle_mouse(&s, k); continue; }  /* RFC-0014: global, not recorded */
        if (!from_feed && s.macro.rec) macro_append(&s, s.macro.rec_slot, k);  /* record live keys */
        s.message[0] = '\0';                   /* a one-shot message lasts until the next key */
        if (s.panel_open)                      panel_key(&s, k);
        else if (s.hex_pending != HEX_PEND_NONE) handle_hex_pending_key(&s, k);  /* save-before-reload prompt */
        else if (s.prompt_kind != PROMPT_NONE) handle_prompt_key(&s, k);
        else if (buf_is_binary(&s, active_buf(&s))) handle_hex_key(&s, k);  /* RFC-0019: binary buffer owns input */
        else if (s.mode == MODE_FIELD)         handle_field_key(&s, k, vis);
        else if (s.mode == MODE_ED)            handle_ed_key(&s, k, vis);
        else                                   handle_zx_key(&s, k, vis);
    }

    prov_term_shutdown();
    save_state(&s);              /* persist find/replace + type-filter history rings */
    proven_u8str_destroy(a, &s.scratch);
    search_cache_end(&s);
    if (s.search.re) prov_regex_destroy(a, s.search.re);
    panel_close(&s);
    for (int i = 0; i < 36; i++) {
        if (s.regs[i].bytes) a.free_fn(a.ctx, s.regs[i].bytes);
        if (s.macro.slot[i]) a.free_fn(a.ctx, s.macro.slot[i]);
    }
    if (s.feed.keys) a.free_fn(a.ctx, s.feed.keys);
    if (s.hex.buf) a.free_fn(a.ctx, s.hex.buf);
    if (s.hex.clip) a.free_fn(a.ctx, s.hex.clip);
    if (s.hexedit.ed) prov_editor_destroy(s.hexedit.ed);
    if (s.clip_last) a.free_fn(a.ctx, s.clip_last);
    prov_browser_free(&s.browse.model, a);
    if (s.browse.view) a.free_fn(a.ctx, s.browse.view);
    if (grid) a.free_fn(a.ctx, grid);
    if (tmp) a.free_fn(a.ctx, tmp);
    for (int i = 0; i < s.bufs.count; i++) prov_editor_destroy(s.bufs.entries[i].ed);
    return 0;
}
