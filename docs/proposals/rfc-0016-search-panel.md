# RFC-0016 — Search / replace input panel + one-key search

- **Status:** **IMPLEMENTED (2026-06-22).** `PANEL_K_FIND` dialog with pattern +
  replacement `lineedit` fields, the four option toggles, live incremental search,
  match count (`match I / N`), `n/N/r/a` actions, and a one-key `/` entry. PTY +
  ASan/UBSan verified. Phase 4 of the `TODO.md`
  execution roadmap ("search ergonomics"). Builds on RFC-0010 (common modal panel),
  RFC-0015's dialog patterns (options row, `lineedit` fields, compact legends), and
  the existing search engine (`s->search`, `do_search`/`do_replace`, RFC-0009 regex).

## 1. Motivation

Search today is split across the inline command line (`ss` to type a term, live
incremental) and four separate toggle commands the user must remember and issue
*before or after* typing: `sox` regex, `sow` whole-word, `soc` case, `soh`
highlight. Replacement (`sr`) is a second inline prompt with no view of the term
or options. There is no single key to start searching, and no place that shows the
pattern, the options, and the replacement together.

This RFC adds a **dedicated find/replace panel** — one surface that holds the
pattern field, the replacement field, the option toggles (all visible and
mnemonic-keyed), live incremental search, and next/prev/replace/replace-all
actions — plus a **one-key entry** (`/`). The lightweight inline `ss` / `Ctrl+F`
prompt stays for muscle memory; both drive the same `s->search` state.

## 2. Layout (vertical stack, full-width modal panel)

```
┌─ Find ─────────────────────────────────────────── regex ─┐
│ find:    [pattern…………………………………………………]                    │  (1) pattern field
│ replace: [replacement………………………………]                       │  (2) replacement field
│ x:regex   w:word   c:case   h:highlight                   │  (3) options row
│───────────────────────────────────────────────────────── │
│ match 3 / 12                                              │  (4) status / count
│───────────────────────────────────────────────────────── │
│n:next│N:prev│r:replace│a:all│Tab:field│h:help│q/x:close   │  (footer)
└───────────────────────────────────────────────────────── ┘
```

- The title's right tag shows the active modes (`regex`, `word`, `ic`, …) like the
  browser's encoding tag.
- The pattern/replacement fields reuse `src/lineedit.{c,h}` (selection, word
  motion, clipboard, horizontal scroll, underlined editable region, ↑/↓ history).
- The options row reuses the browser's per-segment mnemonic rendering: the shortcut
  letter is bold+underlined, the focused segment is reversed, an enabled toggle is
  shown bright, a disabled one dim.

## 3. Focus & keys

Focus cycles with **Tab / Shift+Tab**: `pattern → replace → regex → word → case →
highlight → pattern`. When focus is on a field, all printable/editing keys go to
the `lineedit`; when on an option, **Enter/Space** toggles it. The immediate verb
keys below fire whenever focus is **not** on a text field (so `n` is "next" on the
options, but a literal `n` while typing the pattern):

| key | action |
|-----|--------|
| `Enter` / `n` | search next (commit: push pattern to history) |
| `N` | search previous |
| `r` | replace the current match, then advance |
| `a` | replace all (whole document) |
| `x` `w` `c` `h` | toggle regex / word / case / highlight (also immediate) |
| `Tab` / `Shift+Tab` | cycle focus |
| `h` (when not editing) | open this panel's help |
| `q` / `x`-on-empty / `Esc` | close; cancel restores the pre-search cursor |

`x` is overloaded: on the options it is reachable as a verb (toggle regex), and the
panel-wide close is `q` (and `Esc`). To avoid the clash, **close is `q` / `Esc`
only** for the find panel (the generic `x` close is dropped here), matching the
"x = a verb (regex)" choice.

Typing in the **pattern** field is **incremental**: each edit re-runs the search
from the saved origin (live preview), exactly like the inline `ss` prompt. Toggling
any option re-runs too. Committing (`Enter`/`n`) pushes the term onto the shared
search-history ring (so `0s` and the inline prompt see it).

## 4. One-key entry

`/` (a top-level command action `PROV_ACT_SEARCH_PANEL`) opens the panel with the
pattern field focused, seeded from the current `s->search.term`. Ed-mode keeps
`Ctrl+F`/`Ctrl+R` (inline). `ss`/`sn`/`sp`/`sr`/`sox`… are unchanged. `?` is left
free for a future "open panel focused for backward search" if wanted.

## 5. State

```c
struct {
    prov_lineedit_t pat,  repl;     /* the two fields                       */
    prov_lehist_t   pathist, replhist; /* ↑/↓ history rings (session)        */
    int             focus;          /* FF_PAT/FF_REPL/FF_REGEX/…/FF_COUNT    */
    proven_size_t   matches, index; /* count + 1-based current (0 = none)    */
} find;
```

Reuses `s->search` (term/regex/word/icase/hl/re/origin/hay) and
`search_cache_begin/end`, `search_recompile`, `search_run`, `do_replace`.

## 6. Match count

When the search re-runs, count matches in the materialized hay (literal: repeated
`prov_search_bytes`; regex: repeated `prov_regex_search`), capped at a sane limit
(e.g. 100000) so a pathological file can't stall the UI, and record the 1-based
index of the match at/after the cursor. Shown as `match I / N` (or `N matches`,
`no match`, `invalid regex: …`).

## 7. Out of scope / future

- Scoped replace (selection / range) — today replace-all is whole-document.
- Multi-file search. (Phase 7 territory.)
- Persisting find/replace history to the config file (session-only for now, same as
  the `zo` type-filter history — a shared follow-up).
