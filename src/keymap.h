#ifndef PROV_KEYMAP_H
#define PROV_KEYMAP_H

/* Shared modal-key engine for panels (RFC-0010 S0): nav decode, a count
 * accumulator, the small g/s grammar, a data-driven verb table, and a legend
 * generated from that table. Pure — the panel (and future surfaces) drive it;
 * the editor's richer zx grammar stays in command.c. */

#include "input.h"
#include "proven/types.h"

typedef enum {
    NAV_NONE, NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT,
    NAV_PGUP, NAV_PGDN, NAV_HOME, NAV_END
} prov_nav_t;

/* arrows + `i k j l` + Page/Home/End → a direction (NAV_NONE if not a nav key). */
prov_nav_t prov_nav_decode(prov_key_t k);

/* a verb: a lowercase key → an action id, with a label for help/legend. */
typedef struct { char key; int action; const char *label; } prov_keymap_entry_t;
typedef struct { const prov_keymap_entry_t *entries; proven_size_t n; } prov_keymap_t;

/* result of feeding one key to the panel parser */
typedef enum {
    PK_NONE,       /* incomplete: a count or `s` prefix is pending */
    PK_MOVE,       /* move selection: `dir`, `count` times */
    PK_GOTO,       /* go to item `index` (0 = last, N>=1 = item N) */
    PK_SEARCH,     /* ss → open search/filter */
    PK_ACTIVATE,   /* Space / Enter */
    PK_VERB,       /* a verb fired: `action` */
    PK_OK, PK_CANCEL, PK_DISCARD,  /* y / c / d footer */
    PK_HELP,       /* h */
    PK_CYCLE,      /* w → cycle panel position */
    PK_CLOSE,      /* Esc */
    PK_INVALID     /* an unbound key */
} prov_pk_kind_t;

typedef struct {
    prov_pk_kind_t kind;
    prov_nav_t     dir;
    proven_u32     count;     /* PK_MOVE repeat (>=1) */
    proven_u32     index;     /* PK_GOTO item (0 = last) */
    int            action;    /* PK_VERB action id */
} prov_pk_t;

typedef struct { proven_u32 count; bool has_count; int state; } prov_keymap_parser_t;

/* Feed one key. `verbs` may be NULL. A pending count is consumed by a nav (repeat)
 * or `g` (item index) and discarded by anything else. */
prov_pk_t prov_keymap_feed(prov_keymap_parser_t *p, prov_key_t k, const prov_keymap_t *verbs);

/* One-line key legend (`ik move · Ng go · ss find · <verbs> · h help · w pos ·
 * y/c/d · Esc`) into `buf`; returns the length that would be written. */
proven_size_t prov_keymap_legend(const prov_keymap_t *verbs, char *buf, proven_size_t cap);

#endif /* PROV_KEYMAP_H */
