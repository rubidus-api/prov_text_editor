# prov v26.06.22-draft

> A terminal-based text editor written in C, aiming for minimal dependencies, deterministic control, and definite responsiveness.
> Named after pointer provenance, a concept recently introduced in the C language.

---

## 0. Design Philosophy

### 0.1 Extreme Minimalism
The editor minimizes hidden abstraction layers. Core state should be visible, explicit, and mechanically understandable.

### 0.2 Fixed Grid System
All visual elements are rendered on terminal cells. Rendering is based on deterministic conversion from buffer state to screen-cell coordinates.

### 0.3 Programmer Sovereignty
The editor provides predictable primitives. It does not attempt to guess complex user intent beyond explicitly defined commands and configuration.

### 0.4 Deterministic Input
Core editing and command operations must not depend on key timing, GUI-specific modifiers, or platform-specific keyboard assumptions.

---

## 1. Core Invariants

1. The editor never modifies the original file in place.
2. All editable buffers use internal UTF-8 byte sequences.
3. Non-UTF-8 files are decoded into internal UTF-8 at load time.
4. Zero-copy `mmap` original storage is used only when the file is already compatible with the internal UTF-8 representation.
5. Rendering must not mutate buffer state.
6. All screen drawing is cell-based.
7. Input backends must produce platform-neutral key events.
8. Every mutating edit operation must be undoable unless explicitly marked non-undoable.
9. File save must either fully succeed or leave the previous file intact.
10. Optional integrations must fail gracefully.
11. Namespace prefix keys in `zx` mode should not also be complete commands.
12. Commands must be deterministic and must not depend on timeout-based disambiguation.

---

## 2. Overview

| Feature | Details |
|---|---|
| Language Target | C23 where available; avoid poorly supported optional C23 features |
| Practical Baseline | C11-compatible compiler with guarded C23 conveniences |
| UI Backend | Raw `termios` + ANSI escape backend on Unix-like systems; Win32 Console (virtual-terminal) backend on Windows. No external terminal library (no ncurses/terminfo). |
| Mouse | Supported, toggleable, but non-essential |
| Build System | `nob.h` method |
| Core Encoding | Internal UTF-8 |
| Buffer Structure | Piece Table |
| Syntax Highlighting | Tree-sitter, statically linked parsers |
| Search Engine | PCRE2 |

---

## 3. Build and Dependency Model

### 3.1 Build Instructions

```sh
# Linux / macOS
cc -o nob nob.c && ./nob

# Windows, MSVC Developer Prompt
cl nob.c && nob.exe

# Options
./nob --release
./nob --clean
```

### 3.2 Dependency Classes

#### Build-Time Dependencies
- C compiler with C23 (or C2x) support
- Platform terminal facilities from the system, no external library:
  - Unix-like systems: `termios` and `ioctl(TIOCGWINSZ)` from the system C
    library (raw mode + ANSI escapes). No ncurses/terminfo.
  - Windows: Win32 Console API (virtual-terminal mode) via the Windows SDK.

#### Statically Linked Source Dependencies
- `nob.h`
- Tree-sitter core
- Selected Tree-sitter language parsers
- PCRE2

#### Runtime Optional Dependencies
- Unix clipboard integration:
  - X11: `xclip` or `xsel`
  - Wayland: `wl-copy` and `wl-paste`
- If missing, clipboard integration degrades gracefully and the editor remains usable.

---

## 4. Licenses

prov core: **MIT License**

### 4.1 Dependency License Compatibility

| Name | License | MIT Compatible |
|---|---|---|
| nob.h | Public Domain | Yes |
| Tree-sitter | MIT | Yes |
| PCRE2 | BSD-2-Clause | Yes |
| Tree-sitter Parsers | MIT / Apache-2.0 / BSD / ISC | Usually yes; must be verified per parser |

### 4.2 Third-Party License Rule

New vendored dependencies or parsers must not be added unless:

1. Their exact upstream source is recorded.
2. Their license file is copied into `THIRD_PARTY_LICENSES/`.
3. Their license is registered in a dependency manifest.
4. Apache-2.0 NOTICE requirements, if any, are preserved.

Suggested structure:

```text
THIRD_PARTY_LICENSES/
├── tree-sitter.LICENSE
├── pcre2.LICENSE
├── tree-sitter-c.LICENSE
├── tree-sitter-cpp.LICENSE
└── ...
```

---

## 5. Text Unit Model

The editor distinguishes storage units, cursor units, logical text units, and display units.

| Concept | Definition |
|---|---|
| Storage Unit | UTF-8 byte |
| Internal Text Unit | Unicode code point, represented in UTF-8 |
| Logical Newline | LF, CRLF, or CR interpreted as one newline on load |
| Display Unit | Terminal cell |
| Visual Column | Cell-based column after tab expansion and width calculation |
| Logical Column | Code-point offset within a logical line |

### 5.1 Initial Cursor Policy

For the initial implementation:

- Left/right movement operates on Unicode code points.
- Line endings are treated as one logical newline.
- CRLF is preserved as the file's default write line-ending format unless changed.
- Full grapheme cluster editing is not guaranteed in the initial version.
- Combining marks and ZWJ emoji may render with best-effort behavior.

### 5.2 Future Unicode Upgrade Path

A later version may upgrade cursor movement from code-point units to extended grapheme clusters. This must not change the internal UTF-8 storage invariant.

---

## 6. Buffer Architecture

### 6.1 Core Model

| Feature | Details |
|---|---|
| Structure | Piece Table |
| Original Buffer | Immutable internal UTF-8 byte sequence |
| Add Buffer | Append-only internal UTF-8 byte sequence |
| Piece Descriptor | Source buffer, start byte offset, byte length |
| Logical Document | Ordered sequence of pieces |

### 6.2 Encoding Policy

1. File bytes are read from disk.
2. Encoding is detected.
3. The file is decoded into internal UTF-8.
4. The decoded UTF-8 byte sequence becomes the immutable original buffer.
5. All edits append UTF-8 bytes to the add buffer.
6. On save, internal UTF-8 is encoded into the configured write encoding.

### 6.3 `mmap` Policy

`mmap` may be used only when doing so does not violate the internal UTF-8 invariant.

- UTF-8 files may use `mmap` as the backing store for the original buffer.
- Non-UTF-8 files must be decoded into an internal buffer before editing.
- Empty files do not use `mmap`; they start with an empty original buffer and an empty add buffer.
- If the file is externally modified, the editor must detect this before saving when possible.

### 6.4 Offset Types

Document byte offsets should use a fixed unsigned 64-bit type internally unless there is a compelling platform reason not to.

Recommended typedef:

```c
typedef uint64_t ProvSize;
typedef uint64_t ProvOffset;
```

All conversions to platform-specific types such as `size_t`, `off_t`, or Windows file sizes must be checked.

### 6.5 Large File Policy

Large files should open predictably, but full support depends on encoding.

- UTF-8 large files: may use `mmap`-backed original storage.
- Non-UTF-8 large files: require decoding and may take longer to open.
- Syntax highlighting and regex search may be disabled or limited above configured thresholds.

Suggested config:

```toml
[performance]
large_file_mb = 50
syntax_max_file_mb = 10
search_chunk_mb = 4
max_line_length = 20000
```

---

## 7. File Save Semantics

### 7.1 Save Invariant

Saving must be atomic from the user's perspective: either the new file is fully written, or the previous file remains available.

### 7.2 POSIX Save Procedure

1. Check current target metadata.
2. Create a temporary file in the same directory.
3. Encode internal UTF-8 into the configured write encoding.
4. Apply configured line-ending format.
5. Write all output bytes.
6. `fsync()` the temporary file.
7. Close the temporary file.
8. Preserve permissions where appropriate.
9. Atomically replace the target using `rename()`.
10. `fsync()` the parent directory when possible.
11. Refresh stored metadata.
12. Mark the buffer clean.

### 7.3 Windows Save Procedure

Windows save should use an atomic replacement API such as `ReplaceFileW` or a carefully defined `MoveFileExW` strategy.

The implementation must explicitly define:

- Whether existing ACLs are preserved.
- Whether the target is followed if it is a symbolic link.
- Whether executable/read-only attributes are preserved.
- How failure during replacement is reported.

### 7.4 External Modification Detection

Before saving an existing file, compare stored metadata with current metadata where possible.

Recommended metadata:

- Device/inode or equivalent file ID
- File size
- Modification timestamp
- Creation/change timestamp when available

If a conflict is detected, the editor should prompt before overwriting.

---

## 8. Undo / Redo

| Feature | Details |
|---|---|
| Mechanism | Action Stack |
| Default Limit | 1000 grouped actions |
| Grouping | Continuous typing sequences are grouped |
| Required Data | Inverse operation, cursor before/after, selection before/after |

Undo is exposed as `u`; redo is exposed as `zy`.

### 8.1 Undo Invariants

1. Every mutating edit creates an undoable action unless explicitly marked otherwise.
2. Cursor and selection state must be restored by undo/redo.
3. Replace-all is one grouped action.
4. Macro execution should be one grouped action by default.
5. File reload, read-encoding change, and destructive reparse operations clear undo history after confirmation.
6. Save does not clear undo history, but updates the clean marker.

### 8.2 Grouping Policy

Continuous typed text belongs to the same undo group until one of the following occurs:

- Cursor movement
- Selection change
- Mode change
- Explicit command execution
- Clipboard operation
- Search/replace operation
- Timeout-free grouping boundary defined by input event type

Grouping must not depend on wall-clock timing.

---

## 9. Modes

### 9.1 Mode Overview

| Mode | Cursor | Description |
|---|---|---|
| `Ed:INS` | Bar | Insert text |
| `Ed:OVR` | Block | Overwrite text |
| `zx` | Block | Command input |

- INS/OVR state is global.
- Entering and leaving `zx` mode preserves INS/OVR state.
- Terminals that cannot change cursor shape use the status bar as fallback.

The global status line shows a two-letter mode code: `Ei` (Ed insert), `Eo` (Ed
overwrite), `Zx` (zx command), `Zv` (zx visual), `Zb` (zx visual-block), `Fi`
(field).

`v` toggles characterwise visual selection; `V` toggles **visual block** — a
rectangular column selection. In block mode, movement sizes the rectangle, `y`
yanks and `d`/`x`/`c` cut the column range into a BLOCK-shaped register, and `p`
pastes such a register column-wise across successive rows (short lines are not
padded). `I` inserts at the block's left column and `A` at its right column on
every row: you type on the top row and `Esc` replicates that text onto the rest.

### 9.2 Mode Transitions

```text
Ed Mode  -- type configured trigger, default "zx" --> zx mode (trigger NOT inserted)
Ed Mode  -- ESC -----------------------------------> zx mode
zx mode  -- command "zx" --------------------------> Ed Mode (plain, no insertion)
zx mode  -- a / [N]a, on / op ---------------------> field mode  (see §9.3)
zx mode  -- c{motion}, cc --------------------------> field mode  (target pre-selected)
zx mode  -- ESC -----------------------------------> cancel pending command, stay in zx mode
field mode -- ESC ---------------------------------> commit, return to zx mode
```

Leaving Ed mode is **ESC** or typing the trigger (`zx`), which switches to `zx`
mode without inserting the trigger. Re-entering Ed mode is the **`zx`** command
(plain return, no insertion).

**ESC in `zx` mode is a cancel, not an exit.** It discards any partially entered
command and any active visual selection and returns `zx` mode to its idle base
state; it does **not** switch to Ed mode.

### 9.3 Field mode (bounded fragment input, RFC-0007)

`a`/`[N]a`, `on`/`op`, and the change operators (`c{motion}`, `cc`) enter **field
mode**: a bounded mini-editor over a growing region `[origin, region_end)`,
underlined on screen. It is **insert-only**; the cursor and selection are clamped
to the region (you cannot touch text outside it), but inside you have full
editing — movement, `Shift`+motion selection, backspace/delete, the single
default clipboard (`Ctrl+X/C/V`), `Ctrl+A` (select region), and an **isolated,
temporary undo stack** (`Ctrl+Z`/`Ctrl+Y`, bounded to the session). The trigger
is disabled (so `zx` is literal text); **ESC is the only exit**. On ESC the whole
session collapses to **one** global undo step and:
- `a`/`on`/`op`: the region is stamped **`N` times** (`[N]a` repeats; `on`/`op`
  carry the line's newline so they tile into `N` lines).
- `c`: the target is **pre-filled and pre-selected** (typing overtypes it) and the
  region **replaces** it once. Pressing ESC without typing keeps the target
  (differs from vim's `cw<Esc>`). The count is the motion extent, not a repeat.

---

## 10. `zx` Command Grammar

### 10.1 Core Principles

1. All essential commands must be available using lowercase letters and numbers.
2. Numbers precede commands as repeat counts or numeric arguments.
3. A leading `0` may be reserved as a command-specific special prefix when the
   command definition explicitly says so.
4. Command parsing is deterministic.
5. Prefix keys and operator starters must not also be complete commands.
6. No timeout-based command resolution.
7. Non-repeatable commands ignore numeric prefixes or report a clear error.
8. Uppercase letters may be used as **documented variant commands** — typically a
   mirrored or stronger form of the lowercase command (vim-like, e.g. `P` vs
   `p`). They are a deliberate extension, not the lowercase-only baseline: an
   essential function must still be reachable without Shift (principle 1), so an
   uppercase command should pair with a lowercase one and its relationship must
   be documented. Uppercase and lowercase of the same letter are distinct,
   deterministic commands.

### 10.2 Recommended Prefix Rule

If a key starts a namespace, the same key should not execute a command alone.

Therefore:

- Use `0w` (the window panel) to pick a window; `wp`/`wn` cycle focus.
- Prefer `gg` / `ge` over bare `g` for document start/end.
- Keep `0g` reserved for the file-last-line jump.
- Keep prefix behavior uniform across namespaces.

### 10.3 Number Repetition

Syntax: `[Number] + Command`

Examples:

```text
5i    move up 5 lines
3od   delete 3 lines
42g   go to line 42, if this shorthand is retained
```

If `[N]g` shorthand is retained, `0g` must be explicitly defined as a special case, not as zero repetitions.

### 10.3.1 Special Zero Prefix

`0` is normally the numeric zero prefix, but selected commands may reserve it as
an explicit special prefix when that behavior is documented. The meaning should
be command-specific and should not be inferred from repetition rules.

Recommended use cases include history browsers, lists, and other secondary
views, but the prefix is not limited to those categories if a different
command-specific behavior is more useful and remains easy to learn.

Most of these open the **common modal panel** (RFC-0010): a single overlay list
over the editor with a shared zx-style keymap (`ik`/arrows + `[N]g` to move, `ss`
to filter, Space/Enter to pick, `h` help, `w` reposition, `c`/Esc to close).
Implemented:

- `0w` opens the window overview (`Ng` focuses, e.g. `2g`; current tab only).
- `0t` opens the tab overview.
- `0b` opens the register list (Enter pastes the slot; `x`<slot> deletes).
- `0e` opens the macro list (`r`<slot> records, `E` stops, Enter replays, `x` deletes).
- `0m` opens the bookmark list (`n`<slot> sets at the cursor, Enter jumps, `x` deletes).
- `0s` opens the search-history panel (Enter searches with that term).
- `0z` opens a command cheat-sheet (Enter opens that key's help page).
- `0g` retains the file-last-line jump behavior.
- `0u` may open an undo history browser (not yet implemented).
- `0n` may open a movement history browser (not yet implemented).
- `0r` is currently unassigned.

The file browser (`zo`) is a vertical open **dialog** (RFC-0015), backed by a
virtual row source so large directories stay cheap: a **file list** (extension
column, long names abbreviated head…tail) over a **preview box** (encoding-aware
text, or a hex view for binary; invalid bytes hidden, never tofu — RFC-0013) over
a **path-name input** over an **options row** (encoding / backend / BOM / RO).
`Tab` cycles focus through every item — list → preview → path → encoding →
backend → BOM → read-only — and Enter/Space activates the focused option. With
the path focused it is a full line editor (←/→/Home/End, Shift-select, Ctrl+C/X/V
via the OS clipboard, control bytes filtered); a typed path on Enter navigates
into a directory or opens a file (relative/absolute, trailing `/` optional, `\`
on Windows). Each option's shortcut letter is emphasized. Verbs: `I`/`K`
parent/enter-dir, `J`/`L` back/forward, `t` sort, `f` type filter, `m` columns,
`v` toggle the preview, `p` path field, `e` encoding sub-screen, `b` backend
sub-screen, `B` save-BOM, `R` read-only open, **`n` read line-endings (Auto / LF /
CRLF / CR)**, **`x` open as binary/hex** (raw bytes; the preview switches to a hex
dump). The selected entry's full name shows in the path line.

The **read line-endings** option (`n`) forces how the file's EOLs are interpreted
on load — `Auto` detects, otherwise `LF`/`CRLF`/`CR` is assumed — and the same EOL
is restored on save. The **preview** is always a continuous, code-point char-wrap
(line breaks render as a space) so its right edge stays a clean rectangle.

If a command reserves `0`, that reservation must be documented alongside the
command itself and must not silently fall back to repeat-count behavior.

**`0`-prefix commands are inherently count-free.** Because `0` is a count digit
when it is not the first character, `[N]0x` is ungrammatical — `30g` parses as
count 30 then `g`, never as "count 3, then `0g`". So a leading-`0` command can
never carry a numeric count, and only actions with no count meaning belong on
the `0` prefix (jumps like `0g`, and history/overview browsers — none of which
repeat). A command that wants `[N]` (e.g. a paste variant) must not live on `0`;
use a normal letter or an uppercase variant instead.

### 10.4 Operator-Pending Editing

`c`, `d`, and `y` are operator starters. They do not complete an edit by
themselves. They must be followed by a motion or a text object.

Operator-pending motion tokens are parsed in a separate context. Prefer
alphabetic motion labels where possible:

- `f` for line start
- `l` for line end
- `m` for matching-delimiter motion

Character-specific find/till motion remains available as `f<char>` or
`t<char>` when the target must be a literal character. `;` and `,` repeat or
reverse-repeat the last character-specific find motion. Standalone (with no
operator), **`f<char>`** is a cursor motion: it moves the cursor to the next
`<char>` on the line, `;` repeats it, and `,` repeats it backward; a count
applies (`3f<char>`). (Standalone `t` is the **tab namespace** — see §10.7 — so
standalone till is only available operator-pending as `dt<char>`/`ct<char>`;
with a count, `[N]t` goes to tab N, mirroring `[N]g`.)

When an operator is active, the next token may be:

- A text object such as `iw`, `aw`, `i"`, `a"`, `i'`, `a'`, `i(`, `a(`,
  `i{`, `a{`, `i[`, `a[`, `i<`, `a<`, `it`, or `ip`
- A motion token such as:
  - `w`, `b`, `e`, or `ge` for word-based motion
  - `f` or `l` for line-boundary motion
  - `m` for matching-delimiter motion
  - `f<char>` or `t<char>` for literal-character find/till motion
  - `;` or `,` to repeat or reverse-repeat the last find/till motion
- A linewise self-targeting form such as `dd`, `cc`, or `yy`, which acts on
  the current line and includes the line break

Operator summary:

| Operator | Core action | Preferred motions | Linewise form | Notes |
|---|---|---|---|---|
| `c` | Replace the target and enter insert mode | `cw`, `cf`, `cl`, `cm`, `ci"`, `caw`, `ct)` | `cc` | The target is removed and typing resumes at the replacement point. |
| `d` | Delete the target without entering insert mode | `dw`, `df`, `dl`, `dm`, `di(`, `da{`, `df,` | `dd` | The target is removed and command mode continues. |
| `y` | Yank the target into registers / clipboard without changing the buffer | `yw`, `yf`, `yl`, `ym`, `yi(`, `ya[` | `yy` | The target is copied and the cursor stays in place. |

Recommended text objects:

| Command | Meaning |
|---|---|
| `iw` / `aw` | Inner / around word or identifier |
| `i"` / `a"` | Inner / around double-quoted string |
| `i'` / `a'` | Inner / around single-quoted string |
| `i(` / `a(` | Inner / around parenthesized block |
| `i{` / `a{` | Inner / around braced block |
| `i[` / `a[` | Inner / around bracketed block |
| `i<` / `a<` | Inner / around angle-bracketed block |
| `it` / `at` | Inner / around tag |
| `ip` / `ap` | Inner / around paragraph-sized block |

Examples:

- `ci"` change the inner double-quoted string
- `da{` delete the braced block including delimiters and surrounding span
- `yi(` yank the inner parenthesized expression
- `caw` change the around-word unit
- `2yw` yank two words using the existing numeric-prefix rule
- `dd` delete the current line
- `cc` change the current line and enter insert mode
- `yy` yank the current line

Representative motion forms:

- `cw`, `cf`, `cl`, `cm`
- `dw`, `df`, `dl`, `dm`
- `yw`, `yf`, `yl`, `ym`
- `ct)`, `df,` when the target is a literal character

Redo is exposed as `zy` rather than bare `y`.

### 10.5 Movement Cluster

The movement cluster uses an inverted-T shape on QWERTY keyboards:

```text
    i
  j k l
```

| Key | Action |
|---|---|
| `i` | Up |
| `k` | Down |
| `j` | Left |
| `l` | Right |

### 10.6 Single-Key Commands

Single-key commands are allowed only when the key is not a namespace prefix.
`c`, `d`, and `y` are operator starters and therefore are not listed here.

| Key | Action |
|---|---|
| `i` | Move up |
| `k` | Move down |
| `j` | Move left |
| `l` | Move right |
| `u` | Undo |
| `p` | Paste (char: at cursor; line: below). `[N]` copies. RFC-0006 |
| `P` | Paste above (char: at cursor; line: above). RFC-0006 |
| `v` | Toggle block selection |
| `x` | Cut character or selection |
| `r` | Replace character under cursor using configured replacement prompt/policy |
| `a` | Append: enter Ed (text-entry) mode at the cursor |
| `[N]n` | Repeat the last command — replays the most recent non-`n` command N times (default 1). A run of `n` keeps replaying that same command (the chain rule: `n` never records itself). |
| `q` | Execute last macro |

### 10.7 Namespaces

#### `g` Namespace: Goto and Movement

Recommended:

| Command | Action |
|---|---|
| `gg` | Document start (no count) |
| `ge` | Document end |
| `0g` | File last line |
| `[N]g` | Go to line N — a count + a single `g` jumps immediately (the goto shorthand). With a count, `g` is goto; with no count, `g` opens this namespace. |
| `gp` | Previous word |
| `gn` | Next word |
| `gf` | Start of line |
| `gl` | End of line |
| `gu` | Half-page up |
| `gd` | Half-page down |

#### `o` Namespace: Open Line

| Command | Action |
|---|---|
| `on` | Create new line below and return to Ed Mode |
| `op` | Create new line above and return to Ed Mode |

Linewise current-line operations are handled by the operator family above
(`dd`, `cc`, `yy`) rather than by the `o` namespace.

#### `b` Namespace: Registers

| Command | Action |
|---|---|
| `b[a-z]x` | Cut to named register *(implemented)* |
| `b[a-z]c` | Copy to named register *(implemented)* |
| `b[a-z]v` | Paste from named register *(implemented; `[N]` copies)* |
| `b[0-9]{x,c,v}` | History registers — usable as plain slots now; auto-population deferred |

#### `m` Namespace: Bookmarks

| Command | Action |
|---|---|
| `0m[a-z]` | Pin bookmark to the named slot *(implemented; buffer-local, edit-tracked)* |
| `m[a-z]` | Navigate to the named bookmark *(implemented)* |
| `0m` | Bookmark setup / overview; show assigned slots and nearby text *(deferred)* |
| `m[0-9]` | Navigate through historical bookmarks *(deferred)* |

#### `e` Namespace: Macros

| Command | Action |
|---|---|
| `0e<slot>` | Start or stop recording macro `<slot>` *(implemented; a-z, 0-9)* |
| `e<slot>` / `[N]e<slot>` | Execute macro N times *(implemented)* |
| `E` / `[N]E` | Replay the last run/recorded macro *(implemented)* |
| `0e` | Macro overview; show assigned slots and recording state *(deferred)* |

#### `s` Namespace: Search and Replace

| Command | Action |
|---|---|
| `ss` | Prompt for search term |
| `sw` | Use word under cursor as search term |
| `sn` | Next result |
| `sp` | Previous result |
| `sr` | Replace prompt |
| `sm` | Jump to matching bracket |
| `sox` | Toggle regex |
| `soc` | Toggle case sensitivity |
| `sow` | Toggle whole word |
| `soh` | Toggle highlight |
| `sor` | Toggle wrap around |

A **buffer** is an open file (the shared content). A **window** is one view of
a buffer — a pane in a layout, with its own viewport and read-only flag; several
windows (even in different tabs) may share one buffer, and an edit through any of
them shows in all. A **tab** is a whole window layout (a "desk"): each tab has
its own split tree, and switching tabs swaps the entire layout. Only the active
tab's windows are reachable; different tabs may edit the same buffer at once.

#### `w` Namespace: Window and Pane Management

| Command | Action |
|---|---|
| `wh` | Horizontal split |
| `wv` | Vertical split |
| `wq` | Close window (frees its buffer if it was the last; see §10.7b) |
| `0w` | Window panel for the current tab (`Ng` focuses, e.g. `2g`) |
| `wp` / `wn` | Focus the previous / next window |
| `wi` `wk` `wj` `wl` | Focus the window up / down / left / right |
| `ws` | Resize mode: `i`/`k` (or ↑/↓) height, `j`/`l` (or ←/→) width |
| `wr` | Toggle this window's read-only flag (shown as `RO`) |
| `wx` | Toggle the focused buffer text ↔ binary/hex (raw bytes; §11.5) |

#### `t` Namespace: Tab Management

A tab is a whole window layout. Switching tabs swaps the layout; buffers are
shared across tabs.

| Command | Action |
|---|---|
| `tn` | New tab (single window on the current buffer) |
| `tq` | Close tab (buffers stay open; closing the last tab quits) |
| `tj` | Previous tab |
| `tl` | Next tab |
| `[N]t` | Go to N-th tab |

Buffers are managed via `zb`: a digit switches the focused window's buffer, `n`
opens a new empty buffer, and `q` closes the active buffer (`zbq`).

#### Buffer lifecycle

A buffer stays open while any window (in any tab) shows it, or while it sits in
the buffer list (loaded but not displayed, or switched away from). A buffer is
closed when its **last window** is closed — by `wq`, or by `tq` closing a
tab whose windows held it — or explicitly via `zbq`, which repoints every window
showing the active buffer to a neighbor (creating a fresh empty buffer if it was
the only one) and then drops it. At that moment, if the buffer has unsaved changes
a save prompt appears on the command line before it is dropped:

- named buffer: `y` save, `n` discard;
- unnamed buffer: `y` opens a save-as input, `n` discard.

Closing a tab resolves each freed buffer in turn. A buffer still shown in
another tab/window is never closed by a window or tab close.

#### `z` Namespace: Meta Editor and Wizards

| Command | Action |
|---|---|
| `zx` | Return to Ed mode |
| `zy` | Redo |
| `zo` | Open file |
| `zw` | Write file |
| `za` | Write As |
| `zq` | Quit sequence wizard; show write, write-as, discard, quit-all, and cancel choices for the current buffer set |
| `zqq` | Secure force-quit path; bypass normal save flow, require explicit discard confirmation for dirty buffers, and exit only when the user confirms the discard action |
| `zh` | Help |
| `zb` | Buffer list |
| `zp` | Command prompt |
| `zi` | Indent |
| `zd` | Dedent |
| `zs` | Syntax settings |
| `zn` | Read encoding / line-ending options |
| `zf` | Write encoding / line-ending options |
| `zc` | Config editor |

#### `0` Special Prefix Commands

| Command | Action |
|---|---|
| `0w` | Window overview panel, current tab (`Ng` focuses, e.g. `2g`) |
| `0t` | Tab overview panel |
| `0b` | Register list panel (Enter pastes; `x`<slot> deletes) |
| `0e` | Macro panel (`r`<slot> records, `E` stops, Enter replays, `x` deletes) |
| `0m` | Bookmark panel (`n`<slot> sets at cursor, Enter jumps, `x` deletes) |
| `0s` | Search-history panel (Enter searches with the term) |
| `0z` | Command cheat-sheet panel (Enter opens that key's help) |
| `0g` | Jump to the file's last line |
| `0u` | Undo history browser (not yet implemented) |
| `0n` | Movement history browser (not yet implemented) |
| `0r` | Unassigned for now |

### 10.7.1 Help Overlay

`h` (or `zh`) opens a full-screen help overlay drawn as a framed page: a
reverse-video breadcrumb bar (with a scroll percentage), a rule, and the page
body (UTF-8, with dim secondary lines). It is a read-only viewer layered over
the current mode; the underlying mode (zx / Ed) is untouched and restored on
exit. Planned-but-inactive keys — the `b`/`m`/`e`/`s` namespaces and the
reserved `r`/`q` — are marked with a `·` in the keyboard overview and a
"planned — not active yet" note on their detail pages, so the help always
reflects what the current build actually does. Navigation is deterministic:

| Key | Action |
|---|---|
| a letter | Open that key's detail page |
| `1`–`9` | Open the count-prefix explainer page |
| `0` | Open the special-prefix page |
| `↑` `↓` `PgUp` `PgDn` | Scroll the current page |
| `Enter` | Step **up** one level: a detail page → the keyboard overview; the overview → leave help (return to the prior mode) |
| `Esc` | Close the overlay from any page |

### 10.8 Alphabetical Command Summary

This table is a condensed index of the top-level alphabetic command space.
The detailed namespace sections above remain authoritative.
Columns 3 and 4 are a Vim cross-reference: column 3 names the native Vim
behavior for the key, and column 4 gives the closest Vim command for the
`prov` action.

| Key | prov summary | Vim key function | Vim equivalent |
|---|---|---|---|
| `a` | Append: enter Ed insert at the cursor | Append after cursor | `a`, `i` |
| `b` | Register namespace | Move backward one word | `"{reg}` register prefix |
| `c` | Operator starter for change | Change operator | `c{motion}`, `ciw` |
| `d` | Operator starter for delete | Delete operator | `d{motion}`, `diw` |
| `e` | Macro namespace | Move to end of word | `q{reg}`, `@{reg}` |
| `f` | No standalone top-level command | Find character forward | N/A |
| `g` | Goto and movement namespace | Prefix for extended motions and commands | `gg`, `G`, `g{motion}` |
| `h` | Open the help overlay | Move left | `:help` |
| `i` | Move up | Insert before cursor | `k` |
| `j` | Move left | Move down | `h` |
| `k` | Move down | Move up | `j` |
| `l` | Move right | Move right | `l` |
| `m` | Bookmark namespace | Set a mark / jump to a mark | `m{mark}`, `` `{mark}` `` |
| `[N]n` | Repeat the last command (N times) | Repeat search forward | `.` |
| `o` | Open line namespace | Open new line below | `o`, `O` |
| `p` | Paste | Put after cursor | `p`, `P` |
| `q` | Execute last macro | Start/stop macro recording | `@@` |
| `r` | Replace character under cursor | Replace one character | `r<char>` |
| `s` | Search and replace namespace | Substitute one character | `:s`, `/`, `?` |
| `t` | Tab namespace | Find till character | `gt`, `gT`, `:tabnew` |
| `u` | Undo | Undo | `u` |
| `v` | Toggle block selection | Enter characterwise visual mode | `Ctrl-v` |
| `w` | Window and pane namespace | Move to next word start | `Ctrl-w` window commands |
| `x` | Cut character or selection | Delete character under cursor | `x` |
| `y` | Operator starter for yank; redo is `zy` | Yank operator | `y{motion}` |
| `z` | Meta editor and wizard namespace | Scroll / fold / position prefix | `:e`, `:w`, `:q`, `:help`, `:set` |

---

## 11. Ed Mode Shortcuts

Ed mode supports conventional shortcuts where the platform provides them reliably. These are convenience features, not the primary command model.

### Cursor Movement
- Arrow keys: move one unit
- Ctrl+Left/Right: word movement
- Home/End: start/end of line
- PgUp/PgDn: page movement
- Ctrl+Home/End: document start/end

### Selection
- Shift+Arrow: extend selection
- Shift+Ctrl+Left/Right: extend by word
- Shift+Home/End: extend to start/end of line
- Ctrl+A: select all

### Editing
- Backspace/Delete
- Ctrl+Backspace/Delete
- Insert: toggle INS/OVR
- Enter: newline
- Tab/Shift+Tab: indent/dedent

### Clipboard, Undo, and Files
- Ctrl+C/X/V
- Ctrl+Z / Ctrl+Y / Shift+Ctrl+Z
- Ctrl+S / Shift+Ctrl+S
- Ctrl+F (find) / Ctrl+R (replace) — open the same prompts as zx `ss`/`sr`
  (implemented). F3 / Shift+F3 (find next/prev) still planned.

### 11.5 Binary / Hex View / Edit Mode (RFC-0018, RFC-0019)

Hex mode edits the **raw file bytes**. Because prov's text buffer holds the
*internal* form (LF-only, BOM-free UTF-8, with the original encoding/EOL/BOM
converted away — §6.2), a hex view of it would show converted bytes, not the
file's. So hex is shown for a **binary buffer**: one loaded and saved **verbatim**,
with no encoding / EOL / BOM conversion (`prov_fileinfo_t.binary`). A window
renders hex iff its buffer is binary.

Two ways in, both reloading the file in the target mode: the **file-open dialog's
`x` toggle** ("binary (raw)"; the preview becomes a hex dump and the selected file
opens as hex), or **`wx`**, which reloads the focused buffer raw (text → binary).
Because the switch reloads the file, **unsaved changes are not silently lost**:
if the buffer is modified, prov first asks **`s` save · `d` discard · `c` cancel**;
`s` saves in the current mode and then reloads. Leaving the hex editor (`Esc` or
`^Q`) reloads as text through the same prompt; **`^S` saves and leaves** in one
step. An unnamed buffer just flips the flag (its bytes are reinterpreted). The layout is a classic dump, 16 bytes
per row: `OFFSET  HH … HH |ascii|`. The byte window can be **nudged** with `[` /
`]` to shift the row alignment for structure inspection. `^S` saves the raw bytes.

Under every hex row a **decoded-string line** shows that row's bytes decoded with
the buffer's *interpretation charset* (chosen with the open dialog's `e`; default
UTF-8) — so an embedded string shows as real glyphs rather than the ascii pane's
`.`. A multi-byte character that straddles the 16-byte row boundary is shown whole
on its starting row (exact for UTF-8; best-effort for other charsets, where the
`[` / `]` nudge can realign a structure).

#### Hex editor keys

Movement — the arrow / Page / Home / End keys work in both panes; the letter keys
work in the hex pane (in the ascii pane they overtype bytes):

- `i` `k` `j` `l` = up / down / left / right (1 byte/row); `←↑↓→` likewise.
- `I` / `K` = PgUp / PgDn; `J` / `L` = Home / End (row start / end).
- `g` / `G` = document start / end; `^G` = jump to a typed hex offset.
- `[` / `]` = nudge the byte window alignment.

Editing (overwrite plus insert/delete; each one undo step, saved through the
verbatim path):

- **Hex pane**: `0-9 a-f` set the byte (high then low nibble); `Tab` switches to
  the ascii pane (where every printable key overtypes the byte).
- `o` / `Insert` insert a `0x00` byte; `x` / `Del` delete the selection or the
  byte; `Backspace` deletes the previous byte.
- `v` (or Shift+arrows) starts/extends a **byte-range selection** (highlighted in
  both panes); `y` copies the selection, `p` pastes the copied bytes.
- **`r` opens the string-replace editor** (`PANEL_K_HEXEDIT`): the selected bytes
  are decoded with the interpretation charset into a full **multi-line ed-mode
  editor** — typing, `Enter` for newlines, arrows + Shift (select) + Shift+Ctrl
  (word), Home/End, `^A`/`^C`/`^X`/`^V`, `^Z`/`^Y`. **`^S` re-encodes the text and
  writes it back** over the range (any new length, one undo); `Esc` cancels.
- **`^S` saves the file (verbatim) and leaves** the hex editor; `^Q` leaves;
  `^Z`/`^Y` undo/redo; `h` opens help.
- `Esc` clears a pending nibble, then the selection, then **leaves the hex editor**
  (reload as text — asking to save first if modified). Read-only windows render
  hex but block edits. Status mode code: `Hx` (hex pane) / `Ha` (ascii pane).

---

## 12. Screen Layout and Rendering

### 12.1 Layout

```text
  L42/123C10/80Ei main.c  UTF-8  LF                  x3
  int main() {         │ #ifndef HEADER_H             │
      int x = 0;       │ #define HEADER_H             │
      return 0;        │ typedef struct {             │
  }                    │     int x;                   │
───────────────[main.c*] } Foo;                       │
  CC = gcc             │                              │
  all: main            │                              │
──────────────[Makefile]────────────────────[header.h]│
  >_                                                  │
──────────────────────────────────────────────────────┘
```

### 12.2 Coordinate Model

| Concept | Description |
|---|---|
| Screen Cell | Terminal grid coordinate |
| Pane Rectangle | Cell rectangle assigned to a pane |
| Text Viewport | Pane area excluding borders/labels |
| Buffer Cursor | Logical position in buffer |
| Visual Cursor | Cell position after rendering rules |
| Scroll Offset | First logical line and horizontal cell offset shown |

### 12.3 Rendering Rules

- A window's own chrome forms its edges, so split siblings sit directly adjacent
  with no separate border cell. It is drawn box-style, sharing the scrollbar code
  with the modal panel (§13): the **right column** is a vertical scrollbar (a `█`
  thumb on a `│` track sized and positioned from the window's scroll state, with
  `▲`/`▼` buttons on the bottom two rows when tall enough; just a plain `│` border
  when the buffer fits), and the **bottom row** is a `─` box bottom border that
  carries the window's status label (cursor `Ln/Co`, a modified `*`, an `RO`
  marker, the file name — see §12.5), a prepared horizontal scrollbar in the space
  that remains, and a `┘` corner in the last cell that meets the vertical scrollbar
  above it. The upper window's bottom border thus separates it from the one below,
  and the left window's scrollbar from the one to its right. Every window — single
  or split — has both. (Implemented.)
- The chrome is normal video; unfocused windows (status + content + scrollbar) use
  dim styling. (Implemented.)
- When `line_numbers` is not `off`, each window draws a left gutter of line
  numbers (§17.4): `absolute` shows every line's number; `relative` shows the
  window's cursor line as an absolute number and all other lines as their
  distance from it. The gutter is per-window (each uses its own cursor) and
  reduces the content width like the scrollbar does. The number sits on a logical
  line's first screen row; wrapped continuation rows and past-EOF rows keep a
  blank gutter. (Implemented.)
- Long-line handling follows `[editor] wrap` (§17.4). With `char` (default) a long
  line soft-wraps onto the next row(s); a 2-cell wide char that would straddle the
  right edge leaves that last cell blank and wraps (no half glyph). With `off` the
  line is **not** wrapped — each logical line is one screen row and the viewport
  scrolls horizontally to keep the cursor's column visible, with the bottom-border
  horizontal scrollbar showing a `█` thumb. With `word`, long lines wrap at word
  boundaries (whole words move down; over-long words still char-break, never
  mid-character) and each continued row gets a reverse-video `<` wrap marker in its
  last column.
- The screen redraws immediately on a terminal resize (no keypress needed).
- Rendering never mutates buffer state.
- Tabs expand according to `tabstop`.
- Fullwidth characters occupy two cells according to the embedded width table.
- Characters with unknown width use a conservative fallback.
- Screen rows past the last buffer line show a `~` end-of-file marker in the
  first column (vi convention); the first such row marks where the file ends.
- The viewport may scroll until only `~` rows remain below the last line, so the
  end of the file can be reached. Page keys scroll the viewport by a page and
  carry the cursor with them; cursor moves still keep the cursor on screen.

### 12.5 Status Lines

There are three kinds of status line, each with a distinct scope:

**Window status line** — the bottom row of *every* window (single or split),
drawn as the box bottom border (a `─` rule, normal video) that carries that
window's own information as a label, single-space separated, left to right.
Example: `X─ L17/36 C3/7 notes.txt* ─────┘`.

- A reverse-video **`X`** in the first column — a mouse close-button (RFC-0014)
  that closes the window, saving first if the buffer is modified.
- A leading `─ ` lead-in after it (the label sits on the rule, panel-title style).
- **Position** `L<line>/<lines> C<col>/<linelen>`, 1-based, for that window's
  cursor. The column and line length are **visual** (a 2-cell wide char counts
  as 2; tabs expand), matching what is on screen.
- **File name**, abbreviated to fit (head + trailing extension joined by `...`),
  with a trailing **`*`** when the buffer is modified (a real name can't contain
  `*`); an unnamed buffer shows `NoName`. An **`RO`** marker follows when the
  window is read-only (`wr`).

The chrome is normal video; unfocused windows are dim. The space after the label
holds a prepared horizontal scrollbar (at least a few cells; the file name is
clipped first to keep it). The last cell is a `┘` corner meeting the vertical
scrollbar above it.

**Global status line** — the second row from the bottom, normal video, for the
focused file as a whole, single-space separated, left to right: a reverse-video
**`X`** (a future mouse close-button, like the per-window one), the
two-letter **mode** (`Ei`/`Eo`/`zx`/`Zx`/`Zv` — see §9), the **code page** and
**encoding** name (e.g. `65001 UTF-8`), the **BOM** status (`BOM`/`noBOM`), the
**line-ending** style (`LF`/`CR/LF`/`CR`/`MIXED`), and the cursor's **byte** and
**char** position as `<cur>/<total>b <cur>/<total>c` (a `k`/`m`/`g`/`t` magnitude
suffix replaces large counts). Example: `X Zx 65001 UTF-8 noBOM LF 6/9b 2/5c`.

**Command line** — always the very bottom row: the global command / search
input and help. In **zx** mode it shows live, deterministic command feedback
(keys typed plus a hint of the sub-commands the prefix can become, or the last
command and its one/two-word function); in **Ed** mode a minimal key legend. It
also hosts `:`-style prompts (`zo`/`zp`), the quit wizard, and messages.

### 12.4 Minimum Terminal Policy

Suggested minimum:

- Width: 40 cells
- Height: 8 cells
- Pane width: at least 8 cells
- Pane height: at least 3 cells including border

If the terminal is too small, prov enters compact single-pane mode or shows a clear error.

---

## 13. Command Window and Wizards

The command window normally occupies one line and expands when necessary.

It is used for:

- Command input
- Search prompts
- Error messages
- Lists
- Wizards

### 13.1 Quit Sequence Wizard

```text
┌─────────────────────────────────────────────────────┐
│ Quit (2/3): header.h                                │
│ w Write  r Write As  d Discard  q Quit All  c Cancel│
└─────────────────────────────────────────────────────┘
```

The implemented quit flow (command-line wizard, deterministic):

- `zq` (and `^Q`) quit immediately when **no** buffer is modified.
- When any buffer is modified they open the quit wizard on the command line:
  - `w` — write every modified buffer that has a name, then quit (stays open if
    some unnamed buffers remain dirty).
  - `d` (or `q`, or a repeated `^Q` — so `zqq` works) — discard all and quit.
  - `c` (or any other key) — cancel.

Open / command prompts share the command line too: `zo` prompts for a file to
open (into a new buffer, or switches if already open); `zp` runs a minimal
command (`w` write, `q` quit, `e <path>` / `o <path>` open).

### 13.2 Read Options Wizard

Changing read options reparses or reloads the file and clears undo history after confirmation.

```text
┌─────────────────────────────────────────────────────┐
│ r:LineEnd[auto] b:BOM[auto] s/Enter:Save c/ESC:Cancel│
├─────────────────────────────────────────────────────┤
│  00000  auto (Auto-detect)                        ▲ │
│  65001  UTF-8                                     █ │
│▶ 51949  EUC-KR                                    █ │
│  00949  CP949 (MS Korean)                         │ │
│  01200  UTF-16 LE                                 │ │
│  ...                                              ▼ │
├─────────────────────────────────────────────────────┤
│ i:Up k:Down j:PgUp l:PgDn f:First t:Last            │
│ p:MoveUp n:MoveDown  5-digits:JumpToCodePage        │
└─────────────────────────────────────────────────────┘
```

### 13.3 Write Options Wizard

Changing write options affects future saves. The internal buffer remains UTF-8.

---

## 14. Search and Replace

### 14.1 Search Model

Search operates on the internal UTF-8 byte sequence.

Regex mode should use:

- `PCRE2_UTF`
- `PCRE2_UCP` when Unicode property behavior is desired

### 14.2 Search Options

| Option | Meaning |
|---|---|
| Regex | Interpret search term as PCRE2 pattern |
| Case Sensitivity | Exact or case-insensitive matching |
| Whole Word | Match word boundaries |
| Highlight | Highlight all visible matches |
| Wrap Around | Continue from beginning/end |

### 14.3 Replace Policy

- Replace current, confirm, and replace-all modes are supported.
- Replace-all is one undo group.
- **Scoped replace**: if a selection is active when the find panel opens, it is
  captured as the replace-all scope and applied by default; an `s:selection`
  toggle reverts to whole-buffer. Only matches that fall entirely inside the scope
  are rewritten; the pattern engine still sees the whole document so `^`/`$`/`\b`
  keep their real context.
- Zero-length matches advance by one code point to avoid infinite loops.
- Replacement capture syntax follows PCRE2 substitution rules.

---

## 15. Syntax Highlighting

### 15.1 Parser Model

Tree-sitter core and selected language parsers are statically linked.

Initial language parsers:

C, C++, Python, Bash, Markdown, JSON, YAML, Rust, Go, JavaScript, TypeScript, Lua, Makefile, Java, Kotlin, C#, SQL, Dockerfile, TOML, XML, INI, PowerShell.

### 15.2 Language Detection

Language may be detected by:

1. File extension
2. Exact filename
3. Shebang line
4. Manual override

### 15.3 Fallback Policy

If parser loading, parsing, or query loading fails:

- Editing continues normally.
- Syntax highlighting is disabled for that buffer.
- A non-fatal status message is shown.

### 15.4 Performance Policy

Suggested config:

```toml
[syntax]
enable = true
max_file_size_mb = 10
max_line_length = 20000
```

Files exceeding the threshold may disable syntax highlighting automatically.

---

## 16. Clipboard and Registers

### 16.1 Registers

- Unnamed register stores the most recent copy/cut.
- Named registers: `a-z`.
- History registers: `0-9`.

### 16.2 Clipboard Sync

The unnamed register may synchronize with the OS clipboard.

- X11: `xclip` or `xsel`
- Wayland: `wl-copy` / `wl-paste`
- Windows: Win32 clipboard API
- macOS: `pbcopy` / `pbpaste` or native API

If unavailable, the editor shows `[no clipboard]` and continues with internal registers.

---

## 17. Configuration

### 17.1 Config File

The config file is named `provconf.ini`. It is looked up in this order:

```text
1. <executable dir>/provconf.ini   # portable mode: if present, it wins
2. ~/.prov/provconf.ini            # the per-user default otherwise
```

A `provconf.ini` placed next to the binary takes priority and the home copy is
ignored — this lets a portable install carry its settings alongside the
executable. `zc` edits whichever file is active (creating `~/.prov/provconf.ini`
from a commented template the first time, when no portable copy exists).

**Session state** (auto-written history) lives in a *separate* file
`prov_state.ini`, in the same directory as the resolved config file. It is
machine-managed (never hand-edited) and holds the find/replace and browser
type-filter history rings under `[find]` / `[replace]` / `[browse_types]`
sections (one `e=<entry>` line each, oldest first). It is written at clean exit
and restored at startup; a missing or malformed file simply yields empty history.
Keeping it apart from `provconf.ini` keeps the hand-edited config clean.

### 17.2 Supported format (TOML / INI common subset)

The parser accepts the common subset of TOML and INI:

- Table headers: `[section]`
- Key-value pairs: `key = value`
- Value types:
  - string
  - integer
  - boolean
- Comments beginning with `#`

Unsupported TOML constructs must be rejected with clear diagnostics.

### 17.3 Example

```toml
[editor]
tabstop = 4
shiftwidth = 4
softtabstop = 4
expandtab = true
line_numbers = "relative"   # off | absolute | relative
scrolloff = 5
wrap = "char"               # char | word | off  (off = horizontal scroll)
mouse = true
trigger = "zx"
undo_limit = 1000

[theme]
colorscheme = "dark"
function = "yellow"
keyword = "red"

[search]
ignorecase = true
highlight = true
wrapscan = true

[clipboard]
sync = true

[syntax]
enable = true
max_file_size_mb = 10
max_line_length = 20000
```

### 17.4 Line numbers

`line_numbers` selects the left gutter style; it applies to **every window**, and
each window numbers relative to **its own** cursor line:

- `off` — no gutter.
- `absolute` — every line shows its 1-based line number.
- `relative` — a hybrid: the window's current (cursor) line shows its absolute
  number, while every other line shows its distance (number of lines above or
  below) from the cursor line. Moving the cursor renumbers the gutter.

`wrap` controls long-line handling:
- `char` (default) — soft-wrap at the column edge onto the next screen row(s).
- `word` — soft-wrap at word boundaries: a word that would overflow the row moves
  whole to the next row (a word longer than the row still char-breaks; breaks fall
  on code-point boundaries, never mid-character). Each continued row shows a
  reverse-video `<` in its reserved last column to mark the wrap.
- `off` — do not wrap; each logical line is one screen row and the viewport scrolls
  horizontally to keep the cursor column visible (the bottom-border horizontal
  scrollbar shows the position). `true`/`false` are also accepted (`true` = `char`,
  `false` = `off`).

### 17.5 Applied vs reserved keys

The parser tolerates every key (unknown keys never error), but only some are
wired to behavior so far. **Applied:** `[editor]` tabstop, expandtab, scrolloff,
trigger, undo_limit, line_numbers, wrap, mouse; `[search]` ignorecase, highlight,
wrapscan; `[clipboard]` sync. **Reserved** (parsed and ignored until their feature
lands): shiftwidth, softtabstop, `[theme]` *, `[syntax]` *. Defaults
reproduce the built-in behavior, so a missing config file changes nothing.

### 17.6 `zc` — config editor

`zc` opens the config file the editor actually reads — an exe-side
`provconf.ini` when present (portable installs), otherwise
`~/.prov/provconf.ini`. The first time, it creates the home directory and writes a
commented starter file (the built-in defaults, with the reserved keys shown
commented out). The buffer carries a ` [config]` tag in its window status line.

**Live apply (RFC-0017):** saving the config file (`zw` / `:w` / `^S` / save-as)
re-parses it and applies the new settings immediately — no restart. A parse error
leaves the running config untouched and reports the offending line. Almost every
setting is read live from the running config; `mouse`, `charset_backend`,
`charset_iconv_path`, and `undo_limit` run their side-effects on change.

---

## 18. File Directory Structure

```text
prov/
├── nob.c
├── nob.h                       # public-domain build header (reused from proven)
├── include/
│   └── proven/                 # vendored proven public headers
├── src/
│   ├── main.c
│   ├── editor.c / editor.h
│   ├── buffer.c / buffer.h     # array-backed piece table + line-offset index + undo inverses
│   ├── encoding.c / encoding.h
│   ├── unicode.c / unicode.h   # code-point cursor + East Asian width
│   ├── unicode_width_table.h   # generated from UAX #11 (see scripts/)
│   ├── display.c / display.h
│   ├── input.c / input.h
│   ├── keymap.c / keymap.h
│   ├── window.c / window.h
│   ├── tab.c / tab.h
│   ├── syntax.c / syntax.h
│   ├── search.c / search.h
│   ├── register.c / register.h
│   ├── macro.c / macro.h
│   ├── bookmark.c / bookmark.h
│   ├── config.c / config.h
│   └── proven/                 # vendored proven library sources
├── platform/
│   ├── platform.h              # backend-neutral terminal/clipboard interface
│   ├── platform_term_posix.c   # raw termios + ANSI + ioctl(TIOCGWINSZ)
│   ├── platform_term_win32.c   # Win32 Console virtual-terminal backend (later)
│   └── proven_sys_*.c / .h     # vendored proven PAL
├── tests/                      # unit tests, one main() per file (exit 0 == pass)
├── scripts/                    # data generators (e.g., Unicode width table)
├── vendor/                     # third-party static deps (tree-sitter, pcre2)
├── queries/                    # tree-sitter .scm queries
├── THIRD_PARTY_LICENSES/
├── build/                      # build artifacts (gitignored)
├── bin/                        # output binaries (gitignored)
└── README.md
```

Note: terminal backends live only under `platform/` (raw `termios`/ANSI, no
ncurses); core `src/` never includes OS or terminal headers. The line-offset
index is part of the `buffer` module, not a separate public component.

### 18.1 Module Dependency Tree

```text
main
 └── editor
      ├── buffer ── encoding ── unicode
      ├── display ── platform
      ├── input ── keymap
      ├── window ── tab
      ├── syntax ── tree-sitter, queries
      ├── search ── pcre2
      ├── register
      ├── macro
      ├── bookmark
      └── config
```

---

## 19. Deferred Features

These are intentionally deferred until the core editor is stable.

- Dynamic plugin system
- Full Unicode grapheme cluster cursor movement
- Cross-session macro persistence
- Granular mouse extras (drag-resize the `+` corner, Windows console mouse) —
  core mouse support shipped (RFC-0014): wheel, click-focus/position, drag-select,
  scrollbar, close-`X`, panel rows
- Relative line number styles
- Advanced help interface
- Markdown embedded language highlighting

### 19.1 Extension Philosophy

Dynamic plugins are not part of the initial architecture. If extension support is added, prefer compile-time extension modules registered into a static extension table.

Suggested directory:

```text
src/extensions/
```

The `d` namespace may be reserved for developer extension features.

---

## 20. Implementation Milestones

### Milestone 1: Minimal Editor Core

Scope decisions for this milestone:

- **Buffer:** array-backed piece table behind an opaque interface (tree backend
  is a later, drop-in upgrade). The buffer owns a **line-offset index**
  maintained incrementally on every edit, and produces **undo inverses**
  (removed spans) as part of each mutating operation.
- **Loading:** read the whole file into an owned buffer (`read-all`). `mmap`
  and external-modification detection are deferred to the portability milestone.
- **Encoding:** the **UTF series** — UTF-8 and UTF-16/UTF-32 (LE/BE, BOM-detected)
  — is decoded to the internal UTF-8 on load and re-encoded to the original on
  save. A leading BOM and every CRLF / lone CR are normalized away (the buffer is
  LF-only, BOM-free) and reproduced on save from the recorded original encoding /
  EOL / BOM (§5.1/§13.3), so a load+save round-trips byte-for-byte. Windows-1252 is
  the self-contained fallback for a no-BOM non-UTF-8 file. Legacy multibyte code
  pages (EUC-KR/CP949/Johab, Shift-JIS, GBK/GB18030, Big5, …) are decoded by
  delegating to the platform converter (iconv / Win32) via the charset PAL
  (`platform_charset`), resolved lazily and selected per `[editor] charset_backend`.
  The backend picker only offers backends that exist on the host (`libc`/`command`
  on POSIX, `win32`/`command` on Windows); the external-`iconv` (`command`) backend
  works on every OS and its executable is set by `[editor] charset_iconv_path` (a
  PATH name, default `iconv`, or a full path for an iconv that isn't on PATH).
  In the `zo` open dialog the open-as encoding is chosen on the `e` sub-screen, the
  charset backend on the `b` sub-screen, and `B` toggles a save-BOM; the default is
  `UTF-8 (auto)`, which never touches a backend.
- **Unicode:** code-point cursor movement plus an embedded **East Asian width**
  table (generated from UAX #11) for visual-column math. Grapheme clusters stay
  deferred per §5.1.
- **Backend:** raw `termios` + ANSI escape backend under `platform/`, behind a
  backend-neutral `platform.h`. A mock in-memory backend is used to unit-test
  rendering.
- **Input scope:** only the Ed-mode conventional shortcuts of §11 (typing,
  arrows, Home/End/PgUp/PgDn, Ctrl-S save, Ctrl-Z/Ctrl-Y undo/redo). The full
  `zx` command grammar (§10) belongs to Milestone 2.

Deliverables:

- Open and validate a UTF-8 file (`read-all`).
- Piece-table insert/delete with maintained line-offset index.
- Cursor movement over code points and logical lines.
- Cell-based rendering of the visible viewport (tab expansion + wide chars).
- Atomic save via temp file + `rename` (§7.2).
- Undo/redo with cursor/selection restoration (§8).

### Milestone 2: `zx` Command Core

- Trigger handling
- Deterministic command parser
- Movement commands
- Line operations
- Save/open/quit commands

### Milestone 3: Multi-Buffer UI

- Panes
- Tabs
- Command window
- Buffer list
- Basic config file

### Milestone 3.5: Field mode — IMPLEMENTED (RFC-0007, 2026-06-19)

The first count-repeat insert (recording the inserted span and replaying it on
the insert→command transition) was removed: allowing cursor movement during the
run made "what is the repeated span" intractable. Replaced by **field mode**
(`docs/proposals/rfc-0007-field-mode.md`, see §9.3): a bounded, insert-only
mini-editor over a growing region with full in-region editing/selection/clipboard
and an isolated temporary undo stack, entered by `a`/`[N]a`, `on`/`op`, and the
change operators. The bounded region makes the repeat span unambiguous (= the
region) and makes the session's net change a single insertion (a/o) or replace
(c), committed as one global undo step on ESC. Buffer additions:
`prov_buffer_undo_scope_begin/end` (stack swap) and `prov_buffer_replace`
(ACT_REPLACE, one undoable action).

### Milestone 4: Search, Registers, Clipboard

- PCRE2 search
- Replace
- Registers
- OS clipboard integration

### Milestone 5: Syntax and Large-File Refinement

- Tree-sitter integration
- Syntax query loading
- Parser fallback
- Large-file thresholds
- Line-number gutter (`line_numbers` = off / absolute / relative-hybrid, §17.4),
  per-window, affecting every window
- Horizontal scrolling option for long lines (alternative to soft wrap)

### Milestone 6: Portability Hardening

- Windows backend
- macOS checks
- Termux behavior
- Unicode width table verification
- Packaging and license audit

### Special Milestone S: proven string-system & library adoption

Cross-cutting cleanup that re-houses prov's hand-rolled string / dynamic-buffer
code onto proven's tested primitives — chiefly the **string system**
(`proven_u8str_t` / `proven_u8str_view_t`), plus `fmt`, `scan`, and `array`.
Detailed plan and step ordering: `docs/proposals/rfc-0004-proven-string-and-library-adoption.md`.

- [x] S0 smoke test (`test_pstr`)
- [x] S1 config parser on views + scan
- [x] S2 bufset path on views
- [x] S3 filename basename on views (shared `prov_basename`)
- [x] S4 session input accumulator → owning `proven_u8str_t`
- [x] S5 global status line → reused scratch `u8str` + `fmt`
- [x] S6 piece-table add-buffer → `proven_u8str_t`
- [x] S7 sweep & document (RFC-0004 §8)

Status: **complete**. Residual libc-string sites are intentional and listed in
RFC-0004 §8 (bounded per-frame snprintf, byte-range memcpy reads).
