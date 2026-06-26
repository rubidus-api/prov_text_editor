#include "command.h"

#include "pstr.h"           /* prov_cstr_set / prov_cstr_view (libc-free) */
#include "proven/u8str.h"   /* proven_u8str_borrow */
#include "proven/fmt.h"     /* proven_u8str_append_fmt / PROVEN_ARG */

/* Internal parser states. */
enum { PS_START = 0, PS_OPERATOR = 1, PS_TEXTOBJ = 2, PS_NAMESPACE = 3, PS_FIND = 4,
       PS_MARK = 5, PS_REG = 6, PS_REG_OP = 7, PS_MACRO_EXEC = 8,
       PS_SEARCH_OPT = 10 };

static prov_command_t make(prov_cmd_kind_t kind) {
    prov_command_t c = { kind, 1, PROV_MOVE_UP, PROV_EDIT_UNDO, PROV_OP_CHANGE,
                         PROV_TARGET_LINEWISE, false, PROV_TOBJ_WORD, 0, 0,
                         PROV_ACT_DOC_START };
    return c;
}

static void reset(prov_cmd_parser_t *p) {
    p->count = 0;
    p->has_count = false;
    p->state = PS_START;
    p->pending_op = PROV_OP_CHANGE;
    p->pending_inner = false;
    p->pending_till = false;
    p->find_move = false;
    p->ns = 0;
}

static proven_u32 effective_count(const prov_cmd_parser_t *p) {
    return p->has_count ? p->count : 1u;
}

/* Build a completed command of `kind`, stamping the active count and resetting. */
static prov_command_t complete(prov_cmd_parser_t *p, prov_cmd_kind_t kind) {
    prov_command_t c = make(kind);
    c.count = effective_count(p);
    reset(p);
    return c;
}

/* Complete a named namespace action. */
static prov_command_t action_cmd(prov_cmd_parser_t *p, prov_action_t act) {
    prov_command_t c = complete(p, PROV_CMD_ACTION);
    c.action = act;
    return c;
}

bool prov_cmd_parser_idle(const prov_cmd_parser_t *p) {
    return p->state == PS_START && !p->has_count && p->ns == 0;
}

proven_u32 prov_cmd_pending_count(const prov_cmd_parser_t *p) {
    if (p->state == PS_START && p->has_count && p->ns == 0) return p->count;
    return 0;
}

prov_command_t prov_cmd_feed(prov_cmd_parser_t *p, proven_u32 key) {
    if (p->state == PS_START) {
        if (key >= '1' && key <= '9') {
            p->count = (p->has_count ? p->count * 10u : 0u) + (key - '0');
            p->has_count = true;
            return make(PROV_CMD_INCOMPLETE);
        }
        if (key == '0') {
            if (p->has_count) {            /* trailing zero in a count */
                p->count *= 10u;
                return make(PROV_CMD_INCOMPLETE);
            }
            p->ns = '0';                   /* leading 0: special-prefix namespace */
            p->state = PS_NAMESPACE;
            return make(PROV_CMD_INCOMPLETE);
        }

        prov_command_t c;
        switch (key) {
            case 'i': c = complete(p, PROV_CMD_MOVE); c.move = PROV_MOVE_UP;    return c;
            case 'k': c = complete(p, PROV_CMD_MOVE); c.move = PROV_MOVE_DOWN;  return c;
            case 'j': c = complete(p, PROV_CMD_MOVE); c.move = PROV_MOVE_LEFT;  return c;
            case 'l': c = complete(p, PROV_CMD_MOVE); c.move = PROV_MOVE_RIGHT; return c;

            case 'u': c = complete(p, PROV_CMD_EDIT); c.edit = PROV_EDIT_UNDO;          return c;
            case 'U': c = complete(p, PROV_CMD_EDIT); c.edit = PROV_EDIT_REDO;          return c;
            case 'p': c = complete(p, PROV_CMD_EDIT); c.edit = PROV_EDIT_PASTE;         return c;
            case 'P': c = complete(p, PROV_CMD_EDIT); c.edit = PROV_EDIT_PASTE_ABOVE;   return c;
            case 'v': c = complete(p, PROV_CMD_EDIT); c.edit = PROV_EDIT_TOGGLE_SELECT; return c;
            case 'x': c = complete(p, PROV_CMD_EDIT); c.edit = PROV_EDIT_CUT;           return c;
            case 'r': c = complete(p, PROV_CMD_EDIT); c.edit = PROV_EDIT_REPLACE_CHAR;  return c;
            case 'a': c = complete(p, PROV_CMD_EDIT); c.edit = PROV_EDIT_APPEND;        return c;
            case 'n': c = complete(p, PROV_CMD_EDIT); c.edit = PROV_EDIT_REPEAT;        return c;
            case 'q': c = complete(p, PROV_CMD_EDIT); c.edit = PROV_EDIT_MACRO;         return c;

            case 'c': p->pending_op = PROV_OP_CHANGE; p->state = PS_OPERATOR; return make(PROV_CMD_INCOMPLETE);
            case 'd': p->pending_op = PROV_OP_DELETE; p->state = PS_OPERATOR; return make(PROV_CMD_INCOMPLETE);
            case 'y': p->pending_op = PROV_OP_YANK;   p->state = PS_OPERATOR; return make(PROV_CMD_INCOMPLETE);

            /* standalone f<char> cursor find (awaits the target char); . , repeat it
             * (. = next/forward ">", , = prev/backward "<") */
            case 'f': p->find_move = true; p->pending_till = false; p->state = PS_FIND; return make(PROV_CMD_INCOMPLETE);
            case '.': return action_cmd(p, PROV_ACT_FIND_NEXT);
            case ',': return action_cmd(p, PROV_ACT_FIND_PREV);
            case '/': return action_cmd(p, PROV_ACT_SEARCH_PANEL);   /* one-key find dialog (RFC-0016) */

            /* t namespace (tabs), mirroring g: count+t goes to a tab, bare t
             * opens the namespace. (Operator-pending t<char> till is unaffected.) */
            case 't':
                if (p->has_count) return action_cmd(p, PROV_ACT_GOTO_TAB);
                p->ns = (int)key; p->state = PS_NAMESPACE; return make(PROV_CMD_INCOMPLETE);

            /* w namespace (panes): wh/wv split, ww focus, wq close */
            case 'w':
                p->ns = (int)key; p->state = PS_NAMESPACE; return make(PROV_CMD_INCOMPLETE);

            case 'g':
                /* [N]g shorthand (RFC 0002 / SPEC §10.7): with a count a single
                 * g jumps to line N immediately; with no count, g opens the
                 * goto/movement namespace (gg, ge, gf, ...). */
                if (p->has_count) return action_cmd(p, PROV_ACT_GOTO_LINE);
                p->ns = (int)key; p->state = PS_NAMESPACE; return make(PROV_CMD_INCOMPLETE);
            case 'z': case 'o': case 's':
                p->ns = (int)key; p->state = PS_NAMESPACE; return make(PROV_CMD_INCOMPLETE);
            case 'm':                                       /* m<letter>: jump to a bookmark (0m opens the panel) */
                p->state = PS_MARK; return make(PROV_CMD_INCOMPLETE);
            case 'b':                                       /* b<reg><op>: named register */
                p->state = PS_REG; return make(PROV_CMD_INCOMPLETE);
            case 'e':                                       /* e<letter>: run a macro */
                p->state = PS_MACRO_EXEC; return make(PROV_CMD_INCOMPLETE);
            case 'E': {                                     /* E: replay the last macro */
                prov_command_t c = complete(p, PROV_CMD_MACRO_LAST);
                return c;
            }

            default: reset(p); return make(PROV_CMD_INVALID);
        }
    }

    if (p->state == PS_OPERATOR) {
        proven_u32 op_char = (p->pending_op == PROV_OP_CHANGE) ? (proven_u32)'c'
                           : (p->pending_op == PROV_OP_DELETE) ? (proven_u32)'d'
                                                               : (proven_u32)'y';
        prov_op_kind_t op = p->pending_op;

        if (key == op_char) {                              /* dd / cc / yy */
            prov_command_t c = complete(p, PROV_CMD_OPERATION);
            c.op = op; c.target = PROV_TARGET_LINEWISE;
            return c;
        }
        switch (key) {
            case 'w': { prov_command_t c = complete(p, PROV_CMD_OPERATION); c.op = op; c.target = PROV_TARGET_WORD;     return c; }
            case 'b': { prov_command_t c = complete(p, PROV_CMD_OPERATION); c.op = op; c.target = PROV_TARGET_BACK;     return c; }
            case 'e': { prov_command_t c = complete(p, PROV_CMD_OPERATION); c.op = op; c.target = PROV_TARGET_END;      return c; }
            case 'l': { prov_command_t c = complete(p, PROV_CMD_OPERATION); c.op = op; c.target = PROV_TARGET_LINE_END; return c; }
            case 'm': { prov_command_t c = complete(p, PROV_CMD_OPERATION); c.op = op; c.target = PROV_TARGET_MATCH;    return c; }
            case 'f': p->pending_till = false; p->state = PS_FIND; return make(PROV_CMD_INCOMPLETE);
            case 't': p->pending_till = true;  p->state = PS_FIND; return make(PROV_CMD_INCOMPLETE);
            case 'i': p->pending_inner = true;  p->state = PS_TEXTOBJ; return make(PROV_CMD_INCOMPLETE);
            case 'a': p->pending_inner = false; p->state = PS_TEXTOBJ; return make(PROV_CMD_INCOMPLETE);
            default: reset(p); return make(PROV_CMD_INVALID);
        }
    }

    if (p->state == PS_FIND) {
        bool till = p->pending_till;
        bool move = p->find_move;
        prov_op_kind_t op = p->pending_op;
        prov_command_t c = complete(p, move ? PROV_CMD_FINDCHAR : PROV_CMD_OPERATION);
        if (!move) c.op = op;
        c.target = till ? PROV_TARGET_TILL : PROV_TARGET_FIND;
        c.param = key;          /* the literal target character */
        return c;
    }

    if (p->state == PS_MARK) {                  /* m<letter>: jump to bookmark slot a–z */
        if (key >= 'a' && key <= 'z') {
            prov_command_t c = complete(p, PROV_CMD_BOOKMARK_JUMP);
            c.param = key;
            return c;
        }
        reset(p);
        return make(PROV_CMD_INVALID);
    }

    if (p->state == PS_SEARCH_OPT) {            /* so<opt> */
        switch (key) {
            case 'c': return action_cmd(p, PROV_ACT_SEARCH_CASE);
            case 'h': return action_cmd(p, PROV_ACT_SEARCH_HL);
            case 'x': return action_cmd(p, PROV_ACT_SEARCH_REGEX);
            case 'w': return action_cmd(p, PROV_ACT_SEARCH_WORDB);
            default: reset(p); return make(PROV_CMD_INVALID);
        }
    }

    if (p->state == PS_MACRO_EXEC) {            /* e<letter>: run macro slot (0e opens the panel) */
        if ((key >= 'a' && key <= 'z') || (key >= '0' && key <= '9')) {
            prov_command_t c = complete(p, PROV_CMD_MACRO_EXEC);
            c.param = key;
            return c;
        }
        reset(p); return make(PROV_CMD_INVALID);
    }

    if (p->state == PS_REG) {                   /* b<reg>: a-z or 0-9, then the op */
        if ((key >= 'a' && key <= 'z') || (key >= '0' && key <= '9')) {
            p->reg_slot = (int)key; p->state = PS_REG_OP; return make(PROV_CMD_INCOMPLETE);
        }
        reset(p); return make(PROV_CMD_INVALID);
    }
    if (p->state == PS_REG_OP) {                 /* b<reg><op>: x cut / c copy / v paste */
        if (key == 'x' || key == 'c' || key == 'v') {
            int reg = p->reg_slot;               /* capture before complete() resets the parser */
            prov_command_t c = complete(p, PROV_CMD_REGISTER);
            c.param = (proven_u32)reg;
            c.param2 = key;
            return c;
        }
        reset(p); return make(PROV_CMD_INVALID);
    }

    if (p->state == PS_TEXTOBJ) {
        prov_op_kind_t op = p->pending_op;
        bool inner = p->pending_inner;
        prov_textobj_t tobj = PROV_TOBJ_WORD;
        bool ok = true;
        switch (key) {
            case 'w':            tobj = PROV_TOBJ_WORD;      break;
            case '"':            tobj = PROV_TOBJ_DQUOTE;    break;
            case '\'':           tobj = PROV_TOBJ_SQUOTE;    break;
            case '(': case ')':  tobj = PROV_TOBJ_PAREN;     break;
            case '{': case '}':  tobj = PROV_TOBJ_BRACE;     break;
            case '[': case ']':  tobj = PROV_TOBJ_BRACKET;   break;
            case '<': case '>':  tobj = PROV_TOBJ_ANGLE;     break;
            case 't':            tobj = PROV_TOBJ_TAG;       break;
            case 'p':            tobj = PROV_TOBJ_PARAGRAPH; break;
            default:             ok = false;                 break;
        }
        prov_command_t c = complete(p, ok ? PROV_CMD_OPERATION : PROV_CMD_INVALID);
        if (ok) {
            c.op = op;
            c.target = PROV_TARGET_TEXTOBJ;
            c.inner = inner;
            c.textobj = tobj;
        }
        return c;
    }

    if (p->state == PS_NAMESPACE) {
        switch (p->ns) {
            case 'g':
                switch (key) {
                    case 'g': return action_cmd(p, PROV_ACT_DOC_START); /* count+g fired at start; here always doc-start */
                    case 'e': return action_cmd(p, PROV_ACT_DOC_END);
                    case 'p': return action_cmd(p, PROV_ACT_PREV_WORD);
                    case 'n': return action_cmd(p, PROV_ACT_NEXT_WORD);
                    case 'f': return action_cmd(p, PROV_ACT_LINE_START);
                    case 'l': return action_cmd(p, PROV_ACT_LINE_END);
                    case 'u': return action_cmd(p, PROV_ACT_HALF_PAGE_UP);
                    case 'd': return action_cmd(p, PROV_ACT_HALF_PAGE_DOWN);
                    default: break;
                }
                break;
            case 'o':
                switch (key) {
                    case 'n': return action_cmd(p, PROV_ACT_OPEN_BELOW);
                    case 'p': return action_cmd(p, PROV_ACT_OPEN_ABOVE);
                    default: break;
                }
                break;
            case 's':
                switch (key) {
                    case 's': return action_cmd(p, PROV_ACT_SEARCH_PROMPT);
                    case 'w': return action_cmd(p, PROV_ACT_SEARCH_WORD);
                    case 'n': return action_cmd(p, PROV_ACT_SEARCH_NEXT);
                    case 'p': return action_cmd(p, PROV_ACT_SEARCH_PREV);
                    case 'r': return action_cmd(p, PROV_ACT_REPLACE);
                    case 'h': return action_cmd(p, PROV_ACT_SEARCH_HELP);   /* sh: regex reference */
                    case 'o': p->state = PS_SEARCH_OPT; return make(PROV_CMD_INCOMPLETE);  /* so<opt> */
                    default: break;
                }
                break;
            case 't':
                switch (key) {
                    case 'n': return action_cmd(p, PROV_ACT_TAB_NEW);
                    case 'q': return action_cmd(p, PROV_ACT_TAB_CLOSE);
                    case 'j': return action_cmd(p, PROV_ACT_TAB_PREV);
                    case 'l': return action_cmd(p, PROV_ACT_TAB_NEXT);
                    default: break;
                }
                break;
            case 'w':
                switch (key) {
                    case 'h': return action_cmd(p, PROV_ACT_PANE_HSPLIT);
                    case 'v': return action_cmd(p, PROV_ACT_PANE_VSPLIT);
                    case 'q': return action_cmd(p, PROV_ACT_PANE_CLOSE);
                    /* `ww` removed — use `0w` for the window panel */
                    case 'p': return action_cmd(p, PROV_ACT_WIN_PREV);
                    case 'n': return action_cmd(p, PROV_ACT_WIN_NEXT);
                    case 'i': return action_cmd(p, PROV_ACT_WIN_UP);
                    case 'k': return action_cmd(p, PROV_ACT_WIN_DOWN);
                    case 'j': return action_cmd(p, PROV_ACT_WIN_LEFT);
                    case 'l': return action_cmd(p, PROV_ACT_WIN_RIGHT);
                    case 's': return action_cmd(p, PROV_ACT_PANE_RESIZE);
                    case 'r': return action_cmd(p, PROV_ACT_PANE_READONLY);
                    case 'x': return action_cmd(p, PROV_ACT_PANE_HEX);   /* wx: toggle hex view/edit */
                    default: break;
                }
                break;
            case 'z':
                switch (key) {
                    case 'x': return action_cmd(p, PROV_ACT_RETURN_ED);
                    case 'o': return action_cmd(p, PROV_ACT_OPEN_FILE);
                    case 'w': return action_cmd(p, PROV_ACT_WRITE_FILE);
                    case 'a': return action_cmd(p, PROV_ACT_WRITE_AS);
                    case 'q': return action_cmd(p, PROV_ACT_QUIT);
                    case 'h': return action_cmd(p, PROV_ACT_HELP);
                    case 'b': return action_cmd(p, PROV_ACT_BUFFER_LIST);
                    case 'p': return action_cmd(p, PROV_ACT_CMD_PROMPT);
                    case 'i': return action_cmd(p, PROV_ACT_INDENT);
                    case 'd': return action_cmd(p, PROV_ACT_DEDENT);
                    case 's': return action_cmd(p, PROV_ACT_SYNTAX);
                    case 'n': return action_cmd(p, PROV_ACT_READ_OPTS);
                    case 'f': return action_cmd(p, PROV_ACT_WRITE_OPTS);
                    case 'c': return action_cmd(p, PROV_ACT_CONFIG);
                    default: break;
                }
                break;
            case '0':
                switch (key) {
                    case 'u': return action_cmd(p, PROV_ACT_UNDO_BROWSER);
                    case 's': return action_cmd(p, PROV_ACT_SEARCH_BROWSER);
                    case 'b': return action_cmd(p, PROV_ACT_REG_BROWSER);
                    case 'z': return action_cmd(p, PROV_ACT_CMD_BROWSER);
                    case 'w': return action_cmd(p, PROV_ACT_WIN_OVERVIEW);
                    case 't': return action_cmd(p, PROV_ACT_TAB_OVERVIEW);
                    case 'e': return action_cmd(p, PROV_ACT_MACRO_OVERVIEW);    /* 0e: macro panel */
                    case 'm': return action_cmd(p, PROV_ACT_BOOKMARK_OVERVIEW); /* 0m: bookmark panel */
                    case 'n': return action_cmd(p, PROV_ACT_MOVE_HISTORY);
                    case 'g': return action_cmd(p, PROV_ACT_FILE_LAST_LINE);
                    default: break;
                }
                break;
            default: break;
        }
        reset(p);
        return make(PROV_CMD_INVALID);
    }

    reset(p);
    return make(PROV_CMD_INVALID);
}

/* ------------------------------------------- status-bar feedback (RFC 0002) */

static void put(char *buf, proven_size_t n, const char *s) {
    if (buf && n) prov_cstr_set(buf, n, prov_cstr_view(s));
}

void prov_cmd_describe(const prov_cmd_parser_t *p, char *buf, proven_size_t n) {
    if (!buf || n == 0) return;
    buf[0] = '\0';
    switch (p->state) {
        case PS_OPERATOR: {
            const char *op = p->pending_op == PROV_OP_CHANGE ? "change"
                           : p->pending_op == PROV_OP_DELETE ? "delete" : "yank";
            char d = p->pending_op == PROV_OP_CHANGE ? 'c'
                   : p->pending_op == PROV_OP_DELETE ? 'd' : 'y';
            proven_u8str_view_t dv = { (const proven_byte_t *)&d, 1 };
            proven_u8str_t s = proven_u8str_borrow((proven_byte_t *)buf, n);
            (void)proven_u8str_append_fmt(&s, "{}: w/e/b f/t<c> i/a<obj> {}{}:line",
                     PROVEN_ARG(prov_cstr_view(op)), PROVEN_ARG(dv), PROVEN_ARG(dv));
            return;
        }
        case PS_TEXTOBJ:
            put(buf, n, p->pending_inner ? "inner obj: w  \" '  ( [ { <"
                                         : "around obj: w  \" '  ( [ { <");
            return;
        case PS_FIND:
            put(buf, n, p->pending_till ? "till: type a character"
                                        : "find: type a character");
            return;
        case PS_MARK:
            put(buf, n, "go to bookmark: a-z");
            return;
        case PS_REG:
            put(buf, n, "register: a-z / 0-9");
            return;
        case PS_REG_OP:
            put(buf, n, "register: x:cut c:copy v:paste");
            return;
        case PS_MACRO_EXEC:
            put(buf, n, "run macro: a-z / 0-9");
            return;
        case PS_SEARCH_OPT:
            put(buf, n, "search: c:case h:highlight x:regex w:word");
            return;
        case PS_NAMESPACE:
            switch (p->ns) {
                case 'g': put(buf, n, "goto: g:top e:end f/l:line u/d:half-pg p/n:word"); return;
                case 'z': put(buf, n, "meta: x:exit w:write a:write-as q:quit o:open i:indent s:syntax h:help"); return;
                case 'o': put(buf, n, "open: n:below p:above"); return;
                case 's': put(buf, n, "search: s:find w:word n:next p:prev r:replace o:opts h:regex-help"); return;
                case 't': put(buf, n, "tab: n:new q:close j:prev l:next"); return;
                case 'w': put(buf, n, "win: h/v:split p/n:prev/next ijkl:move q:close s:resize r:read-only x:hex"); return;
                case '0': put(buf, n, "0: u:undo z:cmd b:reg w:win t:tab g:last-line"); return;
                default:  return;
            }
        case PS_START:
        default:
            if (p->has_count) put(buf, n, "count: g:line ikjl:move dd/cc/yy:lines x:cut n:repeat");
            else              put(buf, n, "ikjl:move c/d/y:op [N]g:goto a:edit n:repeat u:undo v:sel zx:exit");
            return;
    }
}

void prov_cmd_label(const prov_command_t *c, char *buf, proven_size_t n) {
    if (!buf || n == 0) return;
    buf[0] = '\0';
    switch (c->kind) {
        case PROV_CMD_MOVE:
            put(buf, n, c->move == PROV_MOVE_UP ? "up"
                      : c->move == PROV_MOVE_DOWN ? "down"
                      : c->move == PROV_MOVE_LEFT ? "left" : "right");
            return;
        case PROV_CMD_EDIT:
            switch (c->edit) {
                case PROV_EDIT_UNDO:          put(buf, n, "undo");    return;
                case PROV_EDIT_REDO:          put(buf, n, "redo");    return;
                case PROV_EDIT_PASTE:         put(buf, n, "paste below");   return;
                case PROV_EDIT_PASTE_ABOVE:   put(buf, n, "paste above");   return;
                case PROV_EDIT_TOGGLE_SELECT: put(buf, n, "select");  return;
                case PROV_EDIT_CUT:           put(buf, n, "cut");     return;
                case PROV_EDIT_REPLACE_CHAR:  put(buf, n, "replace"); return;
                case PROV_EDIT_APPEND:        put(buf, n, "field");  return;
                case PROV_EDIT_REPEAT:        put(buf, n, "repeat");  return;
                case PROV_EDIT_MACRO:         put(buf, n, "macro");   return;
                default:                      return;
            }
        case PROV_CMD_OPERATION: {
            const char *op = c->op == PROV_OP_CHANGE ? "chg"
                           : c->op == PROV_OP_DELETE ? "del" : "yank";
            const char *t;
            switch (c->target) {
                case PROV_TARGET_LINEWISE: t = "line";      break;
                case PROV_TARGET_WORD:     t = "word";      break;
                case PROV_TARGET_BACK:     t = "word back"; break;
                case PROV_TARGET_END:      t = "word end";  break;
                case PROV_TARGET_LINE_END: t = "to EOL";    break;
                case PROV_TARGET_MATCH:    t = "to match";  break;
                case PROV_TARGET_FIND:     t = "to char";   break;
                case PROV_TARGET_TILL:     t = "till char"; break;
                case PROV_TARGET_TEXTOBJ:  t = c->inner ? "inner obj" : "around obj"; break;
                default:                   t = "target";    break;
            }
            proven_u8str_t s = proven_u8str_borrow((proven_byte_t *)buf, n);
            (void)proven_u8str_append_fmt(&s, "{} {}",
                     PROVEN_ARG(prov_cstr_view(op)), PROVEN_ARG(prov_cstr_view(t)));
            return;
        }
        case PROV_CMD_ACTION:
            switch (c->action) {
                case PROV_ACT_GOTO_LINE:      put(buf, n, "goto line");  return;
                case PROV_ACT_DOC_START:      put(buf, n, "doc start");  return;
                case PROV_ACT_DOC_END:        put(buf, n, "doc end");    return;
                case PROV_ACT_FILE_LAST_LINE: put(buf, n, "last line");  return;
                case PROV_ACT_PREV_WORD:      put(buf, n, "prev word");  return;
                case PROV_ACT_NEXT_WORD:      put(buf, n, "next word");  return;
                case PROV_ACT_LINE_START:     put(buf, n, "line start"); return;
                case PROV_ACT_LINE_END:       put(buf, n, "line end");   return;
                case PROV_ACT_HALF_PAGE_UP:   put(buf, n, "half-pg up"); return;
                case PROV_ACT_HALF_PAGE_DOWN: put(buf, n, "half-pg dn"); return;
                case PROV_ACT_OPEN_BELOW:     put(buf, n, "open below"); return;
                case PROV_ACT_OPEN_ABOVE:     put(buf, n, "open above"); return;
                case PROV_ACT_RETURN_ED:      put(buf, n, "exit zx");    return;
                case PROV_ACT_WRITE_FILE:     put(buf, n, "write");      return;
                case PROV_ACT_WRITE_AS:       put(buf, n, "write as");   return;
                case PROV_ACT_OPEN_FILE:      put(buf, n, "open file");  return;
                case PROV_ACT_QUIT:           put(buf, n, "quit");       return;
                case PROV_ACT_FIND_NEXT:      put(buf, n, "repeat find"); return;
                case PROV_ACT_FIND_PREV:      put(buf, n, "reverse find"); return;
                case PROV_ACT_GOTO_TAB:       put(buf, n, "goto tab");   return;
                case PROV_ACT_TAB_NEW:        put(buf, n, "new tab");    return;
                case PROV_ACT_TAB_CLOSE:      put(buf, n, "close tab");  return;
                case PROV_ACT_TAB_PREV:       put(buf, n, "prev tab");   return;
                case PROV_ACT_TAB_NEXT:       put(buf, n, "next tab");   return;
                case PROV_ACT_PANE_HSPLIT:    put(buf, n, "hsplit");     return;
                case PROV_ACT_PANE_VSPLIT:    put(buf, n, "vsplit");     return;
                case PROV_ACT_PANE_CLOSE:     put(buf, n, "close pane"); return;
                case PROV_ACT_WIN_PREV:       put(buf, n, "prev window"); return;
                case PROV_ACT_WIN_NEXT:       put(buf, n, "next window"); return;
                case PROV_ACT_WIN_UP:         put(buf, n, "focus up");    return;
                case PROV_ACT_WIN_DOWN:       put(buf, n, "focus down");  return;
                case PROV_ACT_WIN_LEFT:       put(buf, n, "focus left");  return;
                case PROV_ACT_WIN_RIGHT:      put(buf, n, "focus right"); return;
                case PROV_ACT_PANE_RESIZE:    put(buf, n, "resize pane"); return;
                case PROV_ACT_PANE_READONLY:  put(buf, n, "read-only"); return;
                case PROV_ACT_PANE_HEX:       put(buf, n, "hex view"); return;
                case PROV_ACT_HELP:           put(buf, n, "help");       return;
                case PROV_ACT_BUFFER_LIST:    put(buf, n, "buffers");    return;
                case PROV_ACT_CMD_PROMPT:     put(buf, n, "command");    return;
                case PROV_ACT_INDENT:         put(buf, n, "indent");     return;
                case PROV_ACT_DEDENT:         put(buf, n, "dedent");     return;
                case PROV_ACT_SYNTAX:         put(buf, n, "syntax");     return;
                case PROV_ACT_READ_OPTS:      put(buf, n, "read opts");  return;
                case PROV_ACT_WRITE_OPTS:     put(buf, n, "write opts"); return;
                case PROV_ACT_CONFIG:         put(buf, n, "config");     return;
                case PROV_ACT_UNDO_BROWSER:   put(buf, n, "undo list");  return;
                case PROV_ACT_SEARCH_PROMPT:  put(buf, n, "search");      return;
                case PROV_ACT_SEARCH_PANEL:   put(buf, n, "find panel");  return;
                case PROV_ACT_SEARCH_WORD:    put(buf, n, "search word"); return;
                case PROV_ACT_SEARCH_NEXT:    put(buf, n, "search next"); return;
                case PROV_ACT_SEARCH_PREV:    put(buf, n, "search prev"); return;
                case PROV_ACT_REPLACE:        put(buf, n, "replace");     return;
                case PROV_ACT_SEARCH_HELP:    put(buf, n, "regex help"); return;
                case PROV_ACT_SEARCH_BROWSER: put(buf, n, "search list"); return;
                case PROV_ACT_REG_BROWSER:    put(buf, n, "registers");  return;
                case PROV_ACT_CMD_BROWSER:    put(buf, n, "command list"); return;
                case PROV_ACT_WIN_OVERVIEW:   put(buf, n, "pane overview"); return;
                case PROV_ACT_TAB_OVERVIEW:   put(buf, n, "tab overview"); return;
                case PROV_ACT_MACRO_OVERVIEW: put(buf, n, "macros");     return;
                case PROV_ACT_BOOKMARK_OVERVIEW: put(buf, n, "bookmarks"); return;
                case PROV_ACT_MOVE_HISTORY:   put(buf, n, "jump list");  return;
                default:                      put(buf, n, "action");     return;
            }
        case PROV_CMD_FINDCHAR:
            put(buf, n, c->target == PROV_TARGET_TILL ? "till char" : "find char");
            return;
        default:
            return;
    }
}
