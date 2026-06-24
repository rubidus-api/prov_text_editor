#ifndef PROV_PLATFORM_H
#define PROV_PLATFORM_H

#include "proven/types.h"

#include "display.h"   /* prov_cell_t */
#include "input.h"     /* prov_key_t  */

/*
 * Terminal backend interface (SPEC.md §2, §7, AGENTS §7).
 *
 * The core (src/) drives the editor through this backend-neutral seam; the
 * implementation (platform_term_posix.c, later platform_term_win32.c) is the
 * only place that touches OS/terminal APIs. This boundary keeps the editor
 * core free of OS headers and makes the backend replaceable.
 */

typedef struct {
    proven_size_t rows;
    proven_size_t cols;
} prov_term_size_t;

/* True if both stdin and stdout are connected to a terminal. */
bool prov_term_is_tty(void);

/* Enter raw mode and the alternate screen. Returns false if there is no
 * terminal or raw mode could not be set. */
bool prov_term_init(void);

/* Restore the original terminal state and leave the alternate screen. */
void prov_term_shutdown(void);

/* Enable or disable mouse reporting (SGR 1006 + button/drag tracking). Called
 * once after the config loads; reports then arrive as PROV_KEY_MOUSE events. */
void prov_term_enable_mouse(bool on);

/* Current terminal size (falls back to 24x80 if it cannot be queried). */
prov_term_size_t prov_term_size(void);

/* Paint a rows*cols cell grid, with an optional `tabbar` (reverse video) on the
 * row above it (NULL = none), `status` (reverse video) on the line below it and
 * `cmdline` (normal video) on the line below that, then place the hardware
 * cursor at (cur_row, cur_col) (0-based, within the grid). */
void prov_term_present(const prov_cell_t *grid, proven_size_t rows,
                       proven_size_t cols, proven_size_t cur_row,
                       proven_size_t cur_col, const char *status,
                       const char *cmdline, const char *tabbar);

/* Block until one key is available and return it. On input EOF/error, returns
 * a synthetic Ctrl-Q so the caller can quit cleanly. */
prov_key_t prov_term_read_key(void);

/* Write the directory containing the running executable (no trailing slash)
 * into `out`. Used to support a portable config (provconf.ini next to the
 * binary). Returns false if it cannot be determined. */
bool prov_platform_exe_dir(char *out, proven_size_t cap);

#endif /* PROV_PLATFORM_H */
