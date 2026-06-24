/*
 * Smoke test: verifies the build foundation links against `proven` and that
 * the heap allocator trait round-trips an allocation. This is the TDD
 * foothold; real module tests are added alongside each milestone.
 *
 * Convention: a test returns 0 on success and non-zero on failure. nob treats
 * a non-zero exit as a failed test.
 */

#include <stdio.h>
#include <string.h>

#include "proven/version.h"
#include "proven/error.h"
#include "proven/heap.h"
#include "proven/allocator.h"

static int fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

int main(void) {
    /* The vendored proven version string must be present and non-empty. */
    if (PROVEN_VERSION_STRING[0] == '\0') {
        return fail("proven version string is empty");
    }

    proven_allocator_t heap = proven_heap_allocator();
    if (!proven_alloc_is_valid(heap)) {
        return fail("heap allocator trait is not valid");
    }

    /* Allocate, write, read back, and free through the trait. */
    const proven_size_t n = 64;
    proven_result_mem_mut_t res = heap.alloc_fn(heap.ctx, n, 16);
    if (!PROVEN_IS_OK(res.err) || res.value.ptr == NULL) {
        return fail("heap allocation failed");
    }

    memset(res.value.ptr, 0xAB, n);
    if (((unsigned char *)res.value.ptr)[0] != 0xAB ||
        ((unsigned char *)res.value.ptr)[n - 1] != 0xAB) {
        heap.free_fn(heap.ctx, res.value.ptr);
        return fail("allocated memory is not writable/readable");
    }

    heap.free_fn(heap.ctx, res.value.ptr);

    printf("ok: smoke test passed (%s)\n", PROVEN_VERSION_STRING);
    return 0;
}
