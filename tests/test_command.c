/*
 * Unit tests for the deterministic zx command parser. Pure; no editor/terminal.
 * One main(), exit 0 == pass.
 */

#include <stdio.h>
#include <string.h>

#include "command.h"

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

/* Feed each byte of `keys`; return the final (latest) command result. */
static prov_command_t parse(const char *keys) {
    prov_cmd_parser_t p = {0};
    prov_command_t cmd = { .kind = PROV_CMD_INCOMPLETE, .count = 1 };
    for (const char *k = keys; *k; k++) {
        cmd = prov_cmd_feed(&p, (proven_u32)(unsigned char)*k);
    }
    return cmd;
}

int main(void) {
    prov_command_t c;

    /* ---- movement ---- */
    c = parse("i");
    CHECK(c.kind == PROV_CMD_MOVE && c.move == PROV_MOVE_UP && c.count == 1, "i = up");
    c = parse("k");
    CHECK(c.kind == PROV_CMD_MOVE && c.move == PROV_MOVE_DOWN, "k = down");
    c = parse("j");
    CHECK(c.kind == PROV_CMD_MOVE && c.move == PROV_MOVE_LEFT, "j = left");
    c = parse("l");
    CHECK(c.kind == PROV_CMD_MOVE && c.move == PROV_MOVE_RIGHT, "l = right");

    /* ---- counts ---- */
    c = parse("5i");
    CHECK(c.kind == PROV_CMD_MOVE && c.move == PROV_MOVE_UP && c.count == 5, "5i = up x5");
    c = parse("10k");
    CHECK(c.kind == PROV_CMD_MOVE && c.move == PROV_MOVE_DOWN && c.count == 10, "10k = down x10");

    /* ---- single-key edits ---- */
    c = parse("u"); CHECK(c.kind == PROV_CMD_EDIT && c.edit == PROV_EDIT_UNDO, "u = undo");
    c = parse("p"); CHECK(c.kind == PROV_CMD_EDIT && c.edit == PROV_EDIT_PASTE, "p = paste");
    c = parse("v"); CHECK(c.kind == PROV_CMD_EDIT && c.edit == PROV_EDIT_TOGGLE_SELECT, "v = select");
    c = parse("x"); CHECK(c.kind == PROV_CMD_EDIT && c.edit == PROV_EDIT_CUT, "x = cut");
    c = parse("r"); CHECK(c.kind == PROV_CMD_EDIT && c.edit == PROV_EDIT_REPLACE_CHAR, "r = replace");
    c = parse("a"); CHECK(c.kind == PROV_CMD_EDIT && c.edit == PROV_EDIT_APPEND, "a = field");
    c = parse("n"); CHECK(c.kind == PROV_CMD_EDIT && c.edit == PROV_EDIT_REPEAT, "n = repeat");
    c = parse("3n"); CHECK(c.kind == PROV_CMD_EDIT && c.edit == PROV_EDIT_REPEAT && c.count == 3, "3n = repeat x3");
    c = parse("q"); CHECK(c.kind == PROV_CMD_EDIT && c.edit == PROV_EDIT_MACRO, "q = macro");
    c = parse("3x");
    CHECK(c.kind == PROV_CMD_EDIT && c.edit == PROV_EDIT_CUT && c.count == 3, "3x = cut x3");

    /* ---- operators: incomplete then linewise ---- */
    c = parse("d");  CHECK(c.kind == PROV_CMD_INCOMPLETE, "d alone incomplete");
    c = parse("dd");
    CHECK(c.kind == PROV_CMD_OPERATION && c.op == PROV_OP_DELETE &&
          c.target == PROV_TARGET_LINEWISE && c.count == 1, "dd = delete line");
    c = parse("cc");
    CHECK(c.kind == PROV_CMD_OPERATION && c.op == PROV_OP_CHANGE &&
          c.target == PROV_TARGET_LINEWISE, "cc = change line");
    c = parse("yy");
    CHECK(c.kind == PROV_CMD_OPERATION && c.op == PROV_OP_YANK &&
          c.target == PROV_TARGET_LINEWISE, "yy = yank line");
    c = parse("2dd");
    CHECK(c.kind == PROV_CMD_OPERATION && c.op == PROV_OP_DELETE &&
          c.target == PROV_TARGET_LINEWISE && c.count == 2, "2dd = delete 2 lines");

    /* ---- operators: word motions ---- */
    c = parse("dw");
    CHECK(c.kind == PROV_CMD_OPERATION && c.op == PROV_OP_DELETE &&
          c.target == PROV_TARGET_WORD, "dw = delete word");
    c = parse("cb");
    CHECK(c.kind == PROV_CMD_OPERATION && c.op == PROV_OP_CHANGE &&
          c.target == PROV_TARGET_BACK, "cb = change back");
    c = parse("ye");
    CHECK(c.kind == PROV_CMD_OPERATION && c.op == PROV_OP_YANK &&
          c.target == PROV_TARGET_END, "ye = yank to end");

    /* ---- operators: line-end / match / find / till motions ---- */
    c = parse("dl");
    CHECK(c.kind == PROV_CMD_OPERATION && c.op == PROV_OP_DELETE &&
          c.target == PROV_TARGET_LINE_END, "dl = delete to line end");
    c = parse("cm");
    CHECK(c.kind == PROV_CMD_OPERATION && c.op == PROV_OP_CHANGE &&
          c.target == PROV_TARGET_MATCH, "cm = change to match");
    c = parse("df");  CHECK(c.kind == PROV_CMD_INCOMPLETE, "df awaits char");
    c = parse("df,");
    CHECK(c.kind == PROV_CMD_OPERATION && c.op == PROV_OP_DELETE &&
          c.target == PROV_TARGET_FIND && c.param == ',', "df, = delete find ','");
    c = parse("ct)");
    CHECK(c.kind == PROV_CMD_OPERATION && c.op == PROV_OP_CHANGE &&
          c.target == PROV_TARGET_TILL && c.param == ')', "ct) = change till ')'");
    c = parse("2dfa");
    CHECK(c.kind == PROV_CMD_OPERATION && c.target == PROV_TARGET_FIND &&
          c.param == 'a' && c.count == 2, "2dfa = delete find 'a' x2");

    /* ---- operators: text objects ---- */
    c = parse("di");  CHECK(c.kind == PROV_CMD_INCOMPLETE, "di incomplete");
    c = parse("diw");
    CHECK(c.kind == PROV_CMD_OPERATION && c.op == PROV_OP_DELETE &&
          c.target == PROV_TARGET_TEXTOBJ && c.inner && c.textobj == PROV_TOBJ_WORD,
          "diw = delete inner word");
    c = parse("daw");
    CHECK(c.kind == PROV_CMD_OPERATION && !c.inner && c.textobj == PROV_TOBJ_WORD,
          "daw = delete around word");
    c = parse("ci\"");
    CHECK(c.kind == PROV_CMD_OPERATION && c.op == PROV_OP_CHANGE && c.inner &&
          c.textobj == PROV_TOBJ_DQUOTE, "ci\" = change inner dquote");
    c = parse("ya(");
    CHECK(c.kind == PROV_CMD_OPERATION && c.op == PROV_OP_YANK && !c.inner &&
          c.textobj == PROV_TOBJ_PAREN, "ya( = yank around paren");
    c = parse("di)");
    CHECK(c.kind == PROV_CMD_OPERATION && c.inner && c.textobj == PROV_TOBJ_PAREN,
          "di) = inner paren via closing");
    c = parse("yi{");
    CHECK(c.kind == PROV_CMD_OPERATION && c.textobj == PROV_TOBJ_BRACE, "yi{ brace");
    c = parse("ca]");
    CHECK(c.kind == PROV_CMD_OPERATION && !c.inner && c.textobj == PROV_TOBJ_BRACKET, "ca] bracket");
    c = parse("di<");
    CHECK(c.kind == PROV_CMD_OPERATION && c.textobj == PROV_TOBJ_ANGLE, "di< angle");
    c = parse("dit");
    CHECK(c.kind == PROV_CMD_OPERATION && c.inner && c.textobj == PROV_TOBJ_TAG, "dit tag");
    c = parse("dap");
    CHECK(c.kind == PROV_CMD_OPERATION && !c.inner && c.textobj == PROV_TOBJ_PARAGRAPH, "dap paragraph");
    c = parse("3yw");
    CHECK(c.kind == PROV_CMD_OPERATION && c.op == PROV_OP_YANK &&
          c.target == PROV_TARGET_WORD && c.count == 3, "3yw = yank 3 words");

    /* ---- namespaces: g ---- */
    c = parse("g");  CHECK(c.kind == PROV_CMD_INCOMPLETE, "g alone incomplete");
    c = parse("gg");
    CHECK(c.kind == PROV_CMD_ACTION && c.action == PROV_ACT_DOC_START, "gg = doc start");
    c = parse("ge");
    CHECK(c.kind == PROV_CMD_ACTION && c.action == PROV_ACT_DOC_END, "ge = doc end");
    /* [N]g shorthand: a count + a single g jumps immediately (RFC 0002). */
    c = parse("1g");
    CHECK(c.kind == PROV_CMD_ACTION && c.action == PROV_ACT_GOTO_LINE && c.count == 1,
          "1g = goto line 1");
    c = parse("5g");
    CHECK(c.kind == PROV_CMD_ACTION && c.action == PROV_ACT_GOTO_LINE && c.count == 5,
          "5g = goto line 5");
    c = parse("120g");
    CHECK(c.kind == PROV_CMD_ACTION && c.action == PROV_ACT_GOTO_LINE && c.count == 120,
          "120g = goto line 120");
    /* With a count the first g fires goto; a trailing g starts a fresh pending
     * namespace (so 5gg = goto-5 then an incomplete g). */
    c = parse("5gg");
    CHECK(c.kind == PROV_CMD_INCOMPLETE, "5gg = goto then a new pending g");
    /* 0g stays the leading-0 special prefix (file last line), not a count. */
    c = parse("0g");
    CHECK(c.kind == PROV_CMD_ACTION && c.action == PROV_ACT_FILE_LAST_LINE, "0g = last line");
    c = parse("gp"); CHECK(c.action == PROV_ACT_PREV_WORD, "gp");
    c = parse("gn"); CHECK(c.action == PROV_ACT_NEXT_WORD, "gn");
    c = parse("gf"); CHECK(c.action == PROV_ACT_LINE_START, "gf");
    c = parse("gl"); CHECK(c.action == PROV_ACT_LINE_END, "gl");
    c = parse("gu"); CHECK(c.action == PROV_ACT_HALF_PAGE_UP, "gu");
    c = parse("gd"); CHECK(c.action == PROV_ACT_HALF_PAGE_DOWN, "gd");

    /* ---- namespaces: o ---- */
    c = parse("on"); CHECK(c.kind == PROV_CMD_ACTION && c.action == PROV_ACT_OPEN_BELOW, "on");
    c = parse("op"); CHECK(c.action == PROV_ACT_OPEN_ABOVE, "op");

    /* ---- namespaces: z (meta) ---- */
    c = parse("z");  CHECK(c.kind == PROV_CMD_INCOMPLETE, "z alone incomplete");
    c = parse("zx"); CHECK(c.action == PROV_ACT_RETURN_ED, "zx = return to Ed");
    c = parse("zy"); CHECK(c.action == PROV_ACT_REDO, "zy = redo");
    c = parse("zw"); CHECK(c.action == PROV_ACT_WRITE_FILE, "zw = write");
    c = parse("zo"); CHECK(c.action == PROV_ACT_OPEN_FILE, "zo = open");
    c = parse("za"); CHECK(c.action == PROV_ACT_WRITE_AS, "za = write as");
    c = parse("zq"); CHECK(c.action == PROV_ACT_QUIT, "zq = quit");
    c = parse("zi"); CHECK(c.action == PROV_ACT_INDENT, "zi = indent");
    c = parse("zc"); CHECK(c.action == PROV_ACT_CONFIG, "zc = config");

    /* ---- 0 special prefix ---- */
    c = parse("0");  CHECK(c.kind == PROV_CMD_INCOMPLETE, "0 alone incomplete");
    c = parse("0u"); CHECK(c.kind == PROV_CMD_ACTION && c.action == PROV_ACT_UNDO_BROWSER, "0u");
    c = parse("0g"); CHECK(c.action == PROV_ACT_FILE_LAST_LINE, "0g = file last line");
    c = parse("0w"); CHECK(c.action == PROV_ACT_WIN_OVERVIEW, "0w");

    /* ---- bookmarks (M4.3): m<letter> jump (quick); 0m opens the panel (RFC-0010) ---- */
    c = parse("0m"); CHECK(c.kind == PROV_CMD_ACTION && c.action == PROV_ACT_BOOKMARK_OVERVIEW, "0m = bookmark panel");
    c = parse("m"); CHECK(c.kind == PROV_CMD_INCOMPLETE, "m awaits a slot letter");
    c = parse("mz"); CHECK(c.kind == PROV_CMD_BOOKMARK_JUMP && c.param == 'z', "mz = jump to mark z");
    c = parse("m1"); CHECK(c.kind == PROV_CMD_INVALID, "m1 (non-letter) invalid");

    /* ---- registers (M4.2) + macros (M4.6): e<letter> run (quick); 0e opens the panel ---- */
    c = parse("bax"); CHECK(c.kind == PROV_CMD_REGISTER && c.param == 'a' && c.param2 == 'x', "bax = cut to reg a");
    c = parse("b2v"); CHECK(c.kind == PROV_CMD_REGISTER && c.param == '2' && c.param2 == 'v', "b2v = paste reg 2");
    c = parse("baq"); CHECK(c.kind == PROV_CMD_INVALID, "ba<bad-op> invalid");
    c = parse("0e"); CHECK(c.kind == PROV_CMD_ACTION && c.action == PROV_ACT_MACRO_OVERVIEW, "0e = macro panel");
    c = parse("ea"); CHECK(c.kind == PROV_CMD_MACRO_EXEC && c.param == 'a', "ea = run macro a");
    c = parse("e"); CHECK(c.kind == PROV_CMD_INCOMPLETE, "e awaits a slot");
    c = parse("E"); CHECK(c.kind == PROV_CMD_MACRO_LAST, "E = replay last");
    c = parse("3E"); CHECK(c.kind == PROV_CMD_MACRO_LAST && c.count == 3, "3E = replay last x3");

    /* ---- invalid ---- */
    c = parse("dx");
    CHECK(c.kind == PROV_CMD_INVALID, "dx invalid target");
    c = parse("d!");
    CHECK(c.kind == PROV_CMD_INVALID, "d! invalid");
    c = parse("gz"); CHECK(c.kind == PROV_CMD_INVALID, "gz invalid g-subkey");
    c = parse("zk"); CHECK(c.kind == PROV_CMD_INVALID, "zk invalid z-subkey");
    c = parse("0q"); CHECK(c.kind == PROV_CMD_INVALID, "0q invalid 0-subkey");

    /* ---- status-bar describe / label (RFC 0002) ---- */
    {
        char buf[128];
        prov_cmd_parser_t p = {0};
        prov_cmd_feed(&p, 'd');                       /* operator pending */
        prov_cmd_describe(&p, buf, sizeof buf);
        CHECK(strstr(buf, "delete") != NULL, "describe d = delete targets");

        p = (prov_cmd_parser_t){0};
        prov_cmd_feed(&p, '1'); prov_cmd_feed(&p, '2');  /* count pending */
        prov_cmd_describe(&p, buf, sizeof buf);
        CHECK(strstr(buf, "line") != NULL, "describe count mentions goto line");

        p = (prov_cmd_parser_t){0};
        prov_cmd_feed(&p, 'g');                       /* g namespace (no count) */
        prov_cmd_describe(&p, buf, sizeof buf);
        CHECK(strstr(buf, "goto") != NULL, "describe g = goto namespace");

        c = parse("12g");
        prov_cmd_label(&c, buf, sizeof buf);
        CHECK(strstr(buf, "goto line") != NULL, "label 12g = goto line");
        c = parse("dw");
        prov_cmd_label(&c, buf, sizeof buf);
        CHECK(strstr(buf, "del") && strstr(buf, "word"), "label dw = del word");
        c = parse("yy");
        prov_cmd_label(&c, buf, sizeof buf);
        CHECK(strstr(buf, "yank") && strstr(buf, "line"), "label yy = yank line");

        /* pending count (consumed by arrow/page keys in the event loop) */
        p = (prov_cmd_parser_t){0};
        prov_cmd_feed(&p, '5');
        CHECK(prov_cmd_pending_count(&p) == 5, "bare count 5 pending");
        prov_cmd_feed(&p, '2');
        CHECK(prov_cmd_pending_count(&p) == 52, "bare count 52 pending");
        prov_cmd_feed(&p, 'd');                       /* operator now pending */
        CHECK(prov_cmd_pending_count(&p) == 0, "no bare count once operator pending");
        p = (prov_cmd_parser_t){0};
        CHECK(prov_cmd_pending_count(&p) == 0, "no count when idle");
    }

    /* ---- standalone find f<char> and ; , repeat ---- */
    c = parse("fx");
    CHECK(c.kind == PROV_CMD_FINDCHAR && c.param == 'x' && c.target == PROV_TARGET_FIND,
          "fx = find char x");
    c = parse("3fx");
    CHECK(c.kind == PROV_CMD_FINDCHAR && c.count == 3, "3fx = find x x3");
    c = parse(";");
    CHECK(c.kind == PROV_CMD_ACTION && c.action == PROV_ACT_FIND_NEXT, "; = repeat find");
    c = parse(",");
    CHECK(c.kind == PROV_CMD_ACTION && c.action == PROV_ACT_FIND_PREV, ", = reverse find");
    /* operator find/till are unchanged (PS_OPERATOR, not standalone) */
    c = parse("dfx");
    CHECK(c.kind == PROV_CMD_OPERATION && c.op == PROV_OP_DELETE &&
          c.target == PROV_TARGET_FIND && c.param == 'x', "dfx still operator find");
    c = parse("dtx");
    CHECK(c.kind == PROV_CMD_OPERATION && c.target == PROV_TARGET_TILL && c.param == 'x',
          "dtx still operator till");

    /* ---- t namespace (tabs), mirroring g ---- */
    c = parse("tn"); CHECK(c.kind == PROV_CMD_ACTION && c.action == PROV_ACT_TAB_NEW,   "tn = new tab");
    c = parse("tq"); CHECK(c.action == PROV_ACT_TAB_CLOSE, "tq = close tab");
    c = parse("tj"); CHECK(c.action == PROV_ACT_TAB_PREV,  "tj = prev tab");
    c = parse("tl"); CHECK(c.action == PROV_ACT_TAB_NEXT,  "tl = next tab");
    c = parse("2t");
    CHECK(c.kind == PROV_CMD_ACTION && c.action == PROV_ACT_GOTO_TAB && c.count == 2, "2t = goto tab 2");
    c = parse("tx"); CHECK(c.kind == PROV_CMD_INVALID, "tx invalid (t is the tab namespace)");

    /* ---- w namespace (panes) ---- */
    c = parse("wh"); CHECK(c.kind == PROV_CMD_ACTION && c.action == PROV_ACT_PANE_HSPLIT, "wh = hsplit");
    c = parse("wv"); CHECK(c.action == PROV_ACT_PANE_VSPLIT, "wv = vsplit");
    c = parse("wq"); CHECK(c.action == PROV_ACT_PANE_CLOSE, "wq = close");
    c = parse("ww"); CHECK(c.kind == PROV_CMD_INVALID, "ww removed (use 0w panel)");
    c = parse("wp"); CHECK(c.action == PROV_ACT_WIN_PREV, "wp = prev window");
    c = parse("wn"); CHECK(c.action == PROV_ACT_WIN_NEXT, "wn = next window");
    c = parse("wi"); CHECK(c.action == PROV_ACT_WIN_UP, "wi = focus up");
    c = parse("wk"); CHECK(c.action == PROV_ACT_WIN_DOWN, "wk = focus down");
    c = parse("wj"); CHECK(c.action == PROV_ACT_WIN_LEFT, "wj = focus left");
    c = parse("wl"); CHECK(c.action == PROV_ACT_WIN_RIGHT, "wl = focus right");
    c = parse("ws"); CHECK(c.action == PROV_ACT_PANE_RESIZE, "ws = resize");
    c = parse("wr"); CHECK(c.action == PROV_ACT_PANE_READONLY, "wr = read-only");
    c = parse("wx"); CHECK(c.action == PROV_ACT_PANE_HEX, "wx = hex view/edit");
    c = parse("wz"); CHECK(c.kind == PROV_CMD_INVALID, "wz invalid w-subkey");

    if (failures) {
        fprintf(stderr, "command: %d checks failed\n", failures);
        return 1;
    }
    printf("ok: command tests passed\n");
    return 0;
}
