# RFC 0009 — Regex engine (Pike VM) + boundary markers + line guards

- Status: **Implemented** (S1-S9; see CHANGELOG + docs/benchmarks/regex-benchmarks.md)
- Created: 2026-06-19 · Revised: 2026-06-20 (review fixes folded in)

## 1. Scope & rationale

prov needs regex search/replace beyond the literal matcher (M4.5a). PCRE2 (~tens
of kLOC, libc-heavy, backtracking) conflicts with prov's self-contained,
libc-free-core, single-small-binary ethos and brings the ReDoS bug class.

**Decision: a self-contained Pike VM (Thompson NFA + submatch tracking)** — the
RE2 / Go / Rust-regex / ripgrep lineage: compile the pattern to tiny bytecode,
run all alternatives as deduplicated "threads" advanced one input position at a
time. **Linear time O(text × program), no catastrophic backtracking**, with
capture groups. The price (no backreferences, no arbitrary lookaround) is
recovered in practice by:

- **`\zs` / `\ze` match-boundary markers** (Vim; PCRE `\K`) — set where the match
  really starts/ends (≈ lookbehind/lookahead-for-bounds).
- **`:g/re/` / `:v/re/` line guards** (ed → vi/sed/sam) — substitute only on lines
  that do / don't match a guard (≈ context assertions at line granularity).

## 2. Key decisions (from review — settle before coding)

These were gaps in the first draft; resolved here.

1. **Semantics = leftmost-greedy (Perl/RE2/ripgrep), NOT POSIX leftmost-longest.**
   This is what users expect and what `\zs`/ripgrep imply. Consequence for
   testing: **libc `regcomp` (POSIX) is the wrong differential oracle** — POSIX
   and greedy disagree even on the overall span (`a|ab` on `"ab"`: greedy → `a`,
   POSIX → `ab`). The oracle is a **small test-only recursive backtracking
   reference matcher** implementing the *same* greedy semantics (it may be
   exponential — fine on small fuzz inputs). libc `regcomp` is used only as a
   loose secondary check on alternation-free patterns.
2. **UTF-8 unit.** The engine is byte-oriented, but matches must land on
   **codepoint boundaries** (the editor highlights/jumps by byte offset). So:
   - `.` and negated classes compile to a **"one UTF-8 codepoint" sub-automaton**
     (never splits a multibyte char; `.` excludes `\n` unless DOTALL).
   - Character classes `[...]` and `\d \w \s` are **ASCII-range in v1**; literal
     bytes in the pattern match their exact UTF-8 byte sequence (so a Korean
     literal in the pattern still matches). Unicode property classes (`\p{…}`)
     and Unicode `\w`/word-boundary are out of scope (future).
   - Case-fold (`ICASE`) is **ASCII-only**, reusing the existing search fold.
3. **Search semantics.** Two entry points:
   - `prov_regex_search(re, hay, len, from, out)` = **unanchored leftmost** match
     whose start ≥ `from` (implicit `.*?` prefix: seed a start thread at each
     position as the scan advances). This drives `ss`/`sw`/`sn`.
   - `prov_regex_match_at(re, hay, len, at, out)` = **anchored** at `at` (drives
     line guards, replace-at-cursor, `\b` checks).
   - **Backward** (`sp` / `?`): an NFA can't run in reverse. Scan forward over
     `[0, cursor)` collecting matches and take the **last** one before the cursor
     (one linear pass over the cached haystack — same cost class as the literal
     backward search). Documented, not "exec in reverse".
4. **Bounded program.** Cap the **total compiled program size** (e.g. ≤ 32 K
   instructions), not just each `{n,m}` m — nested `(a{1000}){1000}` would
   multiply. Over-limit = a clean compile error.
5. **Buffer bridge.** Regex runs on contiguous bytes; it **reuses M4.5a's
   `search_hay` materialization** of the document. Line guards (`:g`/`:v`) operate
   per line, using line ranges from the buffer's line index.
6. **`^`/`$`/`\b` details.** `^`/`$` are buffer ends, or every `\n` boundary under
   MULTILINE (`\n` positions in the cached haystack). `\b`/`\B` and `\w` are
   **ASCII** word semantics. These are zero-width `assert` instructions.

## 3. Module shape & API

New pure module `src/regex.{c,h}` — libc-free, no OS headers, allocator-injected,
operates on byte ranges, no global state; a compiled program is an owned object.

```c
typedef struct prov_regex prov_regex_t;          /* opaque compiled program */

typedef struct {
    proven_size_t start, end;                    /* group 0 span (honors \zs\ze) */
    proven_size_t ngroups;
    struct { proven_size_t start, end; bool set; } groups[PROV_REGEX_MAX_GROUPS];
} prov_regex_match_t;

enum { PROV_RX_ICASE = 1u<<0, PROV_RX_MULTILINE = 1u<<1, PROV_RX_DOTALL = 1u<<2 };

prov_result_regex_t prov_regex_compile(proven_allocator_t a,
                                       proven_u8str_view_t pattern, unsigned flags);
bool prov_regex_search  (const prov_regex_t *, const proven_u8 *hay, proven_size_t len,
                         proven_size_t from, prov_regex_match_t *out);   /* unanchored ≥ from */
bool prov_regex_match_at(const prov_regex_t *, const proven_u8 *hay, proven_size_t len,
                         proven_size_t at,  prov_regex_match_t *out);    /* anchored at `at` */
void prov_regex_destroy (proven_allocator_t, prov_regex_t *);
```

**Bytecode:** `byte b` · `range lo,hi` · `class set` · `utf8any` (one codepoint) ·
`split x,y` · `jmp x` · `save n` (`\zs`=save0-start, `\ze`=save0-end) · `assert
kind` (`^ $ \b \B`) · `match`. Greedy vs lazy = `split` operand order. The
executor keeps clist/nlist of threads, **dedups by program counter per input
position** (the linear bound), and carries each thread's `save` array with
leftmost-greedy thread priority. Iterative; thread storage preallocated to
program length.

## 4. Stages (each: failing test first → build/test → ASan/UBSan → self-review → commit)

- [x] **S1 — Parser → AST.** Literals (incl. multibyte UTF-8 bytes), `.`, classes
      `[...]`/`[^...]` with ranges + `\d\w\s\D\W\S`, quantifiers `*+?{n,m}` + lazy,
      alternation `|`, groups `( )`/`(?: )`, anchors `^ $ \b \B`, escapes,
      markers `\zs`/`\ze`. Compile errors carry offset + message. Pure;
      `tests/test_regex.c` on pattern strings (valid + malformed).
- [x] **S2 — Compiler AST → bytecode.** Emit the instruction set; capture-slot
      assignment; `.`/negated-class → `utf8any`/codepoint sub-automaton; total
      program-size cap (decision 4). Tests: pattern → expected instruction shape.
- [x] **S3 — Pike VM core (anchored).** `prov_regex_match_at`: clist/nlist,
      PC-dedup, `save` captures, assertions, greedy/lazy priority. Golden table of
      (pattern, input, expected span+captures), hand-derived for greedy semantics.
- [x] **S3b — Greedy reference oracle + differential fuzz.** A ~60-line test-only
      recursive backtracking matcher (same greedy semantics); random-pattern
      fuzz comparing span+captures against the Pike VM. This is what makes S3
      trustworthy (decision 1).
- [x] **S4 — Unanchored search + backward.** `prov_regex_search` (leftmost ≥
      `from`) and the backward-scan helper (decision 3). Tests for search-from,
      multiple matches, last-before-cursor.
- [x] **S5 — `\zs` / `\ze`.** Map markers onto group-0 start/end so the reported
      match (and the replace target) excludes the context. Tests for
      `foo\zsbar`, `foo\zebar`, interaction with captures.
- [x] **S6 — Search integration.** `so` regex toggle (e.g. `sox`, beside `soc`
      case); `ss`/`sw`/`sn`/`sp`/highlight route through the regex when on,
      reusing the `search_hay` cache (decision 5). Literal path stays the default.
- [x] **S7 — Replacement.** `sr` replacement supports `\1`..`\9`, `\0`/`&`, and
      `\\`/`\&`/`\n`/`\t` escapes; the replaced span honors `\zs`/`\ze`. One undo
      step (reuse the existing `do_replace`).
- [x] **S8 — Line guards `:g` / `:v`.** A guarded-replace command form (syntax:
      `sr` gains an optional `g/re/` or `v/re/` guard prefix, settled in this
      stage) that restricts substitution to lines matching / not matching the
      guard, via `prov_regex_match_at` per line.
- [x] **S9 — Hardening.** ASan/UBSan; PTY matrix (search/replace/guards on real
      buffers, multibyte text); `docs/benchmarks/regex-benchmarks.md` proving
      linear time — include `(a+)+$` vs a long `aaaa…X` (the anti-ReDoS proof) and
      a UTF-8 throughput row. Update SPEC §2/§20, help, CHANGELOG.

## 5. Out of scope (deliberate)
- **Backreferences** (`\1` in pattern), **lookaround** (`(?=) (?<=) (?!) (?<!)`):
  need backtracking → break the linear guarantee. Covered in practice by
  `\zs`/`\ze` + `:g`/`:v`. Revisit only via PEG/Oniguruma if a real need appears
  (file a REPORT).
- Unicode property classes (`\p{…}`), Unicode `\w`/word-boundary, named groups,
  recursion — future, optional.

## 6. Risks
- **Submatch priority / greedy-vs-lazy correctness** is the subtle part — pinned
  by the greedy reference oracle + differential fuzz (S3b) and the golden table.
- **Scope realism:** parser + compiler + VM + captures + classes + `{n,m}` + lazy
  + assertions + UTF-8 + search/replace/guards is **~800–1500 LOC + a test-only
  reference matcher** — bigger than RFC-0008's "few hundred lines" estimate.
  Mitigated by the fine S1–S9 split and per-stage review.
- **UTF-8 sub-automaton** for `.`/negated classes must be correct at buffer ends
  and around invalid bytes — covered by S2 tests + multibyte PTY in S9.
