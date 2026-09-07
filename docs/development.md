# Development and validation

The required local stage gate is:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
python scripts/check-quality.py --build-dir build/dev
```

Use `cmake --preset asan` for Linux AddressSanitizer/UndefinedBehaviorSanitizer and `cmake --preset release` for a release build.

The native project uses Qt 6.8+ supplied as an SDK and either system packages or vcpkg manifest mode for yaml-cpp, cmark-gfm, and libarchive. CI and release builds pin Qt 6.8.3; this repository does not vendor Qt.

If `VCPKG_ROOT` is set, configure with:

```bash
cmake --preset dev -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
```

Lizard 1.24.0 is required for the cyclomatic gate. A project virtualenv is enough:

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements-dev.txt
.venv/bin/python scripts/check-quality.py --build-dir build/dev
```

Standalone 7-Zip (`7z` or `7zz`) is required for archive interoperability tests.

Packaging entry points and remaining platform gaps are documented in `docs/packaging.md`.

The public persisted-data contract is [workspace-format-v1.md](workspace-format-v1.md).
