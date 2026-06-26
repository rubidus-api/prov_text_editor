# RFC 0002: zx Command Feedback, Goto Shorthand, and End-of-File Viewport

Status: Adopted (all four stages implemented; canonical behavior in SPEC §10.7 / §12)

## Summary

Four related editor-UX changes, each shippable on its own and verified in
stages:

1. **`[N]g` goto-line shorthand** — a count followed by a single `g` jumps to
   line `N` immediately, instead of requiring `[N]gg`. (SPEC §10.7 already lists
   `[N]g` "if shorthand retained"; this adopts it.)
2. **Live zx command feedback in the status bar** — replace the static
   `^S save  ^Z undo  ^Q quit` hint (after the `[zx]` mode tag) with the command
   being typed (e.g. `12`, `123g`) plus either a one/two-word description of what
   it does, or the sub-commands that the current prefix can still become.
3. **End-of-file marker + scroll-to-end** — show a vi-style `~` on every screen
   row past the last buffer line so the end of the file is visible, and let the
   page-scroll keys move the viewport far enough that the last line (and the
   `~` rows below it) can rise to the top of the screen.
4. **Compact position readout** — render the cursor position in the status bar
   as `line:lines/col:linelen` (e.g. `12:345/8:22`) instead of
   `Ln 12, Col 8`.

These are confined to `command.{c,h}`, `display.{c,h}`, and `main.c`; the core
remains terminal-agnostic.

## Why

- The current goto requires `gg` even with a count, which is slower and (as a
  user found) surprising — typing `12g` looks like it "does nothing" because the
  parser is still waiting for a second `g`.
- The trailing `save/undo/quit` text is low-value once learned and wastes the
  one place that could instead teach the deterministic command language while
  it is being typed.
- With no end-of-file indicator and a viewport that pins the last line to the
  bottom, the user cannot tell where the buffer ends or scroll past it.
- `Ln/Col` is verbose; a compact `line:lines/col:linelen` shows position *and*
  extent in less space.

---

## Stage 1 — `[N]g` goto-line shorthand

**Behavior.** In `zx` at the command start:
- `g` **with** a pending count → execute `GOTO_LINE(count)` immediately.
- `g` **without** a count → enter the `g` namespace (`gg`, `ge`, `gf`, `gl`,
  `gu`, `gd`, `gp`, `gn` as today). `gg` (no count) stays Document-start.
- `0g` is unchanged (the leading-`0` special prefix → File-last-line); `0`
  while a count is being entered still extends the count, so `10g` = line 10.

This keeps a single deterministic rule: **count present ⇒ `g` is goto; count
absent ⇒ `g` is a namespace.** No timing, no ambiguity. `[N]gg` is no longer a
goto (the `[N]g` fires first); that is the intended shorthand from SPEC §10.7.

**Touchpoints.** `command.c` `prov_cmd_feed` `PS_START` `'g'` case.
**Test.** `tests/test_command.c`: `1g`/`5g`/`120g` → ACTION GOTO_LINE with the
right count; `g`/`gg`/`ge` unchanged; `0g` → FILE_LAST_LINE.

## Stage 2 — End-of-file marker + scroll-to-end

**EOF marker (pure, in `display.c`).** For every grid row whose document line
index `top+r >= line_count`, render a `~` in column 0 (rest blank). The first
such row sits directly under the last line, marking the end; rows below repeat
`~` (vi convention). This is deterministic and unit-testable without a terminal.

**Scroll-to-end (in `main.c`).** Today `top` is derived purely from the cursor
and clamps so the last line rests on the bottom row. Change the viewport rule so
`top` may range up to `line_count - 1` (last line at the top, `~` rows below):
- Keep the cursor visible: if `cursor_line < top`, `top = cursor_line`; if
  `cursor_line >= top + text_rows`, `top = cursor_line - text_rows + 1`.
- **Page keys scroll the viewport, not only the cursor.** `PageDown` advances
  `top` by a page (`text_rows`, clamped to `line_count - 1`) and moves the
  cursor down with it; `PageUp` symmetrically. This lets the view scroll until
  only `~` rows remain below the last line.

**Touchpoints.** `display.c` render loop (the `line >= lc` branch); `main.c`
viewport computation and the PageUp/PageDown handling.
**Test.** `tests/test_display.c`: a 2-line buffer rendered into 5 rows shows `~`
at col 0 on rows 2..4. Page-scroll is PTY-verified.

## Stage 3 — Compact position readout

Replace `Ln %zu, Col %zu` with `L:Lc/C:Cl` where
- `L` = cursor line (1-based), `Lc` = total line count,
- `C` = cursor column (1-based), `Cl` = current line length in columns.

Rendered example: `12:345/8:22` = line 12 of 345, column 8 of a 22-wide line.

**Touchpoints.** `main.c` status assembly; a small line-length helper (columns
of the cursor's line) — reuse display width so wide chars count as 2, matching
the `Col` semantics.
**Test.** Covered by the status-format path; PTY spot-check.

## Stage 4 — Live zx command feedback

Replace the trailing `zx:cmd  ^S save  ^Z undo  ^Q quit` text. In **Ed** mode the
status keeps a minimal hint. In **zx** mode it shows a command segment:

- **While a command is being entered** (parser not idle): the literal keys typed
  so far (`12`, `d`, `123g`→ fires, `gi`, `df`…) followed by a short hint of what
  can come next — the *derivable sub-commands* for the current prefix. Examples:
  - count `12` → `repeat/goto — g line · ikjl move · dd cc yy`
  - operator `d` → `delete — w e b · f t <c> · i a <obj> · d line`
  - namespace `g` → `goto — g top · e end · f l line · u d ½pg · p n word`
  - find `df` → `delete to char — type a character`
- **Just after a command executes** (parser idle, last command known): show the
  command and its one/two-word function (`123g  goto line` / `dw  del word` /
  `yy  yank line`) until the next key.
- **Idle with nothing recent**: a compact top-level legend.

**Mechanism.**
- `command.{c,h}`: add `prov_cmd_describe(const prov_cmd_parser_t *p, char *buf,
  size_t n)` → writes the short hint for the parser's *current* state (idle /
  count / operator / textobj / find / namespace). Pure and unit-testable.
- `main.c`: accumulate the printable keys fed to the parser since the last
  reset into a small `zx_pending[]`; clear it on reset (ESC / completion /
  invalid). On completion, remember the command string + a function label for
  the idle display. Compose the status segment from these.

**Touchpoints.** `command.{c,h}` (describe), `main.c` (pending buffer + status).
**Test.** `tests/test_command.c`: `prov_cmd_describe` returns the expected
substrings for representative states; status PTY-verified.

---

## Out of scope / deferred

- Horizontal scroll, `scrolloff` config wiring (SPEC §17), and the `;`/`,`
  find-repeat remain on the existing TODO.
- Colour/dim styling of the `~` rows (needs a cell style flag through both
  terminal backends) — render plain `~` for now.

## Verification per stage

Each stage: failing test first (where the logic is pure), `./nob test` green,
ASan/UBSan clean for touched core modules, and a PTY check of the live editor
for the interactive parts. Both native and `win64` keep building.
