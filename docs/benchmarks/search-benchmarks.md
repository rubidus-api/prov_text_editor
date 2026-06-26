# Search benchmarks (M4.5)

`prov_search_bytes` — literal byte search (`src/search.c`). Measured with
`bench/bench_search.c` on a random 8 MiB buffer (letters `a`–`t`).

Build & run:
```
cc -O2 -std=c2x -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -Iinclude -Isrc \
   -o /tmp/bench_search bench/bench_search.c src/search.c && /tmp/bench_search
```

## Results (8 MiB haystack)

| Case | ms | MB/s |
|---|---:|---:|
| absent needle len 1 (full scan) | 10.4 | 805 |
| absent needle len 2 (full scan) | 9.8 | 857 |
| absent needle len 5 (full scan) | 9.8 | 857 |
| absent needle len 16 (full scan) | 9.8 | 859 |
| present at end (near-full scan) | 11.6 | 727 |

## Analysis

- **~850 MB/s, effectively O(n).** The naive scan's `match_at` exits on the first
  mismatching byte, which on real (non-pathological) text is almost always byte
  0 — so needle length barely changes throughput (805→859 MB/s from len 1→16). A
  Boyer–Moore/Horspool skip table would only help on adversarial repetitive text
  with long needles; **not worth the complexity** for an editor. Revisit only if
  a real workload shows the O(n·m) worst case.
- **Per-keystroke cost.** A full scan is ~1 ms per MiB. Incremental search on
  files up to a few MiB is comfortably interactive; an 8 MiB file costs ~10 ms
  per keystroke (plus the materialize below).

## Materialization cache (implemented)

A naive implementation would materialize the whole document (one `copy_range`) on
**every** incremental keystroke. Instead the document is materialized **once when
the search prompt opens** (`search_cache_begin`) and reused by every incremental
keystroke; it is freed on Enter/Esc (`search_cache_end`). The document is static
during the prompt, so no invalidation is needed. `sn`/`sp`/`sw` run outside the
prompt and materialize per call (cold path). Net: each incremental keystroke is
just one `prov_search_bytes` scan (~1 ms/MiB) with no per-keystroke copy.
