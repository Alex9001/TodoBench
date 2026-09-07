#!/bin/sh

set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: package-linux-appimage.sh <version> <build-directory> <output-directory>" >&2
    exit 2
fi

version=${1#v}
build_dir=$2
output_dir=$3
linuxdeploy=${LINUXDEPLOY:-linuxdeploy}
qmake=${QMAKE:-}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

[ "$version" = "$(python3 "$root/scripts/version.py")" ] || { echo "version differs from CMake" >&2; exit 1; }
mkdir -p "$output_dir"

command -v cmake >/dev/null 2>&1 || { echo "cmake is required" >&2; exit 1; }
command -v "$linuxdeploy" >/dev/null 2>&1 || {
    echo "linuxdeploy is required to build an AppImage; it was not found in PATH" >&2
    exit 1
}
command -v sha256sum >/dev/null 2>&1 || {
    echo "sha256sum is required to publish an AppImage checksum" >&2
    exit 1
}
if [ -n "$qmake" ]; then
    qmake=$(command -v "$qmake" 2>/dev/null || printf '%s' "$qmake")
else
    qmake=$(command -v qmake6 || command -v qmake-qt6 || true)
fi
[ -n "$qmake" ] || {
    echo "a Qt 6 qmake is required for AppImage deployment; set QMAKE or install qmake6" >&2
    exit 1
}
[ -x "$qmake" ] || { echo "Qt 6 qmake is not executable: $qmake" >&2; exit 1; }
[ -d "$output_dir" ] || { echo "output directory does not exist: $output_dir" >&2; exit 2; }

case "$(uname -m)" in
    x86_64|amd64) release_arch=x86_64 ;;
    aarch64|arm64) release_arch=aarch64 ;;
    *) echo "unsupported Linux architecture: $(uname -m)" >&2; exit 1 ;;
esac

# linuxdeploy's Qt plugin performs the AppImage runtime bundling.  Avoid CMake's
# host-wide Qt deployment pass, which can encounter third-party plugins without
# an ELF RPATH entry on the build machine.
cmake --preset release -B "$build_dir" -DTODOBENCH_BUNDLE_RUNTIME=OFF
cmake --build "$build_dir" --parallel

app_dir="$build_dir/AppDir"
cmake -E rm -rf "$app_dir"
DESTDIR="$app_dir" cmake --install "$build_dir" --prefix /usr

# Keep the headless platform available for CI and diagnostic smoke tests; the
# linuxdeploy Qt plugin normally deploys only the host's default X11 plugin.
qt_plugins=$("$qmake" -query QT_INSTALL_PLUGINS)
offscreen_plugin="$qt_plugins/platforms/libqoffscreen.so"
test -f "$offscreen_plugin" || {
    echo "Qt 6 offscreen platform plugin was not found: $offscreen_plugin" >&2
    exit 1
}
mkdir -p "$app_dir/usr/plugins/platforms"
cp "$offscreen_plugin" "$app_dir/usr/plugins/platforms/"

desktop="$app_dir/usr/share/applications/todobench.desktop"
icon="$app_dir/usr/share/icons/hicolor/scalable/apps/todobench.svg"
test -f "$desktop"
test -f "$icon"

output="$output_dir/TodoBench-${version}-linux-${release_arch}.AppImage"
# appimagetool treats AppStream warnings as errors; this project has no public homepage yet.
QMAKE="$qmake" NO_STRIP=1 LDAI_NO_APPSTREAM=1 OUTPUT="$output" "$linuxdeploy" \
    --appdir "$app_dir" \
    --desktop-file "$desktop" \
    --icon-file "$icon" \
    --plugin qt \
    --output appimage
test -f "$output"
APPIMAGE_EXTRACT_AND_RUN=1 "$output" --appimage-version >/dev/null || \
    APPIMAGE_EXTRACT_AND_RUN=1 "$output" --version >/dev/null
cp "$root/LICENSE" "$root/packaging/THIRD_PARTY_NOTICES.md" "$output_dir/"
( cd "$output_dir" && sha256sum "$(basename "$output")" > "$(basename "$output").sha256" )
echo "Created $output"
