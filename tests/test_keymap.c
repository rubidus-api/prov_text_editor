/* RFC-0010 S0 — shared keymap engine tests. */

#include <stdio.h>
#include <string.h>

#include "keymap.h"

static int g_fail;
static prov_keymap_parser_t P;

static prov_key_t KC(char c) { prov_key_t k = { .kind = PROV_KEY_CHAR, .cp = (proven_u32)(unsigned char)c, .nbytes = 1 }; k.bytes[0] = (proven_u8)c; return k; }
static prov_key_t KK(prov_key_kind_t kind) { prov_key_t k = { .kind = kind }; return k; }

#define EXPECT(c, m) do { if (!(c)) { printf("FAIL %s\n", m); g_fail++; } } while (0)

/* reset, feed each char of `s`, return the last result */
static prov_pk_t run(const char *s, const prov_keymap_t *v) {
    P = (prov_keymap_parser_t){0};
    prov_pk_t r = { .kind = PK_NONE };
    for (; *s; s++) r = prov_keymap_feed(&P, KC(*s), v);
    return r;
}

int main(void) {
    static const prov_keymap_entry_t verbs[] = { {'e', 100, "open"}, {'r', 101, "rename"} };
    prov_keymap_t vt = { verbs, 2 };

    /* nav decode */
    EXPECT(prov_nav_decode(KC('k')) == NAV_DOWN, "k=down");
    EXPECT(prov_nav_decode(KC('i')) == NAV_UP, "i=up");
    EXPECT(prov_nav_decode(KC('j')) == NAV_PGUP, "j=pgup");
    EXPECT(prov_nav_decode(KC('l')) == NAV_PGDN, "l=pgdn");
    EXPECT(prov_nav_decode(KK(PROV_KEY_LEFT)) == NAV_PGUP, "Left=pgup");
    EXPECT(prov_nav_decode(KK(PROV_KEY_RIGHT)) == NAV_PGDN, "Right=pgdn");
    EXPECT(prov_nav_decode(KK(PROV_KEY_PAGEUP)) == NAV_PGUP, "PgUp");
    EXPECT(prov_nav_decode(KC('x')) == NAV_NONE, "x=none");

    /* movement + count */
    prov_pk_t r = run("k", &vt);  EXPECT(r.kind == PK_MOVE && r.dir == NAV_DOWN && r.count == 1, "k move 1");
    r = run("5k", &vt);           EXPECT(r.kind == PK_MOVE && r.dir == NAV_DOWN && r.count == 5, "5k move 5");
    r = run("12i", &vt);          EXPECT(r.kind == PK_MOVE && r.dir == NAV_UP && r.count == 12, "12i move 12");

    /* count + arrow key */
    P = (prov_keymap_parser_t){0};
    prov_keymap_feed(&P, KC('3'), &vt);
    r = prov_keymap_feed(&P, KK(PROV_KEY_DOWN), &vt);
    EXPECT(r.kind == PK_MOVE && r.dir == NAV_DOWN && r.count == 3, "3<Down> move 3");

    /* goto */
    r = run("g", &vt);   EXPECT(r.kind == PK_GOTO && r.index == 1, "g goto 1");
    r = run("3g", &vt);  EXPECT(r.kind == PK_GOTO && r.index == 3, "3g goto 3");
    r = run("0g", &vt);  EXPECT(r.kind == PK_GOTO && r.index == 0, "0g goto last");

    /* search namespace */
    r = run("ss", &vt);  EXPECT(r.kind == PK_SEARCH, "ss search");
    r = run("sx", &vt);  EXPECT(r.kind == PK_INVALID, "sx invalid");
    P = (prov_keymap_parser_t){0};
    EXPECT(prov_keymap_feed(&P, KC('s'), &vt).kind == PK_NONE, "s pending");

    /* verbs + footer + control */
    r = run("e", &vt);  EXPECT(r.kind == PK_VERB && r.action == 100, "e verb open");
    r = run("r", &vt);  EXPECT(r.kind == PK_VERB && r.action == 101, "r verb rename");
    r = run("z", &vt);  EXPECT(r.kind == PK_INVALID, "z unbound");
    r = run("o", &vt);  EXPECT(r.kind == PK_OK, "o ok");
    r = run("c", &vt);  EXPECT(r.kind == PK_CANCEL, "c cancel");
    r = run("d", &vt);  EXPECT(r.kind == PK_DISCARD, "d discard");
    r = run("h", &vt);  EXPECT(r.kind == PK_HELP, "h help");
    r = run("w", &vt);  EXPECT(r.kind == PK_CYCLE, "w cycle");

    /* special keys */
    P = (prov_keymap_parser_t){0}; EXPECT(prov_keymap_feed(&P, KK(PROV_KEY_ENTER), &vt).kind == PK_ACTIVATE, "Enter activate");
    r = run(" ", &vt);  EXPECT(r.kind == PK_ACTIVATE, "Space activate");
    P = (prov_keymap_parser_t){0}; EXPECT(prov_keymap_feed(&P, KK(PROV_KEY_ESC), &vt).kind == PK_CLOSE, "Esc close");

    /* a pending count is discarded by a non-count/non-motion key */
    r = run("5e", &vt); EXPECT(r.kind == PK_VERB && r.action == 100, "5e: count discarded, verb fires");

    /* digits always build a count consumed by g (no digit-only quick-pick) */
    P = (prov_keymap_parser_t){0};
    r = prov_keymap_feed(&P, KC('1'), &vt); EXPECT(r.kind == PK_NONE, "1 pends a count");
    r = prov_keymap_feed(&P, KC('2'), &vt); EXPECT(r.kind == PK_NONE, "12 pends a count");
    r = prov_keymap_feed(&P, KC('g'), &vt); EXPECT(r.kind == PK_GOTO && r.index == 12, "12g = item 12");

    /* legend includes the verbs */
    char leg[256]; prov_keymap_legend(&vt, leg, sizeof leg);
    EXPECT(strstr(leg, "e:open") && strstr(leg, "r:rename") && strstr(leg, "h:help"), "legend has verbs");

    if (g_fail) { printf("test_keymap: %d FAILED\n", g_fail); return 1; }
    printf("test_keymap: OK\n");
    return 0;
}
