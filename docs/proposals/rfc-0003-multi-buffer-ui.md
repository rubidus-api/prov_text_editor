# RFC 0003: Multi-Buffer UI (Milestone 3)

Status: Implemented

This RFC plans Milestone 3 (SPEC §20: panes, tabs, command window, buffer list,
basic config) as a sequence of independently shippable stages. It doubles as the
**resumable progress record**: the checklist at the end is updated after every
stage so any session can pick up exactly where the last one stopped. Pair it
with `CONTEXT.md` (live snapshot) and `CHANGELOG.md` (shipped changes).

## Goals & constraints

- Keep the deterministic, timeout-free `zx` model; core `src/` stays free of
  OS/terminal headers (those live under `platform/`).
- Each stage: failing test first for pure logic, `./nob test` green, ASan/UBSan
  on touched core, PTY check for interactive parts, native + win64 build clean,
  commit, then update this file's progress log + `CONTEXT.md`.
- Prefer small, opaque modules with Result-pattern APIs (no out-params).

## Architecture target

Today `main.c` owns a single `prov_editor_t` + one viewport (`top`) + the two
bottom rows. M3 grows this into:

```
app
├── config            (M3.1)  parsed ~/.prov/config.toml -> tabstop, trigger, ...
├── buffers[]         (M3.2)  N open buffers/editors; active index; zb list
├── tabs[]            (M3.3)  each tab = a pane tree over the buffers
│   └── pane tree     (M3.5)  leaf = (buffer ref + viewport); split h/v; focus
└── command line      (M3.4)  prompts / wizards / messages (already one row)
```

Stages are ordered so each builds on the last and is useful on its own:
config → multi-buffer+list → tabs → command-window/wizard → panes.

---

## Stage M3.1 — Configuration (TOML subset) · SPEC §17

New `src/config.{c,h}`: a strict TOML-subset parser — `[section]` headers,
`key = value` with string / integer / boolean values, `#` comments; reject
anything else with a clear diagnostic (line number + reason). A `prov_config_t`
holds the resolved settings with documented defaults. `main.c` loads
`~/.prov/config.toml` at startup (missing file = defaults, never an error) and
uses `tabstop` and `trigger` (today hardcoded `PROV_TABSTOP 4`, `"zx"`); other
keys are parsed and stored for later stages (scrolloff, expandtab, ...).

- Pure parser over an in-memory string (no file I/O in the parser) → fully
  unit-testable. A thin loader reads the file via `platform`/proven and feeds
  the parser.
- Test `tests/test_config.c`: valid sample → expected values; comments/blank
  lines; bad syntax → error with line number; unknown key → tolerated (warn) or
  rejected (decide: **tolerate unknown keys**, reject malformed *syntax*).

## Stage M3.2 — Multi-buffer model + buffer list (`zb`) · SPEC §10.7

Introduce a buffer set: an array of open editors with an active index, owned by
the app (lifting ownership out of the single `prov_editor_t` in `main`).

- `zo` opens a file into a **new** buffer and focuses it (interim: open the path
  given on a prompt — depends on M3.4; until then `zo` can cycle/Open-next or be
  wired once the prompt exists). `zb` shows the buffer list (name, modified
  flag, current marker) and lets the user pick one.
- A small `src/bufset.{c,h}` (pure list management: add/remove/active/next/prev,
  find-by-path) with tests. Rendering of the list reuses the command-line/overlay
  area.
- Quit semantics extend: quitting the last buffer exits; otherwise closes the
  buffer (the quit wizard in M3.4 handles dirty buffers across the set).

## Stage M3.3 — Tabs (`t` namespace) · SPEC §10.7

`tn` new tab, `tq` close tab, `tj`/`tl` prev/next, `[N]t` go to N-th. A tab is a
named view; until panes exist (M3.5) a tab holds a single buffer reference + its
viewport. Render a one-line **tab bar** at the top (reuse the reserved-rows
pattern). `src/tabs.{c,h}` pure list logic + tests; `main.c` renders the bar and
routes the `t` namespace.

## Stage M3.4 — Command window / prompt + quit wizard · SPEC §13

A prompt/input mode on the command line (already one row, expands when needed):

- `zp` command prompt, `zo` file-open prompt (text input + Enter/ESC).
- Replace the interim quit guard (§13.1) with the **quit wizard**: across the
  buffer set, offer Write / Write-As / Discard / Quit-All / Cancel for dirty
  buffers. Deterministic key choices (`w`/`r`/`d`/`q`/`c`).
- A small input-line state machine in `main.c` (or `src/prompt.{c,h}` if it
  grows). Search prompt UI is M4; this stage builds the reusable input row.

## Stage M3.5 — Panes / splits (`w` namespace) · SPEC §10.7, §12

The largest change: the screen text area becomes a **pane tree** (binary splits,
each leaf = buffer ref + viewport + focus). `wh` horizontal split, `wv` vertical
split, `wq` close pane, `ww` cycle focus, `wm` focus-move mode, `ws` resize mode.

- `src/layout.{c,h}`: pure pane-tree model — split, close, focus cycle, and
  **rect assignment** (given a screen rect, compute each leaf's rect) with
  borders/labels per §12.1. Unit-tested without a terminal.
- `main.c` render loop iterates panes, renders each viewport into its rect, draws
  borders/labels, and places the cursor in the focused pane. Input routes to the
  focused pane's buffer.

This stage is gated behind M3.2 (buffers) and benefits from M3.3/M3.4; if time is
short it can ship `wh`/`wv`/`ww`/`wq` first and defer `wm`/`ws` resize modes.

---

## Cross-stage notes / decisions

- **Unknown config keys**: tolerated (parsed-and-ignored or warned), only
  malformed *syntax* is an error — keeps forward/backward compatibility.
- **Reserved rows**: top tab bar (M3.3) + bottom status + command line. The text
  area shrinks accordingly; panes subdivide the text area.
- **Ownership**: the app struct owns buffers/tabs/panes; `prov_editor_t` stays
  per-buffer. Keep modules opaque so a pane tree backend can evolve.
- Deferred within M3 if needed: `wm`/`ws` resize/focus-move sub-modes, theme
  colors (`[theme]`), mouse.

## Verification per stage

`./nob test` green (new unit tests per pure module), ASan/UBSan on touched core,
PTY check for the interactive behavior, native + `win64` build clean, commit,
update the progress log below + `CONTEXT.md`.

---

## Progress log (update after every stage — resume here)

- [x] **M3.1 Config** — `config.{c,h}` (TOML subset parser, tolerant unknown
      keys), `load_config()` in main, wired `tabstop` + `trigger` (2-char),
      `tests/test_config.c`. Commit `f22022b`. native+ASan+win64 clean.
- [x] **M3.2 Multi-buffer + `zb`** — `bufset.{c,h}` (set + per-buffer state),
      `main` opens one buffer per arg and mirrors the active buffer
      (buf_load/save/switch); `zb` lists + digit switches; status `[i/n]`.
      `test_bufset.c`. Commit `696b19f`. native+ASan clean.
- [x] **M3.3 Tabs** — `t` mirrors `g` (`[N]t` goto, `tn/tq/tj/tl`); tab bar via a
      new `prov_term_present(..., tabbar)` arg (both backends); tabs==buffers for
      now. Reclaimed `t` from standalone till. Commit `1f41378`. native+ASan clean.
- [x] **M3.4 Command window / quit wizard** — command-line text prompt (zo open
      file, zp `w`/`q`/`e <path>`); multi-buffer quit wizard (w/d/c, zqq). Commit
      `5315a71`. native+ASan clean.
- [x] **M3.5 Panes** — `layout.{c,h}` pure binary pane tree (leaf = buffer index +
      own viewport `top`; H/V split nodes; ops: init/split/close/focus_next/
      leaf_count/rects, 50/50 with 1-cell borders), `tests/test_layout.c`.
      Integrated in `main.c`: `render_panes()` blits each leaf's buffer into the
      composited grid with `─`/`│` borders, cursor lands in the focused pane, and
      the focused pane's height drives the cursor-visible scroll + page size. The
      focused leaf's (buffer, top) is the editing target; `bufs.active` mirrors it
      so the buffer/tab code is unchanged. `w` namespace: `wh` hsplit, `wv` vsplit,
      `ww` focus next, `wq` close pane (`wm`/`ws` deferred). Buffer close (`tq`) is
      pane-aware (fixes up every leaf's buffer index when the set shifts). Commits
      `0ed66da` (model) + this. native+ASan/UBSan+release+win64 clean; PTY verified
      vsplit/hsplit/nested/close, edit reflected across panes, clean quit.

**M3 core complete** (5 deliverables shipped). Post-M3 sweep done so far:
unimplemented-command feedback; scrolloff/expandtab/undo_limit wired (commits
`aaa1948`/`5df6d2b`/`8980147`/`4f6ddf3`).

## Stage M3.6 — Pane polish (the deferred M3 items) · SPEC §10.7, §12.3

Finishes the pane chrome and the two pane submodes left out of M3.5.

- [x] **M3.6a Labels + focus/dim styling** (commit `8e1f…`/see log). `prov_cell_t`
      gained `attr` (REVERSE/DIM); both backends emit the SGR. `render_panes`
      reserves each pane's bottom row for a label (focused REVERSE, others DIM) and
      dims unfocused content; focused text height = `rect.h - 1`. Single-pane uses
      a fast path (render straight into the grid — also drops the extra copy).
- [x] **M3.6b `ws` resize.** Split node `ratio` (default 50); `prov_layout_split_span`
      shared by `rects_rec` AND `render_panes` (fixed a real bug: the renderer had
      its own hardcoded 50/50 math, so resize would have been invisible).
      `prov_layout_resize` clamps [10,90]; `ws` submode + legend.
- [x] **M3.6c `wm` focus move.** `prov_layout_move_focus` (geometric nearest leaf);
      `wm` submode, i/k/j/l directional. Both pure ops unit-tested.
- [x] **M3.6d** efficiency/error pass: single-pane render fast path; the duplicated
      split-math bug above; full re-verify (13/13, ASan/UBSan, release, win64).

`wm`/`ws` are main.c submodes (session `pane_mode`), not parser grammar — the
parser only adds `PROV_ACT_PANE_MOVE`/`PROV_ACT_PANE_RESIZE`.

**Milestone M3 fully complete** (core + all deferred polish). Status: Implemented.
