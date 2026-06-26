# RFC-0015 — File-open panel redesign (`zo`)

- **Status:** **IMPLEMENTED (2026-06-22), Phases A–D + follow-up refinements.**
  Vertical layout (A), path field + `prov_browser_resolve_path` navigation (B),
  encoding/backend sub-screens + read-only (C), extension column + head…tail
  truncation (D) all shipped, PTY + ASan verified. Decisions resolved: D1 =
  Tab-cycle focus (list→preview→path→options); D2 = sub-screen lists on `e`/`b`
  (the inline cycle was dropped). Builds on RFC-0013 (preview/textbox) + RFC-0010
  (panel) + the Phase-3a encoding picker.
- **Follow-up refinements (2026-06-22, shipped in v0.3):**
  - **Row numbers** — every entry is prefixed with a 1-based number; `..` is always
    row 1 and always sorts first (independent of sort direction).
  - **Sort direction** — `t` cycles *field × direction*: name/date/ext, each
    ascending then descending (`browse.sort_desc`); `..` and the folder-before-file
    grouping are unaffected by the reverse.
  - **Extension column auto-width** — tracks the longest extension shown (default
    ≤3, capped at 10 so `.torrent`-class suffixes fit); a longer one is
    middle-elided with `…` (e.g. `supe…12345`). _Note: the marker is plain, not
    reverse-colored — the list row is blitted with a single attribute._
  - **Key map settled** — `e` encoding, `b` backend, `B` BOM, `R` read-only, `v`
    preview, `m` info columns, `t` sort, `f` types, `p` jump to path field. Tab /
    Shift+Tab cycle focus forward / backward.
  - **Type-filter history** — the `f` box recalls past filters with ↑/↓ (a session
    ring, `browse.pfhist`). _Future: persist to the config file._
  - **Compact legends** — footers and the command-line legend pack tightly as
    `key:label│…` (colon, no surrounding spaces).

## 1. Layout (vertical stack, top → bottom)

```
┌─ /abs/dir ─────────────────────────────── N items ─┐
│ name.ext            ext    12 KiB  2026-06-22       │  ← (1) file list (scrolls)
│ …                                                   │
│────────────────────────────────────────────────────│
│ preview line 1                                      │  ← (2) preview box (textbox)
│ preview line 2                                      │
│────────────────────────────────────────────────────│
│ path: ./downloads/new/_                             │  ← (3) path / name input
│ enc: UTF-8(auto)  backend: auto  BOM: off  RO: off  │  ← (4) options row
│o-Open│c-Cancel│ … legend …                          │
└─────────────────────────────────────────────────────┘
```

Row budget (content height `ch`): options = 1, path = 1, a separator each above
the preview and the options block. The remainder splits **list (top ~60%) /
preview (bottom ~40%)**, min 3 rows each; if too short, the preview collapses
(`p` still toggles it).

## 2. File list (section 1)

- An **extension column** (the suffix after the last `.`; blank for dotfiles /
  none), shown alongside the existing size/perms/mtime/owner/group columns.
- Long names render **head…tail**: `abcdefg….txt` — keep the start and the
  trailing extension visible (extend `prov_abbreviate_filename`, which already
  keeps the extension; ensure a `…` ellipsis with both ends).

## 3. Preview box (section 2)

The RFC-0013 textbox, **stacked below the list** (was side-by-side). Always shown
when there's room; `Tab` cycles scroll focus list → preview → path field.

## 4. Path / name input (section 3)

A persistent editable line, prefilled with the **selected entry's full name** (so
a truncated list row is still fully readable). Typing edits it; moving the
selection refills it (until the user has typed). **Enter** resolves it:

- **Relative + absolute paths**: `./downloads/new/`, `../docs`, `/etc`, a bare
  name. Trailing `/` optional. On Windows `\` is also a separator (normalized to
  `/`); on POSIX `\` is a literal filename byte.
- Resolution: join onto the current dir (or cwd for an absolute path), then
  normalize `.` / `..` / empty segments (`browse_path_resolve`, pure + tested).
- If the resolved path is a **directory** → navigate into it (`browser_goto`).
  If a **file** → open it (with the chosen encoding/BOM/RO). If it doesn't exist
  → a message; stay.
- **Input sanitization**: reject control bytes (< 0x20, DEL) and invalid/partial
  UTF-8 as they are typed (never store a malformed byte); on POSIX also reject NUL
  (already impossible) — path separators are allowed, other bytes pass. This keeps
  a broken-codepage paste from entering a name.

## 5. Options row (section 4)

Inline, selectable: **encoding** (open-as codepage), **backend** (charset PAL
backend), **BOM** on save, **read-only** open. `e`/`b` already cycle encoding/BOM;
add `r` = toggle read-only. Encoding **and** backend are picked on a **sub-screen**:
a key (`e` for encoding, `B`… or a verb) replaces the list/preview area with a
selectable list of encodings (or backends); choosing one returns to the main
layout with it applied. (Reuses the panel list rendering in a `browse_subscreen`
state; Esc returns without changing.)

Read-only: when set, opening the file marks the window read-only (`pane.readonly`
/ `s->readonly`), reusing the existing `wr` machinery.

## 6. Selection → name display

Covered by §4: the path field shows the selected entry's full name, satisfying
"show the full chosen filename" without a separate widget.

## 7. Phasing

- **A — layout**: restructure the browser draw into the vertical stack (list /
  preview / path / options), preview moved below; `Tab` focus cycle list→preview→
  path. Sections render; path/options are display-only first.
- **B — path input + navigation**: editable path field, `browse_path_resolve`
  (relative/`..`/trailing-slash/`\`-on-Windows) + sanitization, Enter navigates a
  dir / opens a file; selection refills the field. Unit tests for the resolver.
- **C — options + sub-screen**: `r` read-only toggle; an encoding/backend
  sub-screen (`browse_subscreen`) that picks from a list and returns.
- **D — list polish**: extension column + head…tail truncation.

Every phase: `./nob test`, ASan/UBSan, `--release`, PTY-verify, commit.

## 8. Open decisions

- **D1 — path-field focus:** include it in the `Tab` cycle (list→preview→path),
  *and* auto-focus it the moment the user types a printable that isn't a verb?
  *Recommend Tab-cycle only* (explicit), to avoid stealing the verb keys; a digit
  still quick-picks, letters stay verbs unless the field has focus.
- **D2 — encoding vs backend sub-screen trigger:** `e` cycles inline (fast,
  current) *or* opens the sub-screen list? *Recommend*: keep `e`/`b` inline cycle
  for speed; add a separate verb (e.g. `c`… no—`c` is Cancel) for the full
  sub-screen list of all supported encodings/backends. Settle the key in Phase C.
