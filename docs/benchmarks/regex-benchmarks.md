# Regex engine benchmarks (RFC-0009)

Harness: `bench/bench_regex.c` (build line in its header). Machine: the dev
host; numbers are indicative, not absolute. The point is the *shape* of the
curve, not the constant.

## Anti-ReDoS: linear time on the classic blowup pattern

`(a+)+$` matched against `"a"×n + "X"` is the textbook catastrophic-backtracking
case: a PCRE-style backtracking engine explores an exponential number of ways to
split the `a`s and **hangs by n ≈ 30**. The Pike VM (Thompson NFA + submatch,
deduped by program counter per position) is linear.

| n | total ns | ns/byte |
|---:|---:|---:|
| 10 | 985 | 89.6 |
| 30 | 1,271 | 41.0 |
| 100 | 4,057 | 40.2 |
| 1,000 | 39,096 | 39.1 |
| 10,000 | 389,659 | 39.0 |
| 100,000 | 3,984,523 | 39.8 |

**ns/byte is flat (~39–40) across four orders of magnitude** — O(n), no blowup.
This is the literal acceptance proof of "no ReDoS": the same input that wedges a
backtracker finishes in microseconds.

## Throughput

Scanning `[a-z]+[0-9]` over an 8 MB buffer (repeated `"abcd1 efgh2 …"`):

- ~1.4 M matches in ~280 ms ≈ **30 MB/s**.

Slower than the literal matcher (~850 MB/s — see `search-benchmarks.md`), as
expected: the NFA tracks a thread set and capture slots per byte, and unanchored
search seeds a start thread at each position. For interactive editor search over
a materialized buffer this is comfortably fast; the literal matcher remains the
default, and regex is opt-in (`sox`).

## Notes
- Cost is **O(text × program)**; the program is size-capped (8192 instructions),
  so a single match/search is strictly linear in the text.
- Captures add a per-thread save array (2·(groups+1) offsets); patterns with no
  groups are cheapest.
