# External Dependencies

This document lists all external libraries required for the `prov` editor. The AI agent should refer to this document to fetch the updated libraries when building or extending the editor. **All dependencies, code snippets, and referenced external code must be explicitly MIT-licensed or compatible.**

## 1. proven_c_lib
*   **Description:** The strict, foundational C23 library upon which `prov` is entirely built.
*   **Homepage (reference only):** [https://github.com/rubidus-api/proven_c_lib](https://github.com/rubidus-api/proven_c_lib)
*   **Sourcing Method (local checkout):**
    *   Vendor `proven` **directly from the local checkout** at `../proven_c_lib` (a sibling of this repository). Do not download it from GitHub for the build.
    *   Copy **only** the `include/`, `src/`, and `platform/` subdirectories — mapping to `include/proven/`, `src/proven/`, and `platform/` here. (`tests/` is optional and not vendored by default.)
    *   **WARNING:** Do NOT copy or overwrite the library's root files (e.g., `nob.c`, `SPEC.md`, `AGENTS.md`, `README.md`), which collide with `prov`'s own root files.
    *   The `proven` source must never be edited within `prov`. For defects, follow the halt-and-report procedure in `AGENTS.md` §10.1 (`../proven_c_lib/docs/REPORT.md`).

## 2. PCRE2 (Perl Compatible Regular Expressions)
*   **Description:** Used for advanced, robust regex search and substitution capabilities (e.g., `ss`, `sox` commands).
*   **Homepage:** [https://www.pcre.org/](https://www.pcre.org/) (GitHub: [https://github.com/PCRE2Project/pcre2](https://github.com/PCRE2Project/pcre2))
*   **Download Method:**
    *   Fetch the latest release source code (`.zip` or `.tar.gz`) from the GitHub Releases page.
    *   Place the core C source files into `vendor/pcre2/`.
    *   These will be statically linked directly via `nob.c`.

## 3. Tree-sitter
*   **Description:** Incremental parsing system for robust, real-time syntax highlighting based on `*.scm` queries.
*   **Homepage:** [https://tree-sitter.github.io/tree-sitter/](https://tree-sitter.github.io/tree-sitter/)
*   **Download Method:**
    *   Fetch the core library from: [https://github.com/tree-sitter/tree-sitter](https://github.com/tree-sitter/tree-sitter)
    *   Extract the contents of `lib/include/` and `lib/src/` to `vendor/tree-sitter/`.
    *   *Language Parsers:* Individual tree-sitter language parsers (e.g., tree-sitter-c, tree-sitter-markdown) should be downloaded separately into `vendor/tree-sitter-parsers/` as needed to support various languages. Statically compile them through `nob.c`.

## 4. nob.h (No Build System)
*   **Description:** A pure C header-only build system to replace Makefile/CMake.
*   **Homepage:** [https://github.com/tsoding/nob.h](https://github.com/tsoding/nob.h)
*   **Download Method:**
    *   Download the raw header file from: [https://raw.githubusercontent.com/tsoding/nob.h/master/nob.h](https://raw.githubusercontent.com/tsoding/nob.h/master/nob.h)
    *   Place it directly in the root directory (alongside `nob.c`).
    *   In practice it is reused from the local `proven` checkout (same public-domain upstream); see `AGENTS.md` §10.

## 5. Terminal Backend — no external library
*   **Decision:** The terminal backend uses a **raw `termios` + ANSI escape** model and requires **no external terminal library** (no ncurses / terminfo). This keeps the editor minimal and gives deterministic control over the cell grid, matching the design philosophy (`SPEC.md` §0).
*   **POSIX:** raw mode via `termios`, window size via `ioctl(TIOCGWINSZ)`, output via ANSI escape sequences. All provided by the system C library / kernel headers; nothing to vendor.
*   **Windows (later):** Win32 Console API (virtual-terminal mode) via the Windows SDK. No vendored dependency.
*   **Isolation:** This OS-specific code lives only under `platform/` (e.g., `platform_term_posix.c`); core `src/` never includes terminal headers (`AGENTS.md` §7).
