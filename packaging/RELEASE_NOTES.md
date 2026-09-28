TodoBench 0.1.4 is available for Linux, Windows, and macOS.

## Downloads

| Operating system | Installer | Portable package |
| --- | --- | --- |
| Linux x86_64 | [AppImage](https://github.com/Alex9001/TodoBench/releases/download/v0.1.4/TodoBench-0.1.4-x86_64.AppImage) | [tar.gz](https://github.com/Alex9001/TodoBench/releases/download/v0.1.4/TodoBench-0.1.4-linux-x86_64.tar.gz) |
| Windows x64 | [Per-user installer](https://github.com/Alex9001/TodoBench/releases/download/v0.1.4/TodoBench-0.1.4-windows-x64-setup.exe) | [ZIP](https://github.com/Alex9001/TodoBench/releases/download/v0.1.4/TodoBench-0.1.4-windows-x64.zip) |
| macOS Apple Silicon | [DMG](https://github.com/Alex9001/TodoBench/releases/download/v0.1.4/TodoBench-0.1.4-macos-arm64.dmg) | [Application ZIP](https://github.com/Alex9001/TodoBench/releases/download/v0.1.4/TodoBench-0.1.4-macos-arm64.zip) |
| macOS Intel | [DMG](https://github.com/Alex9001/TodoBench/releases/download/v0.1.4/TodoBench-0.1.4-macos-x86_64.dmg) | [Application ZIP](https://github.com/Alex9001/TodoBench/releases/download/v0.1.4/TodoBench-0.1.4-macos-x86_64.zip) |

Runtime libraries are bundled. Linux targets Ubuntu 22.04 or compatible newer systems. Windows packages have no publisher certificate. macOS apps are ad-hoc signed and not notarized; see the [installation guide](https://github.com/Alex9001/TodoBench/blob/main/docs/packaging.md#packages-and-unsigned-installation) for first-launch steps.

[SHA-256 checksums](https://github.com/Alex9001/TodoBench/releases/download/v0.1.4/zz-TodoBench-0.1.4-SHA256SUMS.txt) and [corresponding source and build information](https://github.com/Alex9001/TodoBench/releases/download/v0.1.4/zz-TodoBench-0.1.4-source-and-build-info.zip) are also available.

## What changed

- Linux packages now build on Ubuntu 22.04 with GCC 11, Qt 6.8.3, and Rust 1.94.0.
- AppImage naming and validated desktop/AppStream metadata follow the catalog requirements.
- Release publication requires native CI, package checks, Ubuntu 22.04 and 24.04 runtime checks, and the AppImage catalog worker.
- Eight platform downloads are followed by one corresponding-source/build archive and one checksum file.

Application features, workspace formats, and public APIs are unchanged from 0.1.3. Existing 0.1.3 downloads remain available. Windows packages are unsigned; macOS packages are ad-hoc signed and not notarized.

TodoBench is free software under GPL-3.0-or-later.
