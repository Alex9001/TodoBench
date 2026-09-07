#!/bin/sh

set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
    echo "usage: package-macos.sh <version> [build-directory] [output-directory]" >&2
    exit 2
fi

version=${1#v}
build_dir=${2:-build/release}
output_dir=${3:-dist}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

[ "$version" = "$(python3 "$root/scripts/version.py")" ] || exit 1

command -v cmake >/dev/null 2>&1 || { echo "cmake is required" >&2; exit 1; }
command -v macdeployqt >/dev/null 2>&1 || { echo "macdeployqt is required" >&2; exit 1; }

case "$(uname -m)" in
    x86_64) release_arch=x86_64 ;;
    arm64) release_arch=arm64 ;;
    *) echo "unsupported macOS architecture: $(uname -m)" >&2; exit 1 ;;
esac

mkdir -p "$output_dir"
output_dir=$(CDPATH= cd -- "$output_dir" && pwd)

cmake --preset release -B "$build_dir" -DTODOBENCH_BUNDLE_RUNTIME=ON
cmake --build "$build_dir" --parallel

stage="$build_dir/macos-stage"
cmake -E rm -rf "$stage"
cmake --install "$build_dir" --prefix "$stage"

bundle=$(find "$stage" -maxdepth 2 -name 'TodoBench.app' -print -quit)
[ -n "$bundle" ] || { echo "TodoBench.app was not installed" >&2; exit 1; }
macdeployqt "$bundle" -always-overwrite
mkdir -p "$bundle/Contents/Resources/doc"
cp -R "$stage/share/doc/TodoBench/." "$bundle/Contents/Resources/doc/"
cp "$root/LICENSE" "$root/packaging/THIRD_PARTY_NOTICES.md" "$bundle/Contents/Resources/" 2>/dev/null || \
    cp "$root/LICENSE" "$root/packaging/THIRD_PARTY_NOTICES.md" "$(dirname "$bundle")"

codesign --force --deep --sign - "$bundle"
codesign --verify --deep --strict "$bundle"
"$bundle/Contents/MacOS/TodoBench" --version

zip_name="TodoBench-${version}-macos-${release_arch}.zip"
( cd "$(dirname "$bundle")" && zip -qry "$output_dir/$zip_name" "$(basename "$bundle")" )
( cd "$output_dir" && shasum -a 256 "$zip_name" > "TodoBench-${version}-macos-${release_arch}.sha256" )
echo "Created $output_dir/$zip_name"

if command -v hdiutil >/dev/null 2>&1; then
    dmg_name="TodoBench-${version}-macos-${release_arch}.dmg"
    dmg_stage="$build_dir/dmg-stage"
    cmake -E rm -rf "$dmg_stage"
    mkdir -p "$dmg_stage"
    cp -R "$bundle" "$dmg_stage/"
    ln -s /Applications "$dmg_stage/Applications"
    hdiutil create -volname "TodoBench" -srcfolder "$dmg_stage" -ov -format UDZO "$output_dir/$dmg_name"
    ( cd "$output_dir" && shasum -a 256 "$dmg_name" > "TodoBench-${version}-macos-${release_arch}-dmg.sha256" )
    echo "Created $output_dir/$dmg_name"
else
    echo "hdiutil was not found; application ZIP was created and DMG packaging was skipped."
fi
echo "Application signed ad-hoc; no publisher certificate or notarization."
