# RFC-0012 — Panel nesting / back-navigation

- **Status:** **Implemented (Option D, 2026-06-25).** Both halves of the depth-1
  mechanism are now built:
  - **P2 value-return** (2026-06-21): a picker panel returns a value to its host
    via `s->panel_pick(s, value)` (NULL = cancel); the host restores itself with
    the value. Consumer: save-as `Tab` → browser → path back into the dialog.
  - **P1 help is context-aware (no global stacking).** Help is resolved by where
    you are, not by parking panels: in a **panel** `h` (or **F1**) shows that
    panel's own intent page; in **zx** mode `h` (or F1) opens the zx keyboard help;
    in **ed** mode (modeless — `h` types text) **F1** opens the ed-mode help page.
    F1 is the one universal, non-typing help key, mode-aware. (An earlier
    suspend/restore "global help over a panel" via `H` was removed in favour of
    this simpler context-help model; §6.2's "global-help-over-panel" is therefore
    intentionally *not* provided.)
  - **P3** stays intra-panel (row-source swap); **P4** keeps replace.
  No arbitrary N-deep stack — depth is 1 (host + one detour: a P2 picker), matching
  the intra-panel sub-modes and prov's no-z-order identity.
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

## 6. Decisions (resolved)
1. **Stack vs. typed (depth-1)?** → **Typed/depth-1 (Option D).** No general N-deep
   stack; one `panel_pick` (P2) + one suspend/restore slot (P1).
2. **Help key from within a panel?** → **Context-aware help, no panel parking.**
   In a panel, `h` (or F1) = that panel's own intent page; in zx, `h`/F1 = the zx
   keyboard help; in ed (modeless), F1 = the ed-mode help. No `H` global-help
   detour (it was tried and removed as unnecessary complexity).
3. **Esc semantics with a detour?** → **Esc pops one level** for a P2 picker
   (cancel back to the host). Help is a plain full panel: Esc/q/x closes it.
4. **Scope?** → Both P1 and P2 are now implemented (P2 first in 2021, P1 in 2025).
