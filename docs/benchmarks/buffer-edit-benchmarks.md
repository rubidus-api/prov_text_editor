# Buffer / piece-table edit benchmarks

Harness: `bench/bench_buffer.c` (`-O2`), single-char edits through the public
editor API. Numbers are wall-clock ns/op on the dev host; treat them as relative
(same machine, same run conditions), not absolute. Build line is in the harness
header. Re-run after any `buffer.c` / `editor.c` change that touches the piece
table, line index, undo, or allocation.

## Phases (see RFC-0005)

- **Baseline** — array piece table, libc `memmove`, full `rebuild_lines` per edit.
- **B** — `memmove` → `proven_mem_move` (expected: perf-neutral).
- **C** — incremental line index (expected: large drop on edit-in-large-doc ops).
- **D** — arena/pool allocation, only where post-C data justifies it.

## Results

| op | Baseline | B (mem_move) | C (incr. lines) | C.2 (coalesce) | vs base |
|---|---:|---:|---:|---:|---:|
| insert@end (20k) | 136,103 | 136,732 | 126,638 | **26,676** | 5.1× |
| insert@front (8k) | 9,769 | 10,226 | 1,609 | **1,750** | 5.6× |
| insert@mid, 4000-line doc (4k) | 85,050 | 85,065 | 6,897 | **3,065** | 27.7× |
| delete@front, 4000-line doc (4k) | 100,946 | 98,511 | 28,004 | **2,008** | 50.3× |
| undo+redo (40k) | 164,412 | 170,042 | 155,594 | **26,464** | 6.2× |
| copy_range whole 108 KB (2k) | 26,901 | 26,816 | 30,143 | **3,216** | 8.4× |

**B (mem_move): perf-neutral** — every row within run-to-run noise of the
baseline, confirming the memmove was not on the critical path. buffer.c is now
free of all libc string/byte functions.

**C (incremental line index): large wins on edit-in-large-doc.** insert@mid
**12×** (85 µs → 6.9 µs), insert@front **6×**, delete@front **3.6×**. copy_range
is unchanged (it never touched the line index). insert@end and undo+redo barely
moved — they are now bottlenecked by a *different* cost: each keystroke creates a
new add-piece, so `insert_raw`'s O(pieces) piece-lookup grows to O(N²) over the
run. That is the Phase C.2 target (coalesce sequential add-pieces), not the line
index. Correctness: a 4000-edit randomized test cross-checks the incremental
index against a reference model, and `-DPROV_DEBUG_LINES` validates every edit in
the whole suite against a full rescan.

**C.2 (coalesce sequential add-pieces): the second structural win.** When `at`
sits at the end of an add-piece whose bytes are contiguous with the new add
bytes, `insert_raw` extends that piece instead of allocating a new one. Typing
no longer grows the piece array, so the O(pieces) lookup stays flat and the
piece count for typed text stays tiny — which also speeds up `copy_range`
(8.4×) and any make_doc-built benchmark. Net vs baseline: every edit op is
5–50× faster.

## Arena / pool verdict (was Phase D)

After C + C.2, edit ops are 3–27 µs and dominated by the unavoidable byte work
(append to the add store, the O(lines-after) index shift, the piece memcpy on
delete). Allocator calls are now a *smaller* fraction than ever: typing
coalesces into one piece (≈no piece allocs), the line array grows
geometrically (amortized O(1)), and the add store is one growable buffer.
**A `proven_pool`/arena for piece or undo nodes would optimize an allocation
cost that the structural fixes already made negligible** — it would add
lifecycle complexity for sub-percent gains. Verdict: **not worth implementing
now.** The honest lever was algorithmic (line index + coalescing), not the
allocator. Revisit only if a future profile on a real workload shows allocation
as a hotspot (e.g. pathological many-tiny-piece documents).

## Reading

Baseline shows edit cost scales with **document size**, not edit size:
`insert@mid` on a 108 KB doc is 85 µs, dominated by `rebuild_lines` rescanning
every byte each edit. The `memmove`s the cleanup targets are sub-µs noise beside
that — so Phase B should not move these numbers, and Phase C (incremental line
index) is where the edit-in-large-doc rows should fall sharply.
