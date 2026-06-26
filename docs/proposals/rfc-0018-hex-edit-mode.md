# RFC-0018 — Hex view/edit mode

Status: Implemented (2026-06-24)
Backlog: user item #3 ("Hex-mode editing for binary files"), Phase 3d.

## Goal

A per-window **hex view that is also editable**. Toggle the focused window
between its normal text view and a hex dump of the same buffer; edit bytes in
place, insert/delete bytes, and inspect alignment by nudging the byte window.
It edits the *open buffer* (so it saves through the normal path) and shares the
editor's byte cursor with the text view, so toggling keeps your position.

## Decisions (from the design pass)

1. **Editing model: overwrite + insert/delete** (length-variable). Nibble
   overtype and ASCII overtype are in-place (no length change); `i` inserts a
   `0x00` byte at the cursor and `x` deletes the byte at the cursor (each one
   undo step, through the existing `prov_editor_*` byte ops).
2. **Per-window toggle on the open buffer** (`wx`, paralleling `wr` read-only).
   A window carries a `hex` flag (persists across focus changes; a split can show
   hex and text side by side). The **byte window can be nudged** left/right by one
   (`[` / `]`) to shift the row alignment for structure inspection.

## Model

`prov_pane_node_t` (a window/leaf) gains:

- `bool hex` — render + edit this window as hex.
- `int  hex_align` — 0..15, the byte-window nudge (byte 0 sits at column `align`).

`hex` is the single source of truth: the renderer draws hex for any window with
`hex` set; the input loop routes the *focused* window's keys to the hex handler
when its leaf has `hex`. `s->mode` stays `MODE_ZX` underneath, so leaving hex
(Esc) just resumes zx on the same cursor. The cursor is the editor byte cursor
(`prov_editor_cursor_byte` / `prov_editor_move_to`), shared with the text view.

Session state for the *focused* edit (reset on enter/exit/any move):
`s->hex { bool ascii; bool pend; proven_u8 hi; }` — `ascii` = cursor is in the
ASCII pane (Tab toggles), `pend`/`hi` = a high nibble has been typed and awaits
its low nibble.

## Layout (fixed 16 bytes/row, BPR = 16)

```
OOOOOOOO  HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH |................|
0      7  10                                            57 59             74 75
```

- offset: 8 hex digits, columns 0..7; then two spaces.
- hex pane: byte slot `s` (0..15) at column `10 + s*3` (two digits + a space).
- ascii pane: `|` at 58, char `s` at `59 + s`, closing `|` at 75. Row width 76.
- Narrow windows clip on the right (like the preview); BPR stays 16 for simple,
  stable cursor math.

With a nudge `align`, byte `b` lives at row `(b+align)/16`, slot `(b+align)%16`;
row `r`'s slot 0 is byte `r*16 - align` (negative ⇒ a blank lead-in on row 0).
The pure geometry + row rendering live in `src/hexview.{c,h}` (unit-tested),
mirroring the `textbox` widget.

## Keys (hex mode — self-contained)

- Move: `i`/`k` up/down a row, `j`/`l` left/right a byte, arrows likewise,
  Home/End = row start/end, PgUp/PgDn = a page, `g`/`G` = doc start/end.
- Hex pane: a hex digit `0-9a-fA-F` sets the high then low nibble of the byte
  under the cursor (overwrite), advancing to the next byte after the low nibble.
  At EOF (cursor past the last byte) the first nibble appends a new byte.
- ASCII pane: a printable byte overwrites the byte under the cursor and advances.
- `Tab` toggles hex ↔ ASCII pane. `i` inserts a `0x00` byte; `x` deletes the
  byte under the cursor. `[` / `]` nudge the byte window. `u`/`Z` undo/redo
  (shared editor undo). `Esc` clears a pending nibble, else leaves hex (→ zx).
- Read-only windows render hex but block the editing keys (status shows `RO`).

## Out of scope (v1)

Variable bytes-per-row, a separate insert *sub-mode* (each `i` inserts one byte),
search-in-hex, and column/scrollbar chrome beyond the shared window status line.
These can layer on later without changing the model.
