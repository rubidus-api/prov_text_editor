# RFC-0021 — Color theme & palette (16-color; color only for syntax; monochrome base UI)

Status: **Implemented (2026-06-25).** Cell `fg` + SGR emission (posix/win32),
built-in `prov_dark`/`prov_light`/`mono`, `*.theme.ini` reader (multi-theme,
`extends`, color/attr grammar), the `[ui] theme=` key with `NO_COLOR`/`mono`
fallback, and session resolution (re-resolved on `zc`). The monochrome-base-UI
policy is enforced (only the highlighter writes `fg`) and guarded by a test
(`tests/test_render_hl.c`). Example theme files ship under `themes/`. (RFC-0022
is the consumer that paints buffer text.)

## Problem

prov renders monochrome: `prov_cell_t` is `{ cp, attr }` with style *attributes*
only (`REVERSE/DIM/UNDERLINE/MATCH/BOLD`) and no foreground/background color. To
support syntax highlighting (RFC-0022) we need a **color model + theme**. But prov
should keep its calm, minimal look: **the base UI must stay monochrome** — black /
white plus one or two grays, with reverse for emphasis — and **color is reserved
exclusively for syntax highlighting** of buffer text. This RFC defines the color
model, the palette/theme, the terminal emission, and that **policy**.

## Goals

- Add `fg`/`bg` color to the cell, mapped through a **theme** to a **16-color**
  terminal (the portable floor), using foreground and background *judiciously*.
- A hard **policy**: chrome (status bars, panels, scrollbars, gutter, splash,
  selection, search, field mode, …) is **monochrome** — default + 1–3 grays +
  reverse — and **never** emits palette color. Only **syntax token classes** do.
- Restrained, legible built-in themes (`dark`, `light`) + a fully monochrome
  `mono` theme; honor `NO_COLOR`; degrade gracefully on color-less terminals.
- Backward-visual-identity: with no theme color set, output is byte-for-attr
  identical to today's monochrome rendering.

## Non-goals

- Truecolor / 256-color as a baseline (optional future; 16-color is the floor).
- Colorful chrome / "theme everything" (deliberately rejected — see Policy).
- Per-element user theming of the UI (UI stays mono by design, not by config).

## The color model

### Cell fields

```c
typedef struct {
    proven_u32 cp;     /* codepoint */
    proven_u8  attr;   /* PROV_ATTR_* (REVERSE/DIM/UNDERLINE/MATCH/BOLD) — unchanged */
    proven_u8  fg;     /* token-class index; 0 = default (terminal default fg) */
    proven_u8  bg;     /* token-class index; 0 = default (terminal default bg) */
} prov_cell_t;
```

`fg`/`bg` are **token-class indices**, *not* raw colors — the active theme maps a
class to an actual 16-color value. Index `0` = "default" = the terminal's own
fg/bg, which is exactly today's monochrome look. **Default-initialized cells
(`fg=bg=0`) reproduce current rendering**, so the change is invisible until a theme
assigns color.

### Token classes (palette keys)

`default keyword type string comment number function constant operator
preprocessor punctuation builtin error key value` — the classes RFC-0022 emits
(`key`/`value` are for config/data formats: JSON/TOML/YAML key names + markup
attributes use `key`, scalar values use `value`). A theme is a table:
`class → { fg: ansi|default, bg: ansi|default, attr_add: bits }`.

### 16-color space

ANSI 8 normal (`30–37` fg / `40–47` bg) + 8 bright (`90–97` / `100–107`).
**Foreground carries the meaning; background is used sparingly** (e.g. `error`
may use a red background; most classes set fg only on the default bg). Themes are
designed for contrast on both dark and light terminals (two built-ins).

## How many colors? — semantic color pools

You do **not** need 16 colors, and using many *hurts* readability. On a black
background with light text, the comfortably-distinguishable foregrounds are about
**7–8 hues**: gray (`90`, bright-black), red, green, yellow, blue (use bright
`94` — plain `34` is too dark on black), magenta, cyan, white. The *bright* variants
add **intensity, not new hues**; with the `DIM`/`BOLD` attrs you get a few extra
*perceived* steps, but the practical ceiling is ~8 distinct meanings.

So the design is a small **semantic color pool** of language-independent **roles**;
each file type's grammar maps its tokens onto the *same* roles, so colors stay
consistent across languages and the whole scheme needs only ~7 colors:

| Role | Intent | Default hue (dark) |
|---|---|---|
| `comment` | recede — skip past it | gray (`90`) + DIM |
| `string` | literal text (incl. char) | green (`32`) |
| `keyword` | control / reserved words | bright-blue (`94`) |
| `type` | types / typedefs / classes | cyan (`36`) |
| `number`/`constant` | literals, ALL_CAPS consts | magenta (`35`) |
| `function` | calls / definitions | yellow (`33`) |
| `preproc`/`meta` | `#…`, decorators, attributes | bright-magenta (`95`) |
| `punctuation`/`operator`/`identifier` | the *mass* of code | **default** (no color) |
| `error` | lexer/limit error | bright-red (`91`) |

**Rule of thumb: 5–7 colored classes is the sweet spot.** Everything else stays
the default light foreground so the screen reads calm.

## Per-language highlighting for readability

Guiding principle: **color the meaning, leave the mass plain.** Identifiers,
operators and brackets are the bulk of the text — keep them the **default fg** so
the screen is quiet; only *semantically distinct* tokens get a hue. The two most
important to separate are the **non-code regions** — `comment` (dim gray, recedes)
and `string` (green) — because the eye uses them to segment the file.

**C** roles (the requested classes): `keyword` (if/for/return…), `identifier`
(default), `type` (`int/char` + harvested `typedef`/`struct`/`*_t`), `preproc`
(`#include`/`#define` — bright-magenta so the directive line stands out),
`number`/`constant` (literals + ALL_CAPS macros), `string`/char, `comment`
(`//`, `/* */` — gray), `function` (`name(` calls / defs — yellow), `punctuation`
(`{} [] () ; ,` — default). Result: a C screen is mostly calm light text, with
comments greyed, strings green, `#…` lines popping, and control words in cool blue.

**Brackets `() [] {}`**: treat as `punctuation` (default/quiet) — do **not** rainbow
them by depth (garish, hurts the calm). To "tell `{`…`}` / `[`…`]` apart" use a
calmer, separate mechanism: an optional **matching-pair highlight** (cursor on a
bracket → briefly emphasize its mate via `REVERSE` or one subtle hue). That is an
editor feature (RFC-0022), *not* a static per-token color, so the resting screen
stays quiet.

Other languages reuse the **same roles**: Python (`@decorator`/`__dunder__` → meta,
`Class` → type, triple-string → string, `#` → comment); JS/TS (template literal →
string, `: type` → type, regex literal → string-ish); OCaml (`Constructor`/module →
type, `(* *)` → comment, `'a` → type). One pool, consistent across file types.

## Policy — color only for syntax; base UI is monochrome

This is the heart of the RFC and is enforced **structurally**, not just by review:

- **Chrome code paths never write `fg`/`bg`.** Status/command/tab bars, window
  labels, scrollbars, the gutter, panels, the splash, help, selection, search
  match, and field mode set **only `attr`** and leave `fg=bg=0`. Therefore they
  render in the terminal's default colors regardless of theme.
- The base UI's "1–3 grays" come from **attributes, not colors**:
  | Shade | Mechanism |
  |---|---|
  | normal | default fg/bg |
  | dim gray | `PROV_ATTR_DIM` (unfocused panes/labels — already used) |
  | bright/white | `PROV_ATTR_BOLD` (titles — already used) |
  | emphasis | `PROV_ATTR_REVERSE` (selection, labels, bars — already used) |
- **Only the buffer-text highlighter (RFC-0022) writes `fg`/`bg`.** It is the sole
  consumer of the palette. (A reviewer/test asserts no chrome path sets fg/bg.)

So prov stays visually calm — a monochrome editor whose *content* gains color only
where syntax highlighting is active. Reverse video remains the base UI's emphasis
tool (the user explicitly allowed it).

## Built-in themes

The default **`prov_dark`** — black background, light foreground, **calm** (most
text is the default light fg; only a few classes are colored; backgrounds unused
except optional `error`):

| Class | fg | attr | note |
|---|---|---|---|
| identifier / operator / punctuation | default | — | the calm mass of the screen |
| comment | gray `90` | `+DIM` | recedes |
| string / char | green `32` | — | classic, easy on the eyes |
| keyword | bright-blue `94` | — | cool, distinct from types |
| type | cyan `36` | — | near keyword hue but separable |
| number / constant | magenta `35` | — | literals, ALL_CAPS macros |
| function | yellow `33` | — | calls / defs, warm |
| preproc / meta | bright-magenta `95` | — | `#…` / decorator lines pop |
| error | bright-red `91` | — | (bg `41` only if a theme opts in) |

Also shipped:
- **`prov_light`**: same roles remapped for a light terminal (darker fgs).
- **`mono`**: every class = default fg + an *attribute* only (keyword `+BOLD`,
  comment `+DIM`, string default, …) — **fully colorless**; the automatic fallback
  for `NO_COLOR` / colorless terminals.

Built-ins are the fallback; user theme files (below) can override or add themes.

## External theme files (`*.theme.ini`)

Themes are data, loadable from files so users restyle without recompiling.

### Location (reuses the config-path resolution)
The same resolution prov uses for `provconf.ini` (portable exe-side first, else the
home config dir):
- **home config dir**: `~/.prov/` (POSIX `$HOME`; **Windows** `%USERPROFILE%` —
  see the separate config-path fix), e.g. `~/.prov/solarized.theme.ini`;
- **next to the executable** (portable): `<exe-dir>/*.theme.ini`.

Any file matching **`*.theme.ini`** in those locations is scanned.

### Format (INI; prov already parses this dialect)
A file holds **one or more** themes, each an INI section; keys are token classes:

```ini
# ~/.prov/mytheme.theme.ini  — may define several themes in one file
[theme_prov_dark]            # section prefix `theme_` (or a bare [name])
background = dark
comment    = gray +dim
string     = green
keyword    = brightblue
type       = cyan
number     = magenta
function   = yellow
preproc    = brightmagenta
error      = brightred
# identifier/operator/punctuation left unset = default (calm)

[theme_midnight]             # a second theme in the same file
background = dark
extends    = prov_dark       # inherit, then override a few
keyword    = brightcyan
string     = brightgreen
```

- **Color names**: `default`, the 8 base (`black red green yellow blue magenta cyan
  white`) and 8 bright (`brightblack`/`gray`, `brightred`, … `brightwhite`).
- **Spec grammar**: `class = <fg> [on <bg>] [+bold] [+dim] [+underline]`
  (e.g. `error = white on red`, `comment = gray +dim`).
- **Meta keys**: `background = dark|light`, `extends = <theme>` (inherit then
  override). Unset classes fall back to `extends`, then the built-in, then default.

### Selecting & editing via `zc`
`provconf.ini` carries `theme = <name>`. **`zc`** opens `provconf.ini` (the existing
live-apply path, RFC-0017); set `theme = midnight`, save, and prov re-resolves: it
scans the `*.theme.ini` files, indexes their section names, and applies the match
(or falls back to `prov_dark`/`mono` if missing). The theme *files* are edited like
any file (open via `zo`); `zc` governs *which* theme is active.

Selection: `zc theme=dark|light|mono`. Default chosen from a `background=dark|light`
hint (config) or assume dark.

### Canonical reference (for the implementation)

**Token-class keys** (the theme map keys; aliases accepted): `default`, `keyword`,
`type`, `string`, `comment`, `number`, `constant`, `function`, `operator`,
`preproc` (alias `preprocessor`), `punctuation` (alias `punct`), `builtin`,
`error`, `key`, `value`.

**Color names → ANSI index** (the value stored; SGR mapping is RFC-0022's job):

| name | idx | name | idx |
|---|---|---|---|
| `default` | −1 | `brightblack`/`gray` | 8 |
| `black` | 0 | `brightred` | 9 |
| `red` | 1 | `brightgreen` | 10 |
| `green` | 2 | `brightyellow` | 11 |
| `yellow` | 3 | `brightblue` | 12 |
| `blue` | 4 | `brightmagenta` | 13 |
| `magenta` | 5 | `brightcyan` | 14 |
| `cyan` | 6 | `brightwhite` | 15 |
| `white` | 7 | | |

**Resolution order** for `theme=<name>`: a `[theme_<name>]` (or `[<name>]`) section
in any `*.theme.ini` → a built-in of that name (`prov_dark`/`prov_light`/`mono`) →
the default `prov_dark`. `NO_COLOR` / `colors=off` forces `mono`.

**Tolerance**: unknown sections/keys and malformed color specs are ignored (that
class keeps its inherited/built-in/default value); a bad theme file never breaks
the editor — it degrades to the built-in.

## Implementation status — DONE

All of RFC-0021 is implemented:

- **Color model** (`src/theme.{h,c}`): token-class enum, `prov_color_t {fg,bg,attr}`,
  `prov_theme_t`; built-in `prov_dark`/`prov_light`/`mono`; the `*.theme.ini` reader
  (multi-theme files, `extends`, `class = <fg> [on <bg>] [+attr]`); the
  `[ui] theme=` config key with `NO_COLOR`/`mono` fallback; session resolution,
  re-resolved on `zc` apply.
- **Rendering** (`src/display.h`, `platform/platform_term_*.c`): cell `fg` (biased,
  0 = default so zero-init is safe), SGR foreground emit/reset on both backends.
  (`bg` per-class was specified but is not emitted in the first cut — only `error`
  would use it; can be added with no interface change.)
- **Policy**: only the highlighter writes cell `fg`; chrome stays monochrome —
  guarded by `tests/test_render_hl.c` (a plain render leaves every cell `fg=0`).
- **Examples**: `themes/solarized.theme.ini`, `themes/vivid.theme.ini`.

RFC-0022 is the consumer that maps token classes to per-cell `fg`.

## Terminal capability & NO_COLOR

- **Assume 16-color** (the portable floor); emit standard SGR.
- Honor the **`NO_COLOR`** convention and a `colors=off` config → force the `mono`
  theme (attrs only).
- A dumb/colorless terminal degrades to `mono` automatically; nothing breaks.
- 256/truecolor are a **future** extension behind the same theme interface.

## SGR emission (posix / win32 backends)

The diff-based renderer already emits attribute SGRs per run; extend it to emit
**foreground/background** when the resolved color changes, and reset at run
boundaries:

- fg: `\x1b[3Nm` (normal) / `\x1b[9Nm` (bright); reset `\x1b[39m`.
- bg: `\x1b[4Nm` / `\x1b[10Nm`; reset `\x1b[49m`.
- Compose with existing attr SGRs; only emit on change (keep the output small).
- **Reverse compositing**: when `PROV_ATTR_REVERSE` is set (selection/label), the
  terminal swaps fg/bg — so a colored token under selection stays visible. Attrs
  are orthogonal to `fg`/`bg`; no special-casing needed beyond ordering the SGRs.

## prov integration

- Extend `prov_cell_t` (`fg`,`bg`) and the renderer's SGR writer.
- A small **theme table** (token class → color/attr) + `dark`/`light`/`mono`
  built-ins; `zc theme=…`, `NO_COLOR`/`colors=off`.
- RFC-0022's highlighter sets per-cell `fg`/`bg` for visible buffer lines; the
  theme resolves them at emit time.
- **Audit**: chrome render paths reviewed/tested to never set `fg`/`bg`.

## Implementation plan (phased; each: build + tests + ASan/UBSan)

- **P0 — cell `fg`/`bg` + SGR.** Add the fields (default 0), extend the posix/win32
  SGR writer (fg/bg, reset, reverse compositing). With everything `0`, rendering is
  visually unchanged. (Pure groundwork — no theme/highlighter yet.)
- **P1 — theme table + built-ins.** `prov_dark`/`prov_light`/`mono`, class→color
  mapping, `theme=` config key; `NO_COLOR`/`colors=off` → `mono`.
- **P2 — external `*.theme.ini` loading.** Scan the config-path locations (home +
  exe-side), parse INI sections (multiple themes per file, `extends`, color/attr
  grammar), index section names, resolve `theme=` against them; `zc` selects.
- **P3 — policy guard.** A test/assert that chrome paths leave `fg=bg=0`; document
  the rule in SPEC.
- **P4 — docs/themes polish.** Tune the default palettes for legibility on common
  terminals; ship a couple of example `*.theme.ini` files.

(RFC-0022 builds on P0–P1; it is the first consumer.)

## Testing

- **Golden SGR**: given cells with class colors + attrs, the backend emits the
  expected SGR sequence (incl. fg/bg change minimization and reverse compositing).
- **Identity**: with all `fg=bg=0`, output equals the pre-RFC monochrome rendering
  (byte-for-attr identical).
- **Policy**: render a screen with panels/selection/search/splash and assert **no
  cell from a chrome path has `fg!=0 || bg!=0`** (color confined to buffer text).
- **NO_COLOR / mono**: forces attrs-only; no color SGR emitted.

## Risks & mitigations

- **Garish / over-coloring.** *Mitigation:* restrained 16-color palettes, fg-first,
  bg only for `error`; and the hard policy that chrome never colors.
- **Terminal color variance.** *Mitigation:* 16-color floor + `mono` fallback +
  `NO_COLOR`. Reverse (already relied upon) remains the UI emphasis.
- **Visual regression in existing UI.** *Mitigation:* `fg`/`bg` default to 0 →
  identical monochrome rendering; chrome paths never set them.

## Relationship to other RFCs

- **RFC-0022 (syntax highlighting)** is the *only* consumer of this color model and
  depends on it (its P0 = this RFC).
- **RFC-0020 (scripting)** is unrelated; a future psl could *select/define* themes,
  but theme data stays the model defined here.

## Future work

- 256-color / truecolor themes behind the same class→color interface.
- A future semantic-token overlay (if an LSP layer lands) reusing the same palette.
