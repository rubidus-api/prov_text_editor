#ifndef PROV_HELP_H
#define PROV_HELP_H

/*
 * In-editor help pages (SPEC §10 reference). The overview (topic 0) lays the zx
 * keys out QWERTY-style; pressing a key opens that key's page. Content is static
 * text; rendering and scrolling live in the event loop. Pure: no I/O.
 */

/* Fill `lines` with up to `cap` static line pointers for help `topic` (0 = the
 * QWERTY overview). Sets *title to the page title. Returns the line count. */
int prov_help_page(int topic, const char **title, const char **lines, int cap);

/* Map a pressed key to a topic key, grouping related keys (i/k/j/l -> cursor,
 * c/d/y -> operators, x/r/o -> edits, e/q -> macros, b/m -> registers, h ->
 * overview). Unknown keys map to the "other keys" page. */
int prov_help_topic_for_key(int key);

#endif /* PROV_HELP_H */
