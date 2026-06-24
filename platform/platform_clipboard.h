#ifndef PROV_PLATFORM_CLIPBOARD_H
#define PROV_PLATFORM_CLIPBOARD_H

/* Best-effort bridge to the OS clipboard. Implemented with the platform's
 * command-line clipboard tools (wl-copy/xclip/xsel on Linux, clip/powershell on
 * Windows); when none is present every call is a graceful no-op, so the editor's
 * own registers keep working. Lives in platform/ (uses libc/POSIX), kept out of
 * the libc-free core. */

#include "proven/types.h"

/* Copy `len` bytes to the OS clipboard. Returns true if a tool accepted it. */
bool prov_os_clip_set(const proven_u8 *data, proven_size_t len);

/* Read the OS clipboard into `buf` (up to `cap` bytes). Returns the byte count,
 * or 0 if no tool is available / the clipboard is empty. */
proven_size_t prov_os_clip_get(proven_u8 *buf, proven_size_t cap);

#endif /* PROV_PLATFORM_CLIPBOARD_H */
