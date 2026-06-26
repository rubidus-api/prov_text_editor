# prov

A terminal-based text editor written in pure C, aiming for minimal
dependencies, deterministic control, and definite responsiveness.

Named after *pointer provenance*, a concept recently introduced in the C
language. `prov` is built strictly on top of the
[`proven`](https://github.com/rubidus-api/proven_c_lib) C23 system library.

## Status

Pre-1.0, under active development.

- Design contract is complete and authoritative: see [`SPEC.md`](SPEC.md).
- Build foundation: `nob`-driven build, vendored `proven` library, native +
  Windows-x64 cross builds.
- Editor core & UX: piece-table buffer, the deterministic `zx` command model
  (motions, operators, text objects, namespaces; `ijkl` cursor, `u`/`U`
  undo/redo), a modeless `ed` text-entry mode, and a TOML-subset config file with
  live re-apply (`zc`).
- Multi-buffer UI: buffers, windows (splits, navigation, per-window read-only),
  tabs (each a whole layout), gutter/scrollbars, status & command lines, and a
  common modal panel (file open `zo` / write `zw`, windows, tabs, registers,
  bookmarks, search history, undo, command/help) with a context-aware help system
  (`h` / `F1`, `sh` regex reference, `hL` screen layout).
- Editing power: search & replace (`ss`/`sr`, literal + a small regex engine),
  registers & clipboard, keystroke macros, mouse support, a file browser with
  preview and Windows drive navigation, and binary/hex editing.
- Syntax highlighting & color themes: a lightweight per-line highlighter for ~20
  languages over a 16-color theme model (`*.theme.ini`); the base UI stays
  monochrome.
- See [`CHANGELOG.md`](CHANGELOG.md) for the running history and
  [`docs/proposals/`](docs/proposals/) for the design RFCs.

## Build

`prov` uses the `nob` build system. No `make`, `cmake`, or external build
dependencies are required — only a C compiler with C23 (or C2x) support.

```sh
cc -o nob nob.c     # bootstrap the build driver (once, or after editing nob.c)

# Common options
./nob              # incremental native debug build -> bin/prov
./nob test         # build and run unit tests
./nob --release    # optimized, size-reduced native build
./nob --clean      # remove build artifacts
./nob win64        # cross-compile for Windows x64 (needs x86_64-w64-mingw32-gcc)
./nob dist         # release/size-reduced binaries for every available toolchain
```

Binaries are written under `bin/` (not copied to the project root). `./nob dist`
produces one optimized, size-reduced binary per platform, named
`prov-<platform>`, skipping any toolchain that is not installed:

| Platform | Toolchain | Binary |
|---|---|---|
| Linux x64 | `cc` (native) | `bin/prov-linux-x64` |
| Linux arm64 | `aarch64-linux-gnu-gcc` | `bin/prov-linux-arm64` |
| Linux arm (hf) | `arm-linux-gnueabihf-gcc` | `bin/prov-linux-armhf` |
| Windows x64 | `x86_64-w64-mingw32-gcc` | `bin/prov-windows-x64.exe` |

`bin/prov` (and `bin/prov.exe`) remain the plain dev-build outputs of `./nob`
(and `./nob win64`). The cross toolchains usually live on one build host;
`scripts/dist.sh` (and `scripts/build-win64.sh`) run the build there over SSH —
the shared tree means the binaries appear in `bin/` directly.

`linux-arm64` is a glibc aarch64 build (Raspberry Pi 64-bit, aarch64 Linux, and
Termux under its glibc layer). A native Android/Termux (bionic) target needs the
Android NDK and can be added to the `dist` matrix once the NDK is available.

## Run

```sh
./bin/prov [file]
```

On Windows, run `bin\prov.exe` from a console.

## Dependencies

- **Build-time:** a C23-capable C compiler. The terminal backend needs no
  external library: raw `termios` + ANSI on Unix-like systems, Win32 Console
  (virtual-terminal) on Windows. Cross-compiling the Windows binary needs
  `mingw-w64`.
- **Vendored (statically linked):** `proven`, and later Tree-sitter and PCRE2.
- **Runtime (optional):** clipboard helpers (`xclip`/`xsel`, `wl-copy`/`wl-paste`).
  Missing helpers degrade gracefully.

See [`DEPENDENCIES.md`](DEPENDENCIES.md) for sourcing and licensing details.

## Documentation

- [`SPEC.md`](SPEC.md) — design contract and implementation milestones.
- [`TEST.md`](TEST.md) — verification contract.
- [`CHANGELOG.md`](CHANGELOG.md) — curated change history.
- [`DEPENDENCIES.md`](DEPENDENCIES.md) — third-party sourcing and licensing.

## License

MIT. Bundled dependencies are MIT/BSD/Public Domain compatible.
