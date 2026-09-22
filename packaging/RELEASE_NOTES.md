TodoBench 0.1.3 macOS beta adds startup diagnostics and safer workspace editing.

## What changed

- Startup logs, **Help → Application Diagnostics**, and `--diagnostics` provide
  version, architecture, runtime, and startup-stage information. `--safe-start`
  bypasses remembered workspace restoration. Existing-instance activation now
  acknowledges requests and forwards an explicitly requested workspace.
- Task and project commands share session undo/redo with conflict checks,
  transactional writes, bounded backups, and retained evidence if rollback fails.
  Trash restore resolves projects by identity and handles destination collisions.
- New and renamed folders use readable names. Rename and move commands update
  local Markdown links while retaining stable task/project IDs.
- Search includes notes; daily due filters, bulk priority/status changes, archived
  project filtering, and the persistent reminder inbox improve daily workflows.
- Workspace monitoring runs scans in the background; task edits retain unrelated
  rows and reject stale scan results.

## Beta downloads and installation

The macOS tester packet contains both Intel and Apple Silicon DMGs, instructions,
checksums, and build provenance. Choose the installer matching your Mac.
Separate macOS application ZIPs, Linux packages, and Windows packages also accompany
this release. Runtime libraries are bundled.

macOS apps are ad-hoc signed and **not notarized**. See `instructions.md` in the
packet for installation and first-launch guidance. Verify downloads against
SHA256SUMS. Exact source, dependency source, notices, and build instructions
accompany the binaries. This prerelease is for testing and is not the latest
stable release.

## Verification and remaining uncertainty

Publication requires all native build/test jobs, Linux sanitizer and quality
checks, and package verification to pass. Both macOS architectures and both
package formats require Cocoa/LaunchServices checks for first launch, explicit
workspace, unavailable remembered workspace, safe start, and existing-instance
activation, plus signing, architecture, deployment-target, and library checks.
See the packet's build-info.txt for the exact commit, workflow, and results.

These automated results are separate from tester feedback. The cause of the
original reported macOS startup failure remains unconfirmed.

## Workspace compatibility

The workspace metadata schema remains version 1; existing workspaces need no
bulk migration. New settings are optional. Renaming a task or project can rename
its folder. Test on a copy of your workspace and keep a backup. Undo history is
session-only. Trash manifests use version 2; legacy manifests remain readable.

TodoBench is free software under GPL-3.0-or-later.
