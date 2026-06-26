# RFC 0007 — `field` mode (bounded fragment input) + scoped undo

- Status: **Accepted, implementing** (replaces the removed M3.5 count-repeat)
- Created: 2026-06-19

## 1. Motivation

Count-repeat insert (`[N]a`), change (`c`), and open-line (`o`) all need to
capture **one contiguous fragment of input** and then act on it (stamp it N
times, replace a target, fill a line). The first attempt embedded this in Ed
mode and drowned in edge cases (what is the repeated span when the cursor
wanders, repeat timing, typing the literal trigger). The fix is a dedicated,
**bounded** input mode so the hard cases *cannot occur*.

## 2. `field` mode

A confined mini-editor over a growing **region** `[origin, region_end)`.

- **Insert-only.** Typing/paste extend the region; there is no overwrite toggle.
- **Region-confined.** The cursor and any selection are clamped to
  `[origin, region_end]`; you cannot touch text outside the region. Because every
  edit is insert-only and confined, the region only ever holds bytes produced
  *this session* (for `a`/`o`) — so the session's net effect is a single
  insertion (see §5).
- **Free editing inside.** Full Ed-style editing *within* the region: move,
  backspace/delete (shrink the region), `Shift`+motion selection, and the
  clipboard. `region_end` follows: insert at/inside ⇒ `+len`, delete inside ⇒
  `-len`.
- **Clipboard:** `Ctrl+X/C/V` use the single program-wide default register
  (shared with Ed mode); no named registers or macros.
- **Undo/redo:** `Ctrl+Z` / `Ctrl+Y` operate on an **isolated, temporary session
  undo stack** (§4), bounded so it cannot rewind past `origin` (the empty/initial
  region).
- **Trigger disabled.** The `zx` trigger does nothing here; `zx` is literal text.
- **Esc is the only exit** → commit (§5).
- **Rendering:** the region is shown **underlined** (never reverse — reverse is
  reserved for the in-region selection, which then renders reverse+underline).
  Mode code: `Fi`. The count, if any, is shown (e.g. `Fi×5`).
- Entry is blocked on a read-only window.

## 3. Entry commands and counts

| Command | Initial region | Count means | Commit (§5) |
| `a` / `[N]a` | empty at the cursor | repeat **×N** | insert `region × N` |
| `on`/`op` `[N]` | empty new line below/above (region carries the leading `\n`) | repeat **×N** | insert `region × N` |
| `c{motion}` / `[N]c…` | the **motion target**, pre-filled and pre-selected | **motion extent** (no repeat) | replace target with `region` (×1) |

`c` pre-fills the region with the target text and selects it, so typing
immediately replaces it (overtype); not typing and pressing Esc keeps it (note:
this differs from vim's `cw<Esc>` which deletes — documented).

## 4. Scoped undo (isolated temporary stack)

prov's undo lives in `prov_buffer` as `action_t` arrays. We add a tiny scope API
and **swap the live stacks** so all existing edit/undo/redo code runs unchanged
on the session stacks:

```c
void prov_buffer_undo_scope_begin(prov_buffer_t *buf);  /* save live stacks, install empty */
void prov_buffer_undo_scope_end(prov_buffer_t *buf);    /* free scope stacks, restore saved */
```

In-field `Ctrl+Z/Y` are just `prov_buffer_undo/redo` on the swapped stack. This
keeps the global stack untouched and un-grown (memory), gives perfect isolation
(no floor bugs), and reuses the whole undo machinery (see RFC review notes).

A single-action **replace** primitive records `c`'s net change as one undo step:

```c
proven_err_t prov_buffer_replace(prov_buffer_t *buf, proven_size_t pos,
                                 proven_size_t del_len,
                                 const proven_u8 *bytes, proven_size_t ins_len);
```

## 5. Commit (Esc) — one global undo step

1. Read `R` = current region bytes `[origin, region_end)`.
2. **Revert** the session: `prov_buffer_undo` until empty → buffer back to the
   pre-field state (`a`/`o`: nothing at origin; `c`: the original target `T`).
3. `prov_buffer_undo_scope_end` → free session stacks, restore the global stacks
   (now consistent with the reverted buffer).
4. Apply the net change as **one** real edit on the global stack:
   - `a`/`o`: `insert(origin, R × N)` — one insert action.
   - `c`: `replace(origin, |T|, R)` — one replace action.
   So a single `Ctrl+Z` after the field session undoes the whole thing, including
   the `×N` copies. An empty `R` with `×N` (or `c` with `R == T`) is a no-op.

## 6. Staged implementation (each stage: build + test + commit)

- **S1** Buffer: `undo_scope_begin/end` + `prov_buffer_replace`, with unit tests
  (scope isolation, replace = one undo, scope-end restores global).
- **S2** `MODE_FIELD` scaffolding: session field state, `Fi` mode code, no entry
  yet. Build clean.
- **S3** `handle_field_key`: insert-only editing, **region clamping** for every
  motion/selection, backspace/delete shrink, clipboard, in-field undo/redo,
  `region_end` tracking. Esc → commit (§5). PTY.
- **S4** Region underline rendering (single- and multi-line; combine with
  reverse for selection).
- **S5** Wire entry: `a`/`[N]a` and `on`/`op`/`[N]` (empty region, ×N).
- **S6** Wire `c{motion}` (pre-filled + pre-selected target, ×1 replace).
- **S7** Sweep: ASan/UBSan on field paths, PTY edge matrix (`80a-`, multi-line
  fill, `cw`, empty commit, in-field undo, clipboard, RO block), docs (SPEC §9 /
  §20, help, CHANGELOG), bench if a hot path appears.

## 7. Verification
`./nob test` (buffer scope/replace units), ASan on the field handler, PTY for the
key behavior and the one-undo-step guarantee.
