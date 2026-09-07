# Build, install, and verify TodoBench

TodoBench is GPL-3.0-or-later. Public source:
<https://github.com/Alex9001/TodoBench>. The CMake project version is authoritative;
`scripts/version.py` reads it for release naming. Release binaries all come from
the same tagged commit. See [workspace v1](workspace-format-v1.md) for portable
editing and snapshots.

## Build from source

Install CMake 3.24+, Ninja, a C++20 compiler, Qt 6.8.3 (Core/Gui/Widgets/Svg/Network),
yaml-cpp, libarchive with 7-Zip/LZMA support, and optionally cmark-gfm. Native
Windows builds use MSVC and the pinned `vcpkg.json` baseline. Linux/macOS may use
system packages as shown in `.github/workflows/ci.yml`. Set `CMAKE_PREFIX_PATH`
to your Qt SDK and dependency prefixes. Install Python and 7-Zip for tests.

```
python -m pip install -r requirements-dev.txt
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev --output-on-failure
python scripts/check-quality.py --build-dir build/dev
```

On Windows, configure from the MSVC Developer shell and add
`-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`. On Linux, the
`asan` preset enables AddressSanitizer and UndefinedBehaviorSanitizer. Release
configuration is `cmake --preset release`. Exact workflow commands and dependency
versions accompany each source release. Dynamic Qt libraries can be replaced by
compatible user-built libraries; no publisher key is required to run modifications.

## Packages and unsigned installation

Check `SHA256SUMS` after downloading. Linux/macOS: `sha256sum -c SHA256SUMS` or
`shasum -a 256 -c SHA256SUMS`. Windows: compare `Get-FileHash -Algorithm SHA256`
with the corresponding line. Download only from the project's release page.

- **Linux x86_64:** make the AppImage executable and launch it. If FUSE is
  unavailable, set `APPIMAGE_EXTRACT_AND_RUN=1`. The portable `.tar.gz` bundles
  the same runtime tree; extract it and run its `TodoBench` launcher. Keep the
  extracted directory intact. Packages target Ubuntu 24.04 or compatible newer
  systems with the required glibc and desktop display libraries.
- **Windows x64:** extract the ZIP and run `TodoBench.exe`, or run the per-user
  installer. It installs under `%LOCALAPPDATA%\TodoBench` without administrator
  privileges. These binaries have no publisher certificate; Windows may show
  an unknown-publisher/SmartScreen prompt. Inspect the source/checksum and use
  **More info → Run anyway** if you choose to proceed. Uninstall from Windows
  Settings. Workspaces outside the installation folder remain your files.
- **macOS Intel / Apple Silicon:** choose the matching architecture. Extract the
  ZIP or mount the DMG and copy `TodoBench.app` to Applications. The app has an
  ad-hoc signature, without Apple notarization. After attempting to open it,
  use **System Settings → Privacy & Security → Open Anyway** if macOS blocks it.
  Updates or local modifications may require re-signing with
  `codesign --force --deep --sign - TodoBench.app`.

## Release procedure

The release workflow creates a draft, tests native Linux, Windows, Intel macOS,
and Apple Silicon macOS builds, and stages packages. Linux sanitizer and quality
checks and workflow lint are required. Package smoke checks verify startup and
runtime dependencies. Native tests exercise workspace round trips. Each package
job records theme screenshots at normal and double scale for review.

The final gate downloads staged assets, verifies their checksum inventory and
required filenames, checks the exact source commit, and publishes only after all
required jobs succeed. A failed platform leaves the release in draft. Publisher
certificates are intentionally not used. See the workflow run for validation
logs; a green startup test does not assert every desktop integration was manually
exercised.

Launcher assets are generated from `packaging/icons/todobench.svg` by
`scripts/generate-icons.py` with CairoSVG/Pillow. Lucide 0.468.0 SVGs and their
ISC/Feather MIT notices are vendored under `native/resources/icons/lucide/`.
