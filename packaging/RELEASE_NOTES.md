TodoBench 0.1.5 is available for Linux, Windows, and macOS.

## Downloads

| Operating system | Installer | Portable package |
| --- | --- | --- |
| Linux x86_64 | [AppImage](https://github.com/Alex9001/TodoBench/releases/download/v0.1.5/TodoBench-0.1.5-x86_64.AppImage) | [tar.gz](https://github.com/Alex9001/TodoBench/releases/download/v0.1.5/TodoBench-0.1.5-linux-x86_64.tar.gz) |
| Windows x64 | [Per-user installer](https://github.com/Alex9001/TodoBench/releases/download/v0.1.5/TodoBench-0.1.5-windows-x64-setup.exe) | [ZIP](https://github.com/Alex9001/TodoBench/releases/download/v0.1.5/TodoBench-0.1.5-windows-x64.zip) |
| macOS Apple Silicon | [DMG](https://github.com/Alex9001/TodoBench/releases/download/v0.1.5/TodoBench-0.1.5-macos-arm64.dmg) | [Application ZIP](https://github.com/Alex9001/TodoBench/releases/download/v0.1.5/TodoBench-0.1.5-macos-arm64.zip) |
| macOS Intel | [DMG](https://github.com/Alex9001/TodoBench/releases/download/v0.1.5/TodoBench-0.1.5-macos-x86_64.dmg) | [Application ZIP](https://github.com/Alex9001/TodoBench/releases/download/v0.1.5/TodoBench-0.1.5-macos-x86_64.zip) |

Runtime libraries are bundled. Linux targets Ubuntu 22.04 or compatible newer systems. Windows packages have no publisher certificate. macOS apps are ad-hoc signed and not notarized; see the [installation guide](https://github.com/Alex9001/TodoBench/blob/main/docs/packaging.md#packages-and-unsigned-installation) for first-launch steps.

[SHA-256 checksums](https://github.com/Alex9001/TodoBench/releases/download/v0.1.5/zz-TodoBench-0.1.5-SHA256SUMS.txt) and [corresponding source and build information](https://github.com/Alex9001/TodoBench/releases/download/v0.1.5/zz-TodoBench-0.1.5-source-and-build-info.zip) are also available.

## What changed

- The AppImage embeds a GitHub stable-release update channel for external tools such as AppImageUpdate.
- The matching `.AppImage.zsync` file is published alongside the AppImage and included in the release checksums. Packaging and fresh-runner checks verify that it matches the AppImage.
- Linux continues to target Ubuntu 22.04 with GCC 11 and Qt 6.8.3. Application features, workspace formats, and public APIs are unchanged from 0.1.4.

Download 0.1.5 manually once to gain external updater support; older AppImages have no embedded update channel. TodoBench does not automatically check for or install updates.

TodoBench is free software under GPL-3.0-or-later.
