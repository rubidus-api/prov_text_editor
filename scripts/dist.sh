#!/bin/sh
# Build the release/size-reduced binaries for every platform on the remote build
# host, which has the cross toolchains (mingw-w64, aarch64-linux-gnu,
# arm-linux-gnueabihf). The repository tree is shared with that host (same
# volume), so `./nob dist` writes straight into bin/ here:
#
#   bin/prov-linux-x64   bin/prov-linux-arm64   bin/prov-linux-armhf
#   bin/prov-windows-x64.exe
#
# Targets whose toolchain is missing on the host are skipped, not fatal. Run
# `./nob dist` locally to build just the native (linux-x64) one.
#
# Connection settings are environment-specific; override via env vars.
set -e

HOST="${PROV_WIN_HOST:?set PROV_WIN_HOST, e.g. user@host}"
PORT="${PROV_WIN_PORT:-22}"
KEY="${PROV_WIN_KEY:-}"
REMOTE_DIR="${PROV_WIN_REMOTE_DIR:?set PROV_WIN_REMOTE_DIR to the repo path on the build host}"

KEY_OPT=""
[ -n "$KEY" ] && KEY_OPT="-i $KEY"

ssh $KEY_OPT -p "$PORT" "$HOST" \
    "cd '$REMOTE_DIR' && cc -o nob nob.c && ./nob dist"

echo "built dist binaries (shared tree):"
ls -l bin/prov-* 2>/dev/null || echo "note: run from the repo root to see bin/prov-*"
