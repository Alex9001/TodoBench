TodoBench 0.1.3 is available for Linux, Windows, and macOS.

## Downloads

| Operating system | Installer | Portable package |
| --- | --- | --- |
| Linux x86_64 | [AppImage](https://github.com/Alex9001/TodoBench/releases/download/v0.1.3/TodoBench-0.1.3-linux-x86_64.AppImage) | [tar.gz](https://github.com/Alex9001/TodoBench/releases/download/v0.1.3/TodoBench-0.1.3-linux-x86_64.tar.gz) |
| Windows x64 | [Per-user installer](https://github.com/Alex9001/TodoBench/releases/download/v0.1.3/TodoBench-0.1.3-windows-x64-setup.exe) | [ZIP](https://github.com/Alex9001/TodoBench/releases/download/v0.1.3/TodoBench-0.1.3-windows-x64.zip) |
| macOS Apple Silicon | [DMG](https://github.com/Alex9001/TodoBench/releases/download/v0.1.3/TodoBench-0.1.3-macos-arm64.dmg) | [Application ZIP](https://github.com/Alex9001/TodoBench/releases/download/v0.1.3/TodoBench-0.1.3-macos-arm64.zip) |
| macOS Intel | [DMG](https://github.com/Alex9001/TodoBench/releases/download/v0.1.3/TodoBench-0.1.3-macos-x86_64.dmg) | [Application ZIP](https://github.com/Alex9001/TodoBench/releases/download/v0.1.3/TodoBench-0.1.3-macos-x86_64.zip) |

Runtime libraries are bundled. Linux targets Ubuntu 24.04 or compatible newer systems. Windows packages have no publisher certificate. macOS apps are ad-hoc signed and not notarized; see the [installation guide](https://github.com/Alex9001/TodoBench/blob/main/docs/packaging.md#packages-and-unsigned-installation) for first-launch steps.

[SHA-256 checksums](https://github.com/Alex9001/TodoBench/releases/download/v0.1.3/SHA256SUMS) and [corresponding source and build information](https://github.com/Alex9001/TodoBench/releases/download/v0.1.3/zz-TodoBench-0.1.3-source-and-build-info.zip) are also available.

## What changed

- Startup logs, **Help → Application Diagnostics**, and `--diagnostics` provide version, architecture, runtime, and startup-stage information. `--safe-start` bypasses remembered workspace restoration. Existing-instance activation now acknowledges requests and forwards an explicitly requested workspace.
- Task and project commands share session undo/redo with conflict checks, transactional writes, bounded backups, and retained evidence if rollback fails. Trash restore resolves projects by identity and handles destination collisions.
- New and renamed folders use readable names. Rename and move commands update local Markdown links while retaining stable task and project IDs.
- Search includes notes; daily due filters, bulk priority and status changes, archived project filtering, and the persistent reminder inbox improve daily workflows.
- Workspace monitoring runs scans in the background; task edits retain unrelated rows and reject stale scan results.

## macOS tester packet

The [macOS tester packet](https://github.com/Alex9001/TodoBench/releases/download/v0.1.3/zz-TodoBench-0.1.3-macos-tester-packet.zip) contains both DMGs, installation and test instructions, checksums, and build results. Send that one link to a tester. The original reported macOS startup failure remains unconfirmed; tester feedback has not yet been collected.

## Verification and compatibility

All Linux, Windows, and macOS native build and test jobs, Linux sanitizer and quality checks, and package verification passed for [source commit `ed10b657`](https://github.com/Alex9001/TodoBench/commit/ed10b65776e366e853482a29be871aad3aa6f722) in [this workflow run](https://github.com/Alex9001/TodoBench/actions/runs/35794883909). Both macOS architectures and both package formats passed native Cocoa/LaunchServices launch scenarios, plus signing, architecture, deployment-target, and bundled-library checks.

Workspace metadata remains schema version 1; existing workspaces need no bulk migration. New settings are optional. Renaming a task or project can rename its folder, so keep a backup. Undo history is session-only. Trash manifests use version 2; legacy manifests remain readable.

TodoBench is free software under GPL-3.0-or-later.
