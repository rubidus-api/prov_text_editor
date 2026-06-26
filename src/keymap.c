#include "keymap.h"

prov_nav_t prov_nav_decode(prov_key_t k) {
    switch (k.kind) {
        case PROV_KEY_UP:       return NAV_UP;
        case PROV_KEY_DOWN:     return NAV_DOWN;
        case PROV_KEY_LEFT:     return NAV_PGUP;    /* arrows mirror i k j l (j/l = page) */
        case PROV_KEY_RIGHT:    return NAV_PGDN;
        case PROV_KEY_PAGEUP:   return NAV_PGUP;
        case PROV_KEY_PAGEDOWN: return NAV_PGDN;
        case PROV_KEY_HOME:     return NAV_HOME;
        case PROV_KEY_END:      return NAV_END;
        case PROV_KEY_CHAR:
            switch (k.cp) {
                case 'i': return NAV_UP;
                case 'k': return NAV_DOWN;
                case 'j': return NAV_PGUP;   /* page up (was left) */
                case 'l': return NAV_PGDN;   /* page down (was right) */
                default:  return NAV_NONE;
            }
        default: return NAV_NONE;
    }
}

enum { KS_START = 0, KS_S = 1 };

prov_pk_t prov_keymap_feed(prov_keymap_parser_t *p, prov_key_t k, const prov_keymap_t *verbs) {
    prov_pk_t r = { .kind = PK_NONE };

    if (p->state == KS_S) {                          /* after `s`: only `ss` = search */
        p->state = KS_START;
        r.kind = (k.kind == PROV_KEY_CHAR && k.cp == 's') ? PK_SEARCH : PK_INVALID;
        p->count = 0; p->has_count = false;
        return r;
    }

    prov_nav_t nav = prov_nav_decode(k);
    if (nav != NAV_NONE) {                            /* movement: [N] repeats */
        r.kind = PK_MOVE; r.dir = nav;
        r.count = (p->has_count && p->count) ? p->count : 1;
        p->count = 0; p->has_count = false;
        return r;
    }

    if (k.kind == PROV_KEY_CHAR && k.cp >= '0' && k.cp <= '9') {
        p->count = p->count * 10u + (proven_u32)(k.cp - '0');   /* digits build [N], consumed by g */
        p->has_count = true;
        return r;                                     /* PK_NONE: count pending */
    }

    if (k.kind == PROV_KEY_ENTER) r.kind = PK_ACTIVATE;
    else if (k.kind == PROV_KEY_ESC) r.kind = PK_CLOSE;
    else if (k.kind == PROV_KEY_CHAR) {
        switch (k.cp) {
            case ' ': r.kind = PK_ACTIVATE; break;
            case 'g': r.kind = PK_GOTO; r.index = p->has_count ? p->count : 1; break;  /* 0g=last */
            case 's': p->state = KS_S; return r;       /* PK_NONE: await second key */
            case 'h': r.kind = PK_HELP; break;
            case 'w': r.kind = PK_CYCLE; break;
            case 'o': r.kind = PK_OK; break;       /* o = OK / select (was y) */
            case 'c': r.kind = PK_CANCEL; break;
            case 'q': r.kind = PK_CANCEL; break;   /* q = back: alias of cancel (close the panel) */
            case 'd': r.kind = PK_DISCARD; break;
            default:
                r.kind = PK_INVALID;
                if (verbs)
                    for (proven_size_t i = 0; i < verbs->n; i++)
                        if (verbs->entries[i].key == (char)k.cp) { r.kind = PK_VERB; r.action = verbs->entries[i].action; break; }
                break;
        }
    } else r.kind = PK_INVALID;

    p->count = 0; p->has_count = false;               /* any non-count/nav/g key clears it */
    return r;
}

/* ---- legend ------------------------------------------------------------- */

typedef struct { char *b; proven_size_t cap, len; } lbuf;
static void l_str(lbuf *d, const char *s) { for (; *s; s++) { if (d->len + 1 < d->cap) d->b[d->len] = *s; d->len++; } }
static void l_ch(lbuf *d, char c) { if (d->len + 1 < d->cap) d->b[d->len] = c; d->len++; }

/* the shortcut separator: a clean 1-cell box-drawing vertical (U+2502), packed
 * with no surrounding spaces — `key:label│` segments, one bar trailing each. */
#define SEP "\xe2\x94\x82"
proven_size_t prov_keymap_legend(const prov_keymap_t *verbs, char *buf, proven_size_t cap) {
    lbuf d = { .b = buf, .cap = cap, .len = 0 };
    l_str(&d, "ik:move" SEP "Ng:go" SEP "ss:find" SEP);
    if (verbs)
        for (proven_size_t i = 0; i < verbs->n; i++) {
            l_ch(&d, verbs->entries[i].key);
            l_ch(&d, ':');
            l_str(&d, verbs->entries[i].label ? verbs->entries[i].label : "");
            l_str(&d, SEP);
        }
    l_str(&d, "h:help" SEP "w:pos" SEP "Esc" SEP);
    #undef SEP
    if (cap) d.b[d.len < cap ? d.len : cap - 1] = '\0';
    return d.len;
}
