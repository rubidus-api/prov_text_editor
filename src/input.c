#include "input.h"

#include "unicode.h"

/* Expected total byte length of a UTF-8 sequence from its lead byte. */
static int utf8_need(proven_u8 c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;   /* stray continuation / invalid lead */
}

prov_key_t prov_decode_key(const proven_u8 *buf, proven_size_t len) {
    prov_key_t k = { .kind = PROV_KEY_NONE };
    if (len == 0) return k;

    proven_u8 c = buf[0];

    /* ---- escape / CSI / SS3 ---- */
    if (c == 0x1B) {
        if (len == 1) { k.kind = PROV_KEY_ESC; k.consumed = 1; return k; }

        if (buf[1] == '[' && len >= 3 && buf[2] == '<') {
            /*
             * SGR mouse report (RFC-0014): ESC [ < Cb ; Cx ; Cy (M|m).
             * Cb low 2 bits = button; bit5 (32) = motion/drag; bits6-7 (64) =
             * wheel (64 up / 65 down); bits2-4 = Shift(4)/Alt(8)/Ctrl(16).
             * `M` = press/motion, `m` = release. Coords are 1-based -> 0-based.
             */
            proven_size_t e = 3;
            while (e < len && buf[e] != 'M' && buf[e] != 'm') e++;
            if (e >= len) return k;                  /* incomplete: need more */
            unsigned vals[3] = { 0, 0, 0 };
            int nf = 0;
            for (proven_size_t j = 3; j < e; j++) {
                proven_u8 ch = buf[j];
                if (ch >= '0' && ch <= '9') { if (nf < 3) vals[nf] = vals[nf] * 10u + (unsigned)(ch - '0'); }
                else if (ch == ';') { if (nf < 3) nf++; }
            }
            unsigned cb = vals[0];
            k.kind = PROV_KEY_MOUSE;
            k.consumed = e + 1;
            k.mcol = vals[1] > 0 ? (int)vals[1] - 1 : 0;
            k.mrow = vals[2] > 0 ? (int)vals[2] - 1 : 0;
            k.shift = (cb & 4u) != 0;
            k.alt   = (cb & 8u) != 0;
            k.ctrl  = (cb & 16u) != 0;
            if (cb & 64u) {                          /* wheel */
                k.mbtn = (cb & 1u) ? PROV_MB_WHEEL_DOWN : PROV_MB_WHEEL_UP;
                k.mact = PROV_ME_PRESS;
            } else {
                unsigned b = cb & 3u;
                k.mbtn = b == 0 ? PROV_MB_LEFT : b == 1 ? PROV_MB_MIDDLE
                       : b == 2 ? PROV_MB_RIGHT : PROV_MB_NONE;
                k.mact = (buf[e] == 'm') ? PROV_ME_RELEASE
                       : (cb & 32u)      ? PROV_ME_DRAG : PROV_ME_PRESS;
            }
            return k;
        }

        if (buf[1] == '[' || buf[1] == 'O') {
            /*
             * Parse a full CSI/SS3 sequence: optional parameter/intermediate
             * bytes (0x20-0x3F) followed by a final byte (0x40-0x7E). Any
             * complete sequence is consumed so an unrecognized one can never
             * jam the decoder; recognized ones decode their key and modifiers
             * (`ESC [ 1 ; mod <letter>` and `ESC [ num ; mod ~`).
             */
            proven_size_t e = 2;
            while (e < len && !(buf[e] >= 0x40 && buf[e] <= 0x7E)) e++;
            if (e >= len) return k;                  /* incomplete: need more */

            proven_u8 final = buf[e];

            unsigned param1 = 0, param2 = 0;
            bool have2 = false;
            int field = 0;
            for (proven_size_t j = 2; j < e; j++) {
                proven_u8 ch = buf[j];
                if (ch >= '0' && ch <= '9') {
                    if (field == 0) param1 = param1 * 10u + (unsigned)(ch - '0');
                    else { param2 = param2 * 10u + (unsigned)(ch - '0'); have2 = true; }
                } else if (ch == ';') {
                    field = 1;
                }
            }
            if (have2 && param2 >= 2) {
                unsigned m = param2 - 1u;
                k.shift = (m & 1u) != 0;
                k.alt   = (m & 2u) != 0;
                k.ctrl  = (m & 4u) != 0;
            }

            k.consumed = e + 1;
            switch (final) {
                case 'A': k.kind = PROV_KEY_UP;    return k;
                case 'B': k.kind = PROV_KEY_DOWN;  return k;
                case 'C': k.kind = PROV_KEY_RIGHT; return k;
                case 'D': k.kind = PROV_KEY_LEFT;  return k;
                case 'H': k.kind = PROV_KEY_HOME;  return k;
                case 'F': k.kind = PROV_KEY_END;   return k;
                case 'Z': k.kind = PROV_KEY_TAB;   k.shift = true; return k;  /* CSI Z = Shift+Tab (back-tab) */
                case '~':
                    switch (param1) {
                        case 1: case 7: k.kind = PROV_KEY_HOME;     return k;
                        case 2:         k.kind = PROV_KEY_INSERT;   return k;
                        case 4: case 8: k.kind = PROV_KEY_END;      return k;
                        case 3:         k.kind = PROV_KEY_DELETE;   return k;
                        case 5:         k.kind = PROV_KEY_PAGEUP;   return k;
                        case 6:         k.kind = PROV_KEY_PAGEDOWN; return k;
                        default:        k.kind = PROV_KEY_ESC;      return k;
                    }
                default:
                    k.kind = PROV_KEY_ESC;  /* unrecognized but consumed */
                    return k;
            }
        }

        k.kind = PROV_KEY_ESC; k.consumed = 1; return k;      /* ESC + other */
    }

    /* ---- named control keys ---- */
    if (c == '\r' || c == '\n') { k.kind = PROV_KEY_ENTER;     k.consumed = 1; return k; }
    if (c == '\t')              { k.kind = PROV_KEY_TAB;       k.consumed = 1; return k; }
    if (c == 0x7F || c == 0x08) { k.kind = PROV_KEY_BACKSPACE; k.consumed = 1; return k; }

    /* ---- other control bytes: Ctrl + letter ---- */
    if (c < 0x20) {
        k.kind = PROV_KEY_CTRL;
        k.cp = (proven_u32)(c + 'a' - 1);   /* 0x01->'a' ... 0x1A->'z' */
        k.consumed = 1;
        return k;
    }

    /* ---- printable: decode one UTF-8 code point ---- */
    int need = utf8_need(c);
    if ((proven_size_t)need > len) return k;        /* truncated: need more */

    prov_decode_t d = prov_utf8_decode(buf, len);
    k.kind = PROV_KEY_CHAR;
    if (d.valid) {
        k.cp = d.cp;
        k.nbytes = d.len;
        for (proven_size_t i = 0; i < d.len && i < 4; i++) k.bytes[i] = buf[i];
        k.consumed = d.len;
    } else {
        /* substitute U+FFFD for a malformed byte and advance one byte */
        k.cp = PROV_CP_REPLACEMENT;
        k.bytes[0] = 0xEF; k.bytes[1] = 0xBF; k.bytes[2] = 0xBD;
        k.nbytes = 3;
        k.consumed = 1;
    }
    return k;
}
