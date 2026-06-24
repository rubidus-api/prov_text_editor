# RFC 0008 — Milestone 4 roadmap (registers, clipboard, bookmarks, search, macros)

- Status: **Active** (sequencing + key decisions; sub-RFCs/stages below)
- Created: 2026-06-19

## 1. Scope & order

M4 (SPEC §20) adds the `b`/`m`/`e`/`s` top-level namespaces (currently free at
PS_START) and the `0`-series browsers (already parsed, not yet implemented).
Ordered so each piece builds on the last, no-dependency pieces first:

| # | Piece | Namespace | Depends on | External dep |
|---|-------|-----------|-----------|--------------|
| **M4.1** | Paste shapes (linewise `p`/`P`) | — | RFC-0006 | none |
| **M4.2** | Named registers + browser `0b` | `b` | M4.1 (register shape) | none |
| **M4.3** | Bookmarks + overview `0m` | `m` | — | none |
| **M4.4** | OS clipboard sync (unnamed register) | — | M4.2 | xclip/wl-copy/win32 (runtime, optional) |
| **M4.5** | Search / replace + history `0s` | `s` | — | **regex engine (see §2)** |
| **M4.6** | Macros record/replay + overview `0e` | `e`,`q`,`E` | M4.2 (registers) | none |

Each piece: failing test first for pure logic, build+test+commit per stage,
ASan/UBSan + PTY, docs (SPEC/help/CHANGELOG), benchmark under `docs/benchmarks/`
where a hot path appears.

## 2. Decision — search engine (M4.5)

SPEC §2 names PCRE2, but vendoring PCRE2 (~tens of kLOC, libc-heavy) conflicts
with prov's *self-contained, vendored, libc-free-core* ethos and is a large
surface. **Decision: stage it.**

1. **Literal + incremental search first** (M4.5a): exact substring with the
   existing byte/line primitives — `/` (Ed `Ctrl+F`) live-incremental, `n`/`N`
   next/prev, search history register `0s`. Covers the overwhelmingly common
   case with zero dependency.
2. **Small built-in regex** (M4.5b): a self-contained **Pike VM** (Thompson NFA
   + submatch tracking; the RE2 / Go / Rust-regex lineage) over byte ranges —
   linear-time, no ReDoS, capture groups. Lookaround/backrefs are replaced by
   `\zs`/`\ze` markers + `:g`/`:v` line guards rather than backtracking. Full
   plan and stages: **`docs/proposals/rfc-0009-regex-engine.md`**. Revisit
   PEG/Oniguruma only if a real lookaround/backref need appears (file a REPORT).

Replace (`s` namespace `:s/old/new/`) lands on top of whichever matcher.

## 3. Register model (M4.2)

Generalize the single `ed->reg` into a small **register file**:
- The **unnamed** register (current behavior: `yy`/`dd`/`x`/`p`, `Ctrl+C/X/V`).
- **Named** registers `a`–`z` (`"a yy` style or a prov-idiomatic prefix — decide
  at M4.2: likely `b<letter>` to set the active register for the next op).
- Each register carries `(bytes, shape)` from RFC-0006 (char/line/block).
- `0b` browser lists registers with a preview; selecting pastes.
- The unnamed register is what M4.4 syncs to the OS clipboard.

## 4. Bookmarks (M4.3)

Named marks `m{a-z}` storing `(buffer, byte)`; `'{a-z}` (or a prov key) jumps.
`0m` overview lists marks with nearby text. Marks shift with edits (track like a
cursor, or store line+col and re-resolve). Simplest robust: store an absolute
byte and adjust on edits to that buffer, or re-clamp on jump.

## 5. Macros (M4.6)

`e{a-z}` / `q` record a key sequence into a register-like slot; `[N]E` / `e{a-z}`
replays it N times. Recording captures decoded keys; replay feeds them back
through the input dispatch. Reuses the register storage (M4.2). `0e` overview.

## 6. Status

- [x] **M4.1** paste shapes (RFC-0006 S1–S3): register shape tag + linewise `p`/`P`
      + uppercase parsing. (Block shape S4 waits on visual-block; overwrite S5 later.)
- [~] **M4.2** named registers `b<reg>{x,c,v}` (36 slots) **done**. Remaining: `0b` browser, history auto-population.
- [~] **M4.3** bookmarks: `m<letter>` jump / `0m<letter>` set, buffer-local +
      edit-tracked **done**. Remaining: `0m` overview, `m[0-9]` history.
- [!] M4.4 clipboard sync — **deferred (environment-blocked)**: no xclip/wl-copy/
      display here to verify, and xclip's daemonizing pipe risks an untestable
      hang. Implement+verify on a desktop (popen-based set/get on the unnamed reg).
- [~] **M4.5a** search: `ss`/`sw`/`sn`/`sp` + incremental + match highlight +
      highlight + Esc-clear + `sr` + **`soc` case-insensitive** + `soh` + cache +
      benchmark **done**. Remaining: `sow` whole-word, `0s` history, regex (M4.5b, optional).
- [~] **M4.6** macros `0e<slot>` record / `e<slot>` / `E` replay **done**. Remaining: `0e` overview.
