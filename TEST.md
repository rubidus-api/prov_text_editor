# Test and Verification

This document is the public verification contract for `prov`.

`prov` follows strict TDD: every feature and core module is preceded by a
failing unit test under `tests/`. Tests are built and run through `nob`.

## Build the editor

```sh
cc -o nob nob.c     # bootstrap the build driver (once, or after editing nob.c)
./nob               # incremental debug build -> bin/prov
./nob --release     # optimized build
./nob --clean       # remove build artifacts
```

Expected result: a successful compile and link produces `bin/prov`.

## Run unit tests

```sh
./nob test
```

Expected result: every test binary under `tests/` builds and exits `0`. A
non-zero exit from any test marks the run as failed and lists the failing
binary.

## Testing strategy

`prov` splits into a backend-independent core and a thin platform layer, and
tests each differently:

- **Pure unit tests (TDD, automated):** buffer / piece table, line-offset
  index, Unicode width and cursor math, UTF-8 encoding validation, undo/redo,
  and the `zx` command parser are pure logic with no I/O. They are written
  test-first under `tests/` and run by `./nob test`.
- **Rendering via a mock backend:** the display layer targets the
  backend-neutral `platform.h` interface. Tests drive it against an in-memory
  mock backend (a cell grid + recorded output), asserting the produced cells
  rather than touching a real terminal.
- **Real terminal (manual):** raw `termios` mode, ANSI sequences, window-size
  detection, and key decoding in `platform_term_posix.c` are verified manually
  by running `./bin/prov` in a real terminal. These are intentionally not part
  of `./nob test`.

Each test file is a standalone `main()` that returns 0 on success and non-zero
on failure; `./nob` builds and links it against the library objects and treats
a non-zero exit as a failure.

## Release verification order

1. `./nob --clean`
2. `./nob` (debug build must succeed)
3. `./nob test` (all tests pass)
4. `./nob --release` (optimized build must succeed)

## When to update

Update this file when any of the following change:

- a new automated check is added or removed
- the build, test, clean, or release command changes
- manual verification steps change
- supported platform or toolchain assumptions change
- a release requires a different verification order

## Writing rules

- Keep commands short and reproducible.
- Prefer the narrowest command that proves the behavior in question.
- Group checks by purpose instead of dumping raw terminal history.
- Record the expected result or failure condition when it is not obvious.
