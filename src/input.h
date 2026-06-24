#ifndef PROV_INPUT_H
#define PROV_INPUT_H

#include "proven/types.h"

/*
 * Key decoding (SPEC.md §0.4, §11). `prov_decode_key` is a pure function from a
 * byte slice to one key event, so it is unit-testable without a terminal. The
 * platform backend reads raw bytes and feeds them here; only that thin
 * read()/termios shell is left untested.
 */

typedef enum {
    PROV_KEY_NONE = 0,   /* no complete key in the slice (need more bytes) */
    PROV_KEY_CHAR,       /* a printable code point (cp / bytes / nbytes set) */
    PROV_KEY_CTRL,       /* Ctrl + ASCII letter (cp holds the lowercase letter) */
    PROV_KEY_ENTER,
    PROV_KEY_TAB,
    PROV_KEY_BACKSPACE,
    PROV_KEY_DELETE,
    PROV_KEY_ESC,
    PROV_KEY_LEFT,
    PROV_KEY_RIGHT,
    PROV_KEY_UP,
    PROV_KEY_DOWN,
    PROV_KEY_HOME,
    PROV_KEY_END,
    PROV_KEY_PAGEUP,
    PROV_KEY_PAGEDOWN,
    PROV_KEY_INSERT,
    PROV_KEY_MOUSE       /* SGR mouse report (RFC-0014): mrow/mcol/mbtn/mact set */
} prov_key_kind_t;

/* Mouse button / wheel (RFC-0014). WHEEL_* arrive with mact == PROV_ME_PRESS. */
typedef enum {
    PROV_MB_LEFT = 0, PROV_MB_MIDDLE, PROV_MB_RIGHT,
    PROV_MB_WHEEL_UP, PROV_MB_WHEEL_DOWN, PROV_MB_NONE
} prov_mbtn_t;

typedef enum { PROV_ME_PRESS = 0, PROV_ME_RELEASE, PROV_ME_DRAG } prov_mact_t;

typedef struct {
    prov_key_kind_t kind;
    proven_u32      cp;          /* CHAR: code point; CTRL: 'a'..'z' letter */
    proven_u8       bytes[4];    /* CHAR: raw UTF-8 bytes */
    proven_size_t   nbytes;      /* CHAR: number of raw bytes */
    proven_size_t   consumed;    /* bytes consumed from the input slice */
    bool            shift;       /* modifier flags (set for modified CSI / mouse) */
    bool            ctrl;
    bool            alt;
    int             mrow, mcol;  /* MOUSE: 0-based row/col of the event */
    prov_mbtn_t     mbtn;        /* MOUSE: button or wheel direction */
    prov_mact_t     mact;        /* MOUSE: press / release / drag */
} prov_key_t;

/* Decode one key from the front of [buf, buf+len).
 *
 * Returns PROV_KEY_NONE with consumed == 0 when the slice is empty or holds an
 * incomplete multi-byte escape/UTF-8 sequence (the caller should read more and
 * retry). A lone ESC byte (len == 1) decodes as PROV_KEY_ESC. */
prov_key_t prov_decode_key(const proven_u8 *buf, proven_size_t len);

#endif /* PROV_INPUT_H */
