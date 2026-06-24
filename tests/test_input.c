/*
 * Unit tests for the pure key decoder. One main(), exit 0 == pass.
 */

#include <stdio.h>
#include <string.h>

#include "input.h"

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static const proven_u8 *U(const char *s) { return (const proven_u8 *)s; }

/* decode a NUL-terminated literal (length excludes the terminator) */
static prov_key_t K(const char *s, proven_size_t len) {
    return prov_decode_key(U(s), len);
}

int main(void) {
    prov_key_t k;

    /* ---- empty / incomplete ---- */
    k = K("", 0);
    CHECK(k.kind == PROV_KEY_NONE && k.consumed == 0, "empty -> none");
    k = K("\x1B[", 2);
    CHECK(k.kind == PROV_KEY_NONE && k.consumed == 0, "incomplete CSI -> none");
    k = K("\x1B[3", 3);
    CHECK(k.kind == PROV_KEY_NONE && k.consumed == 0, "incomplete CSI num -> none");

    /* ---- plain chars ---- */
    k = K("a", 1);
    CHECK(k.kind == PROV_KEY_CHAR && k.cp == 'a' && k.nbytes == 1 &&
          k.consumed == 1 && k.bytes[0] == 'a', "char a");
    k = K("\xC3\xA9", 2);                              /* é */
    CHECK(k.kind == PROV_KEY_CHAR && k.cp == 0xE9 && k.nbytes == 2 &&
          k.consumed == 2, "char é");
    k = K("\xC3", 1);                                  /* truncated lead */
    CHECK(k.kind == PROV_KEY_NONE && k.consumed == 0, "truncated utf8 -> none");

    /* ---- named keys ---- */
    k = K("\r", 1);  CHECK(k.kind == PROV_KEY_ENTER, "CR enter");
    k = K("\n", 1);  CHECK(k.kind == PROV_KEY_ENTER, "LF enter");
    k = K("\t", 1);  CHECK(k.kind == PROV_KEY_TAB, "tab");
    k = K("\x7F", 1); CHECK(k.kind == PROV_KEY_BACKSPACE, "DEL backspace");
    k = K("\x08", 1); CHECK(k.kind == PROV_KEY_BACKSPACE, "BS backspace");

    /* ---- ctrl combos ---- */
    k = K("\x13", 1);
    CHECK(k.kind == PROV_KEY_CTRL && k.cp == 's', "ctrl-S");
    k = K("\x1A", 1);
    CHECK(k.kind == PROV_KEY_CTRL && k.cp == 'z', "ctrl-Z");
    k = K("\x11", 1);
    CHECK(k.kind == PROV_KEY_CTRL && k.cp == 'q', "ctrl-Q");

    /* ---- escape sequences (CSI) ---- */
    k = K("\x1B[A", 3); CHECK(k.kind == PROV_KEY_UP && k.consumed == 3, "up");
    k = K("\x1B[B", 3); CHECK(k.kind == PROV_KEY_DOWN, "down");
    k = K("\x1B[C", 3); CHECK(k.kind == PROV_KEY_RIGHT, "right");
    k = K("\x1B[D", 3); CHECK(k.kind == PROV_KEY_LEFT, "left");
    k = K("\x1B[H", 3); CHECK(k.kind == PROV_KEY_HOME, "home H");
    k = K("\x1B[F", 3); CHECK(k.kind == PROV_KEY_END, "end F");
    k = K("\x1B[3~", 4);
    CHECK(k.kind == PROV_KEY_DELETE && k.consumed == 4, "delete 3~");
    k = K("\x1B[5~", 4); CHECK(k.kind == PROV_KEY_PAGEUP, "pageup 5~");
    k = K("\x1B[6~", 4); CHECK(k.kind == PROV_KEY_PAGEDOWN, "pagedown 6~");
    k = K("\x1B[1~", 4); CHECK(k.kind == PROV_KEY_HOME, "home 1~");
    k = K("\x1B[4~", 4); CHECK(k.kind == PROV_KEY_END, "end 4~");

    /* SS3 arrows (application keypad mode) */
    k = K("\x1BOA", 3); CHECK(k.kind == PROV_KEY_UP, "SS3 up");

    /* ---- modified keys (Shift/Ctrl + arrows/Home/End) ---- */
    k = K("\x1B[1;2C", 6);
    CHECK(k.kind == PROV_KEY_RIGHT && k.shift && !k.ctrl && k.consumed == 6, "shift-right");
    k = K("\x1B[1;2D", 6);
    CHECK(k.kind == PROV_KEY_LEFT && k.shift, "shift-left");
    k = K("\x1B[1;2A", 6); CHECK(k.kind == PROV_KEY_UP && k.shift, "shift-up");
    k = K("\x1B[1;5D", 6);
    CHECK(k.kind == PROV_KEY_LEFT && k.ctrl && !k.shift, "ctrl-left");
    k = K("\x1B[1;2H", 6); CHECK(k.kind == PROV_KEY_HOME && k.shift, "shift-home");
    k = K("\x1B[3;2~", 6); CHECK(k.kind == PROV_KEY_DELETE && k.shift, "shift-delete");
    k = K("\x1B[6;5~", 6); CHECK(k.kind == PROV_KEY_PAGEDOWN && k.ctrl, "ctrl-pagedown");

    /* plain arrows carry no modifiers (regression) */
    k = K("\x1B[C", 3);
    CHECK(k.kind == PROV_KEY_RIGHT && !k.shift && !k.ctrl, "plain right no mods");

    /* ---- robustness: unrecognized but complete CSI is consumed, never jams ---- */
    k = K("\x1B[1;2X", 6);
    CHECK(k.kind == PROV_KEY_ESC && k.consumed == 6, "unknown final consumed");
    k = K("\x1B[200~", 6);
    CHECK(k.kind == PROV_KEY_ESC && k.consumed == 6, "unknown ~ consumed");
    k = K("\x1B[1;2", 5);
    CHECK(k.kind == PROV_KEY_NONE && k.consumed == 0, "incomplete CSI needs more");

    /* ---- lone escape ---- */
    k = K("\x1B", 1);
    CHECK(k.kind == PROV_KEY_ESC && k.consumed == 1, "lone ESC");

    /* ---- Shift+Tab (CSI Z), and modified arrows used by the line editor ---- */
    k = K("\x1B[Z", 3);
    CHECK(k.kind == PROV_KEY_TAB && k.shift, "Shift+Tab (back-tab)");
    k = K("\x1B[1;5C", 6);
    CHECK(k.kind == PROV_KEY_RIGHT && k.ctrl && !k.shift, "Ctrl+Right (word)");
    k = K("\x1B[1;2D", 6);
    CHECK(k.kind == PROV_KEY_LEFT && k.shift && !k.ctrl, "Shift+Left (select)");
    k = K("\x1B[1;6C", 6);
    CHECK(k.kind == PROV_KEY_RIGHT && k.ctrl && k.shift, "Ctrl+Shift+Right (word-select)");

    /* ---- SGR mouse (RFC-0014): ESC [ < Cb ; Cx ; Cy (M|m) ---- */
    #define MK(s) K((s), strlen(s))
    k = MK("\x1B[<0;10;5M");
    CHECK(k.kind == PROV_KEY_MOUSE && k.mbtn == PROV_MB_LEFT && k.mact == PROV_ME_PRESS
          && k.mcol == 9 && k.mrow == 4 && k.consumed == 10, "mouse left press @ (4,9)");
    k = MK("\x1B[<0;10;5m");
    CHECK(k.kind == PROV_KEY_MOUSE && k.mbtn == PROV_MB_LEFT && k.mact == PROV_ME_RELEASE,
          "mouse left release");
    k = MK("\x1B[<2;3;7M");
    CHECK(k.kind == PROV_KEY_MOUSE && k.mbtn == PROV_MB_RIGHT && k.mcol == 2 && k.mrow == 6,
          "mouse right press");
    k = MK("\x1B[<32;4;4M");
    CHECK(k.kind == PROV_KEY_MOUSE && k.mbtn == PROV_MB_LEFT && k.mact == PROV_ME_DRAG,
          "mouse left drag (bit5)");
    k = MK("\x1B[<64;1;1M");
    CHECK(k.kind == PROV_KEY_MOUSE && k.mbtn == PROV_MB_WHEEL_UP && k.mact == PROV_ME_PRESS,
          "wheel up");
    k = MK("\x1B[<65;1;1M");
    CHECK(k.kind == PROV_KEY_MOUSE && k.mbtn == PROV_MB_WHEEL_DOWN, "wheel down");
    k = MK("\x1B[<16;2;2M");             /* Ctrl modifier (bit 16) on a left press */
    CHECK(k.kind == PROV_KEY_MOUSE && k.ctrl && !k.shift && !k.alt, "mouse ctrl modifier");
    k = MK("\x1B[<0;10;5");              /* missing final M/m */
    CHECK(k.kind == PROV_KEY_NONE && k.consumed == 0, "incomplete mouse needs more");
    k = MK("\x1B[<");                    /* prefix only */
    CHECK(k.kind == PROV_KEY_NONE && k.consumed == 0, "bare SGR prefix needs more");
    #undef MK

    if (failures) {
        fprintf(stderr, "input: %d checks failed\n", failures);
        return 1;
    }
    printf("ok: input tests passed\n");
    return 0;
}
