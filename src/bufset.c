#include "bufset.h"
#include "pstr.h"

void prov_bufset_init(prov_bufset_t *bs) {
    bs->count = 0;
    bs->active = 0;
}

int prov_bufset_add(prov_bufset_t *bs, prov_editor_t *ed, const char *path) {
    if (bs->count >= PROV_MAX_BUFFERS) return -1;
    int i = bs->count++;
    prov_buf_t *e = &bs->entries[i];
    e->ed = ed;
    e->modified = false;
    e->top = 0;
    e->overwrite = false;
    e->info = prov_fileinfo_default();
    prov_cstr_set(e->path, sizeof e->path, prov_cstr_view(path));  /* NULL/"" -> "" */
    return i;
}

int prov_bufset_close(prov_bufset_t *bs, int i) {
    if (i < 0 || i >= bs->count) return bs->active;
    for (int j = i; j + 1 < bs->count; j++) bs->entries[j] = bs->entries[j + 1];
    bs->count--;
    if (bs->count == 0) { bs->active = 0; return -1; }
    if (bs->active > i) bs->active--;
    else if (bs->active == i && bs->active >= bs->count) bs->active = bs->count - 1;
    return bs->active;
}

int prov_bufset_find(const prov_bufset_t *bs, const char *path) {
    if (!path || !*path) return -1;
    proven_u8str_view_t want = prov_cstr_view(path);
    for (int i = 0; i < bs->count; i++)
        if (proven_u8str_view_eq(prov_cstr_view(bs->entries[i].path), want)) return i;
    return -1;
}

void prov_bufset_next(prov_bufset_t *bs) {
    if (bs->count > 0) bs->active = (bs->active + 1) % bs->count;
}

void prov_bufset_prev(prov_bufset_t *bs) {
    if (bs->count > 0) bs->active = (bs->active + bs->count - 1) % bs->count;
}
