# RFC-0013 — File-browser preview pane (+ reusable text-box widget)

- **Status:** **Phase A IMPLEMENTED (2026-06-22).** `textbox.{c,h}` widget +
  browser `p` split + encoding-aware/hex preview + `Tab` focus shipped; decisions
  resolved as recommended (D1a `Tab` focus, D2 defer format-header info, D3
  read-only). Phase B (format-header info, editable preview) deferred. Backlog #2;
  built on Phase 2 (panel value-return) + Phase 3a (encoding picker — preview reuses
  `s->open_enc`).

## 1. Problem

The file-open browser (`zo`, `PANEL_K_BROWSER`) lists entries with size / perms /
mtime / owner / group columns and an open-as encoding picker (`e`/`b`), but you
cannot **see** a file before opening it. Goals (backlog #2):

1. A **preview** of the selected entry that re-decodes with the chosen encoding.
2. **No broken glyphs ever** — a binary file falls back to a **hex view**;
   invalid / unmappable code points are hidden, never rendered as tofu (`�`).
3. (deferred) For well-known formats, parse the header for basic info (audio
   duration + ID3, image dimensions, video codec, …).
4. The preview behaves like a **fixed-width, vertically-scrollable text-box**
   control that can be read-only or editable — a **reusable widget**, since the
   same control is wanted later (e.g. an editable postfix box, config hints).

## 2. Layout

The browser panel is `PANEL_FULL`. Split its interior **left = list / right =
preview** (a vertical split at ~50%, min list width 24 cols; below that the
preview hides and `p` toggles it). The list keeps its columns; the preview gets
the right half with its own thin frame + vertical scrollbar. A new browser verb
**`p`** toggles the preview on/off; **`I`/`K`** still move the list selection and
the preview re-renders for the new selection (lazy — only when the preview is on).

Preview scrolling: when the preview has focus (a `>` / `<` toggle, or `Tab`
between list and preview), `I`/`K`/PgUp/PgDn scroll the preview; otherwise they
move the list. Decision below.

## 3. The reusable text-box widget (`src/textbox.{c,h}`)

A pure, render-free, fixed-width, vertically-scrollable text region:

```
typedef struct {
    const proven_u8 *bytes;   /* borrowed content (UTF-8 for text mode) */
    proven_size_t    len;
    proven_size_t    top;     /* first visible logical row (scroll) */
    bool             hex;     /* hex dump vs text */
    bool             editable;/* future: accept edits (Phase A = read-only) */
} prov_textbox_t;
```

- `prov_textbox_rows(tb, width)` → row count (text: wrapped at width, code-point
  safe, invalid bytes skipped; hex: `ceil(len/bytes_per_row)`).
- `prov_textbox_render(tb, width, row, out, cap)` → one visible row as UTF-8,
  width-clamped via the existing `prov_fit_field` / `prov_str_disp_width`.
- Hex mode: `OFFSET  HH HH … HH  |ascii|` with a fixed bytes-per-row (16, or
  width-derived). Non-printable bytes show `.` in the ascii gutter.
- **No-tofu rule:** text mode decodes with `prov_utf8_decode`; an invalid /
  incomplete sequence is **skipped** (advance 1 byte, render nothing) rather than
  emitting `�`. (Same spirit as the loader's lossy `sanitize_utf8`.)

This is the "reusable widget" deliverable; the browser preview is its first
consumer. Editing (`editable`) is wired but inert in Phase A.

## 4. Encoding-aware preview + binary detection

- The preview reads up to a **cap** (e.g. 64 KiB) of the selected file via
  `proven_fs_read_all` (bounded read; never the whole large file).
- **Binary detection:** if the capped bytes contain a NUL, or > ~30% non-text
  bytes, default to **hex**; else text. A verb (`x`?) force-toggles hex/text.
- **Text decode:** if `s->open_enc` is set, route the bytes through the charset
  PAL (`prov_charset_to_utf8`) exactly like the loader's forced path; else
  UTF-8 (lossy-sanitized) with the Windows-1252 fallback the loader uses. So the
  preview matches what an actual open would produce.
- The decode result is held in a session-owned scratch buffer, refreshed when the
  selection or `open_enc` changes (cached by entry index + enc).

## 5. Open decisions (author input)

- **D1 — preview focus model:** (a) `Tab` toggles list⇄preview focus (scroll keys
  follow focus); (b) list selection always drives, preview scrolls with a separate
  key pair (e.g. `<`/`>` or `f`/`F`). *Recommend (a) `Tab`* — simplest mental model,
  matches the save-as `Tab`-to-browser precedent.
- **D2 — format-header info (sub-feature 3):** MP3/ID3, image dims, video codec…
  This is open-ended and arguably out of a text editor's scope. *Recommend
  DEFER to Phase B* (a follow-up RFC), implementing only generic text/hex now;
  add a one-line "binary, N bytes" summary in hex mode meanwhile.
- **D3 — editable preview:** ship the widget read-only in Phase A; wire `editable`
  later when there's a concrete consumer (it would let the preview become an
  in-place quick-edit). *Recommend read-only now.*

## 6. Phasing

- **Phase A (this pass):** `textbox.{c,h}` widget + unit tests; browser left/right
  split; `p` toggle; encoding-aware text preview + binary→hex fallback + no-tofu;
  `Tab` focus (D1a); read-only (D3). PTY-verified.
- **Phase B (deferred):** format-header parsing (D2), editable preview (D3),
  preview for the save-as / other panels.

## 7. Testing

- `tests/test_textbox.c`: row counting + per-row render for text (wrap, wide
  glyphs, invalid-byte skip) and hex (offsets, ascii gutter, partial last row).
- Browser PTY: toggle preview, move selection (text file decodes; CP949 file with
  `e`→CP949 decodes correctly; a binary file shows hex; an invalid-UTF-8 file
  shows no `�`).
