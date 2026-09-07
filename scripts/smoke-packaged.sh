#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: smoke-packaged.sh <TodoBench-binary-or-AppImage>" >&2
    exit 2
fi

binary=$1
[ -x "$binary" ] || { echo "not an executable: $binary" >&2; exit 1; }

"$binary" --version | grep -q TodoBench
"$binary" --help | grep -q TodoBench

timeout_bin=$(command -v timeout || true)
if [ -n "$timeout_bin" ]; then
    smoke_state=$(mktemp -d)
    trap 'rm -rf -- "$smoke_state"' EXIT
    mkdir -p "$smoke_state/config" "$smoke_state/data" "$smoke_state/cache" "$smoke_state/tmp"
    set +e
    XDG_CONFIG_HOME="$smoke_state/config" XDG_DATA_HOME="$smoke_state/data" \
        XDG_CACHE_HOME="$smoke_state/cache" TMPDIR="$smoke_state/tmp" APPIMAGE_EXTRACT_AND_RUN=1 QT_QPA_PLATFORM=offscreen "$timeout_bin" --signal=TERM --kill-after=2s 4s "$binary"
    status=$?
    set -e
    if [ "$status" -ne 124 ] && [ "$status" -ne 0 ]; then
        echo "packaged binary exited unexpectedly with status $status" >&2
        exit 1
    fi
fi

echo "Packaged binary started and reported version TodoBench."
echo "Create/open workspace, editor image rendering, and tray handling still require an interactive session."
