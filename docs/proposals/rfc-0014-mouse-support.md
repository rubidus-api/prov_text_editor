# RFC-0014 — Mouse support (SGR parse + hit-testing)

- **Status:** **IMPLEMENTED (2026-06-22), Phases 1–3d.** SGR 1006 parse + event
  model (P1), terminal enable + `mouse` config (P2), and hit-testing/dispatch —
  wheel scroll (P3a), click-focus + position + drag-select (P3b), scrollbar
  click/drag + close-`X` (P3c), panel row click/activate + wheel (P3d) — all shipped,
  PTY + ASan verified, on POSIX. **Deferred:** drag-resize the `+` corner (the
  keyboard `ws` covers resize) and Windows console mouse (D3 — console delivers
  MOUSE_EVENT records, not SGR; `prov_term_enable_mouse` is a no-op there). Built on
  `prov_layout_rects` for window hit-testing.

## 1. Goal

Make the laid-out UI clickable with a mouse: scroll wheel, click-to-focus a
window + position the cursor, drag the scrollbar / resize corner, click panel
rows, and the close-`X`. Keyboard remains fully sufficient; the mouse is additive.

## 2. Wire protocol — SGR 1006 only

We enable **SGR extended mouse mode** (`ESC [ ? 1006 h`) plus button+motion
tracking (`ESC [ ? 1000 h` for press/release, `ESC [ ? 1002 h` for drag while a
button is held). SGR is the only mode we parse — it is unambiguous, has no
coordinate byte-range limit (works past column 223, unlike the legacy X10
encoding), and every modern terminal emits it once 1006 is set.

A report is `ESC [ < Cb ; Cx ; Cy M` (press/motion) or `… m` (release), where:
- `Cb` low 2 bits = button (0 left, 1 middle, 2 right, 3 = none/release);
  bit 5 (32) = motion/drag; bits 6-7 (64) = wheel (64 = up, 65 = down);
  bits 2-4 = Shift(4)/Alt(8)/Ctrl(16) modifiers.
- `Cx`, `Cy` = 1-based column / row → stored 0-based.

The current CSI parser already consumes a final `M`/`m` (so mouse reports never
jam the decoder — they decode to a harmless ESC today). Phase 1 routes the
`ESC [ <` prefix to a dedicated mouse parse instead.

## 3. Input model (Phase 1)

Extend `prov_key_t` with a mouse event (no new struct — reuse the key path so the
main loop stays single-dispatch):

```
PROV_KEY_MOUSE                      /* new prov_key_kind_t */
typedef enum { PROV_MB_LEFT, PROV_MB_MIDDLE, PROV_MB_RIGHT,
               PROV_MB_WHEEL_UP, PROV_MB_WHEEL_DOWN, PROV_MB_NONE } prov_mbtn_t;
typedef enum { PROV_ME_PRESS, PROV_ME_RELEASE, PROV_ME_DRAG } prov_mact_t;
/* on prov_key_t: */ int mrow, mcol; prov_mbtn_t mbtn; prov_mact_t mact;
/* shift/ctrl/alt reuse the existing modifier flags */
```

`prov_decode_key` parses `ESC [ < b ; x ; y (M|m)` into this. Pure and
unit-tested (`tests/test_input.c` / a new `test_mouse`): button/wheel/modifier
decode, 0-based coords, partial-sequence `NONE`, release vs press.

## 4. Terminal enable/disable + config (Phase 2)

- New PAL call `prov_term_enable_mouse(bool)` writing `?1000h?1002h?1006h` /
  the matching `…l` resets. Added to both `platform_term_posix.c` and
  `platform_term_win32.c` (win32 console: enable via the input mode +
  `ENABLE_MOUSE_INPUT`, and translate `MOUSE_EVENT` records to the same
  `prov_key_t` — or, simpler, rely on Windows Terminal's VT input which emits SGR
  when 1006 is set; decide during Phase 2).
- Config: a real `mouse` bool (default **true**), applied after `load_config`;
  `prov_term_enable_mouse(cfg.mouse)`. Documented caveat: mouse capture overrides
  the terminal's own text selection — users who copy with the mouse set
  `mouse = false` (and the editor's own selection/`y` still works).

## 5. Hit-testing + dispatch (Phase 3, incremental)

A new `handle_mouse(s, k)` dispatched in the main loop **before** the mode/panel
handlers (mouse is global). Order of increments, each shippable + PTY-tested:

- **3a — wheel scroll.** Wheel up/down scrolls the window under the pointer (or the
  panel, when one is open) by a few lines — no focus change. Lowest-risk, high
  value. Uses `prov_layout_rects` to find the pane under `(mrow,mcol)`.
- **3b — click to focus + position.** Left-press inside a pane focuses that window
  (`L->focus`) and moves the cursor to the clicked cell (reuse
  `prov_editor_byte_at_vcol` + the pane's top/leftcol/gutter geometry). Left-drag
  extends a selection.
- **3c — scrollbar + resize corner.** Press on a pane's right scrollbar column
  jumps/drag-scrolls; press on the `+` corner drag-resizes the split; press on the
  status-line close-`X` closes the pane (`pane_close`).
- **3d — panel.** When a panel is open, a click selects the row under the pointer
  (and a second click / release on it activates); the close-`X` closes it; the
  preview pane (RFC-0013) scrolls under the wheel.

Geometry source of truth: `prov_layout_rects(L, area, out)` gives every node's
rect; a small `layout_hit(L, area, row, col)` (pure, in `layout.c`, unit-tested)
returns the leaf whose rect contains the point, so dispatch never re-derives the
split math.

## 6. Open decisions

- **D1 — default on/off:** *Recommend `mouse = true`* (additive, discoverable);
  caveat documented for terminal-copy users. Reversible via config.
- **D2 — drag selection (3b):** include left-drag→selection in 3b, or defer? *Recommend
  include* — it's the natural expectation and the selection machinery exists.
- **D3 — Windows:** SGR-via-VT-input if the host emits it, else translate
  `MOUSE_EVENT` records. *Decide in Phase 2 by testing*; Phase 1/3 are
  platform-neutral (pure parse + hit-testing), so Windows can lag without blocking.

## 7. Phasing summary

1. **Phase 1** — input model + SGR parse + unit tests (pure; no terminal).
2. **Phase 2** — `prov_term_enable_mouse` (posix; win32 best-effort) + `mouse` config.
3. **Phase 3a→3d** — hit-testing + dispatch, one increment at a time, each
   PTY-verified and committed.

Every change runs the full gate (`./nob test`, ASan/UBSan, `--release`).
