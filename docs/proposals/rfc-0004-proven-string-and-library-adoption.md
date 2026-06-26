# RFC 0004 — proven string-system & library adoption

- Status: **Implemented** (S0–S7 complete)
- Milestone: **Special Milestone S** (cross-cutting; runs alongside M3.5/M4)
- Author: prov maintainers
- Created: 2026-06-18

## 1. Motivation

A bottom-up review of `prov` shows the editor still hand-rolls a large amount
of string and dynamic-buffer code that `proven_c_lib` already provides in a
tested, bounds-checked form. The vendored proven build already compiles and
links `u8str`, `fmt`, `scan`, `array`, and `buffer` — we just don't use them
yet. Adopting them (especially the **string system**, `proven_u8str_t` /
`proven_u8str_view_t`) reduces fixed-buffer truncation bugs, removes manual
`memcpy`/index arithmetic, and centralises growth on tested code.

### 1.1 Survey (libc string/stdio call sites)

| File | string/stdio ops | stack `char buf[N]` | notes |
|---|---:|---:|---|
| `src/main.c` | 52 | 23 | status/cmdline/tabbar/popup formatting, prompts, basenames |
| `src/config.c` | 11 | 3 | TOML parser: fixed `section[32]`/`key[48]`/`sv[64]`, manual int/bool |
| `src/buffer.c` | 6 | 0 | piece-table **add buffer** = hand-rolled growable byte store |
| `src/command.c` | 4 | 0 | `put()` label/describe writes |
| `src/save.c` | 3 | — | temp-path construction for atomic save |
| `src/bufset.c` | 3 | — | `path[1024]` copy/compare |

Hand-rolled structures worth re-housing on proven:
- piece-table `add` / `add_len` / `add_cap` → `proven_u8str_t` (append-only).
- session string fields `prompt_buf[1024]`, `message[120]`, `zx_pending[24]`,
  `zx_last[80]`, `bufset.path[1024]` → owning `proven_u8str_t` or bounded views.
- read-only slicing/compare (config tokens, basenames, prefix tests) →
  `proven_u8str_view_t` + `proven_u8str_view_eq` / `_starts_with` / `_slice`.

### 1.2 proven string API in scope

- `proven_u8str_view_t {ptr,size}` — read-only view; `PROVEN_LIT("..")`,
  `proven_u8str_view_from_cstr`, `_eq`, `_starts_with`, `_ends_with`, `_slice`,
  `_find`. Zero-allocation; the primary safety win.
- `proven_u8str_t` — owning, growable UTF-8 string over `proven_buf_t`.
  `_create`, `_reserve`, `_append`/`_append_grow`/`_append_byte`, `_insert`,
  `_remove`, `_replace_at`, `_as_cstr`, `_as_view`, `_destroy`.
- `proven_u8str_append_fmt[_grow]` (`fmt.h`) — type-safe structural formatting
  via `_Generic` `PROVEN_ARG`; an allocation-aware alternative to `snprintf`.
- `proven_scan_*` (`scan.h`) — `proven_scan_i64`/`_u64`/`_str` over a view; safe
  integer/token parsing for the config file.
- `proven_array_*` (`array.h`) — generic dynamic array (considered for
  pieces/lines/undo, but **out of initial scope** — see §5).

## 2. Non-goals / constraints

- **No behaviour change.** Every step keeps `./nob test` green and the live
  editor PTY-identical. Steps that touch core logic add/extend unit tests first.
- **Architecture rules hold:** core `src/` includes no OS headers; `display.c`
  stays free of `<stdio.h>` (proven `fmt`/`u8str` are allowed there and are in
  fact a *better* fit than the current manual char copies).
- **No new per-frame heap allocations.** The render loop builds the status,
  command, and tab lines every frame into stack buffers. We do **not** swap
  those for freshly-allocated `proven_u8str_t` per frame. Where we adopt proven
  formatting there, we reuse a **session-owned scratch `proven_u8str_t`** that
  is reserved once and cleared (truncate to 0) each frame — allocation-free in
  steady state. Otherwise the bounded `snprintf` stays (it is already safe).
- **proven is read-only vendor.** Any proven defect found goes to
  `../proven_c_lib/docs/REPORT.md` per AGENTS §10.1 — we do not patch it here.

## 3. Why proven has no "wrap stack buffer" constructor matters

`proven_u8str_t` always owns allocator-backed memory; there is no public
constructor over caller-owned `char buf[N]`. Therefore:
- read-only call sites → **views** (no ownership needed);
- accumulating/long-lived strings → **owning `proven_u8str_t`**;
- per-frame transient formatting → keep `snprintf` **or** a reused scratch
  `proven_u8str_t`, never a fresh allocation per frame.

This split drives the step ordering (low-risk views first, owning strings next,
core add-buffer last).

## 4. Plan — ordered, independently-committable steps

Each step is one commit: build (`./nob`), `./nob test`, ASan where logic
changed, PTY spot-check for UI-visible steps, then commit. Risk noted.

### S0 — Groundwork & smoke test  *(risk: trivial)*
- Add `tests/test_pstr.c`: exercises the exact APIs later steps depend on
  (`append_grow`, `as_view`, `view_eq`, `starts_with`, `slice`, `scan_i64`).
  Pins the vendored build wiring and documents our usage idioms in one place.
- Wire it into `nob.c`'s test list.

### S1 — `config.c` parser on views + scan  *(risk: low; self-contained, tested)*
- Rewrite the TOML-subset parser to slice each line into `proven_u8str_view_t`
  and compare keys via `proven_u8str_view_eq(key, PROVEN_LIT("tabstop"))` etc.;
  parse integers with `proven_scan_i64`; detect `true`/`false` and quoted
  strings on views. Removes `section[32]`/`key[48]`/`sv[64]` fixed buffers and
  the manual digit loop, eliminating silent key/value truncation.
- `apply()` keeps its signature contract but receives views; `trigger` is copied
  into the config's fixed `char trigger[3]` through one bounded helper.
- Extend `tests/test_config.c` with over-long key/value and edge cases that the
  old fixed buffers would have truncated.

### S2 — `bufset.c` path handling on views  *(risk: low)*
- Keep `path[1024]` storage (it is a stable struct field) but route the copy and
  the `prov_bufset_find` compare through `proven_u8str_view_t` /
  `_eq` / bounded copy, removing raw `strcmp`/`strncpy`. Lock with a unit test
  for over-long paths and exact-match find.

### S3 — `prov_abbreviate_filename` + basename on views  *(risk: low; display.c stays pure)*
- Re-express `prov_abbreviate_filename` and the repeated "find last `/`"
  basename loops (main.c, several copies) as a shared `basename_view()` returning
  a `proven_u8str_view_t`, plus a view-based abbreviator. One helper replaces ~5
  duplicated inline loops. Unit-test the abbreviator.

### S4 — Session input strings → owning `proven_u8str_t`  *(risk: medium; UI)*
- Convert the accumulating buffers to owning strings: `prompt_buf` (text input),
  `message` (transient). Append/backspace become `proven_u8str_append_byte` /
  `proven_u8str_remove`, removing manual index math and the 1024/120 caps.
  `zx_pending`/`zx_last` are short and bounded by the parser; convert only if it
  reads cleanly (otherwise leave with a note). PTY-verify prompts (zo/zp/save-as)
  and messages.

### S5 — Reusable scratch `proven_u8str_t` + `fmt` for status/command lines  *(risk: medium; hot path)*
- Add one session-owned scratch `proven_u8str_t`, `reserve`d once. Build the
  global status line and command line with `proven_u8str_append_fmt` (type-safe;
  no format/argument mismatch class of bug), clearing the scratch each frame.
  Measure: no per-frame allocation after warmup. Keep the window-status and
  tab-bar `snprintf` if conversion adds risk without clear benefit. PTY-verify
  the status/command rendering is byte-identical.

### S6 — Piece-table **add buffer** → `proven_u8str_t`  *(risk: higher; core, heavily tested)*
- Replace `add`/`add_len`/`add_cap` + manual `ensure_cap` growth with a
  `proven_u8str_t` (append via `proven_u8str_append_grow`). The piece-table
  pieces still index into it by offset (`proven_u8str_as_view().ptr`). Keep the
  opaque `prov_buffer_t` interface byte-for-byte; all existing buffer/editor
  tests must pass unchanged, plus ASan. This is the deepest structural change and
  is deliberately last so the low-risk wins land first.

### S7 — Sweep & document  *(risk: low)*
- Re-run the §1.1 survey; record the residual libc-string sites that were
  intentionally kept (and why). Update CHANGELOG, SPEC §20 (mark Milestone S
  steps done), and memory.

## 5. Explicitly deferred

- `pieces` / `lines` / `undo` / `redo` dynamic arrays → `proven_array_t`: viable
  but high churn in the hot edit path for modest gain; revisit only if the
  array bookkeeping shows up in a profile or a bug. Documented, not scheduled.
- `proven_map` for config/registers; `proven_arena` for per-frame scratch;
  `proven_ring` for an input/event queue — candidates for M4+, out of scope here.

## 6. Verification matrix

| Step | unit test | ASan | PTY |
|---|---|---|---|
| S0 | new `test_pstr` | — | — |
| S1 | extend `test_config` | yes | — |
| S2 | new bufset test | yes | — |
| S3 | extend display test | yes | — |
| S4 | — | yes | prompts/messages |
| S5 | — | yes | status/cmd line |
| S6 | existing buffer/editor suite | yes | edit/undo/save |

## 7. Rollback

Each step is an isolated commit; reverting one leaves the rest intact. No step
changes a public header's contract except S6, which preserves the opaque
`prov_buffer_t` interface exactly.

## 8. Outcome (S7 sweep)

All eight steps landed as isolated commits; tests stayed green throughout
(15/15) and every touched path is ASan/UBSan-clean.

libc string/stdio call sites, before → after:

| File | before | after | what changed |
|---|---:|---:|---|
| `config.c` | 11 | 2 | views + scan; only `memcpy`/`strcmp` gone, two `is_*`-adjacent stays |
| `bufset.c` | 3 | 0 | path copy/compare fully on views |
| `buffer.c` | 6 | 5 | add-store on `proven_u8str_t`; remaining `memcpy` are byte-range *reads* |
| `main.c` | 52 | 48 | 5 basename loops collapsed; status line + prompt accumulator off snprintf |

New proven string-system surface: `proven_u8str_view_t` ×18, `prov_cstr_view`
×13, `PROVEN_ARG`/fmt ×11, `proven_u8str_view_eq` ×10, owning `proven_u8str_t`
(add-store, prompt, status scratch), `proven_scan_i64`. Shared bridge:
`src/pstr.h`.

### Residual libc-string sites — RESOLVED (2026-06-18)

All four sites were converted to be libc-free once proven gained a fixed-
capacity string over caller memory (`proven_u8str_borrow` / `proven_u8str_reset`)
and a bounded `proven_mem_copy` in `proven_c_lib-v26.06.18a` (the request in
`../proven_c_lib/docs/REPORT.md` was implemented upstream; see that project's
`docs/internal/proposals/rfc-0002`). Conversion (prov commits, Phase B):

- **Site 1** `main.c` command-line / window-status / tab-bar / `message[]`
  builders → `proven_u8str_append_fmt_trunc` over a borrowed `proven_u8str_t`
  via the `FMT_INTO` helper (no per-frame allocation; PTY byte-identical).
- **Site 2** `buffer.c` original-bytes / `copy_range` / undo-action copies →
  `proven_mem_copy`.
- **Site 3** `command.c` `put()` → `prov_cstr_set`; multi-arg describe/label →
  borrowed `proven_u8str_append_fmt`.
- **Site 4** `save.c` temp path → owning `proven_u8str_t` + `append_grow`.
- Bonus: `config.c`'s remaining byte copies → `prov_cstr_set` / `proven_mem_copy`.

Every prov source is now free of libc string functions except `buffer.c`'s two
`memmove` calls — internal piece/undo **array-element shifts** (overlapping),
which belong to the deferred `proven_array` migration, not a string site.
`main.c` keeps `<stdio.h>` for the architecturally-sanctioned app-entry concerns
(config file load, stderr diagnostics) — not string assembly.

### Deferred (unchanged from §5)

`proven_array` for pieces/lines/undo (would also retire the two `memmove`s),
`proven_map`, `proven_arena`, `proven_ring` — documented, not scheduled.
