#ifndef PROV_PSTR_H
#define PROV_PSTR_H

/*
 * Small shared helpers bridging proven's string system (proven_u8str_view_t)
 * and prov's fixed-capacity cstr fields (e.g. bufset path, config trigger,
 * prompt label). Header-only and libc-free so any core module can use them
 * without pulling in <string.h>. Part of RFC-0004 / Special Milestone S.
 */

#include "proven/u8str.h"
#include "proven/fmt.h"

/* Compile-time guard: `a` must be a real array, not a pointer — `sizeof(a)`
 * would otherwise yield the pointer size, not the buffer capacity (a bug that
 * recurred). For an array, typeof(a) and typeof(&a[0]) differ; for a pointer
 * they are the same, which trips the static assertion. GCC/clang/mingw only;
 * other compilers skip the check. */
#if defined(__GNUC__) || defined(__clang__)
#define PROV_REQUIRE_ARRAY(a) \
    _Static_assert(!__builtin_types_compatible_p(__typeof__(a), __typeof__(&(a)[0])), \
        "FMT_INTO requires a char array, not a pointer; " \
        "for a pointer + capacity use proven_u8str_borrow(ptr, cap) directly")
#else
#define PROV_REQUIRE_ARRAY(a) ((void)0)
#endif

/* Format into a fixed char array `arr` using proven's type-safe structured
 * formatter over a borrowed string (no allocation, no snprintf). `arr` must be
 * an array (sizeof gives its capacity); passing a pointer is a compile error.
 * Arguments after the format string use PROVEN_ARG / a view. Truncates (and
 * always NUL-terminates) on overflow. */
#define FMT_INTO(arr, ...) do { \
    PROV_REQUIRE_ARRAY(arr); \
    proven_u8str_t fmt_s_ = proven_u8str_borrow((proven_byte_t *)(arr), sizeof (arr)); \
    (void)proven_u8str_append_fmt_trunc(&fmt_s_, __VA_ARGS__); \
} while (0)

/* Copy a view into a fixed-capacity, NUL-terminated cstr field. The result is
 * bounded to `cap` bytes including the terminator (no overflow, always
 * terminated). Returns the number of content bytes written. */
static inline proven_size_t prov_cstr_set(char *dst, proven_size_t cap,
                                          proven_u8str_view_t v) {
    proven_size_t m = (cap == 0) ? 0 : (v.size < cap - 1 ? v.size : cap - 1);
    for (proven_size_t i = 0; i < m; i++) dst[i] = (char)v.ptr[i];
    if (cap) dst[m] = '\0';
    return m;
}

/* A read-only view over a NUL-terminated cstr field (NULL -> empty view). */
static inline proven_u8str_view_t prov_cstr_view(const char *s) {
    return proven_u8str_view_from_cstr(s);
}

#endif /* PROV_PSTR_H */
