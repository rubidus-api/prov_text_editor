# RFC 0005 — buffer container migration, line-index efficiency, and arena analysis

- Status: **Complete** (A, B, C, C.2 done; D analyzed → not worth implementing)
- Created: 2026-06-18
- Scope: remove `buffer.c`'s remaining libc `memmove`s; analyze (with
  benchmarks) where arena/pool-style allocation helps prov; act on the findings.

## 1. Baseline measurements

`bench/bench_buffer.c`, `-O2`, single-char edits through the public editor API.
Array-backed piece table, libc `memmove`, full `rebuild_lines` per edit:

| op | ns/op |
|---|---:|
| insert@end (20k) | 136,103 |
| insert@front (8k) | 9,769 |
| **insert@mid, 4000-line doc (4k)** | **85,050** |
| delete@front, 4000-line doc (4k) | 100,946 |
| undo+redo (40k) | 164,412 |
| copy_range whole 108 KB doc (2k) | 26,901 |

### 1.1 What the numbers say

The cost of an edit scales with **document size**, not with the edit. An insert
in the middle of a 108 KB document costs 85 µs — almost entirely
`rebuild_lines`, which **re-scans every byte of the document on every edit** to
rebuild the line-start index (`buffer.c:105`). `insert@end` degrades to ~O(n²)
across the run for the same reason (plus one new piece per keystroke).

The two `memmove`s the goal targets are **not** on this critical path:
- `pieces_insert_at` shifts the piece array once per insert — for the 4000-line
  doc that is ~4000×`sizeof(piece_t)` ≈ 96 KB moved, a few µs, versus the 108 KB
  byte rescan that dominates.
- `trim_undo` shifts the undo array only when it exceeds `undo_limit` (default
  1000) — rare.

**Conclusion:** removing the `memmove`s is a *correctness/cleanliness* change
(the last libc string/byte calls in prov), not a performance one. The real
efficiency lever is the **line index**, and the real allocation question is
secondary to it.

## 2. Why not migrate pieces to `proven_array` or `proven_list`

- `proven_array` is end-only (`push`/`pop`); it cannot middle-insert (pieces) or
  front-trim (undo), so it does not remove either `memmove`.
- `proven_list` (intrusive) *would* give O(1) middle-insert and front-remove,
  but the pieces array is iterated **fully** on the hottest paths
  (`rebuild_lines`, `copy_range`, `delete_raw`). A linked list replaces a
  cache-friendly contiguous scan with pointer chasing over thousands of nodes —
  measurably worse on exactly the paths that dominate. So list-ifying pieces is
  a net negative here.

The correct, benchmark-justified choice is to **keep the contiguous array** and
replace libc `memmove` with a proven primitive.

## 3. Plan (staged; each step build + test + bench + commit)

### Phase A — proven (one small addition)
- **A1** Add `proven_mem_move(dst, dst_cap, src_view)` (bounded, overlapping-safe)
  to `memory.{h,c}`, mirroring `proven_mem_copy`; test, alias, version bump
  `v26.06.18b`, doc sync.

### Phase B — prov: remove the memmoves (libc-free buffer.c)
- **B1** Re-vendor proven; replace both `memmove`s with `proven_mem_move`. Bench
  to confirm perf-neutral; `buffer.c` is now free of `<string.h>`.

### Phase C — prov: the actual efficiency win (incremental line index)
- **C1** Maintain the line-start index **incrementally** on insert/delete instead
  of a full `rebuild_lines` rescan: an insert shifts later line offsets by the
  inserted length and splices in the new line starts; a delete drops the line
  starts inside the removed range and shifts the rest. Keep `rebuild_lines` as
  the load-time builder and a debug cross-check. Benchmark the edit ops; this is
  where the large numbers should drop.

### Phase C.2 — coalesce sequential add-pieces (emerged from C's data)
- The C benchmark showed insert@end / undo+redo were now bound by `insert_raw`'s
  O(pieces) lookup, because each keystroke made a new add-piece. **Done:**
  `insert_raw` extends the preceding add-piece when the insert is at its end and
  contiguous in the add store. Net vs baseline: every edit op 5–50× faster
  (insert@mid 27.7×, delete@front 50×, copy_range 8.4×).

### Phase D — arena/pool: analyzed, NOT implemented (data-backed)
- Re-profiled after C + C.2. Edit ops are now 3–27 µs, dominated by unavoidable
  byte work (add-store append, the O(lines-after) index shift, the delete piece
  copy). Allocation is a *smaller* fraction than ever: typing coalesces to one
  piece (≈no piece allocs), the line array amortizes geometric growth, the add
  store is one growable buffer, layout/render already reuse.
- **Verdict: a `proven_pool`/arena for piece or undo nodes would optimize a cost
  the structural fixes already made negligible — sub-percent gains for real
  lifecycle complexity. Not worth implementing now.** The honest efficiency
  lever was algorithmic, not the allocator. (Details + numbers in
  `docs/benchmarks/buffer-edit-benchmarks.md`.) Revisit only if a profile on a
  real workload shows allocation as a hotspot.

## 4. Memory-safety / efficiency analysis (the second ask)

Allocation sites in prov and the arena/pool verdict (pre-C; revisited in D):

| site | pattern | arena/pool fit |
|---|---|---|
| piece array | one block, geometric `realloc` | already optimal; no |
| line index | one block, geometric `realloc`, rebuilt per edit | the cost is the *rebuild*, not the alloc (Phase C) |
| undo/redo arrays | grow + per-action `bytes` malloc | `bytes` is variable-size & individually freed → not arena; nodes could pool (Phase D) |
| `delete_raw` `np` | malloc+free of a fresh piece array per delete | reusable scratch arena candidate (Phase D) |
| layout pane nodes | fixed array + free-list slot reuse | already pool-like; no change |
| render grid/tmp | one block, reused across frames (cap-based) | already arena-like reuse; no |
| status/prompt strings | reused `proven_u8str` scratch / owning | already covered by Milestone S |

Pre-C verdict: prov's allocation is already lean (reused buffers, free-list
layout, one-block arrays). Arena/pool would shave allocator calls that are
**dwarfed by `rebuild_lines`** today. The honest sequence is **fix the
algorithm first (Phase C), then re-measure** before adding allocator machinery
(Phase D) — otherwise we would optimize the invisible 1%.

### 4.1 The one real growth concern: the append-only add store (RESOLVED)

The add store accumulates every inserted byte and never reclaims deleted text
(orphaned bytes are held, not leaked — freed at buffer destroy; ASan clean). Over
a long type-and-delete session it grows toward "total bytes ever inserted," not
the current document size. **Resolved by save-time compaction**
(`prov_buffer_compact`, run on `zw`/`:w`/`^S`): the current content is
re-materialized as a single original piece with a fresh empty add store,
reclaiming the orphaned bytes and collapsing the piece list. Content, length,
line index, cursor, and undo/redo are unchanged; the operation is
failure-atomic. `prov_buffer_store_bytes()` exposes the held-bytes figure.

## 5. Verification

`./nob test` (buffer/editor suites) green at every step; ASan/UBSan on touched
paths; `bench/bench_buffer.c` numbers recorded in `docs/benchmarks/` before and
after each phase. Incremental line index (C1) additionally cross-checked against
the full `rebuild_lines` under a debug flag.
