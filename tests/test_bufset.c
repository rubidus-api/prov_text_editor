/* Unit tests for the buffer-set list logic (pure; uses dummy editor pointers). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "bufset.h"

static int failures = 0;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } \
    } while (0)

static prov_editor_t *fake(int i) { return (prov_editor_t *)(intptr_t)(i + 1); }

int main(void) {
    prov_bufset_t bs;
    prov_bufset_init(&bs);
    CHECK(bs.count == 0 && bs.active == 0, "init empty");

    CHECK(prov_bufset_add(&bs, fake(0), "a.txt") == 0, "add a -> 0");
    CHECK(prov_bufset_add(&bs, fake(1), "b.txt") == 1, "add b -> 1");
    CHECK(prov_bufset_add(&bs, fake(2), NULL)    == 2, "add unnamed -> 2");
    CHECK(bs.count == 3, "count 3");
    CHECK(bs.entries[2].path[0] == '\0', "unnamed has empty path");

    CHECK(prov_bufset_find(&bs, "b.txt") == 1, "find b -> 1");
    CHECK(prov_bufset_find(&bs, "missing") == -1, "find missing -> -1");
    CHECK(prov_bufset_find(&bs, NULL) == -1, "find NULL -> -1");
    CHECK(prov_bufset_find(&bs, "b.tx") == -1, "find prefix is not an exact match");
    CHECK(prov_bufset_find(&bs, "") == -1, "find empty -> -1");
    CHECK(strcmp(bs.entries[0].path, "a.txt") == 0, "path stored verbatim");

    /* over-long path: bounded copy stays within the field and NUL-terminated */
    {
        char longp[2048];
        memset(longp, 'x', sizeof longp - 1);
        longp[sizeof longp - 1] = '\0';
        int big = prov_bufset_add(&bs, fake(9), longp);
        CHECK(big == 3, "over-long path buffer added");
        CHECK(strlen(bs.entries[big].path) == sizeof bs.entries[big].path - 1,
              "over-long path bounded to field capacity");
        CHECK(bs.entries[big].path[sizeof bs.entries[big].path - 1] == '\0',
              "over-long path stays NUL-terminated");
        prov_bufset_close(&bs, big);   /* restore the 3-buffer state below */
    }

    bs.active = 0;
    prov_bufset_next(&bs); CHECK(bs.active == 1, "next -> 1");
    prov_bufset_next(&bs); CHECK(bs.active == 2, "next -> 2");
    prov_bufset_next(&bs); CHECK(bs.active == 0, "next wraps -> 0");
    prov_bufset_prev(&bs); CHECK(bs.active == 2, "prev wraps -> 2");

    /* close the active (last) -> active clamps to new last */
    bs.active = 2;
    CHECK(prov_bufset_close(&bs, 2) == 1, "close last -> active 1");
    CHECK(bs.count == 2, "count 2 after close");
    CHECK(bs.entries[0].ed == fake(0) && bs.entries[1].ed == fake(1), "remaining order");

    /* close before active shifts active down */
    prov_bufset_add(&bs, fake(3), "c.txt");   /* now [a,b,c], active 1 */
    bs.active = 2;
    CHECK(prov_bufset_close(&bs, 0) == 1, "close index 0 -> active 1");
    CHECK(bs.entries[0].ed == fake(1) && bs.entries[1].ed == fake(3), "shifted down");

    /* close down to empty */
    CHECK(prov_bufset_close(&bs, 0) >= 0, "close -> still one left");
    CHECK(prov_bufset_close(&bs, 0) == -1, "close last buffer -> -1 (empty)");
    CHECK(bs.count == 0, "empty");

    if (failures) {
        fprintf(stderr, "bufset: %d checks failed\n", failures);
        return 1;
    }
    printf("ok: bufset tests passed\n");
    return 0;
}
