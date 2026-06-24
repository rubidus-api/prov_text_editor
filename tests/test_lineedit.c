/* Unit tests for the reusable single-line text-input control (RFC-0015). */
#include <stdio.h>
#include <string.h>

#include "lineedit.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } } while (0)

static void ins(prov_lineedit_t *le, const char *s) { prov_le_insert(le, s, strlen(s)); }

int main(void) {
    prov_lineedit_t le;
    prov_le_clear(&le);

    /* type, cursor at end */
    ins(&le, "abc");
    CHECK(strcmp(le.buf, "abc") == 0 && le.cur == 3, "type abc");

    /* Home / Left / insert at cursor */
    prov_le_move(&le, PROV_LE_HOME, false);
    CHECK(le.cur == 0, "home");
    ins(&le, "X");
    CHECK(strcmp(le.buf, "Xabc") == 0 && le.cur == 1, "insert at start");
    prov_le_move(&le, PROV_LE_END, false);
    CHECK(le.cur == 4, "end");

    /* Left twice, Delete (removes char at cursor), Backspace */
    prov_le_move(&le, PROV_LE_LEFT, false);     /* between b and c */
    prov_le_move(&le, PROV_LE_LEFT, false);     /* between a and b */
    prov_le_delete(&le);                        /* delete 'b' -> "Xac" */
    CHECK(strcmp(le.buf, "Xac") == 0 && le.cur == 2, "delete mid");
    prov_le_backspace(&le);                     /* delete 'a' -> "Xc" */
    CHECK(strcmp(le.buf, "Xc") == 0 && le.cur == 1, "backspace mid");

    /* multibyte: cursor moves by whole code points */
    prov_le_clear(&le);
    ins(&le, "a\xea\xb0\x80z");                 /* a 가 z  (가 = 3 bytes) */
    CHECK(le.len == 5 && le.cur == 5, "multibyte length");
    prov_le_move(&le, PROV_LE_LEFT, false);     /* before z */
    prov_le_move(&le, PROV_LE_LEFT, false);     /* before 가 */
    CHECK(le.cur == 1, "left over wide char (whole cp)");
    prov_le_backspace(&le);                     /* delete 'a' */
    CHECK(strcmp(le.buf, "\xea\xb0\x80z") == 0 && le.cur == 0, "backspace before wide char");

    /* selection: Shift-move, range, replace-on-insert, delete-sel */
    prov_le_clear(&le);
    ins(&le, "hello");
    prov_le_move(&le, PROV_LE_HOME, false);
    prov_le_move(&le, PROV_LE_RIGHT, true);     /* select 'h' */
    prov_le_move(&le, PROV_LE_RIGHT, true);     /* select 'he' */
    CHECK(prov_le_has_sel(&le), "has selection");
    proven_size_t a, b; prov_le_sel_range(&le, &a, &b);
    CHECK(a == 0 && b == 2, "sel range 0..2");
    ins(&le, "HE");                             /* replace selection */
    CHECK(strcmp(le.buf, "HEllo") == 0 && !prov_le_has_sel(&le), "replace selection");
    prov_le_move(&le, PROV_LE_END, true);       /* select 'llo' from cursor (at 2) */
    prov_le_delete_sel(&le);                    /* cut path */
    CHECK(strcmp(le.buf, "HE") == 0, "delete selection (cut)");

    /* capacity clamp doesn't corrupt */
    prov_le_clear(&le);
    char big[PROV_LE_CAP + 50]; memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = '\0';
    ins(&le, big);
    CHECK(le.len < PROV_LE_CAP && le.buf[le.len] == '\0', "capacity clamp");

    /* word movement: Ctrl+Left/Right hop whole words */
    prov_le_clear(&le);
    ins(&le, "foo bar baz");                    /* cursor at end (11) */
    prov_le_move(&le, PROV_LE_WORD_LEFT, false);  CHECK(le.cur == 8, "word-left to 'baz'");
    prov_le_move(&le, PROV_LE_WORD_LEFT, false);  CHECK(le.cur == 4, "word-left to 'bar'");
    prov_le_move(&le, PROV_LE_WORD_RIGHT, false); CHECK(le.cur == 8, "word-right to 'baz'");
    prov_le_move(&le, PROV_LE_HOME, false);
    prov_le_move(&le, PROV_LE_WORD_RIGHT, true);   /* select first word + sep */
    CHECK(prov_le_has_sel(&le) && le.cur == 4, "ctrl+shift word-select");

    /* history ring: push, Up recalls older, Down restores the draft */
    prov_lehist_t h = {0}; h.pos = -1;
    prov_lehist_push(&h, "first");
    prov_lehist_push(&h, "second");
    prov_lehist_push(&h, "second");             /* dedup vs newest */
    CHECK(h.count == 2, "history dedups consecutive");
    prov_le_clear(&le); ins(&le, "draft");
    prov_le_history(&le, &h, true);             /* Up -> newest "second" */
    CHECK(strcmp(le.buf, "second") == 0, "Up = newest");
    prov_le_history(&le, &h, true);             /* Up -> "first" */
    CHECK(strcmp(le.buf, "first") == 0, "Up = older");
    prov_le_history(&le, &h, false);            /* Down -> "second" */
    CHECK(strcmp(le.buf, "second") == 0, "Down = newer");
    prov_le_history(&le, &h, false);            /* Down past newest -> draft */
    CHECK(strcmp(le.buf, "draft") == 0, "Down past newest restores draft");

    /* render scroll: cursor at end of a long string stays visible in a small width */
    prov_le_clear(&le); ins(&le, "0123456789ABCDEF");   /* 16 chars, cursor at 16 */
    char vis[64]; int curc, sa, sb;
    prov_le_render(&le, 8, vis, sizeof vis, &curc, &sa, &sb);
    CHECK(curc <= 8 && strlen(vis) <= 8 && strstr("0123456789ABCDEF", vis) && vis[0] != '0',
          "render scrolls to keep the end-cursor visible");

    if (failures) { fprintf(stderr, "lineedit: %d failed\n", failures); return 1; }
    printf("ok: lineedit tests passed\n");
    return 0;
}
