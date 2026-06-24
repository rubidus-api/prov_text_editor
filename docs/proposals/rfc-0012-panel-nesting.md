# RFC-0012 — Panel nesting / back-navigation

- **Status:** **Option D — P2 value-return mechanism IMPLEMENTED 2026-06-21.** A
  panel opened as a picker returns a value to its host via a single
  `s->panel_pick(s, value)` callback (NULL = cancel); the host reopens itself with
  the value or its prior input. First consumer: save-as `Tab` → browser → path
  back into the dialog. P1 (help-over-panel) was found already covered by the
  existing per-panel intent-help sub-mode (`h` opens it, Esc returns), so it
  needed no new work. P3 stays intra-panel; P4 keeps replace. Sub-decisions §6.2
  (a *global*-help-over-panel key) and §6.3 (Esc layering) remain open but are not
  needed by P2; revisit if/when global-help-over-panel or deeper nesting is wanted.
  Further consumers (history→search #4, encoding→browser #2) reuse `panel_pick`.
- **Depends on:** RFC-0010 (common modal panel).
- **Blocks:** the panel-heavy roadmap items — backlog #2 (browser encoding +
  preview), #3 (hex editing), #4 (one-key search + regex panel). Settle this before
  building those, to avoid reworking their interaction model.

## 1. Problem

The modal panel (RFC-0010) is **single-instance**: `s->panel_open` + one
`s->panel`. Opening any panel while another is open does
`if (panel_open) prov_panel_free(&s->panel)` and replaces it. There is **no way to
return to the previous panel**. Within one panel there is already 1-level intra
nesting via flags — `ss` filter, `h` panel-help, verb+slot — and Esc backs out of
those to the panel's list. RFC-0010 deliberately avoided floating / z-order /
stacked boxes ("one panel, snapped to FULL or a half").

The question: do real workflows need panel→panel nesting with a "back", and if so,
what is the *smallest* mechanism that serves them without giving up the one-panel
identity?

## 2. The 12 panels today
WINDOWS (0w), TABS (0t), REGS (0b), MACROS (0e), BOOKMARKS (0m), SEARCH-history
(0s), CMDS (0z), MOVES (0n), UNDO (0u), BROWSER (zo), SAVEAS (za), HELP (h).

## 3. Scenario catalog — grouped by *interaction pattern*

The genuine needs are not "arbitrary panels stacking"; they fall into four
patterns. Each pattern wants a *different* shape of "back", which is the crux.

### P1 — Transient overlay over a panel, return to the SAME panel
- **Help while in a panel.** In the browser/any panel, "how do I do X?" → open the
  keyboard help → close it → **be back in the browser, unchanged**. (The author's
  named example: file-open → help → return.)
- Need: suspend the current panel, show help, restore it. Read-only detour, depth 1.

### P2 — Sub-picker that returns a VALUE to a host input  (the strongest case)
- **Save-as path ← browser.** Typing a path in SAVEAS, want to *browse* for the
  directory → open BROWSER → pick → return to SAVEAS with the path filled.
- **Search term ← history.** Typing in the (planned, #4) search/regex panel, pull a
  past term from SEARCH-history (0s) → return to the search input with it inserted.
- **Encoding ← list.** In the (planned, #2) browser preview, pick an encoding from a
  small list → return to the browser with the preview re-decoded.
- Need: open a picker "for a result", and on selection **return the value to the
  caller and restore the caller**. (Esc = cancel, return nothing.)

### P3 — Drill-down (list → sub-list → back)
- **Tab → its windows**, **register → its content preview**, **undo entry → diff**.
- Need: descend into a detail view, Esc pops back up one level.
- Note: today this is achievable *intra-panel* (the panel's row source swaps), no
  separate panel required.

### P4 — Independent panel → panel, NO return wanted
- 0w → 0t, 0b → 0e, picking a command in 0z that opens another panel.
- Replace (current behavior) is correct; nesting here would be clutter.

## 4. Design options

| Option | What | Serves | Cost / risk |
|---|---|---|---|
| **A. Full panel stack** | Push/pop N-deep; Esc pops; each level saves full panel state | P1,P2,P3 uniformly | Heaviest. Save/restore every panel's kind+scroll+sel+filter+verb+source; reintroduces z-order layering RFC-0010 rejected; deep stacks confuse in a minimalist editor |
| **B. 1-deep "previous"** | Remember exactly one previous panel; Esc returns to it | P1 + shallow P2 | Cheap. Breaks on X→Y→Z (loses X); no value-return semantics |
| **C. Typed special-cases (no general stack)** | Help = suspend/restore the one current panel (depth-1). Value pickers (P2) = the host opens a picker "for a result" with a return-target, restoring the host on pick/cancel. Drill-down (P3) = intra-panel row-source swap. P4 = keep replace | All four, each with the right semantics | Most code paths, but each is small and explicit; keeps "one panel visible"; matches prov's no-z-order ethos |
| **D. Hybrid = C + a generic 2-slot save/restore** | One reusable "suspended panel" slot powering both help-over-panel (P1) and pick-and-return (P2); P3 intra; P4 replace | P1,P2,P3,P4 | A middle ground: one mechanism, depth ~1, value-return built in; not an arbitrary stack |

## 5. Recommendation (for discussion)

**Option D** (≈ C consolidated): keep the single visible panel and the
no-z-order identity, and add **one** lightweight "suspend the current panel +
optionally return a value" mechanism that powers:
- **Help over any panel** (P1): `h`-from-panel (or a global help key) suspends the
  panel, opens HELP, and Esc restores the suspended panel verbatim.
- **Pick-and-return** (P2): a host (SAVEAS, search input, browser) opens a picker
  with a *return target*; selecting feeds the value back and restores the host;
  Esc cancels back to the host.
- **Drill-down** (P3): stays intra-panel (row-source swap), not a second panel.
- **Independent switches** (P4): unchanged replace.

This avoids an arbitrary N-deep stack (which fights the minimalist model and risks
"how many Escs am I from the editor?" confusion) while covering every concrete
scenario above. Depth is effectively 1 (a host + one detour), which matches how the
intra-panel sub-modes already behave.

## 6. Open decision for the author
1. **Stack vs. typed (depth-1) mechanism?** Recommendation: typed/depth-1 (D). Or do
   you want a true general stack (A) for future-proofing?
2. **Help key from within a panel:** keep `h` = the panel's *own* intent page and add
   a separate key for the *global* keyboard help (suspend+restore)? Or make `h`
   context-aware?
3. **Esc semantics with a detour:** Esc from a returned-to host = cancel the field,
   or close the whole host? (Proposed: Esc pops one level — detour → host → closed.)
4. **Scope now:** implement only P1 (help-over-panel) now as the concrete first step,
   and add the P2 return-target mechanism when #2/#4 land? Or build the full D
   mechanism up front?
