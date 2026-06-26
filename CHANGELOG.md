# Changelog

All notable changes to this project will be documented in this file.

The format is based on Keep a Changelog, and the project should keep the
entries human-curated and chronologically ordered.

## [Unreleased]

### Changed
- **`sh` opens a full regex reference.** A scrollable help page documents every
  feature of the regex engine (RFC-0009): literals/escapes, `.`/character classes
  (`\d\w\s`), anchors (`^ $ \b \B`) and Vim-style `\zs`/`\ze`, quantifiers
  (`* + ? {n,m}` + lazy), groups (`( )`/`(?:)`) and alternation, plus an explicit
  list of what is not supported (backreferences, look-around, named groups, inline
  flags, POSIX/Unicode classes).
- **Search marks the current match as a selection.** A search hit (`ss`/`sn`/`sp`,
  literal or regex) is now shown as a reverse-video selection so the current match
  stands out from the yellow highlights of the other occurrences; the cursor stays
  at the match start.
- **Find-repeat keys are now `.` (next) and `,` (prev)** — previously `;`/`,` — for
  the `f<char>` line find.
- **`zx` redo key is now `U`** (count-aware: `3U` redoes three steps); the old `zy`
  redo is removed. `u` undo / `U` redo are now a natural pair.
- **In-progress command hint is compact `key:label` (bright+bold).** Pressing a
  prefix key shows e.g. `z:meta x:exit w:write …` (was `z    meta …`); the missing
  `s:` (search) namespace help now appears too.
- **`zw` write panel (browser-style) + `zo` Enter/discard tweaks.** `zw` now opens a
  file panel modeled on `zo`: navigate folders/drives, type a name, **Enter** (or
  `o`) writes the buffer there; it has a **`d` discard** (reset name/options) and
  shows BOM/EOL but **no hex / preview / read-only** (irrelevant to saving). In the
  `zo` open panel, **`d` is removed** (no discard there); **Enter** opens the
  selected file or enters the selected folder/drive.
- **zo file-open panel polish.** The read-options row now shows **EOL** (CR/LF) next
  to encoding / backend / BOM / read-only, each as `key label:value` with the key
  letter bold+underlined; `RO` is spelled **Read-only**, and Tab focus cycles the
  EOL option too. The type-filter (`f`) edit shows a clear vertical-bar cursor `▏`
  (was a hard-to-see caret).
- **Windows drive navigation.** The path field accepts drive paths (`c:`, `C:\`,
  `c:\dir`) and resolves them correctly; the parent of a drive root opens a new
  **drives screen** listing each mounted drive with its volume label and free/total
  capacity (Win32 `GetLogicalDrives`/`GetVolumeInformation`/`GetDiskFreeSpaceEx`
  via a new `prov_platform_list_drives` PAL; POSIX unaffected).
- **Config/theme naming rule tightened.** INI section and key names must now be
  `[a-z][a-z0-9_]*` — start with a lowercase letter, then lowercase letters,
  digits, or underscore (no uppercase, spaces, or hyphens). `*.theme.ini` theme
  sections use the **`theme_`** prefix (e.g. `[theme_solarized_dark]`), and theme
  names follow the same rule. (All built-in names already conform.)
- **Context-aware help; the global-help `H` over panels is removed.** Help is now
  resolved by where you are: in a panel, `h`/F1 shows that panel's help; in zx,
  `h`/F1 the zx keyboard help; in ed mode (modeless), **F1** opens the ed-mode
  help page; in **field mode** (`a`/`[N]a`), **F1** opens the field-mode help. F1
  is the one universal, non-typing help key.
- **Panel legend unified onto the command line.** A panel no longer draws its own
  footer legend inside the box; the reclaimed row becomes panel content, and the
  key legend appears on the global **command line** (bottom row) for every context
  — editor, panels, find, save-as — one consistent place. Shortcut keys are shown
  **bold + underlined** (a `\x01` marker the terminal backends interpret); the
  go-to hint spells out both forms (`0g:last`, `3g:3rd item`); keys already shown
  on their rows/fields (e.g. `p` for the path field) are omitted from the legend.
- **New "screen layout" help page.** `h` then `L` (or the overview's `hL` entry)
  names every part of the window — tab bar, window/pane, gutter, scrollbar, window
  status, the global status and command lines, and panels.

### Added
- **Syntax highlighting (RFC-0021/0022), first cut.** A lightweight, line-oriented
  lexical highlighter colors buffer text through a resolved color theme. Cells gain
  a foreground field and the terminal backends emit/reset SGR colors; chrome stays
  monochrome (color is confined to buffer text). Language packs (detected by
  extension): **Markdown, C, Python, JavaScript, TypeScript, shell, JSON, TOML**
  (also `.ini`/`.cfg`/`.conf`/`.env`)**, YAML, CSS, Rust, Go, Java, Kotlin,
  Swift, Lua, SQL, HTML/XML**. Multi-line constructs (fenced code, block
  comments) use a small carry state seeded by a bounded look-back above the
  viewport; cost is bounded by hard caps with a plain-text fallback. The active
  theme comes from `[ui] theme=` and any `*.theme.ini` (built-ins
  `prov-dark`/`prov-light`/`mono`). More language packs follow incrementally.

### Changed
- **Hex editor exit & text↔hex switching ask before discarding edits.** Switching
  a buffer between text and hex reloads the file, so prov no longer refuses (or
  silently loses) unsaved changes: it prompts **`s` save · `d` discard · `c`
  cancel** first (`s` saves in the current mode, then reloads). In the hex editor,
  **`Esc` and `^Q` leave** (to text) through that prompt, and **`^S` saves and
  leaves** in one step. Opening a file from the dialog with the `x` (binary)
  toggle on reads it as hex.

- **Hex editor key map reworked** (RFC-0019). Movement is now `i`/`k`/`j`/`l` +
  arrows, with **`I`/`K` = PgUp/PgDn** and **`J`/`L` = Home/End**; `^G` jumps to a
  typed hex offset; `o`/`Insert` insert a byte (since `i` is now "up"); `y`/`p`
  copy/paste bytes; `h` opens help. **`v` selects a byte range and `r` now opens a
  full multi-line ed-mode editor** (`PANEL_K_HEXEDIT`) on the decoded selection —
  typing, `Enter` newlines, Shift+arrows / Shift+Ctrl+arrows selection, `^A`/`^C`/
  `^X`/`^V`, `^Z`/`^Y` — and **`^S` re-encodes it (charset) and writes it back** over
  the range (any length, one undo); `Esc` cancels. (Replaces the previous one-line
  string prompt.) "Hex editor" is the term used throughout.

### Added
- **Open dialog: read line-endings (`n`)** — choose how a file's EOLs are read:
  `Auto` (detect) / `LF` / `CRLF` / `CR`; the chosen style is restored on save.
  Shown in the dialog title and legend.

### Fixed
- **File-open preview** now flows as **one continuous code-point char-wrap** with a
  clean, straight right edge: line breaks (and tabs) in the file render as a space
  instead of ending the row, so short lines no longer leave a ragged right edge
  against the box border (`prov_textbox_t.flow`; hex preview unchanged). The
  binary/hex open toggle moved to the **`x`** key (was `H`) and is now shown in the
  browser legend (`x:hex`) and the panel title (`as binary/hex`).

### Added
- **Binary / hex editing mode** (RFC-0018 + RFC-0019, Phase 3d / backlog #3).
  Edits the **raw file bytes** — prov's text buffer is converted to internal UTF-8
  on load, so hex must operate on a *binary* buffer that loads and saves **verbatim**
  (no encoding / EOL / BOM conversion). Two ways in: the **file-open dialog `x`
  toggle** ("binary (raw)" — the preview switches to a hex dump), or **`wx`**, which
  reloads the focused buffer's file raw (and `wx` again / `Esc` reloads it as text;
  refused while modified, since a reload would lose edits). Editing is overwrite
  **plus** insert/delete: in the **hex pane** type `0-9 a-f` to set a byte, `i`
  inserts a `0x00`, `x` deletes, `[`/`]` nudge the byte window for alignment
  inspection, `g`/`G` jump to start/end, `jkl`+arrows move; the **ascii pane**
  (`Tab`) overtypes printable bytes; `Del`/`Backspace` delete in either pane;
  `^S` saves the raw bytes, `^Z`/`^Y` undo/redo, `^Q` quits. Read-only windows
  render hex but block edits. New pure, unit-tested `src/hexview.{c,h}` widget;
  `prov_load_binary` / verbatim `prov_encode_save`; mode label `Hx`/`Ha`.
  Under **every hex row** a **decoded-string line** shows that row's bytes in the
  interpretation charset (the open-dialog `e` encoding; default UTF-8) — real
  multi-byte glyphs, with a character straddling the 16-byte boundary shown whole
  on its starting row (UTF-8 exact; other charsets best-effort, nudge with `[`/`]`).
  **Range string editing**: `v` (or Shift+arrows) selects a byte range; **`Enter`**
  opens a prompt whose text is **encoded with the interpretation charset** and
  written over the selection (any new length, one undo) — or inserted at the cursor
  when nothing is selected; `x`/`Del` delete the selection. So you can replace a
  CP949/UTF-8 string in place even when the new text is a different byte length.

- **Persistent history** (small follow-up). The find/replace pattern + replacement
  rings and the file-browser `f` type-filter ring now survive a restart. They are
  saved to a **separate state file** `prov_state.ini` that sits next to the resolved
  config file (exe-side in portable mode, else `~/.prov/`), keeping the
  hand-edited `provconf.ini` clean. Written at clean exit, restored at startup; the
  ring's newest-first order is reproduced. A missing/garbled file is harmless
  (empty rings).
- **Scoped replace** (RFC-0016 §7). When a selection is active as the find panel
  opens, **replace-all (`a`) is restricted to the selection** by default; an
  `s:selection` toggle (shown only when a selection was captured) flips it back to
  the whole buffer. Single-match `r`, `n`/`N`, and regex `^`/`$`/`\b` context are
  unchanged (the regex still sees the whole document; only matches fully inside the
  scope are rewritten). One undo step.
- **Browser elision accent**. The middle-elision marker `…` in the file browser's
  name / extension columns is now reverse-colored per-cell (XOR, so it stays
  distinct on a selected row too), reading as "more here".

## [0.4] - 2026-06-22

### Added
- **`zc` config live-apply** (RFC-0017, Phase 6). The config file the editor reads
  at startup is now **re-applied the moment you save it** — no restart. The buffer
  showing it carries a ` [config]` tag in its status line; on save the text is
  re-parsed and applied (`mouse`, `charset_backend`/`charset_iconv_path`,
  `undo_limit` run their side-effects, everything else is read live from `s->cfg`),
  with **"config applied (live)"** on success or **"config saved — NOT applied
  (line N: …)"** (running config untouched) on a parse error.
- **Find / replace panel + one-key search** (RFC-0016, Phase 4). A new `/` key
  opens a dedicated find dialog (`PANEL_K_FIND`) that gathers everything in one
  place: a **pattern** field and a **replacement** field (both the reusable
  `lineedit`, with ↑/↓ history), the four option toggles **regex / whole-word /
  case / highlight**, **live incremental** search from the open point, and a
  **`match I / N`** count. Verbs: `n`/`N` next/prev, `r` replace-one-and-advance,
  `a` replace-all, `x`/`w`/`c` toggle options, `Tab`/`Shift+Tab` cycle focus,
  `h` help, `q`/`Esc` close (Esc restores the original cursor). Regex replacements
  expand `\1..\9` / `&` captures. The inline `ss` / `Ctrl+F` prompt and `sn`/`sp`/
  `sox`… commands are unchanged; both drive the same search state.

## [0.3] - 2026-06-22

### Changed
- **`zo` open-file dialog refinements (RFC-0015 follow-up).**
  - **Row numbers**: every entry is prefixed with a 1-based number, `..` always
    pinned to row 1 (and always first regardless of sort direction).
  - **Sort direction**: `t` now cycles *field × direction* — name/date/ext, each
    ascending then descending (`browse.sort_desc`); `..` and folder-before-file
    grouping are unaffected by the reverse.
  - **Extension column auto-width**: tracks the longest extension shown (default
    ≤3, capped at 10 so `.torrent`-class suffixes fit); anything longer is
    middle-elided with `…` (e.g. `supe…12345`).
  - **Key reshuffle / mnemonics**: `b` = backend, `B` = BOM, `R` = read-only,
    `p` = jump to path field, `v` = preview, `m` = info columns.
  - **Type-filter history**: the `f` box now recalls past filters with ↑/↓
    (session ring; config-file persistence is a documented future step).
- **OS-appropriate charset backends.** The backend picker is now built from the
  charset layer's actual platform registry (`prov_charset_backend_names`), so it
  never offers an impossible backend — **libc/command on POSIX, win32/command on
  Windows**. The external-`iconv` (`command`) backend is now portable and present
  on **every OS** (popen/_popen + a portable temp file), and its executable is
  configurable via **`charset_iconv_path`** (a PATH name, or a full path for an
  iconv that isn't on PATH; `prov_charset_set_iconv_path`).
- **Compact shortcut legends.** Panel footers and the bottom command-line legend
  now pack tightly as `key:label│…` (colon, no surrounding spaces) instead of the
  old ` │ ` / `key label` spacing.

### Changed
- **Keyboard-help panel keys realigned with the common panel model.** A bare key
  no longer silently drills into that key's page (which collided with the shared
  panel keys); instead **`h` is a prefix** — `h` then a key (or `h` then 1-9) opens
  that page. `w` repositions, `ikjl`/arrows/PgUp/PgDn scroll (so a narrow half-panel
  is navigable), **`q`/`x` close** the panel, and **Esc / Space / Enter** return to
  the key-arrangement overview. More broadly, every panel now acts only on its own
  shortcuts — editor keystrokes (e.g. `a`, `u`, `p`) are inert while a panel is open
  (already true by dispatch; the help panel was the lone exception, now fixed).

### Performance
- **Document code-point count is now O(1)** (Phase 1 ★). `prov_buffer_char_count`
  was an O(n) decode walk run **every frame** for the status line; the buffer now
  maintains `total_chars` incrementally on each insert/delete (counting UTF-8
  lead bytes over the edited span only). A `-DPROV_DEBUG_LINES` cross-check traps
  if the cached count ever diverges from a full rescan — the whole edit-test suite
  validates it. (`buffer.c`; `prov_buffer_char_total`.)

### Changed
- **Line endings and BOM are normalized on load and reproduced on save** (Phase
  3a; SPEC §5.1/§13.3). The loader (`prov_load_file`) strips a leading UTF-8 BOM
  and converts every CRLF / lone CR to LF, so the buffer is always LF-only,
  BOM-free UTF-8 — fixing stray-`\r` blanks (Mac/DOS files) and a BOM glyph at the
  start. The original encoding / EOL / BOM ride along on the buffer and
  `prov_save_buffer` rewrites LF to the original EOL and re-adds the BOM, so a
  round-trip is byte-faithful. (`wrap`/MIXED EOL saves as LF.)
- **Decode the full UTF series** (Phase 3a step 2). `prov_load_file` now detects
  and decodes **UTF-16 LE/BE** and **UTF-32 LE/BE** via their BOM (code pages
  1200/1201/12000/12001), converting to the internal UTF-8; `prov_encode_save`
  re-encodes to the original on save (BOM + EOL preserved), so a UTF-16/32 file
  round-trips byte-for-byte. All UTF codecs are algorithmic (no tables). The
  status line shows the detected encoding (e.g. `1200 UTF-16LE BOM`). Codec logic
  lives in `encoding.c` (`to_utf8` / `prov_encode_save`).
- **Windows-1252 fallback** (Phase 3a step 3, single-byte). A no-BOM file that is
  not valid UTF-8 now loads as Windows-1252 (a 128-entry table) instead of dropping
  bytes, so Western legacy files open correctly and round-trip on save
  (un-encodable chars → `?`); the status shows `1252 Windows-1252`. The single-byte
  codec is table-driven (`decode_singlebyte`), so ISO-8859-x / KOI8-R are a table
  away once an encoding picker can select them. Asian multibyte (CP949/Johab,
  Shift-JIS, GBK/GB18030, Big5) will be decoded by delegating to the platform
  converter (iconv / Win32) via the PAL — no in-binary tables — as the next step.

### Added
- **Reusable line-edit control for every text input** (`src/lineedit.{c,h}`). A
  pure, unit-tested single-line UTF-8 editor — code-point cursor + selection,
  ←/→/Home/End, **Ctrl+arrow word hops**, **Ctrl+Shift+arrow word selection**,
  Ctrl+C/X/V (OS clipboard), horizontal scroll (`prov_le_render` keeps the cursor
  in view), an editable-region underline, and a caller-owned **Up/Down history
  ring** (`prov_lehist_t`, no copy on push). A shared `le_handle_key` now drives
  the zo **path field**, the **type filter** (`f`), and all command-line / save-as
  **prompts** (zp, ss search, sr replace, za) — so they all edit like ed mode with
  the real cursor shown and per-prompt history. Ed mode itself gained Ctrl+arrow
  word movement and Ctrl+Shift+arrow word selection; **Shift+Tab** reverses panel
  focus.
- **Open-dialog field/help refinements** (RFC-0015 follow-up). The path field is
  now a full line editor (`src/lineedit.{c,h}`, reusable + unit-tested): ←/→/Home/
  End move the cursor (Shift extends a selection), insert/Delete at the cursor, and
  Ctrl+C/X/V copy/cut/paste through the OS clipboard (control bytes filtered). **Tab**
  cycles every dialog item — list → preview → path → encoding → backend → BOM →
  read-only — and Enter/Space activates the focused option. Each option's shortcut
  letter is emphasized (bold + underline) and the focused one is reversed. Every
  panel's `h` intent-help now shares one key model (q / x / h / Space / Enter / Esc
  exit, `w` reposition, ik/jl/arrows scroll); the browser's help text was rewritten
  for the dialog.
- **File-open dialog redesign** (RFC-0015). `zo` is now a vertical dialog —
  **file list / preview box / path-name input / options row** — instead of a bare
  list. The **path field** (Tab cycles focus list → preview → path) takes a typed
  path and on Enter navigates into a directory or opens a file: relative paths
  (`./downloads/new/`, `../docs`), absolute paths, trailing `/` optional, `\` as a
  separator on Windows, all normalized by `prov_browser_resolve_path` (`.`/`..`/
  empty/dup-separator collapse); control bytes are rejected as you type and the key
  decoder already blocks invalid UTF-8, so a broken-codepage paste can't enter a
  name. The **options row** carries encoding / backend / BOM / read-only: `e` opens
  an **encoding sub-screen**, `B` a **charset-backend sub-screen** (both take over
  the body, pick, and return), `b` toggles save-BOM, and `r` toggles **read-only
  open** (the opened window gets `RO`). The list gains an **extension column** and
  **head…tail** truncation for long names (`a_long_name….txt`); the selected
  entry's full name shows in the path line. The preview (RFC-0013) is stacked below
  the list and on by default.
- **Mouse support** (RFC-0014; SGR 1006). The laid-out UI is now clickable, gated
  by `[editor] mouse` (default on; set false to keep the terminal's own text
  selection). `prov_decode_key` parses SGR mouse reports into `PROV_KEY_MOUSE`
  events; `prov_term_enable_mouse` turns reporting on after the config loads.
  Behaviors: the **wheel** scrolls the window (or panel / preview) under the
  pointer; a **left click** focuses that window and positions the cursor (a
  **drag** extends the selection); the **scrollbar** click/drag jumps the
  viewport; the status-line **close-`X`** closes a pane; in a panel a click
  selects a row and clicking it again opens it. (Windows console mouse and the
  resize-`+` corner stay keyboard-driven for now — `ws` covers resize.)
- **File-browser preview pane** (RFC-0013 Phase A; backlog #2). `p` in the browser
  splits the panel into a file list (left) and a live preview (right) of the
  selected entry. The preview re-decodes with the chosen open-as encoding (the
  charset PAL when an encoding is picked with `e`, else UTF-8 with invalid bytes
  hidden — never a tofu glyph), and a **binary file falls back to a hex view**
  (`OFFSET  HH … HH  |ascii|`). `Tab` moves scroll focus between the list and the
  preview; the read is bounded (64 KiB, never the whole file). Built on the new
  reusable `src/textbox.{c,h}` widget (see below). Format-header info (MP3/ID3,
  image dims, …) and an editable preview are deferred to RFC-0013 Phase B.
- **Reusable text-box widget** (`src/textbox.{c,h}`). A pure, fixed-width,
  vertically-scrollable view that renders a borrowed byte span as code-point-safe
  wrapped text (invalid/partial UTF-8 skipped, no replacement glyph) or a hex dump.
  `tests/test_textbox.c` covers wrap, wide glyphs, invalid-byte skip, and the hex
  layout.
- **Explicit close-buffer command `zbq`** (small follow-up). Closes the active
  buffer without needing to close its windows: every window (across all tabs)
  showing it is repointed to a neighbor — a fresh empty buffer is created if it was
  the only one — then the buffer is dropped through the existing close path, so a
  modified buffer still raises the save-before-close prompt. (`zb` now: digit
  switch, `n` new, `q` close.)
- **Ed-mode `Ctrl+F` (find) / `Ctrl+R` (replace)** (small follow-up). The
  conventional shortcuts now open the same search / replace prompts the zx `ss` /
  `sr` commands use, so editing-mode users get inline search without leaving to the
  zx namespace. `Ctrl+R` keeps the "search first" guard and read-only guard. (Both
  keys were previously inert in ed mode; redo stays on `Ctrl+Y`.)
- **File-browser owner / group columns** (Phase 3b). The directory browser can now
  show an entry's owner and group (resolved to names via `getpwuid`/`getgrgid`, with
  a numeric fallback), filled lazily per row alongside perms/mtime. `v` cycles a new
  `ls -l`-style column preset (`perms owner group size mtime`). Needs proven
  **v26.06.22a**, whose `proven_fs_stat` now exposes `uid`/`gid` (re-vendored here);
  the long-standing REPORT request that blocked these columns is resolved.
- **Regex `^`/`$` consistency test** (Phase 1 ★). A `tests/test_regex.c` check
  asserts the whole-doc MULTILINE search (main.c) and the per-visible-line
  highlight (display.c, slicing each line as `[start, '\n')` per `line_end_of`)
  agree on every match start for anchored patterns — including the degenerate
  empty-line `^$` case, which pins the renderer's newline-excluding slice contract.
- **Unicode width-table verification test** (M6 / Phase 1 ★). `tests/test_unicode.c`
  now walks `prov_width_zero` / `prov_width_wide` to assert the table is sorted
  ascending and non-overlapping (it is binary-searched, so a stray edit would
  silently corrupt width queries), that `prov_char_width` returns the right width
  at every range endpoint, and that the zero and wide sets are mutually disjoint.
- **Open-panel encoding / BOM picker** (Phase 3a step 4). The file-open browser
  gains two verbs: **`e`** cycles the *open-as* encoding through a preset list
  (`UTF-8 (auto)` → CP949 → Shift-JIS → GBK → Big5 → EUC-JP), skipping any the
  active backend can't provide; **`b`** toggles a save-BOM. The chosen encoding
  shows in the panel header (`as CP949 · N items`) and, after opening, in the
  status line (`CP949`). A forced encoding routes the load through the charset
  PAL (multibyte CJK now opens correctly instead of falling back to Windows-1252),
  and a save re-encodes through the same backend so the file round-trips. Default
  stays `UTF-8 (auto)`, which never touches a backend. (`browser_cycle_enc` /
  `ENC_CYCLE` in `main.c`; `prov_editor_open`/`prov_load_file` `want_enc`.)
- **Pluggable charset-conversion backends** (`platform/platform_charset.{c,h}`).
  A registry of conversion backends resolved **lazily** — nothing is probed until
  a non-UTF-8 encoding is actually requested, and the result is cached for the
  session (so the UTF-8 default pays nothing). The selected backend is the
  configured preference (`[editor] charset_backend`) or the first working one:
  `libc` (the linked iconv), `command` (the external `iconv` tool, via a temp file
  + pipe), and `win32` (`MultiByteToWideChar`/`WideCharToMultiByte`). This is how
  legacy multibyte encodings (CP949/Johab, Shift-JIS, GBK/GB18030, Big5, EUC-JP, …)
  convert to/from the internal UTF-8 — **no in-binary tables**, riding on what the
  host already provides. API: `prov_charset_configure`/`_active`/`_supports`
  (memoized per encoding) /`_to_utf8`/`_from_utf8`. Tested: CP949 ↔ UTF-8
  round-trip through both the `libc` and `command` backends, and an end-to-end
  forced-CP949 load+save via the PAL (`test_save.c`).
- **Panel value-return picker** (RFC-0012 P2, the Option-D core). A panel can be
  opened as a *picker* that returns a value to its host: selecting a leaf calls a
  stored `panel_pick(value)` callback, Esc calls it with NULL (cancel), and the
  host reopens itself with the value (or its prior input on cancel). First
  consumer: in the **save-as** dialog, `Tab` opens the file browser at the typed
  path's directory; picking a file returns its path into the dialog, Esc returns
  with the typed text intact. (Per the RFC, this is a depth-1 suspend/return, not
  an arbitrary panel stack.)
- **Word-wrap** (`[editor] wrap = word`). A word that would overflow the row moves
  whole to the next row instead of char-wrapping mid-word; a word longer than the
  row still char-breaks, and every break falls on a code-point boundary so a
  multibyte character is never split. Each continued row shows a **reverse-video
  `<`** in its reserved last column to mark the wrap. The break decision lives in
  one place (`wordwrap_breaks`) shared by the renderer, cursor positioning, and
  row counting (gutter / scroll), so geometry stays consistent. Splash now also
  notes how to type a literal "zx" in ed mode (`zx` then Enter, nothing between).
- **Horizontal scroll** (`[editor] wrap = off`; also `wrap = false`). With wrap
  off, each logical line is one screen row and the viewport scrolls horizontally
  to keep the cursor column visible (`prov_hscroll_left`), clipping at the edges
  instead of wrapping; the bottom-border horizontal scrollbar shows a `█` thumb at
  the line position. `wrap = char` (default) keeps soft-wrap; `wrap = word` is
  accepted but currently behaves as `char` (word-wrap pending). Per-window origin
  (`leftcol`) persists across splits. The renderer's cell placement was unified
  into `put_cell` so both modes share one code path (char-wrap unchanged).
- **Line-number gutter** (`[editor] line_numbers = off | absolute | relative`,
  §17.4). Per-window, left-side, dim; `absolute` numbers every line, `relative`
  shows the cursor line's absolute number and every other line's distance. The
  number sits on a logical line's first screen row; wrapped-continuation and
  past-EOF rows keep a blank gutter (aligned via the same wrap formula the
  renderer uses). The gutter shrinks the content width like the right scrollbar;
  the cursor and content offset to its right. `prov_gutter_width()` in `display.c`.

### Changed
- **Re-vendored proven to `v26.06.21a`** (was `v26.06.18b`): picks up the
  upstream `map.c` `-Wunused-parameter` fix prov had reported, so the
  `-Wall -Wextra -DNDEBUG` release/dist build is now warning-clean. Vendored
  delta is just `src/proven/map.c` + `include/proven/version.h`.
- **`./nob --no-float` opt-in to drop proven's float `{}` formatter**
  (`-DPROVEN_FMT_NO_FLOAT`). Float formatting is **kept by default** so prov can
  grow float input/output later without foreclosing it; `--no-float` reclaims
  ~8 KB in release via `--gc-sections`/LTO (`bin/prov` 245,832 vs 254,024 bytes),
  for when prov is confirmed float-free. (Supersedes the earlier always-on
  define.)
- **Status/chrome polish.** The leading `X` on the global status line is now
  reverse-video (a mouse close-button cue). Editing windows regain a reverse `X`
  in their bottom-left corner — a future click closes the window (saving first if
  modified) — followed by a one-cell `─` rule and then the status label. The
  bottom-line key guides switch to a compact `key:label` style (e.g.
  `meta: x:exit w:write q:quit y:redo o:open h:help`).
- **Splash tweaks.** The drop shadow's right (vertical) arm is now two columns
  wide. The `prov text editor` title is drawn in the original (non-reversed)
  colors but bold, so it pops against the reverse-video card (the version beside
  it stays reverse).
- **Editing windows now use the box-style chrome of the modal panel, and the two
  share their scrollbar code.** The window's right column is the same vertical
  scrollbar as a panel (a `█` thumb on a `│` track with `▲`/`▼` buttons; just the
  plain `│` border when the buffer fits — previously it always drew a track on a
  `░` shade). The window's bottom row is now a `─` box bottom border carrying the
  status label (panel-title style), a horizontal scrollbar over the space that
  remains, and a `┘` corner that meets the vertical scrollbar (replacing the old
  full-width reverse-video bar and its `+` resize handle). The chrome is normal
  video, dim when unfocused. The leading `X` mouse-button placeholder was dropped.
- **Horizontal scrollbar capability** (`prov_draw_hscroll`) is implemented and
  wired into both the panel bottom border and the window bottom row, but stays a
  plain `─` track for now — it activates only once a horizontal-scroll mode (with
  wrapping off) is added. A minimum width is reserved for it in the window row.

## [0.2] - 2026-06-21

### Changed
- **Panel scrollbar now matches the editing window.** The modal panel's right
  border uses the same `█` thumb on a `│` track with `▲`/`▼` buttons as an
  editing window (was a `░`-shaded track). When the content fits, the scrollbar
  is dropped entirely — the right edge is just the plain `│` box border. The
  panel's box corners (`┐`/`┘`) are always left intact.
- **Redesigned start splash:** a centered reverse-video card with a bold
  `prov text editor` title and version, a help/mode-switch hint, and a one-line
  description of zx and ed modes, with a soft drop shadow at the bottom-right.
  Adds a `PROV_ATTR_BOLD` cell attribute (rendered via SGR, both backends).
- **Visual-block (`V`) is now display-column based.** Block cut/yank/paste/insert
  work on visual columns, so wide (CJK/emoji, 2-cell) glyphs and tabs form a true
  on-screen rectangle. A block cut is a single undo step. Cutting a rectangle
  leaves short lines that don't reach the block untouched; pasting/inserting a
  block pads short lines with spaces so the columns stay aligned. A wide glyph
  that straddles a block edge is left in place (not cut); its in-region cells
  become blanks in the yanked rectangle (e.g. a 3-cell glyph with 1 cell outside
  and 2 inside stays put and contributes 2 spaces), so only fully-contained
  characters are removed.
- **`tn` and 0t `n` open a new tab on an empty buffer** (was: reused the current
  buffer). `0t` gained `n` (new tab); `0w` gained `S` (mark a window, then `S` on
  another swaps their contents).

### Fixed
- **Stray characters after paste (and other escape-sequence misreads under
  latency):** a lone `ESC` byte is no longer immediately treated as the Esc key.
  When the pty splits an escape sequence (e.g. delivers `\x1b` separately from
  the `[A` of an arrow key — which the OS clipboard `popen` on `p`/`P` could
  trigger), the reader now waits ~30 ms (a standard ttimeout) for the
  continuation and decodes the full key. Standalone Esc still works.
- **Clipboard tools can no longer disturb the terminal:** the spawned helpers
  (`wl-copy`/`xclip`/`xsel`, …) have stdin/stdout/stderr redirected to
  `/dev/null` (except the data pipe) so a forked selection server cannot inherit
  and write to the controlling terminal.

### Changed
- **Config file is now `provconf.ini`** (was `config.toml`), read from
  `~/.prov/provconf.ini` — or from a `provconf.ini` next to the executable, which
  takes priority (portable mode) and makes the home copy ignored. The format is
  unchanged (the common subset of TOML and INI).

### Added
- **Uppercase navigation in zx mode:** `I`/`K` page up/down, `J`/`L` jump to line
  start/end (lowercase `ikjl` stay up/down/left/right).
- **Save-as dialog (`za`):** a full-panel save dialog. Type the target path and
  press Enter; if the file already exists it asks to overwrite (y) or rename
  (n), and a failed write keeps the dialog open with an error notice so you can
  fix the path and retry. `zw` on an unnamed buffer opens the same dialog; after
  a successful save the buffer is named and `zw` writes straight to it.
- **Tab management in `0t`:** the top tab bar is gone — the global status line
  shows the tab count as `Tab:N`, and `0t` lists/manages tabs. In `0t`: `I`/`K`
  move the selected tab up/down one, `J`/`L` send it to the top/bottom, `f`
  folds a tab open to list its windows (indented), and `x` closes a tab
  (prompting to save each modified window).
- **`zc` config editor:** opens `~/.prov/config.toml` for editing, creating
  `~/.prov/` and writing a commented starter file (the built-in defaults) the
  first time. The editor reads the same file at startup.
- **More config keys applied:** `[search] ignorecase / highlight / wrapscan`
  and `[clipboard] sync` now affect behavior (defaults reproduce the built-in
  behavior, so a missing config changes nothing). The remaining SPEC §17.3 keys
  (shiftwidth, line_numbers, wrap, mouse, theme.*, syntax.*) stay parsed-and-
  tolerated, documented as reserved in the starter file.

### Changed
- **Starts in zx (command) mode** instead of Ed (insert) mode, so movement and
  commands work immediately on launch; type the trigger (`zx`) to drop into Ed.

### Fixed
- **Linewise paste stability:** `p` after a linewise yank (`yy`/`3yy`) reliably
  pastes whole lines again. The OS-clipboard sync no longer mistakes a
  round-trip trailing-newline normalization for an external edit, which had
  let it clobber the register's LINE shape with CHAR (pasting mid-line). The
  in-editor command/automata state was audited across all zx commands: every
  command (except `a`/`o`/`c`, which intentionally enter field mode) returns to
  zx idle, mid-command `Esc` cancels back to idle, and `n` repeats correctly.
- **File browser crash** on a directory that can't be listed (e.g. permission
  denied): `prov_browser_load` now lists the target *first* and only frees/replaces
  the current entries on success, so a failed load leaves the listing — and the
  panel's view indices into it — intact instead of freed-but-referenced.

### Changed
- **Panel look:** panels now draw as a **box** — the title sits on the top border
  (`┌─ Title ──── N items ─┐`), the right border is a `░`-track / `█`-thumb
  scrollbar, the bottom border carries a (currently unused) horizontal scrollbar,
  and a status/legend row sits just inside the bottom border. The footer leads
  with the action triad `│o-OK│c-Cancel│d-Discard│` then the key legend. The
  **OK / select key is now `o`** (was `y`); the browser's info-columns key moved
  to `v`.
- **`ws` resize** is a continuous loop: `ik`/`jl` (or arrows) resize, `c` closes
  the window (area merges into a neighbour), `q` exits. Removed `ww` (use `0w`);
  `0t` switches tabs with `[N]g` like `0w` and mirrors its row layout.
- **Panel navigation:** `j`/`l` (and ←/→) are now **PgUp/PgDn** (were left/right). In the
  file browser: `i`/`k` move, `j`/`l` page, `y` opens/enters, **`I`** goes to the
  parent folder, **`J`** goes back to the previous folder (a visit history), `o`
  cycles columns. (The browser is shared with the upcoming Save-As dialog.)
- **Shortcut hints** use a clean 1-cell `│` (U+2502) separator between keys.
- **Splash screen** redesigned: `key - description`, items separated by `│`.
- **Help overlay** is now a positionable panel-like layer: `h` cycles its position
  (FULL / halves, code visible behind), **Space** → keyboard overview, **Enter**
  closes (was Esc), `i`/`k`/arrows scroll, a key drills into its page.

### Added
- **Visual block mode (`V`)**: a rectangular column selection (status code `Zb`).
  Move to size the block; `y` yanks and `d`/`x`/`c` cut the column range into a
  BLOCK-shaped register; `p` pastes a block column-wise across successive rows;
  `I`/`A` insert/append on every row (type on the top row, `Esc` replicates).
  Highlighted live in the editor.
- **Numbered register ring (`0`-`9`)**: every unnamed yank/cut pushes its text
  onto a kill-ring (newest in `0`); `b0v`..`b9v` walk back through recent ones.
- **0-series panels completed**: `0u` undo history (undo back to a point), `0n`
  jumplist (jump back to a recent origin). Plus a field-mode underline cue at the
  insertion point for an empty region.
- **OS clipboard bridge** (`platform/platform_clipboard.{c,h}`): the unnamed
  register mirrors the system clipboard — a yank/cut (`y`/`d`/`x`, `^C`/`^X`,
  `b<reg>c/x`) pushes it out; a paste (`p`/`P`, `^V`) pulls an externally-changed
  clipboard back in (keeping the internal linewise shape when unchanged). Uses the
  platform's CLI tools (`wl-copy`/`xclip`/`xsel`; `clip`/`powershell`); when none
  is present every call is a graceful no-op, so the editor's registers keep working.
- **Common modal panel** (RFC-0010): a single overlay panel composited over the
  editor (FULL or a TOP/BOTTOM/LEFT/RIGHT half so code stays visible), driven by a
  shared zx-style keymap (`src/keymap.{c,h}`) and a render-free model
  (`src/panel.{c,h}`): `i k`/arrows + `[N]` to move, `[N]g`/`0g` to jump, `ss` to
  filter, Space/Enter to pick, `w` to reposition, `h` for a per-panel help page
  (intent + key reference), `c`/Esc to close — the footer legend and help are
  generated from the keymap (self-documenting, no global key table). Some panels
  carry **verbs** (`r` record, `n` set, `x` delete) that take a slot key. In the
  windows panel, `Ng` (e.g. `2g`) focuses window N (current tab; supports any count).
  Consumers: **`0w`** windows, **`0t`** tabs, **`0b`** registers (pick pastes; `x`
  deletes), **`0e`** macros (`r`<slot> records, `E` stops, Enter replays), **`0m`**
  bookmarks (`n`<slot> sets at the cursor, Enter jumps), **`0s`** search history,
  **`0z`** command cheat-sheet (Enter opens that key's help), **`ww`** windows
  (`Ng` focuses). The **file browser** (`zo`) is now a panel too, backed by
  a virtual row source so huge directories stay cheap (lazy per-row stat); `l`/→
  opens or descends, `j`/← goes up, `o` cycles the info columns. Replaces the old
  bespoke browser/`ww`-popup UI.
- **Regex search & replace** (RFC-0009): a self-contained **Pike VM** (Thompson
  NFA + submatch tracking — the RE2/Go/Rust-regex lineage) in `src/regex.{c,h}`:
  **linear time, no catastrophic backtracking / ReDoS**, capture groups, libc-
  free. Supports literals, `.`, classes `[...]`/`[^...]` + `\d\w\s\D\W\S`,
  quantifiers `*+?{n,m}` + lazy, alternation, capturing/`(?:)` groups, anchors
  `^ $ \b \B`, and `\zs`/`\ze` match-boundary markers; UTF-8 aware (`.`/negated
  classes match whole codepoints). **`sox`** toggles regex mode for the `s`
  namespace — `ss`/`sw`/`sn`/`sp` and the match highlight all route through it;
  **`sr`** replacement expands `\1`..`\9` / `&` captures and accepts a `g/re/`
  or `v/re/` **line guard** (substitute only on lines that do / don't match).
  Backreferences and lookaround are intentionally out (they need backtracking);
  `\zs`/`\ze` + line guards recover their practical value. Validated by a
  differential fuzz against an independent backtracking oracle (~60k cases, 0
  mismatches) and an anti-ReDoS benchmark proving flat ns/byte
  (`docs/benchmarks/regex-benchmarks.md`).
- **File-open browser**: `zo` now opens a full-screen directory browser instead
  of a bare path prompt. It lists entries dirs-first with row numbers and a
  leading `..`, shows toggleable info columns (size / permissions / mtime /
  type) via a **Ctrl+O** options box of checkboxes (default: size + modified
  time; or names only), and is navigable by arrows (`↑↓` select, PgUp/PgDn,
  `←` parent, `→`/Enter open-or-descend) **or** the command line — type to
  filter (case-insensitive substring), digits to pick a row number, **Tab** to
  autocomplete to the selection, Backspace to edit (or ascend when empty), Esc
  to cancel. The start directory is resolved to an absolute path so `..` can
  ascend past the cwd. Listing/stat is via `proven_fs_list` / `proven_fs_stat`;
  entries are stat'd lazily (only displayed rows), so large directories open
  without a stall. *Owner/group columns are deferred —* `proven_fs_stat`
  exposes no uid/gid and has no Windows owner equivalent.
- **Welcome splash**: the empty start buffer now centers a brief intro naming
  the `h` help key and explaining the `zx` command mode and the role of `Esc`.
- **Help overview shortcut**: in the `h` overlay, Space or Enter returns to the
  keyboard overview, and each key's page advertises its `0`-prefixed variant.

### Fixed
- **Windows rightmost column**: dropped the redundant erase-to-EOL on
  full-width rows that, with auto-wrap off, blanked the pinned last column
  (the black vertical stripe on Windows consoles).

### Added (M4)
- **Macros** (RFC-0008 M4.6): `0e<slot>` records keystrokes into a slot (`a`–`z`,
  `0`–`9`) until `0e<slot>` again; `e<slot>` / `[N]e<slot>` replays it; **`E`** /
  `[N]E` replays the last macro. Recorded keys are fed back through the input
  dispatch (with a runaway guard); nesting works.
- **Named registers** (RFC-0008 M4.2): `b<reg>c` / `b<reg>x` copy/cut the
  selection into register `<reg>` (`a`–`z`, `0`–`9`), `b<reg>v` pastes from it
  (`[N]` copies). 36 session-global slots, independent and overwritable.
- **Bookmarks** (RFC-0008 M4.3): buffer-local marks — **`0m<letter>`** pins slot
  `a`–`z` at the cursor, **`m<letter>`** jumps to it. Marks are stored in the
  buffer and **auto-shift with edits** (insert/delete before them), so they keep
  pointing at their content — no drift. `0m` overview deferred.
- **Search** (RFC-0008 M4.5a): the `s` namespace — `ss` prompts for a term with
  **incremental** jump-as-you-type (Esc restores the cursor), `sw` searches the
  word under the cursor, `sn`/`sp` go to the next/previous match (wrapping). All
  visible matches are **highlighted** (Esc clears). `sr` replaces every
  occurrence of the current term in one undo step. **`soc`** toggles
  case-insensitive search (applied to find/replace/highlight alike); **`soh`**
  toggles the highlight. Literal byte search for now (`prov_search_bytes`,
  ~850 MB/s — see `docs/benchmarks/search-benchmarks.md`); regex is planned.
- **Paste shapes** (RFC-0006, M4.1): the register now carries a *shape*
  (char/line). `yy`/`dd` tag it linewise; `p` then pastes whole line(s) **below**
  the current line and **`P`** **above** (column-independent; `p` on the last
  line appends without a trailing blank). A characterwise register still inserts
  at the cursor (`p` == `P`). `[N]p`/`[N]P` paste N copies. `P` is the first
  uppercase command (SPEC §10.1 principle 8). New `prov_editor_paste_lines`.
- **Field mode** (RFC-0007, SPEC §9.3): `a`/`[N]a`, `on`/`op`, and the change
  operators (`c{motion}`, `cc`) now enter a bounded, underlined input region — a
  mini-editor that is insert-only and clamps the cursor/selection to the region,
  with in-region movement, `Shift`-selection, clipboard (`Ctrl+X/C/V`), `Ctrl+A`
  select, and an isolated temporary undo stack (`Ctrl+Z/Y`). `zx` is literal
  here; ESC is the only exit and commits the whole session as one undo step:
  `a`/`on`/`op` stamp the region `N` times (`80a-` ⇒ 80 dashes; `3on` ⇒ 3 lines),
  `c` pre-selects + replaces the target. Buffer gains `prov_buffer_undo_scope_*`
  and `prov_buffer_replace` (one undoable replace).
- Save-time compaction: the piece table's append-only add store accumulates
  orphaned bytes as you delete text; saving (`zw` / `:w` / `^S`) now runs
  `prov_buffer_compact`, which re-materializes the current content as one piece
  with a fresh add store, reclaiming that memory. Content/cursor/undo unchanged.

### Changed
- Buffer/piece-table efficiency and the last libc removal (RFC-0005). buffer.c's
  two array-shift `memmove`s use the new `proven_mem_move` (vendored
  `v26.06.18b`) and the create-time `memset` is a struct zero-init — buffer.c is
  now fully libc-free. The line index is updated **incrementally** on each edit
  instead of a full O(document) rescan, and sequential typing **coalesces** into
  one add-piece. Net: edits in a large document are 5–50× faster (insert@mid
  27.7×, delete@front 50×; see `docs/benchmarks/buffer-edit-benchmarks.md`). An
  arena/pool node allocator was analyzed and found not worth it — the wins were
  algorithmic, not allocator-bound.
- Vendored `proven_c_lib-v26.06.18a` and made every prov source free of libc
  string functions (RFC-0004 §8 follow-up). proven gained `proven_u8str_borrow`
  / `proven_u8str_reset` (a fixed-capacity string over caller-owned memory) and
  a bounded `proven_mem_copy`; with these, the per-frame UI builders (command
  line, window status, tab bar, messages) format via a borrowed
  `proven_u8str_t` (`FMT_INTO`, no per-frame allocation), `command.c` /
  `save.c` / `buffer.c` / `config.c` drop their `snprintf`/`memcpy`/`strcmp`,
  and only `buffer.c`'s two internal array-shift `memmove`s remain (the deferred
  `proven_array` work). PTY output is byte-identical.
- Adopted proven's string system across the codebase (Special Milestone S /
  RFC-0004): the piece-table insert store and the prompt input buffer are now
  owning `proven_u8str_t`; the config parser, buffer-set paths, filename
  basenames, and the global status line use `proven_u8str_view_t`,
  `proven_scan_i64`, and the type-safe `proven_*_fmt` formatter. New shared
  header `src/pstr.h` bridges proven views and fixed cstr fields. No behaviour
  change; the opaque `prov_buffer_t` interface is unchanged.

### Added
- `[N]n` repeats the last command N times. `n` replays the most recent non-`n`
  command, so a run of `n` keeps repeating it (the chain rule — `n` never
  records itself).
- The help overlay (`h`) was redesigned: a reverse-video breadcrumb bar with a
  scroll percentage, a rule, and a refined keyboard overview / detail pages
  (UTF-8, bullets, arrows, dim secondary lines). `Enter` steps *up* one level
  (detail → overview → exit), `Esc` closes from anywhere, and the digits `1`–`9`
  open a dedicated count-prefix explainer page. Planned-but-inactive keys
  (`b` `m` `e` `s` namespaces, and the reserved `r` `q`) are marked with a `·`
  in the overview and a "planned — not active yet" note on their pages; the
  overview now labels `y` correctly as the yank operator.
- `a` enters Ed (text-entry) mode at the cursor (append). The byte/char position
  in the global status is a plain comma-grouped integer (no k/m/g/t). The
  Ed-mode legend now explains block selection and the clipboard.
- Planned (Milestone 3.5, SPEC §20): `[N]a` repeats the inserted run N times.
- The terminal redraws immediately on resize (SIGWINCH), without waiting for a
  key.
- The global status line shows the cursor's byte and char position
  `<cur>/<total>b <cur>/<total>c` (k/m/g/t suffix for large counts); the window
  line's `C<col>/<linelen>` now uses the visual column (a 2-cell wide char counts
  as 2).
- A wide (2-cell) char that would straddle a window's right edge pads the last
  cell and wraps to the next row instead of spilling a half glyph.
- Status lines reformatted (single-spaced): the window line is
  `X L<cur>/<lines> C<col>/<linelen> <name>[*][ RO]` (modified `*` is a name
  suffix; unnamed = `NoName`), the global line is
  `X <mode> <codepage> <enc> <BOM> <eol>` (e.g. `X Zx 65001 UTF-8 noBOM LF`;
  EOL `CR/LF`, marker `noBOM`). The fresh zx state shows lowercase `zx`.
- Esc in Ed mode switches to zx mode.
- Help overview is a keyboard layout (key row above a description row); see the
  redesign entry above for navigation.
- Long lines soft-wrap onto the next row instead of being truncated (a
  horizontal-scroll option is planned). The cursor follows wrapped rows.
- Window chrome polish: the scrollbar is a `│` track with a `█` thumb plus
  `▲`/`▼` scroll buttons near the bottom; the window's bottom-right corner is a
  `+` resize handle (normal color); the window status line is always reverse and
  starts with an `x ` close-button. The global status line is now normal video.
- Planned (Milestone 5, SPEC §17.4): a `line_numbers` gutter with `off` /
  `absolute` / `relative`-hybrid styles, per-window.

### Fixed
- The literal-trigger Enter (typing "zx") now fires only when Enter is the very
  first key after entering zx mode — any other key, Esc included, cancels it.
- Linewise yank/cut (`yy`/`dd`) of a last line without a trailing newline now
  pastes as a whole line (the register always carries a trailing newline).

### Added (earlier this cycle)
- Window borders are now the window's own chrome: the bottom row is its status
  line and the right column a vertical scrollbar. Split siblings sit directly
  adjacent — no `─`/`│` line borders. `ws` resize is directional: `i`/`k` (or
  ↑/↓) change height, `j`/`l` (or ←/→) width; down/right grow.
- Buffer / window / tab model made explicit: a **buffer** is an open file
  (shared content), a **window** is one view of a buffer (a pane, with its own
  viewport and read-only flag), and a **tab** is a whole window layout (a desk).
  The `t` namespace now switches/creates/closes tabs (each its own layout); `zb`
  + digit switches a window's buffer and `zb` + `n` makes a new buffer. The tab
  bar lists tabs (`+` marks a split tab).
- Window navigation: `ww` opens a popup list of the tab's windows (file + Ln/Co;
  digit focuses), `wp`/`wn` previous/next window, `wi`/`wk`/`wj`/`wl` focus the
  window up/down/left/right (replacing the old `wm` submode).
- A buffer auto-closes when its last window (across all tabs) closes; if it has
  unsaved changes a save prompt appears first (named: y save / n discard;
  unnamed: y save-as / n discard). Closing a tab resolves its freed buffers in
  turn. Buffers shown elsewhere, switched away from, or never shown are kept.
- Per-window read-only (`wr`): blocks edits through that window and shows `RO` in
  its status line; another window on the same buffer can still edit.
- In-editor help: `h` (in zx) opens a full-screen overlay laying the keys out
  QWERTY-style with short labels; pressing a key opens that key's page (cursor,
  operators, namespaces, …), arrows/PgUp/PgDn scroll, Esc closes. (`src/help.c`)
- Global status line shows the code page before the encoding (e.g. `65001 UTF-8`,
  with an optional country tag for legacy code pages).
- Three distinct status lines: a **window status line** under every window
  (tab number, `Ln/Co` position, modified `*`, abbreviated file name — focused
  reverse, others dim), a **global status line** (mode, encoding, line-ending,
  BOM for the focused file), and the **command line** (input + help). File
  encoding / line-ending (LF/CRLF/CR/MIXED) / BOM are detected at load.
- Split windows (`w` namespace): `wh` horizontal split, `wv` vertical split, `wq`
  close. Each window is an independent viewport over a buffer (its own scroll
  position); siblings share a single `─`/`│` border, with a resizable split ratio
  (10–90%, `ws` submode). Every window shows a status row (filename, modified
  flag, position); the focused window is reverse-video, others dimmed. The
  focused window drives the cursor-visible scroll and page size.
- Config options now applied (previously parsed but inert): `scrolloff` (keep N
  rows of margin above/below the cursor), `expandtab` (Tab inserts spaces to the
  next tab stop), and `undo_limit` (bound retained undo history; 0 = unbounded).
- Feedback for recognized-but-unimplemented zx commands ("…: not implemented
  yet") instead of a silently dropped keypress.
- Paragraph text objects `ip`/`ap` and tag text objects `it`/`at`
  (`prov_motion_textobj`), so `dip`, `cap`, `dit`, `cat`, etc. work.
- Standalone `f<char>`/`t<char>` cursor find motions with `;` (repeat) and `,`
  (reverse), plus counts (`3f<char>`). Operator-pending `dfx`/`dtx` unchanged.
- Quit guard: `zq`/`^Q` on a modified buffer arm a confirmation (warning on the
  command line); `zqq` (or `q`/`^Q` again) force-quits, `zw` saves, any other
  key cancels. `zq`/`^Q` on a clean buffer quit immediately.
- End-of-file marker: screen rows past the last buffer line show a vi-style `~`
  in the first column, and the viewport can scroll until only `~` rows remain
  below the last line (Page keys scroll the viewport to the end). RFC 0002.
- Live zx command feedback in the status bar: the keys typed so far plus a short
  hint of the sub-commands they can become, or — right after a command runs —
  the command and its one/two-word function (e.g. `12g  goto line`). Backed by
  pure `prov_cmd_describe()` / `prov_cmd_label()` helpers. RFC 0002.
- Vim-style operator-pending editing with text objects for words, quotes,
  delimiters, tags, and paragraphs.
- Ed-mode block selection (SPEC §11): editor selection anchor/range with
  extend-on-Shift-move, select-all, delete/cut/copy/paste via an internal
  register; selection-aware insert/backspace/delete (typing replaces the
  selection); reverse-video highlight in the renderer and both terminal
  backends; wired into `main.c` (Shift+movement, Ctrl+A/C/X/V). Key events now
  carry Shift/Ctrl modifier flags.
- Milestone 2: deterministic `zx` command engine (`src/command.{c,h}`,
  `tests/test_command.c`). The parser is incremental and timeout-free and
  covers repeat counts; movement (i/k/j/l); single-key edits (u/p/v/x/r/a/q);
  the operator family (c/d/y) with linewise (dd/cc/yy), word (w/b/e), line-end
  (l), match (m), find/till (f<char>/t<char>), and text-object targets; the
  g/o/z namespaces; and the 0-special-prefix. `main.c` adds the Ed<->zx "zx"
  trigger, INS/OVR toggle, and executes movement, line operators (dd/yy/cc),
  goto, and meta actions (zx/zy/zw/zq, gg/ge/gf/gl/gu/gd, on/op).
- Text motions and objects (`src/motion.{c,h}`, `tests/test_motion.c`): word
  next/prev/end, find/till on the line, matching-bracket, and text objects
  (word, () [] {} <>, quotes). Wired into the zx operators so dw/cw/de/db,
  df<char>/dt<char>, dm, diw/caw, ci(/da{/yi", and gp/gn execute. PTY-verified
  end-to-end (dw, de, di(, ca(, dfX, dt., yy/p, diw). The register/bookmark/
  macro/search/window/tab namespaces and `;`/`,` find-repeat are later milestones.
- `nob`-based build foundation: incremental `nob.c`, vendored `proven`
  library, minimal runnable `src/main.c` skeleton, and a smoke test.
- `AGENTS.md` guideline: vendor `proven` from the local checkout, and the
  `docs/REPORT.md` halt-and-report procedure for upstream `proven` defects.
- Milestone 1 `buffer` module: array-backed piece table with a buffer-owned
  line-offset index and per-edit undo/redo (`src/buffer.{c,h}`), with
  `tests/test_buffer.c` (passes under ASan/UBSan). nob now links core modules
  into tests separately from the `main.c` entrypoint.
- Milestone 1 `unicode` module: UTF-8 decoding (rejecting overlong, surrogate,
  and out-of-range sequences) plus UAX #11 East Asian width
  (`src/unicode.{c,h}`, curated `src/unicode_width_table.h`), with
  `scripts/gen_unicode_width.py` to regenerate the table and
  `tests/test_unicode.c` (passes under ASan/UBSan).
- Milestone 1 `encoding` loader: read-all UTF-8 file loading with validation
  (`src/encoding.{c,h}`, `tests/test_encoding.c`).
- Milestone 1 `editor` layer: cursor (byte/line/col), insert/backspace/delete,
  code-point and line movement with a preserved goal column, and undo/redo
  that restores the cursor (`src/editor.{c,h}`, `tests/test_editor.c`). The
  buffer's undo/redo now report the edit site (`prov_edit_info_t`).
- Milestone 1 `display` renderer: pure editor-state-to-cell-grid rendering with
  tab expansion, wide characters, and a control-character fallback, plus cursor
  screen position (`src/display.{c,h}`, `tests/test_display.c`).
- Milestone 1 atomic `save` (SPEC §7.2): serialize + temp file + atomic rename
  (`src/save.{c,h}`, `tests/test_save.c`).
- Milestone 1 `input` key decoder: pure bytes-to-key-event decoding for chars,
  Ctrl combos, arrows/Home/End/PageUp/Down, Enter/Tab/Backspace/Delete, and ESC
  (`src/input.{c,h}`, `tests/test_input.c`).
- Milestone 1 terminal backend and event loop: raw `termios` + ANSI backend
  (`platform/platform.h`, `platform/platform_term_posix.c`) and the Ed-mode
  editing loop in `src/main.c`. Verified end-to-end via a PTY harness (open,
  type, arrow-move, Ctrl-S save, Ctrl-Q quit).
- Windows x64 support: Win32 Console virtual-terminal backend
  (`platform/platform_term_win32.c`) sharing the ANSI rendering and key decoder
  with POSIX, a `win64` cross-compile target in `nob.c` (mingw-w64, per-target
  build dirs), and `scripts/build-win64.sh` to cross-build on the remote mingw
  host. Produces a verified `PE32+` console executable. (Unblocked by the
  upstream `proven_c_lib-v26.06.16x` panic portability fix.)
- Per-platform build artifacts: each build now publishes a platform-named
  binary into the project root — `prov-linux-x64` (`./nob`) and
  `prov-windows-x64.exe` (`./nob win64`). Codified as an AGENTS.md guideline
  (cross-platform from the start; `prov-<os>-<arch>` naming).

### Changed
- In zx, the arrow and Page keys now honor a pending count (e.g. `5<Down>` moves
  five lines, `2<PageDown>` scrolls two pages); previously the count was dropped.
- `[N]g` is now the goto-line shorthand: a count followed by a single `g` jumps
  to line N immediately (previously `[N]gg` was required). With no count, `g`
  still opens the goto/movement namespace (`gg`, `ge`, ...). RFC 0002 / SPEC §10.7.
- The bottom of the screen is now two rows: a status bar and a command line
  below it. The status bar is packed tight (no inner padding): position
  `L<line>/<lines>C<col>/<linelen>`, then a no-bracket two-letter mode (`Ei` Ed
  insert, `Eo` Ed overwrite, `Zx` zx, `Zv` zx visual), then a one-char modified
  flag (`*` or a space), then the file name — e.g. `L12/123C8/54Ei notes.txt` /
  `L12/123C8/54Ei*notes.txt`. The command line carries the live zx command
  feedback (and, in Ed, a minimal key legend). Replaces the earlier single
  `Ln/Col` status row.
- Page keys (PageUp/PageDown) now scroll the viewport by a page and can reach
  the end of the file (the last line and the `~` rows below it can rise to the
  top), instead of pinning the last line to the bottom row. RFC 0002.
- Release build is now size-reduced: `./nob --release` compiles with
  `-ffunction-sections -fdata-sections` and links with `-Wl,--gc-sections -s`
  (on top of `-O2 -DNDEBUG`), so the linker drops the large unused portion of
  the vendored `proven` library and strips symbols. Native `bin/prov` shrinks
  from ~192 KB (plain `-O2`) / ~493 KB (default debug `-g -O0`) to **~56 KB**;
  the mingw `bin/prov.exe` from ~730 KB to **~173 KB**. Release objects now live
  in a separate `build/obj-release` (and `build/win64/obj-release`) so switching
  between debug and release no longer reuses objects compiled with other flags;
  nob also stamps the last build config per target and force-relinks on a
  switch, so `./nob --release` always produces the release binary (and `./nob`
  the debug one) without needing `--clean`.
- zx-mode ESC is now a **cancel**, not an exit: it discards any partially
  entered command and active visual selection and keeps `zx` mode at its idle
  base state instead of returning to Ed mode (SPEC §9.2 updated). Leave `zx`
  via the `zx` command, `Enter` (literal trigger), or an Ed-entering operator.
- Build output: binaries are written under `bin/` only and are no longer copied
  to the repository root (`nob.c` drops the per-platform root publishing;
  AGENTS §5 / README updated).
- Vendored `proven` `v26.06.17a` (portable panic handler + 32-bit-target
  `float_decimal.c` build fix), sourced from the local checkout.
- Rewrote `README.md` and `TEST.md` for the C23/`nob` toolchain, replacing the
  leftover AI Studio React/Vite scaffolding.
- Adopted a raw `termios` + ANSI terminal backend (no ncurses/terminfo) and
  updated `SPEC.md` §2/§3.2/§18, `DEPENDENCIES.md`, and `TEST.md` accordingly.
- Pinned Milestone 1 scope (`SPEC.md` §20): array-backed piece table with a
  buffer-owned line-offset index, `read-all` file loading, UTF-8-only encoding,
  embedded UAX #11 width table, and Ed-mode shortcuts only.
- Reassigned redo from bare `y` to `zy`.
- Reclassified `c`, `d`, and `y` as operator starters instead of single-key
  commands.
- Reworked operator-pending motions to prefer alphabetic line and delimiter
  markers (`f`, `l`, `m`) instead of punctuation-heavy `^`, `$`, and `%`.
- Moved current-line operations from the `o` namespace into the operator
  family as `dd`, `cc`, and `yy`; reduced `o` to open-line helpers.
- Moved paste from `v` to `p` and block selection from `h` to `v`.
- Moved bookmarks from `n` to `m`, with bookmark setup on `0m` and movement
  history on `0n`.
- Expanded the alphabetical command summary with Vim key and function
  equivalents.

### Deprecated

### Removed
- AI Studio React/Vite scaffolding: `package.json`, `index.html`,
  `vite.config.ts`, `tsconfig.json`, `.env.example`, and `metadata.json`.

### Fixed
- zx mode now honors the SPEC §9.2 trigger-ambiguity escape: pressing Enter
  immediately after the "zx" trigger inserts the literal trigger text ("zx")
  and returns to Ed mode. Previously Enter was a no-op in zx mode, so an
  accidental trigger could not be recovered as literal text.
- Input decoder no longer jams on unrecognized escape sequences. The CSI parser
  now reads a full sequence (params/intermediates/final) and consumes it even
  when unrecognized, instead of returning "incomplete" forever. It also decodes
  Shift/Ctrl modifiers (`ESC [ 1 ; mod <letter>` and `ESC [ N ; mod ~`), which
  previously hung the editor (e.g. Shift+Arrow) and swallowed later keys.

### Security
