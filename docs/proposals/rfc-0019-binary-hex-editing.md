# RFC-0019 — Binary / hex editing: raw bytes, charset interpretation, range string ops

Status: Implemented (2026-06-24) — **P1 + P2 + P3 done**. Evolves RFC-0018 (which
edited the *converted* internal-UTF-8 buffer — wrong for non-UTF-8 / binary files).

## Problem

prov's buffer always holds the **internal form**: LF-only, BOM-free UTF-8, with
the original encoding/EOL/BOM converted away on load and restored on save (see
`encoding.h`). RFC-0018's hex mode therefore showed and edited the *converted*
bytes, not the file's real bytes — correct only for a UTF-8 + LF file, wrong for
CRLF, UTF-16, CP949, or any binary file.

Hex/binary editing must operate on the **raw file bytes**, verbatim in and out.

## Decisions (design pass, 2026-06-24)

1. **Hex always means raw bytes.** "Binary" is a *buffer* property
   (`prov_fileinfo_t.binary`): such a buffer is loaded and saved **verbatim** (no
   encoding / EOL / BOM conversion). A window renders hex **iff** its buffer is
   binary — hex-view ⟺ binary-buffer, so the old per-window `leaf.hex` flag is
   dropped (the per-window byte-window nudge `hex_align` stays).
2. **Two ways in, both reload raw:** the file-open panel gains a **binary (raw)**
   toggle (key **`x`**; loads verbatim, opens hex, preview shows the hex dump); and **`wx`**
   reloads the focused buffer's file in the other mode (raw ↔ text). `wx` refuses
   on a modified buffer (reload would lose edits) and just flips the flag for an
   unnamed buffer (no disk source).
3. **A decoded-string line under every hex row** (always on): each hex row is
   followed by a line showing that row's bytes decoded with the buffer's
   *interpretation charset* (the open-panel `e` encoding; default UTF-8). A
   multi-byte character that straddles the 16-byte row boundary is handled (shown
   on the row where it starts / continued marker).
4. **Range → string editing:** visual-select a byte range in hex, then a key opens
   a one-line text input; the typed string is **encoded with the interpretation
   charset** and replaces the whole range (any new length); a delete key removes
   the range. This is how variable-length string edits happen in binary mode.

## Phases

- **P1 — Binary buffer foundation** (this RFC's core): `prov_fileinfo_t.binary`;
  `prov_load_binary` (verbatim read → buffer); `prov_encode_save` returns a
  verbatim copy when `info->binary`; `prov_editor_open_binary`. Open-panel
  **binary** toggle (`open_binary`) → `open_path` loads raw; preview shows hex.
  `wx` becomes a text↔binary reload. Hex render/input/status are driven by
  `info.binary`. The RFC-0018 hexview widget, `render_hex_pane`, and
  `handle_hex_key` are reused unchanged.
- **P2 — Decoded-string line**: a second screen row under each hex row, charset-
  decoded, multi-byte-boundary aware; `render_hex_pane` draws both. The
  interpretation charset is `info.enc_name` (else UTF-8).
- **P3 — Range string ops**: visual byte selection in hex (`v` / Shift+arrows),
  `Esc` clears. **`r` opens a full multi-line ed-mode editor** (`PANEL_K_HEXEDIT`)
  on the decoded selection — a temporary editor reusing the editor primitives
  (movement via `move_by` by repointing `s->ed`, Shift selection, Shift+Ctrl word,
  `^A`/`^C`/`^X`/`^V`, `^Z`/`^Y`, multi-line). **`^S` re-encodes via the charset and
  `prov_editor_replace_range`s the selection** (any new length, one undo); `Esc`
  cancels. `y`/`p` copy/paste raw bytes; `x`/`Del` delete the selection.

### Key map (final, 2026-06-24)

Movement: `ikjl`+arrows (1 byte/row), `I`/`K`=PgUp/PgDn, `J`/`L`=Home/End,
`g`/`G`=doc start/end, `^G`=goto offset, `[`/`]`=byte-window nudge. Edit: `0-9a-f`
set a byte, `Tab` hex/ascii pane (ascii overtypes), `o`/`Insert` insert `0x00`,
`x`/`Del` delete, `y`/`p` byte copy/paste, `v` select, `r` string-replace editor,
`^S` save file, `^Q` quit, `^Z`/`^Y` undo/redo, `h` help, `Esc` leave. The whole
zx text-command surface (operators/objects/find-char/namespaces/macros/registers/
bookmarks/counts/text-search) does not apply in the hex editor.

## Notes

- Interpretation charset uses the existing `platform_charset` PAL
  (`prov_charset_to_utf8` / `prov_charset_from_utf8`), same as the text load/save.
- Saving a binary buffer is verbatim, so byte offsets shown in hex are real file
  offsets. CRLF/BOM are ordinary bytes you can see and edit.
- Out of scope (later): variable bytes-per-row, search-in-hex, a structured
  format inspector.
