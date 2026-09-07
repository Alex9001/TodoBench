#!/bin/sh
set -eu
version=${1#v}
build_dir=${2:-build/release}
output_dir=${3:-dist}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ "$version" = "$(python3 "$root/scripts/version.py")" ] || { echo "version differs from CMake" >&2; exit 1; }
[ -x "$build_dir/AppDir/AppRun" ] || "$root/scripts/package-linux-appimage.sh" "$version" "$build_dir" "$output_dir"
mkdir -p "$output_dir"
package_name="TodoBench-$version-linux-x86_64"
package_dir="$output_dir/$package_name"
cmake -E rm -rf "$package_dir"
cp -a "$build_dir/AppDir" "$package_dir"
cat > "$package_dir/TodoBench" <<'LAUNCHER'
#!/bin/sh
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$root/AppRun" "$@"
LAUNCHER
chmod +x "$package_dir/TodoBench"
tar -C "$output_dir" -czf "$output_dir/$package_name.tar.gz" "$package_name"
(cd "$output_dir" && sha256sum "$package_name.tar.gz" > "$package_name.sha256")
"$root/scripts/smoke-packaged.sh" "$package_dir/TodoBench"
