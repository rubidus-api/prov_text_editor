# RFC 0011 — main.c / session structure refactor (clarity without regression)

- Status: **Implemented** — Phase 1 (skipped, documented), Phases 2–4 done, Phase 5 done (5a–5d) then stopped at the safe boundary.
- Created: 2026-06-20

## 1. Scope & motivation

`main.c` is the event-loop driver. After the `draw` module extraction (commit
`e49107f`) it is **3363 lines / 167 functions**. An audit (this session) found
that the size itself is not the core problem — only 5 functions exceed 100
lines, and the `switch` usage is overwhelmingly legitimate (translating parsed
enums to actions; the parse-then-execute split between `command.c` and
`zx_execute` is *good* design). The real issues are:

1. **God-object session struct.** `prov_session_t` carries ~100 flat fields
   spanning ~10 unrelated concerns (core editing, tabs/windows, browser, panel,
   search, registers, macros, clipboard, jumplist, prompt/cmdline). It is hard
   to see which field belongs to which subsystem, and every helper takes the
   whole `prov_session_t *`.

2. **Struct memory is reserved, not used.** `sizeof(prov_session_t)` ≈ **80–90
   KB**, of which **`tabs[16]` alone is 45,312 B** (each `prov_layout_t` is
   2,832 B = `nodes[64]` + `freelist[64]`, and 16 tabs are always reserved while
   a typical layout uses 1–7 nodes). This is a **single stack instance** in
   `main()` — never copied per frame, never heap-churned — so it is *not* a
   runtime cost. It is a cognitive/footprint concern only.

3. **Two genuine "ugly branch" hotspots** (not the switches in general):
   - `zx_execute` is a 356-line mega-switch; its `PROV_CMD_ACTION` arm nests a
     ~20-case switch inline.
   - **Panel dispatch is scattered**: "what each panel does" lives across 11
     `panel_open_*` functions, the `panel_activate` switch, `panel_key`, and the
     draw-time help branch. This is the one place a data-driven table is clearly
     more elegant *and* more reusable.

4. **`main()` is 326 lines** — the whole event loop + per-frame render assembly
   in one function.

### Guiding constraint (carried from the user's priorities)

**Stability #1, performance #2, readability #3.** No change may regress
correctness or performance. Because release builds use `-flto`, moving pure code
across translation units is performance-neutral (calls inline back). Anything
touching a hot per-character render/parse path, or that cannot be proven
behavior-identical, is out of scope. Every phase ends green: `./nob` +
`./nob test` (21/21) + ASan/UBSan + a PTY smoke of the touched surface.

## 2. Non-goals

- No behavior, keybinding, or UX changes. Pure structure.
- No change to the core modules' algorithms (buffer/editor/regex/etc.).
- No splitting of session-coupled code into new translation units that the test
  binaries link — `nob` links every `src/*.c` except `main.c` into the tests, so
  a new core file that calls back into `main.c` statics would break the test
  link. New modules must be closed under their call graph (the `draw` extraction
  worked precisely because it was session-free).

## 3. Phased plan (ordered for efficiency & ascending risk)

Each phase is independently committable and independently verifiable. Ordered so
that **low-risk/high-clarity wins land first**, and the riskier wide-churn
grouping is approached last, one subsystem at a time.

### Phase 1 — `tabs` lazy footprint — **SKIPPED (documented)**
Investigated and deliberately skipped. The 45 KB `tabs[16]` reserve is part of a
**single `prov_session_t` stack instance** in `main()` (verified: never copied
by value, never `memcpy`'d), so it costs nothing at runtime — 45 KB on an 8 MB
stack, allocated once. The only ways to shrink it are:
- lower `PROV_MAX_TABS` / `PROV_MAX_PANE_NODES` → a **behavior change** (caps how
  many tabs / window-splits a user can open); or
- make `tabs` a heap pointer with lazy per-tab allocation → relocates rather than
  reduces memory, and introduces **new allocation-failure paths** and 11 access
  sites to touch.

Both violate the stability-first constraint for no real efficiency gain. The
footprint is a non-issue; **the right engineering decision is to leave it.**

### Phase 2 — panel dispatch table *(medium risk, high elegance)*
Replace the scattered panel knowledge with one descriptor table keyed by
`PANEL_K_*`: `{ open_fn, activate_fn, help_lines }` (help text already centralized
in `draw.c::prov_panel_help_lines`). `panel_activate`'s switch collapses to a
table lookup; adding a panel becomes "add a row." Verify every panel still opens,
activates, filters, and closes.

### Phase 3 — `zx_execute` decomposition *(low risk, conservative split)*
Extract cohesive arms into named helpers (`zx_exec_edit`, `zx_exec_operator`,
`zx_exec_action`, `zx_exec_findchar`, …) so the top-level switch reads as a table
of intents. Pure extraction, no table-of-function-pointers (which would hurt the
simple cases). Behavior-identical.

### Phase 4 — `main()` render-assembly extraction *(low risk)*
Pull the per-frame grid assembly out of `main()` into a `compose_frame()` (or
similar) so the loop body shows intent: read key → dispatch by mode → compose →
present. No change to render order or output bytes (PTY byte-compare).

### Phase 5 — session subsystem grouping *(higher risk, wide churn — last, incremental)*
Group flat fields into named sub-structs, **one subsystem per commit**, each
fully verified before the next. Done in ascending site count, the compiler
enforcing completeness (a removed flat field makes any missed access a compile
error), with a byte-identical frame diff vs the pre-commit binary as the
behavioral check:

- **5a `s->jump`** (jumplist; 17 sites) — done, validated the mechanics.
- **5b `s->search`** (12 fields, 127 sites) — done, byte-identical.
- **5c `s->browse`** (11 fields, 97 sites) — done, byte-identical.
- **5d `s->macro` + `s->feed`** (10 fields, 69 sites) — done, byte-identical.

**Stopped here (documented).** The remaining flat state is deliberately left:
- `regs[36]` is **already cohesive** (one struct-array `s->regs[i].{bytes,len,
  shape}`); wrapping it gains nothing.
- The **panel UI flags** (`panel_open/kind/filter/help/verb/scroll/page/
  help_scroll`) cannot be grouped under a `panel` sub-struct without colliding
  with the existing `prov_panel_t panel` model, whose own members (`.filter`,
  `.pos`, `.sel`, `.title`, …) would make a mechanical rename ambiguous and
  error-prone. High collision risk, low naming benefit → leave flat (this is the
  "too invasive to do safely" stop the plan anticipated).
- `prompt_*`, `block_*`, `field_*` are small and tightly loop-coupled; grouping
  them is churn without meaningful clarity gain.

## 4. Exit criterion

Proceed phase by phase until the next available change would either (a) risk a
correctness/performance regression, or (b) yield negligible clarity gain for its
churn. Document where we stop and why. A partially-completed Phase 5 (some
subsystems grouped, others left flat) is a valid, consistent stopping point.

## 5. Verification protocol (every phase)

1. `cc -o nob nob.c && ./nob` — warning-free (vendored proven aside).
2. `./nob test` — 21/21.
3. ASan/UBSan build + PTY exercise of the touched surface.
4. Commit with a focused message; update CHANGELOG when user-visible (none
   expected — these are internal).
