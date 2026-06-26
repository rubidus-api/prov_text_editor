# RFC 0010 — Common overlay panel (full / half-screen) + shared zx-style keymap + 0-series unification

- Status: **Implemented** — P1–P8 done (verbs, quick-pick, heavy/virtual control, browser, 0e/0m/0s/0z). P9 done.
- Created: 2026-06-19

## 1. Scope & motivation

prov grew several "open a window that shows a list / options" surfaces, each
hand-rolled: `draw_help` (full-screen text), `draw_browser` (file-open list +
columns + options box + filter), `draw_popup` (the `ww` window-list box). The
`0`-series commands are already *parsed* but unimplemented and want the same
thing: `0b` registers, `0m` bookmark overview, `0e` macro overview, `0s` search
history, `0u` undo list, `0w` windows, `0t` tabs.

Building each separately repeats render, scroll, selection, and filtering.
**This RFC defines one data-driven modal panel** that all list/option surfaces
use, with a deliberately tiny geometry model.

Non-goal (settled in design): floating/draggable windows, z-order, and
concurrent editing while a panel is open. The panel is **modal** — it owns input
while open; the editor underneath is visible *context*, not editable. That is
enough for the 0-series ("glance, pick, dismiss"), and keeps the implementation
small.

**Guiding principles:**
- **One at a time.** At most one panel is open; there is no panel stacking. So
  the keymap need not be globally collision-free across panels — each panel
  reuses letters as fits. What gives a *consistent* UX is **self-documentation**:
  every panel shows its keys (the `h` help page, the footer legend, and inline
  key hints), so the user never has to memorize a global table.
- **Minimal core.** The panel module stays small and simple. Existing surfaces
  (browser, `ww`, the 0-series) migrate onto it or share its code, but the panel
  must not grow into a verbose framework — lean structure, few features, the rest
  supplied by each caller's data.

## 2. Design

### 2.1 Geometry — full or half of the edit region, snapped (no center box, no auto-size)
The panel is positioned relative to the **whole edit-window region** (the area
the edit panes occupy), not relative to any single window. Five snapped positions:

- `FULL` — the entire edit region. (Per §2.7 it also takes the global status-line
  row, since that row shows editor file info that is irrelevant under a modal
  panel; the **command line** still renders below for `ss`/rename text input.)
- `TOP½` / `BOTTOM½` / `LEFT½` / `RIGHT½` — half of the *whole* edit region; the
  other half keeps showing the editor (peek at code). For a half panel the global
  status line stays (the editor is still partly visible).

A single key (`w`, §2.6) **cycles** the position (`FULL → BOTTOM → TOP → LEFT →
RIGHT → …`); default `FULL`. No center popup, no content-based auto-sizing —
predictable and trivial to lay out.

### 2.2 Implementation = a layer composited over the edit region (no reflow)
The panel is a **separate layer drawn on top of the already-rendered editor** —
the edit windows are **not** resized or reflowed. The frame renders the editor
normally into the full edit region (exactly as today), then `draw_panel`
**overwrites the grid cells** of the panel's rect (FULL, or one half of the
region). Where the panel sits, you see the panel; the uncovered half shows the
editor untouched (peek at code).

Consequences (all simplifications):
- No `render_panes`-into-a-sub-rect plumbing, no viewport re-clamp, no cursor
  re-flow. The editor render is unchanged; the panel is a rectangular blit over
  it. This is the same idea as today's `draw_help`/`draw_browser`/`draw_popup`,
  generalized to a positioned rect.
- A left/right half is just a rectangular region of cells overwritten — there is
  **no mid-editor wrap recomputation** and no per-window splitting, because the
  editor underneath keeps its own layout. (This removes the earlier left/right
  reflow risk entirely.)
- The panel rect carries its own chrome (§2.7); its right/bottom edges are clean
  rectangle borders over whatever the editor drew.

### 2.3 Input reuse — the command line hosts text sub-modes
The panel base mode is **modal command input** (digits = counts, letters = verbs
— §2.6), *not* type-to-filter. When a panel needs free text — `ss` search/filter,
or a `rename`-style verb — it enters a text sub-mode that reuses the existing
prompt machinery on the **command line** (`handle_prompt_key`-style: typing, Tab
autocomplete, Enter commit, Esc back). So the panel needs **no input row of its
own**; it is a **display + selection** surface, and the one persistent command
line serves every panel's text entry.

### 2.4 Data model / API (`src/panel.{c,h}`, pure, allocator-injected)
```
typedef enum { PANEL_FULL, PANEL_TOP, PANEL_BOTTOM, PANEL_LEFT, PANEL_RIGHT } prov_panel_pos_t;

typedef struct {            /* one row, caller-owned */
    const char  *text;      /* primary label */
    const char **cols;      /* optional extra columns (NULL/0 = none) */
    int          ncols;
    int          id;        /* caller's meaning (slot letter, buffer idx, …) */
} prov_panel_row_t;

typedef struct {
    const char        *title;
    prov_panel_row_t  *rows;
    proven_size_t      nrows;
    prov_panel_pos_t   pos;
    const prov_keymap_t *keys;   /* lowercase-verb table (§2.6); drives dispatch + help */
    const prov_helpdata_t *help; /* compact per-panel help data, rendered live (§2.9) */
    /* on_select: Space/Enter activates the focused row (the OK for a picker). */
    void (*on_select)(void *ctx, int row_id);
    /* on_action: a verb / footer key from `keys` fired (o, r, y, c, d, …); gets
     * the action and the focused row so one handler covers every command. */
    void (*on_action)(void *ctx, int action_id, int row_id);
    void  *ctx;
    /* selection/scroll state owned here; reused from the browser logic */
    /* optional: column-visibility options box (browser-style); detail strip */
} prov_panel_t;
```
Navigation (`prov_nav_decode`: arrows + `ijkl` + Page/Home/End), counts, the
`g`/`s` namespaces, verb dispatch, and `h` help all come from the shared `keymap`
module (§2.6). `Space`/`Enter` call `on_select`; every other bound key (verbs and
the `y`/`c`/`d` footer actions) calls `on_action` with its `action_id`. The
`ss` search/filter and column display are **lifted from the file browser**
(`browse_refilter` / `browse_mode_key`), which becomes the first consumer rather
than a parallel implementation.

When `on_action` mutates the underlying data (e.g. a verb deletes a register),
the caller rebuilds its rows and calls **`prov_panel_reload(p, rows, nrows)`** to
re-point the panel and re-clamp selection/scroll — the rows are caller-owned, so
the panel never copies or frees them.

### 2.5 Lifecycle
Session holds one `prov_panel_t` + an `open` flag (mirrors `help_mode`/
`browse_mode`). Render: the editor draws normally, then `if (panel.open)
draw_panel(...)` composites the panel layer over its rect (§2.2). Input:
`if (panel.open) panel_key(...)` ahead of the mode dispatch. `on_select` /
`on_action` perform the result (jump to bookmark, switch register, …) and usually
close the panel.

### 2.6 Interaction model — a shared, zx-style keymap (`src/keymap.{c,h}`)
The panel should *feel like zx mode*: modal, deterministic, count-prefixed,
lowercase-verb keys, `g`/`s` namespaces, `h` for help. It is therefore a small
deterministic key parser (a state machine like `command.c`, not a flat table)
plus a per-panel verb table, both built on a shared pure module
`src/keymap.{c,h}`.

**Concrete key grammar (current control = the focused list):**

| Key | Action |
|---|---|
| `↑↓←→` / `i k j l` | move selection (up/down); left/right surface-defined (browser: parent / enter-dir) |
| `[N]` + a motion | repeat N times (`5k`, `5↓` = up 5) — a leading number is a pending prefix; a key that does *not* take a count **discards** it and acts plainly (`5`+Enter = Enter; to pick row 5 use `5g`) |
| `[N]g` | go to item **N** (1-based) in the current control |
| `0g` | go to the **last** item |
| `g` + a letter | **(v2)** the **`g`-namespace is reserved for movement**; each panel assigns its own letters (`gf gl gu gd gp gn gg ge …`) to whatever motions fit its content — they need *not* mean "first"/"last" globally. Only `[N]g`/`0g` are universal (those ship in v1). |
| `[N]t` | **(v2)** switch to panel **tab N** (when the panel has tabs) |
| `ss` | **search/filter** — opens a regex pattern prompt on the command line (RFC-0009 engine; literal substring until it lands) that incrementally narrows the list; `Tab` autocompletes, `Enter` commits the filter (or jumps to the match), `Esc` cancels |
| `Space` / `Enter` | **activate** the focused row (`on_select`) — for a picker this *is* the OK; in an options box it toggles the focused option |
| lowercase verbs | per-panel `prov_keymap_t` (`o` open, `r` rename…) → `on_action` |
| `y` / `c` / `d` | footer actions **OK / Cancel / Discard** (§2.7). `y` appears **only when committing the whole panel differs from activating one row** (multi-toggle / confirm panels); a pure picker shows just `c` (and `Esc`), since Enter already is the OK |
| `h` | context help — the panel's own help page, rendered live from compact data (§2.9) |
| `w` | position-cycle `FULL → BOTTOM → TOP → LEFT → RIGHT` (the `+` corner is its mouse target) |

All command/footer keys are **lowercase letters** (zx's intent — no Enter/Esc/Ctrl
as a *canonical command*). Text **sub-modes** (`ss` search, `rename`) are the one
exception: while typing on the command line, Enter commits / Esc backs out / Tab
autocompletes / Backspace edits, naturally. `Esc` also backs out of the base mode
globally, but is not shown as a footer button.

*Reserved letters (for panel authors):* `i j k l` (nav), `g` `s` `t` namespaces,
`h` help, `w` cycle, `y` `c` `d` footer, and the digits. A panel picks its verbs
from the rest (`a b e f n o p q r u v x z`). There is no global table to memorize
(panels are mutually exclusive); this list just keeps an author from colliding
with the universal keys.

This mirrors zx exactly: a leading number is a **count/index** consumed by the
next key (motion → repeat; `g` → item index, `0g` = last; `t` → tab index). The
`g` namespace parallels the editor's goto/movement namespace, collapsed to a
1-D list. So muscle memory carries over from editing to the panel.

**Shared module (`src/keymap.{c,h}`, pure) — a reusable data+handler engine.**
It is not panel-specific: it is a small reusable modal-key engine that future
surfaces can drive with their own table, and the panel is its first (seed)
consumer. It provides:
- `prov_nav_decode(prov_key_t) → NAV_{NONE,UP,DOWN,LEFT,RIGHT,PGUP,PGDN,HOME,END}`
  (arrows + `ikjl` + Page/Home/End — the shared atom that dedups `is_movement`/
  `move_by` and the browser's arrow handling);
- the count accumulator + discard rule (shared atom);
- a tiny `g`/`s` namespace dispatch.

**Grammar is data + handler, not one or the other.** Each `prov_keymap_t` entry
is **data** — `{ key, action_id, label }` — which makes the binding *introspectable*:
the same table generates the dispatch, the footer legend, **and** the `h` help
page. The *behavior* is a **function** (`on_action(action_id, row)`); a rare
irregular action (e.g. a verb that consumes an argument) may carry an optional
per-entry handler as an escape hatch. This keeps ~all of the grammar declarative
(so help/legend/consistency come for free) while staying fully expressive. A pure
function-pointer "swap a whole behavior set" was rejected: it is expressive but
*opaque*, losing exactly the introspection (auto-help, consistency-by-construction)
that motivates the abstraction.

The zx **text-editing** grammar (operators, text objects, buffer motions) is
richer and irregular, so it **stays imperative in `command.c`** and is not
rewritten. It may later adopt `prov_nav_decode` (to dedup motion handling) or
migrate onto this engine incrementally — an explicit *future option*, not part of
this RFC.

### 2.7 Panel chrome — a window-style frame (reuses edit-window conventions)
A panel renders as a window-like rect with chrome on all four edges, reusing the
edit-window idiom so it reads as a sibling of an edit window:

- **Top row** — reverse video, left-aligned: the **title**, optionally followed
  by `│`-separated **tabs** (`Title │ tab │ tab`, active tab highlighted);
  title-only when the panel has no tabs.
- **Bottom row** — reverse video: a **footer action bar** plus the keymap legend.
  The action bar shows the fixed triad — **`y` OK**, **`c` Cancel**, **`d`
  Discard** — each rendered *only when meaningful* (a pure picker shows just `c`;
  a save-on-close panel shows all three). `c`/`d` match prov's quit-wizard
  (`zq` → w/d/c); `y` generalizes its save-specific `w`. Remaining width holds the
  verb legend (generated from `prov_keymap_t`). For a `FULL` panel this footer
  also stands in for the editor's global status line (which is covered, §2.1).
- **Right edge** — a vertical **scrollbar** like edit windows (`draw_scrollbar`:
  track + thumb, `▲`/`▼` buttons when tall enough).
- **Bottom-right corner** — a **`+`** glyph = the position-cycle handle; a future
  mouse click there cycles the panel position (keyboard equivalent = `w`). Reuses
  the existing window corner-handle convention.

Because edit windows already carry exactly this chrome (`draw_window_status`,
`draw_scrollbar`, the `+` corner), the panel reuses those routines rather than
new ones.

### 2.8 Panel tabs (optional — v2)
A panel MAY split its content into tabs — e.g. registers `a–z` vs `0–9`, or a
files/bookmarks/recent split. Tab titles render in the top bar; `[N]t` switches;
the active tab is highlighted. Most panels have none (title only). Tabs are a
panel feature, independent of the editor's global tab bar (desks). **Deferred to
v2** (the top-bar title already renders; tabs add to it later) — no v1 consumer
needs them.

### 2.9 Help — per-panel, but stored as compact data and rendered live
Each panel has **its own** `h` page (decided). To avoid baking large pre-formatted
strings, a help page is **compact data, reflowed at render time** to the current
panel/screen size — never a fixed-width blob:

- Most of the page is **derived from the keymap**: each `prov_keymap_t` entry
  already carries `{ key, label }`, so the key list generates itself (one source
  → dispatch + footer legend + help body).
- A panel adds only a short `prov_helpdata_t` — a small array of
  `{ section, line }` description strings (intent, not key list). Sections/lines
  are plain UTF-8, wrapped to the live width when drawn.
- Rendering reuses the existing help overlay's word-wrap + scroll, so no new
  layout engine. Because the source is data, the same page lays out correctly at
  any width without per-size copies.

`h` opens the page **over** the panel and `Esc`/`h` returns to it (a one-level
return, not a general stack — panels do not nest beyond this). The editor's own
`help.c` topic pages can migrate to the same `helpdata`-render path later, but
that is not required here.

## 3. Consumers (unified onto the panel)
- **File browser** — refactor onto the panel **and adopt the modal key grammar**
  (decided): letters become verbs, navigation is arrows/`ikjl` + the `g`-series,
  and filtering moves to `ss` (no more filter-on-any-keystroke). It keeps its
  file-info columns + options box as panel features.
- **`ww` window list** (`draw_popup`) — becomes a panel; digit/Enter focuses.
- **0-series**: `0b` registers, `0m` bookmark overview, `0e` macro overview,
  `0s` search history, `0u` undo list, `0w` windows, `0t` tabs — each is "fill
  rows + on_select". The actions are already parsed (`PROV_ACT_*_BROWSER` /
  `_OVERVIEW`).
- **Editor help (`help.c`)** stays its own paged-text overlay for now (breadcrumb
  + scroll %); each *panel* has its own `h` page via the compact `helpdata` render
  (§2.9). The two render paths may converge later; not required here.

**v1 / v2 split (keep the core minimal):** v1 = arrows/`ikjl` + `[N]g`/`0g` + `ss`
+ verbs + `y c d` footer + `w` cycle + `h` help. **Deferred to v2:** panel tabs
(§2.8, `[N]t`) and the extended `g`-motions (`gf gl gu gd gp gn`). v1 covers the
0-series ("glance, pick, confirm"); v2 features attach when a panel needs them.

**Engine decision (open-Q2, resolved — see §7):** `keymap.c` is built as a small
**reusable data+handler engine** (the panel is its seed consumer, future surfaces
reuse it). It shares the *atoms* (`prov_nav_decode` + the count rule) with
`command.c` and owns a small `g`/`s` dispatch; the grammar is a **data table**
(`{key, action_id, label}`, introspectable → auto help/legend) with a **function
handler** (`on_action`, expressive). A pure function-pointer abstraction was
rejected (opaque — loses introspection). `command.c` stays imperative and is **not**
rewritten; migrating it onto this engine is an explicit future option.

- [x] **S0 — Shared keymap module (v1).** `src/keymap.{c,h}` (pure):
      `prov_nav_decode` (arrows + `ijkl` + Page/Home/End — the shared atom), the
      count accumulator + discard rule (shared atom), a small `g`/`s` dispatch for
      `[N]g`/`0g` and `ss`, the `prov_keymap_t` verb-table dispatch, and
      keymap→help/legend generation. Unit-tested on a fixed keymap (pure
      key-sequence → action table); the browser's arrow handling re-homes here as
      the first user. (Extended `g`-motions and `[N]t` are v2.)
- [x] **S1 — Panel core.** `src/panel.{c,h}`: model + filter/select/scroll over
      the S0 keymap (extract the rest from the browser). Unit-test the pure parts
      (filter, numeric pick, selection clamp) with a fixed row table.
- [ ] **S1.5 — Help data render.** `prov_helpdata_t` + a live-reflow renderer
      (reusing the help overlay's word-wrap) driven by the keymap labels + a short
      description; `h` opens it over the panel, returns on `Esc`/`h`.
- [x] **S2 — Geometry + render.** Position enum + `w` cycle; `draw_panel` as a
      **composite layer** over the edit region (FULL covers the global status row;
      halves leave the editor visible). Blit hygiene: clear wide-glyph tails at
      the panel edge. PTY: each position renders, editor visible in the uncovered
      area. *(Row storage is a fixed 64-slot session buffer — fine for 0w/0t; S3
      will need dynamic rows for directories.)*
- [ ] **S3 — Browser onto the panel.** Re-home the file browser; columns +
      options box become panel features. Keep `test_browser` green (model split
      from UI as today). *(Deferred — needs dynamic panel rows first (a directory
      far exceeds the 64-slot buffer), and the file browser is already polished &
      well-tested; rewriting it risks regressions. Worth doing, but as its own
      careful pass, not bolted onto this one.)*
- [ ] **S4 — `ww` onto the panel.** *(Re-evaluated: not a clean win. `draw_popup`
      is also used by the browser's column-options box, so it can't be deleted;
      and `ww` is a deliberate one-keystroke quick-picker (digit focuses a window)
      — a faster UX than the modal panel, which `0w` already provides for the
      navigable case. Keeping both is the better outcome.)*
- [~] **S5 — 0-series.** `0w` (windows), `0t` (tabs), `0b` (registers; pick =
      paste) **done** (data already to hand; activate via `L->focus` / `tab_goto`
      / `reg_load`+paste). **Blocked, need a decision:** `0e`/`0m` already mean
      macro-record / bookmark-set, so the macro/bookmark *overviews* need a spare
      key; `0s` (search history) and `0u` (undo list) have no backing store yet;
      `0z` (command list) needs an introspectable command table (command.c is
      imperative today).
- [ ] **S6 — Sweep.** SPEC §9 / CHANGELOG; confirm every panel shares the one
      idiom (`ik`/↑↓ select · `jl`/←→ context · `[N]`+motion repeat · `[N]g`/`0g`
      goto · `g*` panel motions · `[N]t` tab · `ss` search+Tab · lowercase verbs ·
      Space/Enter activate · `y c d` footer · `h` help · `w` cycle · Esc back),
      with the footer legend and each panel's `h` page both generated from its
      `prov_keymap_t` + `helpdata` — the self-documentation that keeps panels
      consistent without a global key table.

## 5. Out of scope (deliberate)
- Floating/draggable windows, z-order, multiple simultaneous panels.
- Concurrent editing with a panel open (panel is modal).
- Center auto-sized popup and a separate "fullscreen mode" toggle — collapsed
  into the FULL/half snap model.
- Mouse interaction (a later, orthogonal addition; the rect model is ready for
  hit-testing when it lands).

## 6. Risks
- Compositing a layer must cleanly overwrite cells, including wide-glyph **tails**
  left by the editor underneath at the panel's left/right edge — clear a covered
  cell's continuation flag so no half a wide char bleeds through. (This is a small
  blit-hygiene detail, not the editor-reflow problem the old design had.)
- Extracting browser logic without regressing it — keep the browser model/tests
  intact (only the UI moves onto the panel).
- A HALF panel with full four-edge chrome can get cramped at small terminal sizes
  — clamp to a minimum interior and fall back to `FULL` when a half would be
  unusably small.
- Scope: the keymap mini-parser (§2.6) is the heaviest piece. Bounded by the v1/v2
  split (§4) and the resolved sharing decision (§7): share only the atoms
  (`prov_nav_decode` + count rule), keep a small own `g`/`s` dispatch — so it does
  not grow into a second full command engine.

## 7. Decisions & open questions
**Decided:**
- **Layer, not reflow (§2.2).** The panel is a layer composited over the *whole*
  edit region; edit windows are never resized. The uncovered area shows the editor
  unchanged. (This was a correction — it also removes the left/right reflow risk.)
- **One panel at a time; no global key table.** Panels are mutually exclusive, so
  letters may be reused across panels. Consistency comes from **self-documentation**
  (footer legend, `h` page, inline hints), all generated from each panel's
  `prov_keymap_t` + `helpdata` (§2.9) — not a memorized global map.
- **Minimal core (§1).** The panel stays lean; existing surfaces migrate onto it
  without bloating it.
- **Browser adopts the modal grammar** (letters = verbs; filter via `ss`).
- **`g`-series is a reserved movement namespace**; `[N]g`/`0g` universal, the rest
  assigned per panel. A pending count is **discarded** if the next key takes none
  (`5`+Enter = Enter; row 5 is `5g`).
- **Activation vs OK:** `Space`/`Enter` activate the focused row (= OK for a
  picker); the footer `y` OK appears only where committing the whole panel differs
  from activating one row.
- **`on_select` + `on_action` (§2.4):** Space/Enter → `on_select`; every verb and
  footer key → `on_action(action_id, row)`.
- **Footer/control keys (lowercase):** `y` OK, `c` Cancel, `d` Discard (match the
  quit-wizard's w/d/c spirit), **`w`** cycles position (the `+` corner is its
  mouse path). Text sub-modes (`ss`, rename) use Enter/Esc/Tab on the command line.
- **Per-panel help as compact data, rendered live (§2.9)** — no fixed-width blobs.
- **FULL covers the global status line; halves keep it.** The command line always
  stays (text sub-modes).
- **`prov_panel_reload` (§2.4)** lets a verb that mutates data refresh the rows.
- **v1 / v2 split (§4):** v1 keeps the core minimal; panel tabs and the extended
  `g`-motions are v2.
- **Keymap = reusable data+handler engine (open-Q2, resolved).** `keymap.c` is a
  small reusable modal-key engine (panel = seed consumer; future surfaces reuse
  it). Grammar is **data** (`{key, action_id, label}` → introspectable, so
  dispatch + legend + `h` help auto-generate) plus a **function handler**
  (`on_action`, expressive; optional per-entry handler for rare irregular actions).
  It shares the *atoms* (`prov_nav_decode` + count rule) with `command.c`. A pure
  function-pointer "swap whole behavior" was rejected — opaque, loses the
  introspection that motivates the abstraction. `command.c` stays imperative and is
  **not** rewritten; adopting `prov_nav_decode` or migrating onto this engine is an
  explicit future option.

**Open:**
1. **`ss` regex depends on RFC-0009.** Literal substring until the Pike VM lands;
   gains regex transparently after — so the panel can be built first. (Only
   remaining cross-dependency; not blocking — v1 ships with literal `ss`.)

## 8. Batch completion plan (2026-06-20) — P1…P9

The remaining work, bundled into one program (user-approved). Three design axes:
**(a) verb+slot input** (panel verbs + an "await slot key" sub-state, generalizing
"press set-key → slot-key → act"); **(b) light vs heavy controls** (the current
caller-owned `rows[]` for small lists; a new *virtual* source — count/row/filter
callbacks, visible-window lazy fetch, heap-allocated only while a heavy panel is
open — for the browser and future huge lists); **(c) speed** (zero per-frame heap
in the light path, cache the legend on open, and a `quick_pick` flag where a digit
`1-9` = goto+activate in one key, restoring the old `ww<digit>` feel).

Confirmed decisions: verb keys macro `r`=record/`x`=delete, bookmark `n`=set/
`x`=delete, register `x`=delete; quick-pick on the **windows** panel only;
**macro stop = `E`** (overloaded: `E` while recording = stop, `E` idle = replay
last, unchanged); `0z` activate = jump to that key's help page; `0u` (undo list)
skipped.

- [x] **P1 — Panel verb framework + slot sub-state.** `panel_key` dispatches
      `PK_VERB`; an "await slot key (a-z/0-9)" sub-state with a footer/cmdline
      prompt and Esc cancel. Per-kind verb handler. Unit-test verb dispatch.
- [x] **P2 — Macro panel (0e) + bookmark panel (0m): list + record/set/delete.**
      `0e` → macro panel (lists slots+lengths; `r`=record into slot, `x`=delete,
      Enter=replay); recording is **stopped by `E`**. `0m` → bookmark panel
      (slot+line+preview; `n`=set at cursor, `x`=delete, Enter=jump). `e<slot>`
      replay / `m<slot>` jump stay as power-user shortcuts. Helpers factored.
- [x] **P3 — `ww` → windows panel + instant digit quick-pick.** `quick_pick` flag
      + `PK_PICK` (goto+activate); `ww` routes to the windows panel; remove
      `win_list`/`win_list_key`/the `ww` `draw_popup` branch (keep `draw_popup`
      for the browser options box). Verify `ww<digit>` parity with the old popup.
- [x] **P4 — Panel speed polish.** Footer legend is built once at `prov_panel_init`
      (cached in `prov_panel_t.legend`), not regenerated each frame. The light
      render path is allocation-free per frame (stack buffers only; the filtered
      `view[]` reallocs only when the filter changes, never during steady-state
      render or navigation), so a panel opens and responds in O(visible rows) with
      no heap churn. The heavy/virtual control (P5) keeps this by fetching only the
      visible window.
- [x] **P5 — Dynamic / virtual "heavy" panel control.** A vsource interface
      (`count`/`row`/`filter`/`id`) + a panel dynamic mode (light path untouched;
      heavy state allocated on demand, freed on close). Unit-test the dynamic
      model against a synthetic 100k-item source (count/fetch/filter/clamp/scroll).
- [x] **P6 — Browser onto the heavy panel (was S3).** Adapt `prov_browser_t` as a
      vsource; route `zo` render+keys through the dynamic panel; info columns +
      Ctrl+O options become panel features. Keep `test_browser` green; PTY a large
      directory + ASan.
- [x] **P7 — Search-history panel (0s).** Session ring buffer of recent terms
      (dedup, ~32), pushed on search commit; `0s` lists them, activate = set term
      + search.
- [x] **P8 — Command browser (0z).** A static `{keys, description}` const table on
      the light panel; filterable cheat-sheet; activate = jump to that key's help.
- [x] **P9 — Sweep.** help pages (every 0-series live + manage verbs), SPEC §9,
      CHANGELOG, this RFC; full tests + ASan/UBSan + `-DPROV_DEBUG_LINES` + release
      + dist (win64 cross-build); memory.
