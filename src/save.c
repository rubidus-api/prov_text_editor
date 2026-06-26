#include "save.h"

#include "proven/error.h"
#include "proven/fs.h"
#include "proven/u8str.h"
#include "proven/memory.h"

proven_err_t prov_save_buffer(proven_allocator_t alloc, const prov_buffer_t *buf,
                              const char *path, const prov_fileinfo_t *info) {
    if (!proven_alloc_is_valid(alloc) || buf == NULL || path == NULL)
        return PROVEN_ERR_INVALID_ARG;

    proven_size_t docn = prov_buffer_byte_len(buf);

    /* serialize the document (internal LF-only, BOM-free UTF-8) */
    proven_u8 *doc = NULL;
    if (docn > 0) {
        proven_result_mem_mut_t rm = alloc.alloc_fn(alloc.ctx, docn, 16);
        if (!PROVEN_IS_OK(rm.err)) return rm.err;
        doc = (proven_u8 *)rm.value.ptr;
        prov_buffer_copy_range(buf, 0, docn, doc, docn);
    }

    /* reproduce the file's original form (EOL + encoding + BOM) */
    proven_size_t n = 0;
    proven_u8 *data = prov_encode_save(alloc, doc ? doc : (const proven_u8 *)"", docn, info, &n);
    if (doc) alloc.free_fn(alloc.ctx, doc);
    if (!data) return PROVEN_ERR_NOMEM;

    /* build "<path>.prov-tmp" in the same directory (owning proven string) */
    proven_result_u8str_t tr =
        proven_u8str_create_from_view(alloc, proven_u8str_view_from_cstr(path));
    if (!PROVEN_IS_OK(tr.err)) {
        if (data) alloc.free_fn(alloc.ctx, data);
        return tr.err;
    }
    proven_u8str_t tmp = tr.value;
    proven_err_t err = proven_u8str_append_grow(alloc, &tmp, PROVEN_LIT(".prov-tmp"));
    if (!PROVEN_IS_OK(err)) {
        proven_u8str_destroy(alloc, &tmp);
        if (data) alloc.free_fn(alloc.ctx, data);
        return err;
    }
    proven_u8str_view_t tview = proven_u8str_as_view(&tmp);

    proven_result_file_t of = proven_fs_open(
        alloc, tview, PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    if (!PROVEN_IS_OK(of.err)) { err = of.err; goto cleanup; }

    if (n > 0) {
        proven_mem_view_t mv = { (const proven_byte_t *)data, n };
        err = proven_fs_write_all(of.value, mv);
    }
    proven_fs_close(of.value);
    if (!PROVEN_IS_OK(err)) { (void)proven_fs_remove(alloc, tview); goto cleanup; }

    /* atomically replace the target */
    err = proven_fs_rename(alloc, tview, proven_u8str_view_from_cstr(path));
    if (!PROVEN_IS_OK(err)) (void)proven_fs_remove(alloc, tview);

cleanup:
    proven_u8str_destroy(alloc, &tmp);
    if (data) alloc.free_fn(alloc.ctx, data);
    return err;
}
