/* Unit tests for the static help pages. Pure; no editor/terminal. */
#include <stdio.h>
#include <string.h>

#include "help.h"

static int failures = 0;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } \
    } while (0)

int main(void) {
    const char *title = NULL;
    const char *lines[64];

    int n = prov_help_page(0, &title, lines, 64);
    CHECK(n > 0 && title != NULL, "overview has lines + title");
    CHECK(strstr(lines[0], "then a key") != NULL, "overview explains the h+key lookup");

    /* digits route to the count page; n has its own repeat page */
    CHECK(prov_help_topic_for_key('n') == 'n', "n maps to its own (repeat) page");
    n = prov_help_page('#', &title, lines, 64);
    CHECK(n > 0 && strstr(title, "count") != NULL, "count page resolves under #");
    n = prov_help_page('n', &title, lines, 64);
    CHECK(n > 0 && strstr(title, "repeat") != NULL, "n page is about repeat");

    /* cap is respected */
    int capped = prov_help_page(0, &title, lines, 2);
    CHECK(capped == 2, "cap limits the returned line count");

    /* key -> topic grouping */
    CHECK(prov_help_topic_for_key('k') == 'i', "k groups under cursor (i)");
    CHECK(prov_help_topic_for_key('d') == 'c', "d groups under operators (c)");
    CHECK(prov_help_topic_for_key('y') == 'c', "y groups under operators (c)");
    CHECK(prov_help_topic_for_key('q') == 'e', "q groups under macros (e)");
    CHECK(prov_help_topic_for_key('m') == 'b', "m groups under registers/bookmarks (b)");
    CHECK(prov_help_topic_for_key('h') == 0,   "h returns to the overview");
    CHECK(prov_help_topic_for_key('w') == 'w', "w maps to itself");
    CHECK(prov_help_topic_for_key('Q') == '?', "unknown key -> other-keys page");

    /* a known detail page resolves and has a sensible title */
    n = prov_help_page('c', &title, lines, 64);
    CHECK(n > 0 && strstr(title, "operators") != NULL, "operators page title");
    n = prov_help_page('w', &title, lines, 64);
    {
        int found = 0;
        for (int i = 0; i < n; i++) if (strstr(lines[i], "wh")) found = 1;
        CHECK(found, "window page mentions wh");
    }

    if (failures) {
        fprintf(stderr, "help: %d checks failed\n", failures);
        return 1;
    }
    printf("ok: help tests passed\n");
    return 0;
}
