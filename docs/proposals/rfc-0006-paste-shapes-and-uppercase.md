# RFC 0006 — paste shapes (`p`/`P`), register shape tag, uppercase commands

- Status: **Planned** (design agreed; implementation pending — feeds M4 registers)
- Created: 2026-06-18

## 1. Motivation

prov's `p` currently inserts the register bytes at the cursor (characterwise
only). It has no notion of *linewise* or *blockwise* content, so pasting a
yanked line splits the current line and "paste after the last line" has no clean
home. The fix is a small, mathematically-closed model: the register carries a
**shape**, and the paste **key** chooses the **placement** explicitly.

## 2. Model

### 2.1 Register shape (set at yank/cut time, never inferred at paste)
A register is `(bytes, shape)` with `shape ∈ { char, line, block }`:
- `char` — a character span (`yw`, visual-char yank, `x`/`d` register).
- `line` — whole line(s) (`yy`, `dd`); the bytes carry a trailing `\n` (prov
  already does this via `prov_editor_reg_ensure_trailing_newline`).
- `block` — a rectangle (future visual-block selection), normalized to a true
  `W×H` rectangle at yank time (short source rows padded with spaces) so paste
  is uniform.

### 2.2 `p` — paste at / after the cursor
- `char`: insert the bytes at the bar cursor.
- `line`: insert as whole line(s) **below** the current line (column ignored).
  Last line: append at end of document with a separating `\n` — so "paste after
  the last line" is just `p` with the cursor on the last line.
- `block`: rectangular **insert**, the rectangle's top-left at the bar; pad short
  lines with spaces to reach the cursor column and extend the document with new
  lines when the rectangle runs past the last line (see RFC-0005 §… / the block
  algorithm below).
- `[N]p` = N copies.

### 2.3 `P` — paste before the cursor
With a **bar** cursor there is no before/after for a point, so:
- `char`: **identical to `p`** (insert at the bar).
- `block`: **identical to `p`** (point-like; top-left at the bar).
- `line`: insert as whole line(s) **above** the current line. `[N]P` = N above.

**One-line rule:** `P ≡ p`, *except* for a `line` register, where `p` = below and
`P` = above. So uppercase `P` only carries new meaning for linewise content.

### 2.4 Block paste algorithm (for §2.2)
Block `B` = `H` rows of width `W`; cursor at line `L0`, visual column `C`.
```
for i in 0 .. H-1:
    line = L0 + i
    if line >= line_count: append empty lines up to `line`
    if visual_width(line) < C: pad with spaces to column C
    col = byte offset on `line` at visual column C
    insert B[i] at col            (shift the rest of the line right)
cursor -> (L0, C)
```
Tricky bit: visual-column→byte with tabs / wide chars (prov has
`prov_cursor_screen_pos` / `prov_line_visual_width`). v1 may snap `C` to a char
boundary and treat tab/wide-straddle conservatively.

## 3. `0p` is intentionally left empty

prov's grammar makes `0` a count digit when it is **not** the first character
(`30p` parses as count 30 then `p` = paste ×30). So `[N]0p` is unreachable, and a
counted paste variant cannot live on `0p`. We therefore do **not** assign `0p`.
(All existing `0`-series commands are count-free jumps/browsers, so they are
unaffected — see the 0-series review.)

## 4. Overwrite paste — deferred

A "replace instead of insert" paste (char: overwrite `W` bytes within the line,
extending like Ovr mode; block: stamp the rectangle over existing cells) is
desirable but needs a **count-accepting** key (not `0p`). Deferred to M4; pick a
key then (candidate: a register-namespace command, or another uppercase command
once the uppercase convention is in). Recorded in TODO.

## 5. Uppercase command extension

The zx command system was lowercase-only; it now admits **uppercase letters as
distinct commands** (SPEC §10 guideline added). Convention: prefer lowercase;
use an uppercase letter only for a natural "stronger/mirrored variant" of its
lowercase command (vim-like), and document the relationship. Uses:
- `P` — linewise paste above, mirroring `p` (below).
- `E` — macro execution / replay, moved off `0e`. Macros are run repeatedly
  (`[N]E` = run N times), and a leading-`0` command cannot carry a count
  (§3), so macro execution must live on a count-accepting key. The
  count-free macro *overview/setup* panel can stay on `0e` (or fold into `E`'s
  feedback); the count-bearing *execute/replay* is `E`.

## 6. Staged implementation (each: build + test + commit)

- **S1** Register shape tag: add `shape` to the editor register; set `char`/`line`
  at the existing yank/cut sites (`yy`/`dd` → line, others → char). No behavior
  change yet (paste still characterwise). Unit test the tag.
- **S2** Linewise `p`/`P`: `p` = line(s) below, `P` = line(s) above; last-line
  handling; `[N]`. Core helper `prov_editor_paste_line(ed, below, count)` (testable),
  wired to `p`/`P`. Keep `char` paste = insert at cursor for both. Unit + PTY.
- **S3** Uppercase in the parser + SPEC/help: parse `P`; document the convention.
- **S4** (with visual-block, later) `block` shape + the §2.4 algorithm for `p`;
  block normalization at yank.
- **S5** (M4) overwrite paste on a chosen count-friendly key.

## 7. Verification
`./nob test` (new register-shape + paste-line unit tests), ASan on the paste
paths, PTY for the key behavior (last line, `[N]p`/`[N]P`, char vs line).
