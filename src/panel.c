#include "panel.h"

static bool ci_contains(const char *hay, const char *needle) {
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

void prov_panel_init(prov_panel_t *p, proven_allocator_t a, const char *title,
                     const prov_panel_row_t *rows, proven_size_t nrows, const prov_keymap_t *keys) {
    *p = (prov_panel_t){ .title = title, .rows = rows, .nrows = nrows, .keys = keys, .a = a };
    prov_keymap_legend(keys, p->legend, sizeof p->legend);   /* cache: not rebuilt per frame */
    prov_panel_refilter(p);
}

void prov_panel_init_dynamic(prov_panel_t *p, proven_allocator_t a, const char *title,
                             const prov_panel_vsource_t *src, const prov_keymap_t *keys) {
    *p = (prov_panel_t){ .title = title, .src = src, .keys = keys, .a = a };
    prov_keymap_legend(keys, p->legend, sizeof p->legend);
    prov_panel_refilter(p);
}

void prov_panel_free(prov_panel_t *p) {
    if (p->view) p->a.free_fn(p->a.ctx, p->view);
    p->view = NULL; p->nview = p->view_cap = 0;
}

void prov_panel_refilter(prov_panel_t *p) {
    if (p->src) {                                    /* heavy mode: the source filters + counts */
        p->src->filter(p->src->ctx, p->filter);
        p->nview = p->src->count(p->src->ctx);
        if (p->nview == 0) p->sel = 0;
        else if (p->sel >= p->nview) p->sel = p->nview - 1;
        return;
    }
    p->nview = 0;
    for (proven_size_t i = 0; i < p->nrows; i++) {
        const char *t = p->rows[i].text ? p->rows[i].text : "";
        if (p->flen && !ci_contains(t, p->filter)) continue;
        if (p->nview >= p->view_cap) {
            proven_size_t cap = p->view_cap ? p->view_cap * 2 : 64;
            proven_result_mem_mut_t m = p->view
                ? p->a.realloc_fn(p->a.ctx, p->view, p->view_cap * sizeof(proven_size_t), cap * sizeof(proven_size_t), 16)
                : p->a.alloc_fn(p->a.ctx, cap * sizeof(proven_size_t), 16);
            if (!PROVEN_IS_OK(m.err)) break;
            p->view = (proven_size_t *)m.value.ptr; p->view_cap = cap;
        }
        p->view[p->nview++] = i;
    }
    if (p->nview == 0) p->sel = 0;
    else if (p->sel >= p->nview) p->sel = p->nview - 1;
}

void prov_panel_set_rows(prov_panel_t *p, const prov_panel_row_t *rows, proven_size_t nrows) {
    if (p->src) return;                              /* heavy panels mutate their source, then refilter */
    p->rows = rows; p->nrows = nrows;
    prov_panel_refilter(p);
}

void prov_panel_filter_push(prov_panel_t *p, char c) {
    if (p->flen + 1 < sizeof p->filter) { p->filter[p->flen++] = c; p->filter[p->flen] = '\0'; }
    p->sel = 0;
    prov_panel_refilter(p);
}
void prov_panel_filter_pop(prov_panel_t *p) {
    if (p->flen) { p->filter[--p->flen] = '\0'; prov_panel_refilter(p); }
}
void prov_panel_filter_clear(prov_panel_t *p) {
    p->filter[0] = '\0'; p->flen = 0; p->sel = 0;
    prov_panel_refilter(p);
}

void prov_panel_move(prov_panel_t *p, prov_nav_t dir, proven_u32 count, proven_size_t page) {
    if (p->nview == 0) { p->sel = 0; return; }
    proven_size_t n = count ? count : 1;
    switch (dir) {
        case NAV_UP:   p->sel = p->sel > n ? p->sel - n : 0; break;
        case NAV_DOWN: p->sel = p->sel + n < p->nview ? p->sel + n : p->nview - 1; break;
        case NAV_PGUP: { proven_size_t s = page ? page : 1; p->sel = p->sel > s ? p->sel - s : 0; break; }
        case NAV_PGDN: { proven_size_t s = page ? page : 1; p->sel = p->sel + s < p->nview ? p->sel + s : p->nview - 1; break; }
        case NAV_HOME: p->sel = 0; break;
        case NAV_END:  p->sel = p->nview - 1; break;
        default: break;                                /* LEFT/RIGHT: surface-defined */
    }
}

void prov_panel_goto(prov_panel_t *p, proven_u32 index) {
    if (p->nview == 0) { p->sel = 0; return; }
    if (index == 0) p->sel = p->nview - 1;             /* 0g = last */
    else p->sel = index - 1 < p->nview ? index - 1 : p->nview - 1;
}

int prov_panel_selected_id(const prov_panel_t *p) {
    if (p->nview == 0) return -1;
    if (p->src) return p->src->id(p->src->ctx, p->sel);
    return p->rows[p->view[p->sel]].id;
}
