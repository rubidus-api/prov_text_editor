# RFC 0001: Vim-Style Text Objects and Operator Editing for Code

Status: Adopted

## Summary

This document records the adopted Vim-style text objects and operator-pending
editing model for `prov_text_editor` so code editing can target syntactic units
directly instead of relying on repeated cursor nudging and manual selection.

The main benefit is speed with less ambiguity: a user can say "change the inner
string", "delete the current argument", or "yank the current word" without
first constructing a selection by hand.

## Current design baseline

Several related command-map decisions are already part of the current
`SPEC.md` and should be treated as settled context for this RFC:

- `g` owns document-level goto plus local movement (`gg`, `ge`, `0g`, `gp`,
  `gn`, `gf`, `gl`, `gu`, `gd`).
- `m` owns bookmarks (`0m`, `0m[a-z]`, `m[a-z]`, `m[0-9]`).
- `e` owns macros (`0e`, `0ea`, `ea`, `[N]ea`).
- `o` only owns open-line helpers (`on`, `op`); current-line linewise actions
  are handled by `dd`, `cc`, and `yy`.
- `p` is paste, `v` toggles block selection, and `h` is unassigned.
- `zb` is the buffer list.
- `zq` and `zqq` are already explicit quit flows and are not reopened here.
- `0` is a command-specific special prefix for selected secondary views,
  browsers, and special jumps, including `0u`, `0s`, `0b`, `0r`, `0z`, `0w`,
  `0t`, `0e`, `0m`, `0n`, and `0g`.
- `n` is currently unassigned at the top level.

The canonical command definitions now live in `SPEC.md`; this RFC is retained
as the rationale record for the adopted design.

## Why this belongs in `prov_text_editor`

Code editing repeatedly needs the same shapes:

- quoted strings
- identifiers and keywords
- arguments and call expressions
- parenthesized, bracketed, and braced blocks
- tags, docstrings, and paragraph-sized blocks
- linewise refactors

Without text objects, those edits require several separate motions and a
selection step. That makes common programming edits slower and more error
prone than they need to be.

## Feature set

### 1. Inner and around text objects

These are the core feature.

Recommended objects:

- `iw` / `aw` for word or identifier text
- `i"` / `a"` for double-quoted strings
- `i'` / `a'` for single-quoted strings
- `i(` / `a(` for parenthesized expressions
- `i{` / `a{` for braced blocks
- `i[` / `a[` for bracketed lists
- `i<` / `a<` for angle-bracketed forms
- `it` / `at` for tags
- `ip` / `ap` for paragraph-sized blocks

Why:

- `ci"` updates string literals without requiring cursor placement inside the
  quotes.
- `ciw` and `caw` are ideal for renaming identifiers and patching keywords.
- `di(` and `da{` are useful for call arguments, conditionals, and structured
  code blocks.
- `cit` matters for markup-heavy editing, documentation, or template code.
- `cip` is useful when a code comment, docstring, or prose block should be
  replaced as a unit.

### 2. Operator-pending motion editing

The same operators should work with motions, not only with text objects.

Operator summary:

| Operator | Core action | Preferred motions | Linewise form | Notes |
|---|---|---|---|---|
| `c` | Replace the target and enter insert mode | `cw`, `cf`, `cl`, `cm`, `ci"`, `caw`, `ct)` | `cc` | The target is removed and typing resumes at the replacement point. |
| `d` | Delete the target without entering insert mode | `dw`, `df`, `dl`, `dm`, `di(`, `da{`, `df,` | `dd` | The target is removed and command mode continues. |
| `y` | Yank the target into registers / clipboard without changing the buffer | `yw`, `yf`, `yl`, `ym`, `yi(`, `ya[` | `yy` | The target is copied and the cursor stays in place. |

The preferred motion labels are alphabetic:

- `f` for line start
- `l` for line end
- `m` for matching-delimiter motion

Literal-character find/till motions stay available as `f<char>` and
`t<char>` when the target needs to be a specific character.

Representative forms:

- `cw`, `cf`, `cl`, `cm`
- `dw`, `df`, `dl`, `dm`
- `yw`, `yf`, `yl`, `ym`
- `ct)`, `df,`, `2yw`

Why:

- This keeps edits deterministic and compact.
- It avoids a separate visual-selection step for many common code changes.
- It makes repeated edits easy to describe in the same grammar as movement.

### 3. Dedicated change, delete, and yank operators

Use a small operator family instead of overloading a single key for many
different actions.

Recommended operators:

- `c` change
- `d` delete
- `y` yank / copy

Why:

- Programming work needs different semantics for "remove", "copy", and
  "replace then continue typing".
- A dedicated operator family makes text-object commands predictable.
- The command set becomes easier to remember because it matches Vim's mental
  model.

The motion side of the grammar should stay mostly alphabetic. In particular,
`f` and `l` are the preferred line-boundary markers, and `m` is the preferred
matching-delimiter marker. That keeps `gf` / `gl` and `cf` / `cl` visually
aligned across the global goto and operator-pending grammars.

The previous design kept `c` as copy, `y` as redo, `v` as paste, and `h` as
block selection.
Adopting this operator family required a coordinated remap.

## `prov` key mapping

This proposal keeps the current `zx` grammar of lowercase letters and numbers.
It does not require punctuation-based commands such as `.` or `*`.

### Command map

| Command | Meaning | SPEC change |
|---|---|---|
| `c` + object / motion | Change the selected text and return to insert mode | Change `c` from copy to change operator |
| `d` + object / motion | Delete the selected text without entering insert mode | New command family |
| `y` + object / motion | Yank or copy the selected text | Change `y` from redo to yank |
| `zy` | Redo | Move redo here from the current single-key `y` |
| `x` | Quick delete or cut of the character / selection under the cursor | Keep as a fast one-key shortcut |
| `p` | Paste | Move paste from `v` to `p` |
| `v` | Toggle block selection | Move block selection from `h` to `v` |

### Special zero-prefixed commands

The current `SPEC.md` already defines a command-specific `0` prefix. The RFC
does not reopen that decision; it uses the same map as baseline.

| Current command | Meaning | Notes |
|---|---|---|
| `0u` | Undo history browser | Current design baseline |
| `0s` | Search history or saved-search browser | Current design baseline |
| `0b` | Register list browser | Current design baseline |
| `0r` | Unassigned for now | Current design baseline |
| `0z` | Command history or command browser | Current design baseline |
| `0w` | Window or pane overview | Current design baseline |
| `0t` | Tab overview | Current design baseline |
| `0e` | Macro setup / overview; show assigned macro slots and current recording state | Current design baseline |
| `0m` | Bookmark setup / overview; show assigned bookmark slots and nearby text | Current design baseline |
| `0n` | Movement history browser | Current design baseline |
| `0g` | File last-line jump | Must remain unchanged |

### Linewise operator forms

The adopted operator model also includes current-line commands:

- `dd` delete the current line
- `cc` change the current line and enter insert mode
- `yy` yank the current line

Numeric prefixes apply to these forms in the normal way, so `3dd` deletes
three lines and `2yy` yanks two lines.

### Text-object grammar

Object selection should use the following modifiers:

- `i` means inner
- `a` means around

Examples:

- `ci"` change the inner double-quoted string
- `ca"` change the double-quoted string including quotes
- `diw` delete the inner word
- `daw` delete the word including surrounding whitespace
- `yi(` yank the inner parenthesized expression
- `caw` change the around-word unit

### Existing commands that should remain

The proposal does not replace the current open-line helpers or the already
adopted navigation and wizard commands. These remain useful and orthogonal:

- `on`, `op`
- `dd`, `cc`, `yy`
- `gg`, `ge`, `0g`, `gp`, `gn`, `gf`, `gl`, `gu`, `gd`
- `ss`, `sw`, `sn`, `sp`, `sr`, `sm`, `sox`, `soc`, `sow`, `soh`, `sor`
- `u` for undo
- `a` for repeat last action
- `q` for execute last macro
- `p` for paste
- `v` for block selection
- `0m` / `m[a-z]` / `m[0-9]` for bookmarks
- `0e` / `0ea` / `ea` for macros
- `zb` for the buffer list
- `0b` for register list browsing
- `zq` / `zqq` for quit flows

## Compatibility notes

- `c`, `d`, and `y` become namespace-style operator starters, so they should
  not remain stand-alone commands.
- `y` currently means redo in `prov`; this proposal moves redo to `zy`.
- `c` currently means copy in `prov`; this proposal moves copy semantics out of
  the single-key slot and into the new `y` operator family.
- `p` now means paste in `prov`, `v` toggles block selection, and `h` is
  intentionally left unassigned.
- Current-line operations are now handled by `dd`, `cc`, and `yy`; the `o`
  namespace is reduced to open-line helpers.
- `m` now holds bookmarks; `n` is unassigned at the top level.
- `zr` is not used in the current `SPEC.md`; register browsing is handled by
  `0b` under the special zero-prefix rule.
- `0r` is intentionally left unused for now.
- `0g` is reserved by the current `SPEC.md` as the file-last-line jump and is
  not part of the proposal's special browser family.
- Local motion commands live under `g` as `gp`, `gn`, `gf`, `gl`, `gu`, and
  `gd`.
- Bookmark setup uses `0m`, bookmark navigation uses `m[a-z]`, and historical
  bookmark navigation uses `m[0-9]`.
- Macro setup uses `0e`, macro recording toggles with `0ea`, and macro
  execution uses `ea`.

## Why not visual mode first

Visual mode is useful, but for code editing the operator + object pattern gives
more immediate value.

It is better suited to:

- string literals
- argument lists
- identifiers
- nested syntax blocks

Visual mode can come later as a separate proposal if the command-space and
selection semantics need it.

## Non-goals

- This is not a full Vim clone.
- This does not require punctuation-based command parsing.
- This does not change the current `zx` trigger model.
- This does not reopen the already adopted `g` / `n` / `e` / `z` / `0*`
  namespace layout.
- This does not define search, regex, or macro behavior beyond the existing
  command map.

## Implementation order

If adopted, the recommended order is:

1. Add the operator-pending parser.
2. Add word and delimiter text objects.
3. Add motion-based operator support.
4. If the operator family is adopted, move redo to `zy` and move copy semantics
   out of the current single-key `c`.
5. Keep the current `g`, `n`, `e`, `z`, and `0*` namespace layout unchanged.
6. Add tag and paragraph text objects.
7. Revisit visual selection if it still looks necessary.
