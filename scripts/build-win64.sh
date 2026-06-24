#!/bin/sh
# Cross-build prov for Windows x64 on the remote build host.
#
# The repository tree is shared with the build host (same volume), and
# `./nob win64` writes the binary to bin/prov.exe inside this tree, so after
# this runs the .exe is present at bin/prov.exe. Binaries are not copied to the
# repository root (see AGENTS.md §5).
#
# Connection settings are environment-specific; override via env vars.
set -e

HOST="${PROV_WIN_HOST:?set PROV_WIN_HOST, e.g. user@host}"
PORT="${PROV_WIN_PORT:-22}"
KEY="${PROV_WIN_KEY:-}"
REMOTE_DIR="${PROV_WIN_REMOTE_DIR:?set PROV_WIN_REMOTE_DIR to the repo path on the build host}"

KEY_OPT=""
[ -n "$KEY" ] && KEY_OPT="-i $KEY"

# mingw-w64 (x86_64-w64-mingw32-gcc) must be installed on the build host.
ssh $KEY_OPT -p "$PORT" "$HOST" \
    "cd '$REMOTE_DIR' && cc -o nob nob.c && ./nob win64"

echo "built bin/prov.exe (shared tree)"
ls -l bin/prov.exe 2>/dev/null || echo "note: run from the repo root to see bin/prov.exe"
