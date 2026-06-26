# RFC-0020 — Scripting: a minimal Lisp for editor macros (tree-walked, arena-scoped)

Status: Draft, revision 2 (2026-06-25). **Supersedes** the previous draft's
three-layer "typed stack VM + Lisp + blocks" design — the Forth VM and its
compiler are dropped in favour of a single tree-walking interpreter. See
"What changed and why" below.

Working names (provisional): the language is **psl** (*prov script language*),
files `*.psl`, codename *loom*. Names are open.

## Problem

prov has a fixed command vocabulary (zx, the `zp` prompt, `zc` config) but no way
for a user to express a custom, repeatable text transformation. The editor needs a
small **scripting system** that is cheap to implement and audit, deterministic and
dependency-free (no GC pauses), expressive enough for real text work yet
approachable, and **wired to what prov already has**: the regex engine (RFC-0009)
and the editor command set.

## What changed and why (vs revision 1)

The first draft layered a Forth typed stack VM (L0) under a Lisp AST (L1) under
block sugar (L2). Review concluded that the VM layer **increases** implementation
cost (a compiler + bytecode + two execution models) for speed an *editor macro*
language does not need — the real work is in C (regex, buffer edits), and the
script just orchestrates a handful of operations. This revision keeps the parts
that earned their keep and removes the rest:

- **Removed:** the stack VM, the Lisp→words compiler, bytecode/threaded execution.
- **Kept & simplified:** a small, homoiconic Lisp **evaluated directly by a
  tree-walker**; the regex bridge; the command bridge; the optional block surface
  (now macros over the core forms); one-undo transactions; bounded execution.
- **Sharpened:** the Lisp is deliberately **trimmed to "editor-macro size"** (not
  a faithful general Lisp), memory is **arena-per-run with hard caps**, and heavy
  loops run in **C builtins**.

## Goals

- A single-layer, tree-walking interpreter that is **small to implement** (on par
  with prov's existing regex engine) and easy to audit.
- First-class **regex** and one-line **editor commands**, both *thin bindings over
  existing code* — no new regex/command logic.
- **No GC.** Memory is a **proven arena per execution**: allocate as you go, free
  the whole arena at once when the run ends. Hard size limits prevent runaway use.
- Heavy work (scanning, find-all, replace-all, split) lives in **C builtins**; the
  Lisp body stays tiny.
- Editor-aware: a run is one **undo transaction**; faults never crash prov.

## Non-goals

- A faithful or general-purpose Lisp. We intentionally omit the numeric tower,
  tail-call optimization, continuations, conditions/restarts, full macro hygiene,
  cons-cell improper lists, and a large stdlib (see "Trimmed Lisp" below).
- A fast VM / bytecode / JIT, async, threads, arbitrary C FFI.
- Live, per-keystroke work over huge files (e.g. whole-file syntax highlighting) —
  that stays in C. psl targets *edit-time* transforms (≤ a few thousand lines).

## Architecture (two conceptual layers, one evaluator)

```
  surface (optional)   block / pattern-action     /re/ { replace $0 up($1) }
        │  desugar (macros)                                  │
        ▼                                                    ▼
  core    homoiconic s-expr Lisp        (rule /re/ (fn (m) (replace (g m 0) (up (g m 1)))))
        │  tree-walk eval  (no VM, no compiler)
        ▼
  prov    regex engine (RFC-0009)  +  editor command words  +  proven arena
```

The block surface is **macros** that expand to core s-expressions, so there is one
reader and one evaluator. Power users may write core forms directly; casual users
never see a parenthesis.

## The core language — a trimmed, text-first Lisp

### Values (dynamically typed, tagged)

```c
typedef enum { PV_NIL, PV_BOOL, PV_INT, PV_REAL,        /* REAL gated: -DPSL_NO_REAL */
               PV_STR, PV_RANGE, PV_MATCH, PV_SEQ,
               PV_FN, PV_BUILTIN, PV_SYM, PV_HANDLE } pv_type_t;

typedef struct pv_cell {
    pv_type_t tag;
    union {
        bool         b;
        int64_t      i;
        double       r;
        pv_str_t     s;        /* {ptr,len,owned?} — a SLICE; borrowed by default */
        pv_range_t   range;    /* buffer region: byte span or line span */
        pv_match_t   match;    /* regex captures (group offsets), from RFC-0009 */
        pv_seq_t    *seq;      /* array-backed sequence; also the AST node type */
        pv_fn_t     *fn;       /* closure: params + body(seq) + captured env */
        pv_builtin_t builtin;  /* C function: pv_cell (*)(pvm*, pv_cell* args, n)  */
        uint32_t     sym;      /* interned symbol id */
        pv_handle_t  handle;   /* buffer / window / tab id, opaque */
    } as;
} pv_cell_t;
```

Two pragmatic, "not-faithful-Lisp" choices that matter:

- **Sequences are array-backed (`PV_SEQ`, a proven array), not cons cells.** This
  is true for both data lists *and* code (the AST is a `PV_SEQ` of cells, so it is
  still homoiconic → macros work), but it avoids millions of tiny cons allocations
  and is cache- and arena-friendly.
- **Strings are slices (`{ptr,len,owned}`), borrowed by default.** Motions and
  regex yield borrowed views into the buffer/source; only `replace`/`fmt`/concat
  materialize arena-owned strings. Fewer copies, less memory.

### Forms that are IN (editor-macro sized)

`def`, `fn` (lambda), `let`, `if`, `cond`, `when`, `unless`, `do`/`begin`,
`while`, `and`, `or`, `set!`, `quote`, and a *small* `defmacro` (mainly to power
the block surface; user macros allowed but not emphasized). Everything else is a
function/builtin call.

```lisp
(def strip-trailing (fn () (replace-all (buffer) /[ \t]+$/ "")))
(def up (fn (s) (upcase s)))
```

### Deliberately OUT
Numeric tower / bignums / rationals (just `i64` + gated `f64`); **tail-call
optimization** (use a recursion-depth cap + `while`/`each` iteration instead);
`call/cc`, dynamic-wind, conditions/restarts; full macro hygiene (simple `gensym`
only); cons/improper lists; packages/modules; reader macros beyond the fixed text
literals; a large standard library.

### Text-first literals
integers/reals; `"strings"` and `r"raw"`; **regex `/pattern/flags`** (a first-class
literal compiled once via RFC-0009); `:keywords`; `symbols`; `(call …)`,
`[seq …]`; `; line comments`.

## Evaluation — a tree-walker

- `eval(cell, env)` recurses over the AST: self-evaluating atoms return
  themselves; a `PV_SEQ` whose head is a special-form symbol runs that form; else
  it is a call (`eval` the head + args, then apply).
- **Environments**: a frame chain; symbols are interned (ids, not strings) and
  frames are array-backed for fast lookup; globals live in the **session
  dictionary** (persistent allocator), locals in the run arena.
- **Recursion uses the C stack**, so a **depth cap** faults before a C stack
  overflow. No TCO; loops cover the iterative cases.

## Memory model — arena per run, scratch arenas on demand, hard caps

This is the core of the revision and follows the requested model exactly.

1. **One arena per execution.** Each top-level run (a `zp` line, a sourced file, a
   pattern-action pass) gets a fresh **proven arena `A0`**. Every transient value
   — sequences, owned strings, closures, match lists — **bump-allocates into `A0`**.
   When the run returns, **`A0` is freed in one shot.** No GC, no per-object
   reclamation, deterministic.
2. **Scratch arenas on demand (region discipline).** A form or C builtin that
   needs a large *intermediate* (e.g. build-then-filter a big list) may open a
   **child arena `A1`**, do the work there, **copy the small result into the parent
   arena, then free `A1`**. So transient bloat never pollutes the run arena. This
   is the "allocate one more arena when needed, process, release" pattern.
3. **Hard caps at the source.** Oversized data is refused *before* it is built, so
   a script can never OOM the editor — it faults instead. All caps are configurable
   (`zc`); defaults are illustrative:

   | Limit | Default | Faults when |
   |---|---|---|
   | run arena bytes | 64 MB | total allocation in a run exceeds it |
   | single allocation | 16 MB | one value (string/seq) would exceed it |
   | sequence length | 5,000,000 | a list / `find-all` would exceed it |
   | string length | 64 MB | a built string would exceed it |
   | match count | 1,000,000 | `find-all` / `each-match` exceeds it |
   | recursion depth | 4,000 | eval nests deeper |
   | op / step budget | 50,000,000 | runaway loop |
   | wall time | 2,000 ms | a run takes too long |

   A breach raises a fault: `limit: <which> exceeded` — shown in prov's message
   line; the editor stays responsive and consistent.
4. **Persistent vs transient.** User `def`s (from `~/.prov/init.psl`) and REPL
   bindings live in the **session dictionary** (a long-lived allocator), *not* the
   per-run arena. In the REPL, each entered form evaluates in its own run arena;
   any value the user *binds* into a global is deep-copied into the session
   allocator before the arena is freed. Transient results are printed, then dropped.

## Integration 1 — regex (reuse RFC-0009, no new logic)

Regex literals compile through the existing engine (cached). Builtins (the heavy
work is C):

| Builtin | Effect |
|---|---|
| `match? S /re/` | bool |
| `find S /re/` → `match`/`nil`, `find-next` | one match / iterate |
| `find-all S /re/` → `seq` of `match` | all (capped by `match count`) |
| `replace TARGET /re/ REPL` / `replace-all` | `REPL` is a template (`$1`,`${n}`) **or** `fn (match)->str` (computed substitution) |
| `extract S /re/` → `seq` | capture groups |
| `split S /re/` → `seq` of `str` | split |
| `each-match /re/ REGION body` | iterate; binds `$0..$n` |

`S` / `TARGET` / `REGION` may be a `str` slice, a `range`, the `selection`, or the
whole `buffer`.

**Edit-while-iterate correctness.** Replacing text shifts the offsets of later
matches. `replace-all` / `each-match` therefore either collect matches first and
**apply edits right-to-left** (earlier offsets stay valid), or accumulate a single
patch applied once. This is handled in the C builtin so scripts stay simple.

## Integration 2 — editor commands (reuse, one undo)

Editor operations are builtins backed by existing `prov_editor_*` / command
actions: `goto move line col line-count selection insert delete replace-range
replace-line yank paste select mark buffer buffers open write focus tab search
panel echo`, plus an escape hatch **`(cmd "…")`** that runs any raw prov/zx command
string (full reach). Guarantees: a whole run is **one undo transaction**; edit
builtins honor the **read-only guard** and clamp / fault on out-of-bounds ranges.

## Block surface (optional convenience) — macros over the core

Brace-delimited sugar that desugars to core forms (one reader; deterministic
braces, not indentation). Top level may be defs, statements, or **pattern-action
rules** (sed/awk-friendly).

```
def strip-trailing() { replace-all buffer /[ \t]+$/ "" }

/^\s*TODO/ { yank-line }                       # pattern-action over matching lines

let n = 1
each line {
    if match? line /^\d+\./ { replace-line fmt("{}. {}", n, after-dot(line)); n = n + 1 }
}

each match /(\w+)@(\w+)/ in buffer { replace $0 "${2}.${1}" }   # a@b -> b.a
```

Desugaring: `each match /re/ in R { B }` ⇒ `(each-match /re/ R (fn (m) B))`;
`/re/ { B }` ⇒ `(rule /re/ (fn (line) B))`; `if C { A } else { D }` ⇒
`(if C (do A) (do D))`; `NAME(a){B}` (top level) ⇒ `(def NAME (fn (a) B))`.
`$0..$n` / `${name}` are a reader rewrite to `(g m N)` scoped to the **innermost**
`each-match` / `rule` (lexically nearest binding; nested matches shadow outward).

## Surfaces in prov

- **`zp` REPL**: a leading sigil (e.g. `=`) routes the rest to psl.
- **`~/.prov/init.psl`**: user defs / rules loaded at startup (reuses `zc` reload).
- **`zr`**: evaluate the current selection / buffer as a script.
- **`source` / `:source <path>`**: load a script file.

All share one evaluator and one session dictionary.

## Implementation plan (phased; each phase: build + tests + ASan/UBSan)

- **P0 — core.** Reader (s-expr + text literals), value model, **run-arena +
  scratch arenas + caps**, tree-walk `eval`/`apply`, environments,
  depth/op/mem/time budgets.
- **P1 — minimal stdlib.** Arith/compare, string-slice ops, `seq` ops, `fmt`,
  `echo`, the IN-forms. Printer (REPL + round-trip tests).
- **P2 — regex.** `/re/` literal + builtins (reuse RFC-0009) + `each-match` +
  edit-while-iterate (right-to-left) correctness.
- **P3 — command bridge.** Editor builtins + `(cmd "…")` + one-undo transaction +
  read-only / bounds guards.
- **P4 — block surface.** Brace blocks, `if/for/each/when`, pattern-action, `$n`
  scoping — all `defmacro` desugaring to core.
- **P5 — surfaces.** `zp` sigil, `init.psl` load, `zr`, `source`; persistent vs
  transient split.
- **P6 — small scanning helpers + optional user macros** (`gensym`).
- **P7 — hardening.** Cap tuning, fuzz the reader, golden desugar tests, leak runs.

The whole feature is **gated** (`PSL` compile flag and/or runtime config) so a
minimal prov build omits it entirely.

## Worked example (computation + regex + command, one undo)

"Rewrite `- [ ] x` / `- [x] x` to `TODO: x` / `DONE: x`, report counts":

```
let todo = 0
let done = 0
each line {
    if match? line /^- \[ \] (.*)/   { replace-line fmt("TODO: {}", $1); todo = todo + 1 }
    elif match? line /^- \[x\] (.*)/i { replace-line fmt("DONE: {}", $1); done = done + 1 }
}
echo fmt("{} todo, {} done", todo, done)
```

The `each line` loop and offsets are driven by C builtins; the regex engine does
the matching; `replace-line` / `echo` are command builtins; everything allocates in
one run arena freed at the end, inside one undo transaction.

## Testing

- Reader / printer **round-trip** + reader **fuzz** (never crash on bad input).
- `eval` unit tests: each form + each builtin's contract; type faults; **arena
  reset leaves zero leak** (ASan across many runs); **scratch-arena copy-out**
  correctness.
- **Cap / budget** tests: each limit faults cleanly (big seq, deep recursion,
  infinite loop → bounded fault, editor still responsive).
- Regex builtins vs the **existing regex test corpus**; **edit-while-iterate**
  right-to-left correctness (overlapping / adjacent replacements).
- Command builtins under the editor harness; **one `u` reverts** a multi-edit run;
  read-only / bounds faults.
- **Golden desugar** tests (block → core); `$n` nesting / scoping.

## Risks & mitigations

- **Scope / size for a minimalist editor.** *Mitigation:* single evaluator (no VM /
  compiler), heavy reuse of proven (arena / u8str / array) + the regex / command
  code, and a `PSL` compile gate. Core is ~regex-engine-sized.
- **Speed on large hot loops.** Tree-walking is fine for edit-time transforms;
  heavy work is in C builtins, and per-keystroke whole-file jobs stay in C.
- **Memory.** Arena-per-run + scratch arenas + hard caps make peak memory bounded
  and reclaim deterministic; no GC, no leaks.
- **Macro-error clarity.** Block faults must map to the user's source, not the
  desugared core — keep source spans through the reader / expander (a P4/P7 concern).

## Alternatives considered

- **The three-layer typed stack VM (revision 1)** — elegant but more code (compiler
  + VM) for speed not needed here. Rejected; door left open to add a bytecode VM
  *later* only if profiling demands it (the tree-walker can be swapped underneath
  the same language).
- **Embed Lua** — dependency + GC; conflicts with prov's no-dep / deterministic
  identity.
- **sed/ex-only DSL** — simplest, but cannot compute (no counters / accumulation).

## Future work (only if justified)

- A bytecode VM under the same surface, *iff* profiling shows the tree-walker is a
  real bottleneck.
- Keybinding definitions in psl; a small scanning / lexing toolkit; a step debugger.
