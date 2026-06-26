#ifndef PROV_COMMAND_H
#define PROV_COMMAND_H

#include "proven/types.h"

/*
 * Deterministic zx-mode command parser (SPEC.md §10).
 *
 * Keys are fed one code point at a time; the parser is an incremental, fully
 * deterministic state machine with no timeout-based disambiguation (§10.1). It
 * returns PROV_CMD_INCOMPLETE while a command is still being entered, a
 * concrete command kind once one is recognized, or PROV_CMD_INVALID for a
 * sequence that cannot form a command. On any terminal result the parser
 * resets itself, ready for the next command.
 *
 * This first slice covers the deterministic core: a numeric repeat count,
 * cursor movement, single-key edits, and the operator family (c/d/y) with
 * linewise, word-motion, and text-object targets. Namespaces (g/z/s/...) and
 * the 0-special-prefix are added in later slices.
 */

typedef enum {
    PROV_CMD_INCOMPLETE = 0,  /* need more keys */
    PROV_CMD_INVALID,         /* not a valid command */
    PROV_CMD_MOVE,            /* cursor movement */
    PROV_CMD_EDIT,            /* single-key edit / action */
    PROV_CMD_OPERATION,       /* operator applied to a target */
    PROV_CMD_ACTION,          /* a named namespace command (g/z/o/0) */
    PROV_CMD_FINDCHAR,        /* standalone f/t cursor find (param + target) */
    PROV_CMD_BOOKMARK_JUMP,   /* m<letter>: jump to mark `param` */
    PROV_CMD_REGISTER,        /* b<reg><op>: named register; reg in `param`, op in `param2` */
    PROV_CMD_MACRO_EXEC,      /* e<letter>: run macro `param` (count times) */
    PROV_CMD_MACRO_LAST       /* E: replay the last-run macro (count times) */
} prov_cmd_kind_t;

/* Named commands from the g/z/o namespaces and the 0-special-prefix
 * (SPEC §10.7). The register/bookmark/macro/search/window/tab namespaces
 * (b/m/e/s/w/t) belong to later milestones and are not parsed yet. */
typedef enum {
    /* g namespace (goto / movement) */
    PROV_ACT_DOC_START,        /* gg (no count) */
    PROV_ACT_DOC_END,          /* ge */
    PROV_ACT_GOTO_LINE,        /* [N]gg -> line `count` */
    PROV_ACT_FILE_LAST_LINE,   /* 0g */
    PROV_ACT_PREV_WORD,        /* gp */
    PROV_ACT_NEXT_WORD,        /* gn */
    PROV_ACT_LINE_START,       /* gf */
    PROV_ACT_LINE_END,         /* gl */
    PROV_ACT_HALF_PAGE_UP,     /* gu */
    PROV_ACT_HALF_PAGE_DOWN,   /* gd */
    /* o namespace (open line) */
    PROV_ACT_OPEN_BELOW,       /* on */
    PROV_ACT_OPEN_ABOVE,       /* op */
    /* z namespace (meta editor / wizards) */
    PROV_ACT_RETURN_ED,        /* zx */
    PROV_ACT_OPEN_FILE,        /* zo */
    PROV_ACT_WRITE_FILE,       /* zw */
    PROV_ACT_WRITE_AS,         /* za */
    PROV_ACT_QUIT,             /* zq */
    PROV_ACT_HELP,             /* zh */
    PROV_ACT_BUFFER_LIST,      /* zb */
    PROV_ACT_CMD_PROMPT,       /* zp */
    PROV_ACT_INDENT,           /* zi */
    PROV_ACT_DEDENT,           /* zd */
    PROV_ACT_SYNTAX,           /* zs */
    PROV_ACT_READ_OPTS,        /* zn */
    PROV_ACT_WRITE_OPTS,       /* zf */
    PROV_ACT_CONFIG,           /* zc */
    /* s namespace: search (M4.5) */
    PROV_ACT_SEARCH_PROMPT,    /* ss */
    PROV_ACT_SEARCH_WORD,      /* sw */
    PROV_ACT_SEARCH_NEXT,      /* sn */
    PROV_ACT_SEARCH_PREV,      /* sp */
    PROV_ACT_REPLACE,          /* sr */
    PROV_ACT_SEARCH_HELP,      /* sh: regex reference */
    PROV_ACT_SEARCH_CASE,      /* soc: toggle case-insensitive */
    PROV_ACT_SEARCH_HL,        /* soh: toggle match highlight */
    PROV_ACT_SEARCH_REGEX,     /* sox: toggle regex (RFC-0009) */
    PROV_ACT_SEARCH_WORDB,     /* sow: toggle whole-word (\b...\b) */
    PROV_ACT_SEARCH_PANEL,     /* / : open the find/replace dialog (RFC-0016) */

    /* 0-special-prefix browsers/overviews */
    PROV_ACT_UNDO_BROWSER,     /* 0u */
    PROV_ACT_SEARCH_BROWSER,   /* 0s */
    PROV_ACT_REG_BROWSER,      /* 0b */
    PROV_ACT_CMD_BROWSER,      /* 0z */
    PROV_ACT_WIN_OVERVIEW,     /* 0w */
    PROV_ACT_TAB_OVERVIEW,     /* 0t */
    PROV_ACT_MACRO_OVERVIEW,   /* 0e */
    PROV_ACT_BOOKMARK_OVERVIEW,/* 0m */
    PROV_ACT_MOVE_HISTORY,     /* 0n */
    /* find repeat */
    PROV_ACT_FIND_NEXT,        /* ; repeat the last f/t */
    PROV_ACT_FIND_PREV,        /* , repeat the last f/t, reversed */
    /* t namespace (tabs) — `t` mirrors `g`: with a count `t` goes to a tab,
     * with no count it opens the tab namespace (tn/tq/tj/tl). */
    PROV_ACT_GOTO_TAB,         /* [N]t  go to tab N */
    PROV_ACT_TAB_NEW,          /* tn */
    PROV_ACT_TAB_CLOSE,        /* tq */
    PROV_ACT_TAB_PREV,         /* tj */
    PROV_ACT_TAB_NEXT,         /* tl */
    /* w namespace (windows / panes) */
    PROV_ACT_PANE_HSPLIT,      /* wh */
    PROV_ACT_PANE_VSPLIT,      /* wv */
    PROV_ACT_PANE_CLOSE,       /* wq */
    PROV_ACT_WIN_PREV,         /* wp — previous window */
    PROV_ACT_WIN_NEXT,         /* wn — next window */
    PROV_ACT_WIN_UP,           /* wi — focus the window above */
    PROV_ACT_WIN_DOWN,         /* wk — focus the window below */
    PROV_ACT_WIN_LEFT,         /* wj — focus the window to the left */
    PROV_ACT_WIN_RIGHT,        /* wl — focus the window to the right */
    PROV_ACT_PANE_RESIZE,      /* ws — enter resize submode */
    PROV_ACT_PANE_READONLY,    /* wr — toggle the window's read-only flag */
    PROV_ACT_PANE_HEX          /* wx — toggle the window's hex view/edit (RFC-0018) */
} prov_action_t;

typedef enum {
    PROV_MOVE_UP, PROV_MOVE_DOWN, PROV_MOVE_LEFT, PROV_MOVE_RIGHT
} prov_move_dir_t;

typedef enum {
    PROV_EDIT_UNDO,           /* u */
    PROV_EDIT_REDO,           /* U */
    PROV_EDIT_PASTE,          /* p — char at cursor / line below */
    PROV_EDIT_PASTE_ABOVE,    /* P — char at cursor / line above */
    PROV_EDIT_TOGGLE_SELECT,  /* v */
    PROV_EDIT_CUT,            /* x */
    PROV_EDIT_REPLACE_CHAR,   /* r */
    PROV_EDIT_APPEND,         /* a: enter Ed insert at the cursor */
    PROV_EDIT_REPEAT,         /* n: repeat the last command ([N]n repeats it N times) */
    PROV_EDIT_MACRO           /* q */
} prov_edit_kind_t;

typedef enum { PROV_OP_CHANGE, PROV_OP_DELETE, PROV_OP_YANK } prov_op_kind_t;

typedef enum {
    PROV_TARGET_LINEWISE,     /* dd / cc / yy */
    PROV_TARGET_WORD,         /* w */
    PROV_TARGET_BACK,         /* b */
    PROV_TARGET_END,          /* e */
    PROV_TARGET_LINE_END,     /* l (to end of line) */
    PROV_TARGET_MATCH,        /* m (to matching delimiter) */
    PROV_TARGET_FIND,         /* f<char> (to a literal char), char in `param` */
    PROV_TARGET_TILL,         /* t<char> (up to a literal char), char in `param` */
    PROV_TARGET_TEXTOBJ       /* iw/aw/i"/a(/... */
} prov_target_kind_t;

typedef enum {
    PROV_TOBJ_WORD,      /* w */
    PROV_TOBJ_DQUOTE,    /* " */
    PROV_TOBJ_SQUOTE,    /* ' */
    PROV_TOBJ_PAREN,     /* ( ) */
    PROV_TOBJ_BRACE,     /* { } */
    PROV_TOBJ_BRACKET,   /* [ ] */
    PROV_TOBJ_ANGLE,     /* < > */
    PROV_TOBJ_TAG,       /* t */
    PROV_TOBJ_PARAGRAPH  /* p */
} prov_textobj_t;

typedef struct {
    prov_cmd_kind_t    kind;
    proven_u32         count;     /* repeat count, >= 1 */
    prov_move_dir_t    move;      /* kind == PROV_CMD_MOVE */
    prov_edit_kind_t   edit;      /* kind == PROV_CMD_EDIT */
    prov_op_kind_t     op;        /* kind == PROV_CMD_OPERATION */
    prov_target_kind_t target;    /* kind == PROV_CMD_OPERATION */
    bool               inner;     /* target == TEXTOBJ: inner (i) vs around (a) */
    prov_textobj_t     textobj;   /* target == TEXTOBJ */
    proven_u32         param;     /* FIND/TILL char; BOOKMARK slot; REGISTER reg char */
    proven_u32         param2;    /* REGISTER op char ('x' cut / 'c' copy / 'v' paste) */
    prov_action_t      action;    /* kind == PROV_CMD_ACTION */
} prov_command_t;

/* Incremental parser state. Zero-initialize to start fresh. */
typedef struct {
    proven_u32     count;       /* accumulated count (0 = none entered yet) */
    bool           has_count;
    int            state;       /* internal: start / operator / textobj / namespace */
    prov_op_kind_t pending_op;
    bool           pending_inner;
    bool           pending_till;  /* find vs till while awaiting the target char */
    bool           find_move;     /* PS_FIND is a standalone f/t cursor move (no operator) */
    int            reg_slot;      /* PS_REG_OP: the register char captured after `b` */
    int            ns;          /* pending namespace prefix char ('g'/'z'/'o'/'0') */
} prov_cmd_parser_t;

/* Feed one key (a Unicode code point). Returns the current command status;
 * resets the parser on any non-INCOMPLETE result. */
prov_command_t prov_cmd_feed(prov_cmd_parser_t *p, proven_u32 key);

/* True when no partial command is pending (no count, operator, or namespace
 * prefix entered). Used to detect "trigger ambiguity" right after entering
 * zx mode (SPEC §9.2). */
bool prov_cmd_parser_idle(const prov_cmd_parser_t *p);

/* The pending repeat count when only a bare count has been entered (no operator
 * or namespace prefix); 0 otherwise. Lets the event loop apply a count to keys
 * the parser does not itself consume, such as the arrow / page movement keys. */
proven_u32 prov_cmd_pending_count(const prov_cmd_parser_t *p);

/* Write a short, NUL-terminated hint for the parser's *current* in-progress
 * state into `buf` (the sub-commands the typed prefix can still become, or what
 * a pending count will do). Used for live zx status-bar feedback. */
void prov_cmd_describe(const prov_cmd_parser_t *p, char *buf, proven_size_t n);

/* Write a one/two-word label for a completed command into `buf` (e.g.
 * "goto line", "del word", "paste"). */
void prov_cmd_label(const prov_command_t *c, char *buf, proven_size_t n);

#endif /* PROV_COMMAND_H */
