# RFC-0022 — Lightweight, per-language syntax highlighting (heuristic, anchor-resync, cheap)

Status: **Implemented (2026-06-25).** Engine + render integration (cell fg, SGR,
per-byte fg map, hard caps with plain fallback) and the **line-state cache** with
anchor-seeded cold scans + buffer-revision invalidation (§"Core idea") are done.
Packs landed (18): **markdown, c, python, javascript, typescript, shell, json,
toml** (+ini/cfg/conf/.env)**, yaml, css, rust, go, java, kotlin, swift, lua, sql,
markup** (html/xml). Most share a generic C-like lexer; markdown/python/json/toml/
yaml/css/markup are dedicated. The only deferred items are the documented
"extended support" tier (php, perl, ruby, clojure, lisp, haskell, tcl, build
systems) — each is a future data pack; until added, those files render as plain
text (never an error).

## Problem

prov has no syntax highlighting. We want **per-file-format** highlighting that is
*good enough, not perfect*, and **cheap** (compute + memory) — deliberately **not**
a full parser and **not** tree-sitter (a vendored dependency). The strategy is to
exploit each language's **structural cues** ("global state" markers, anchors, and
identifier conventions) to **bound the work** and cover ~90% of what the eye needs
with a small, line-oriented, regex-driven engine over prov's existing regex engine
(RFC-0009).

This RFC formalizes the "요령" (clever corner-cutting): line-carry state, cheap
**resync anchors** to narrow scope, visible-window-only scanning, per-buffer
harvested symbol sets, and hard caps with graceful degradation.

## Goals

- Practical highlighting for common formats (C, C++, Python, JS, TS, OCaml, shell,
  Make, JSON, Markdown, …) via small **data rule packs** + one generic engine.
- **Cheap**: per-frame cost ≈ *visible lines*, not file size. No full-file parse.
- **No dependency**: reuse prov's regex engine; hand-rolled; deterministic; bounded.
- **Imperfect-but-honest**: accept rare mis-highlights; never stall or crash;
  degrade to plain text under budget.

## Non-goals

- Correct parsing, semantic/type awareness, cross-file resolution (that is the
  tags / tree-sitter / LSP layers — see RFC-0020 discussion). Pure *lexical*.
- 100% accuracy on adversarial corners (JS regex-vs-division, nested macros, etc.).

## Prerequisite — the color model (RFC-0021)

This RFC **depends on RFC-0021 (color theme & palette)**, which adds the cell
foreground/background fields (`fg`/`bg` palette indices, 0 = default), the
token-class palette, the SGR emission in the posix/win32 backends, and the policy
that **only syntax highlighting may use color** (the base UI stays monochrome +
grays + reverse). This RFC is RFC-0021's first and only consumer: it computes, per
visible cell, the **token class** whose color RFC-0021's active theme renders.
Token classes (the palette keys): `default keyword type string comment number
function constant operator preprocessor punctuation builtin error`.

## Core idea — lexical engine with line-carry state + resync anchors

### 1. Per-line carry state (the "global state between lines")

Highlighting line *N* depends only on a tiny **state at the start of the line**:

```c
typedef struct {
    uint8_t  kind;    /* NORMAL, BLOCK_COMMENT, STRING, HEREDOC, ... */
    uint8_t  depth;   /* nesting (e.g. OCaml (* (* *) *)) */
    uint32_t param;   /* the open delimiter: string quote char, or heredoc tag id */
} hl_state_t;
```

`end_state = scan_line(line, start_state, rules)` is a **pure function**: it walks
the line, emits colored spans, and returns the state to carry to the next line.
Multi-line constructs (block comments, triple-/template-strings, here-docs) live
*entirely* in this carry state — so a line in the middle of a block comment is
correct given only its start state, no re-parse of the whole file.

### 2. A line-state cache + early-stop incrementality

Each buffer keeps an array of cached **end-states** parallel to its lines, plus a
`valid_through` line marker. On an edit at line *L*:
- invalidate states `>= L`;
- when rendering, recompute states downward from *L*, and **stop as soon as a
  recomputed end-state equals the previously cached one** (the carry *converged*) —
  in practice 1–2 lines for ordinary edits. No full rescan.

### 3. Render only the visible window, seeded by an anchor

To draw the viewport `[top, bot]` we only need `state[top]`:
- if cached (`valid_through >= top`), use it (the common case);
- if **cold** (e.g. a freshly opened buffer, or a big jump), seed by scanning down
  from the **nearest resync anchor above `top`**, bounded by a **lookback cap**
  (e.g. 1000 lines). An *anchor* is a line the rule pack declares **definitely
  NORMAL** regardless of history. If no anchor is found within the cap, assume
  NORMAL and accept a rare transient mis-highlight (corrected as the cache fills).

So per-frame cost is `O(visible lines)` plus a bounded one-time seed — **never the
file size.**

### 4. Resync anchors — the key trick to bound scope

Anchors are per-language regexes that strongly imply `NORMAL` at line start. They
let the engine *start scanning near the viewport* instead of the file top. Examples:

| Lang | Anchor (start-of-line ⇒ almost surely NORMAL) |
|---|---|
| C/C++ | `^\}` (col-0 closing brace), `^#` (preprocessor line), `^[A-Za-z_][\w \t\*]*\b\w+\s*\(` (top-level def) |
| Python | `^(def|class|@|import|from)\b` at column 0 (not inside a triple-string in practice) |
| JS/TS | `^(function|class|export|import|const|let)\b` at column 0 |
| OCaml | `^(let|module|type|open)\b` at column 0 |
| shell | `^(function\s+\w+|\w+\s*\(\)\s*\{|[A-Za-z_]\w*=)` at column 0 |

Anchors are heuristic: a false anchor causes at most a brief local mis-color until
the real cache catches up. That is an accepted, bounded error.

## Rule pack schema (data, one generic engine)

A language is **data**, interpreted by the single engine. Initially C tables;
later loadable from config / psl (RFC-0020) so users add a language without
recompiling.

```c
typedef struct {
    const char *name;                 /* "c", "python", ... */
    const char *file_match;           /* extensions / shebang / modeline regex */
    const char *const *keywords;      /* sorted set → binary search */
    const char *const *builtins;      /* builtins / known types */
    hl_rule_t   tokens[];             /* ordered regex → class (number, op, ...) */
    hl_region_t regions[];            /* multi-line: {start, end, class, nestable, param?} */
    const char *anchors[];            /* NORMAL-implying line regexes */
    hl_ident_t  idents;               /* identifier-kind heuristics (below) */
    hl_harvest_t harvest[];           /* per-buffer symbol harvesting (below) */
} hl_lang_t;
```

`scan_line` algorithm: if `start_state.kind != NORMAL`, try to **close the active
region** (its `end` pattern; adjust `depth` for nestable); whatever is consumed is
colored with the region's class. In `NORMAL`, at each position try, in order:
a **region `start`** (→ enter region, may end the line in that state), a **comment**,
a **string**, a **number**, a **keyword/builtin** (word match + set lookup), an
**identifier** (→ ident heuristics), else **operator/punctuation**. Advance; repeat
to end of line; return the carry state.

## Identifier "tricks" — kinds without a type system

Cheap, convention + local-context heuristics (deliberately imperfect):

| Kind | Trick |
|---|---|
| **function call** | identifier immediately followed by `(` → `function` |
| **function/def** | anchored line pattern per language (`^\s*def (\w+)`, C def regex, `function (\w+)`, `(\w+)\s*=>`) |
| **type (heuristic)** | C: `\w+_t` or a **harvested** `typedef`/`struct` name; typed langs: token after `:` or a `[A-Z]\w*` (CamelCase convention); Python: `[A-Z]\w*` ⇒ class |
| **constant** | `^[A-Z][A-Z0-9_]+$` (ALL_CAPS) ⇒ `constant` |
| **definition vs use** | identifier right after `def`/`class`/`fn`/`let`/`const`/`var`/`struct`/`type` ⇒ definition emphasis |

### Per-buffer harvested symbol sets (the strongest cheap trick)

One linear pass (on open; refreshed on save, incrementally) collects declarations
into small per-buffer sets, then tokens are colored by **membership** — no parser,
but it picks up *project-specific* names:

| Lang | Harvest patterns → set |
|---|---|
| C/C++ | `typedef\b.*\b(\w+)\s*;`, `\bstruct\s+(\w+)`, `\benum\s+(\w+)` → **types**; the C def regex → **functions** |
| Python | `^\s*class\s+(\w+)` → types; `^\s*def\s+(\w+)` → functions |
| JS/TS | `\bclass\s+(\w+)`, `\b(?:function|const)\s+(\w+)` |

Membership lookup is a `proven_map`/sorted set — O(log n) per token, cheap.

## Per-language tips (concrete "팁")

- **C/C++**: `#` at col 0 = whole-line preprocessor. `/* */` = block-comment carry.
  Strings line-local (except trailing `\` continuation). `}` at col 0 = resync
  anchor. `\w+_t` + harvested typedef/struct/enum names = types. `\w+(` = call.
- **Python**: the one hard state is `"""`/`'''` **triple-string carry** (kind=STRING,
  param=quote, until the matching triple). `#` line comment. f-strings: optionally
  re-highlight `{...}` inside. `^def|class|@|import` = anchors / def detection.
  `[A-Z]\w*` ⇒ class, ALL_CAPS ⇒ const.
- **JS/TS**: template literals `` `…` `` = multi-line carry with `${…}` interpolation.
  `//`, `/* */`. **regex-vs-division**: a `/` is a regex literal only after
  `(`,`,`,`=`,`return`,`:`,`[`,`{`,`;`,`!`,`&`,`|`,`?` or line start — else division.
  Heuristic; accept rare misses. TS: token after `:` ⇒ type.
- **OCaml**: `(* … *)` comments are **nestable** → carry `depth` (increment on `(*`,
  decrement on `*)`; NORMAL when 0). `"…"` strings; `[A-Z]\w*` ⇒ constructor/module;
  `'a` ⇒ type var.
- **shell / here-docs**: `<<EOF` opens a **here-doc carry** whose `param` stores the
  delimiter id; stays in HEREDOC until a line equals the delimiter. `#` comments.
- **Make / JSON / Markdown**: line-oriented; Markdown fenced code ```` ``` ```` is a
  region carry (and may delegate the fenced body to another rule pack — optional).

## Supported file types & extensions

The language set is **derived from the editor's existing default file-type list**,
`BROWSE_POSTFIX_DEFAULT` (the `zo` open-panel `f` filter default, in `src/main.c`)
— that constant stays the source of truth, and this RFC ships a rule pack for each
format in it. The current default list is:

```
.txt .md .markdown .rst .html .htm .css .js .ts .json .xml .yml .yaml .toml
.ini .cfg .conf .c .h .cpp .hpp .cc .hh .py .sh .rs .go .java .lua .sql .log .csv .tex
```

Mapping extension → rule pack (one pack may serve several extensions; packs are
*merged* only when their lexical grammar is effectively the same):

| Rule pack | Extensions / names |
|---|---|
| `c` | `.c` `.h` |
| `cpp` | `.cpp` `.hpp` `.cc` `.hh` `.cxx` `.hxx` (`.h` resolves to `c`) |
| `python` | `.py` |
| `javascript` | `.js` `.jsx` |
| `typescript` | `.ts` `.tsx` |
| `rust` | `.rs` |
| `go` | `.go` |
| `java` | `.java` |
| `kotlin` | `.kt` `.kts` |
| `swift` | `.swift` |
| `lua` | `.lua` |
| `ocaml` | `.ml` `.mli` |
| `shell` | `.sh` `.bash` `.zsh` (+ `#!` shebang) |
| `sql` | `.sql` — **its own pack** (SQL keywords, `--` / `/* */` comments); *not* shell |
| `make` | `.mk`, and the basenames `Makefile` / `makefile` |
| `json` | `.json` |
| `toml` | `.toml` `.ini` `.cfg` `.conf`, and `.env` / `.env.*` — **one key=value pack** |
| `yaml` | `.yml` `.yaml` |
| `markup` (XML/HTML) | `.html` `.htm` `.xml` |
| `css` | `.css` |
| `markdown` | `.md` `.markdown` |
| `rst` | `.rst` |
| `latex` | `.tex` |
| `csv` | `.csv` (separator-only highlighting) |
| `text` (none) | `.txt` `.log` (plain; comment/url heuristics at most) |

Grouping decisions (per review):
- **INI merges into `toml`.** TOML and INI/conf share the lexical shape
  (`[section]`, `key = value`, strings, numbers, booleans) and TOML is the richer
  superset, so one pack serves `.toml .ini .cfg .conf`. The pack accepts **both
  `#` and `;`** as line comments (TOML uses `#`, classic INI also uses `;`).
- **`.env` (dotenv) goes in the `toml`/key=value pack** — it is `KEY=value` with
  `#` comments (no sections); the same key/value/string/comment rules cover it
  (treat a leading `export` as a keyword). `.env` is matched by **basename**
  (`.env`, `.env.local`, …), not by a suffix.
- **SQL stays independent** (it is a real language, unrelated to shell). The table
  previously listed `shell` and `sql` on one line for layout only — they are
  separate packs.

Notes:
- **`.h` ambiguity**: defaults to `c` (cheap, correct for C; a near-superset for
  casual C++ headers). A modeline / manual `filetype` override picks `cpp`.
- **Detection**: extension **or basename** (`Makefile`, `.env`) → shebang (`#!`)
  for extensionless scripts → `vim`/`prov` modeline → else `text` (no highlighting).
- **Phasing**: P2 lands `c` + `python`; P4 adds `javascript`/`typescript`/`shell`/
  `json`/`toml`(incl. ini/env)/`yaml`/`markdown`/`css`/`markup`; the rest (`rust`
  `go` `java` `kotlin` `swift` `lua` `ocaml` `sql` `make` `rst` `latex` `csv`)
  follow as small data packs. Any format without a pack falls back to `text`
  (plain) — never an error.
- **The table above is the planned (implemented) ceiling for the first cut.**
  Everything else is in "Extended support" below — listed and reserved, but not
  built initially (so the engine and binary stay lean).
- **Sync point**: `BROWSE_POSTFIX_DEFAULT` (the `zo` filter default) should gain the
  new *suffix* additions (`.jsx .tsx .cxx .hxx .kt .kts .swift .ml .mli .bash .zsh
  .mk`) when this ships; the **basename** matches (`Makefile`, `.env*`) are handled
  by the detector, not the suffix filter.

### Extended support (documented now; implemented later)

These are **reserved and documented but not in the first cut** — each is just a
future data pack (+ a detector entry). Until added, the file opens as `text`
(plain), never an error. Detection uses the suffix *or* the noted basename.

| Pack | Extensions / names | Note |
|---|---|---|
| `php` | `.php` `.phtml` | widely used; deferred for embedded HTML (`<?php ?>` injection) — first version would highlight whole-file as PHP |
| `perl` | `.pl` `.pm` | |
| `ruby` | `.rb` `.rake`, `Rakefile`, `Gemfile` | |
| `clojure` | `.clj` `.cljs` `.cljc` `.edn` | a Lisp dialect — s-expr highlighting is cheap when added |
| `lisp` | `.lisp` `.lsp` `.cl` `.scm` `.ss` `.el` | Common Lisp / Scheme / Emacs Lisp — one s-expr pack |
| `haskell` | `.hs` `.lhs` | layout-sensitive; lexical highlighting (comments `--`/`{- -}`, strings, types) is fine |
| `tcl` | `.tcl` `.tk` `.itcl` | Tcl/Tk — command-word + `$var` + `#` comment |
| `cmake` | `.cmake`, `CMakeLists.txt` | build system (basename) |
| `meson` | `meson.build`, `meson_options.txt` | build system (basename) |
| `ninja` | `.ninja` | build system |
| `bazel` | `.bzl`, `BUILD`, `BUILD.bazel`, `WORKSPACE` | build system (basename) |
| `gradle` | `.gradle` `.gradle.kts` | build system (Groovy/Kotlin-flavored) |
| `dockerfile` | `Dockerfile`, `.dockerfile` | devops-adjacent (basename) |

More languages can be added the same way on request — a new one is only an
extension/basename entry plus a small data pack, so this set can grow cheaply
without enlarging the first-cut implementation.

## Cheap-coverage strategy & graceful degradation

- **Visible window only** + cached states + anchor-seeded cold scans (above).
- **Hard caps** (configurable via `zc`); on breach, color the rest **plain** rather
  than stall:

  | Cap | Default |
  |---|---|
  | cold-scan lookback | 1000 lines |
  | tokens per line | 4000 |
  | regex steps per line | budgeted (RFC-0009 already bounded) |
  | per-frame highlight time | a few ms; over budget ⇒ remaining lines plain this frame, finished next frame |
  | harvest pass cap | e.g. 200k lines; bigger ⇒ skip harvest (keywords/regex only) |

- **Disable on huge/binary**: files over a size/line cap, or binary buffers, get no
  highlighting (plain) — consistent with the hex/binary mode.

## prov integration

- Hook the highlighter where the renderer fills the cell grid (`prov_render_into*`):
  the engine returns per-cell `fg` for the visible lines; selection/search/field
  attrs compose on top (attr bits are orthogonal to `fg`).
- Buffer gains a **line-state cache** + **harvested sets**, invalidated on edit
  (from the edited line) / refreshed on save.
- **Language detection**: by extension, then shebang (`#!`), then a `vim`/`prov`
  modeline; manual override via a command (e.g. `zc filetype=python`).
- Reuse the **regex engine** for all token/anchor/region/harvest patterns.
- **Theme**: a small palette table (token class → terminal color); a couple of
  built-in themes; later user-themable via config.

## Accuracy & honest limits

Lexical heuristics will mis-color some corners — JS regex/division, unusual macro
soup, exotic string forms, a false anchor mid-construct. All are **bounded and
local** (self-correct as the cache fills) and **never** cause a stall or crash. We
accept this in exchange for ~zero-dependency, file-size-independent cost. Users who
need correctness use the (future, optional) tree-sitter or LSP layers.

## Implementation plan (phased; each: build + tests + ASan/UBSan)

- **P0 — color model (RFC-0021).** Land RFC-0021 first: cell `fg`/`bg`, palette,
  themes, SGR emission, the monochrome-base-UI policy. (No language logic yet.)
- **P1 — engine core.** `hl_state_t`, `scan_line`, line-state cache + early-stop,
  visible-window render hook, caps/budget, graceful degrade.
- **P2 — rule pack format + C/Python packs.** keywords/tokens/regions/anchors +
  triple-string & block-comment carry. Language detection.
- **P3 — identifier heuristics + per-buffer harvest** (types/functions membership).
- **P4 — more packs** (JS/TS template literals + regex-vs-division, OCaml nestable
  comments, shell here-docs, Make/JSON/Markdown).
- **P5 — themes + config** (`zc filetype`, theme select, caps).
- **P6 — (optional) rule packs loadable from psl/config** (RFC-0020) so users add a
  language without recompiling.

Gated behind a compile flag / config so a minimal build omits it.

## Testing

- **Golden span tests**: input source + expected (offset,len,class) spans per
  language (incl. the multi-line carry cases: block comment, triple string,
  template literal, nestable OCaml comment, here-doc).
- **Incremental correctness**: edit a line → re-highlight equals a from-scratch
  highlight (cache + early-stop converges to the truth).
- **Anchor seeding**: rendering a viewport deep in a file (cold cache) matches the
  full-file result, within the lookback cap.
- **Budget/degrade**: pathological lines / huge files → bounded time, remaining
  plain, no crash. **Fuzz** the engine (never crash on arbitrary bytes).

## Risks & mitigations

- **Accuracy complaints.** *Mitigation:* set expectations (lexical, ~90%), keep
  corners local/self-correcting, leave the door to tree-sitter/LSP for correctness.
- **Per-language rule sprawl.** *Mitigation:* rule packs are data (one engine); a
  new language is a table, not code; later loadable from config/psl.
- **Cost.** *Mitigation:* visible-window + cache + anchors + caps make it
  file-size-independent and bounded per frame (prov's determinism preserved).
- **Color regression in existing UI.** *Mitigation:* `fg` is orthogonal to attr
  bits; default `fg=0` reproduces today's monochrome rendering exactly.

## Alternatives considered

- **tree-sitter** — accurate, incremental, error-tolerant, but a **vendored C
  dependency + per-language grammar blobs** (RFC-0020 discussion). Higher quality,
  bigger footprint; left as an optional future backend behind the same theme/cell
  layer.
- **Full hand-written parser per language** — accurate but expensive to write and
  to run; overkill for highlighting.
- **Per-line regex with no carry state** — trivial but breaks on every multi-line
  construct (the common, visible failure). Rejected.

## Future work

- Optional **tree-sitter backend** behind the same cell-`fg`/theme interface (swap
  the lexical engine for a syntactic one where the dependency is acceptable).
- Rule packs authored in **psl** (RFC-0020) for no-recompile language support.
- 256-color / truecolor themes; semantic-token overlay if an LSP layer lands.
