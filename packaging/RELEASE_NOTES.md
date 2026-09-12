TodoBench 0.1.2 adds local mdbase conversion and a more capable task workspace.

## What changed

- **Local mdbase import and export.** File-menu wizards convert between TodoBench
  workspaces and mdbase 0.3 collection folders without changing TodoBench's
  native Markdown storage. Import supports type and record selection, field and
  value mapping, project and parent repairs, a result preview, preserved source
  provenance, and direct opening of the new workspace. Export preserves supporting
  files and validates the collection before publishing it.
- **List and Table task layouts.** Each view remembers its layout, visible columns,
  expanded subtasks, selection, filters, and sorting. The task pane now uses square,
  status-colored completion checkboxes and provides row menus plus clearer bulk
  selection controls.
- **Improved task details.** The details pane can be hidden per workspace, restores
  its width, presents status and scheduling controls more directly, and lists
  subtasks and attachments with their actions. Save state and conflicts are easier
  to see, while pending edits are flushed before commands that change tasks.
- **Safer setup and status changes.** Workspace creation handles trailing path
  separators correctly, setup failures use an accessible focused alert, and an
  explicit status choice now keeps the state the user selected.

Website: https://alex9001.github.io/TodoBench/

## Downloads and installation

Linux x86_64: AppImage or bundled portable tarball. Windows x64: portable ZIP or
per-user installer. macOS: separate Intel and Apple Silicon application ZIPs and
DMGs. Runtime libraries are bundled. Linux packages target Ubuntu 24.04 or
compatible newer systems.

Packages have no publisher certificates. macOS apps are ad-hoc signed, without
notarization. See the included installation guide for first-launch instructions.
Verify downloads against SHA256SUMS. Exact source, dependency source, third-party
notices, and build instructions accompany the binaries.

## Workspace compatibility

Existing workspaces require no migration. The workspace format remains schema
version 1. New view-presentation preferences are optional and older settings keep
their defaults. mdbase import always creates a new workspace, and export always
creates a new collection; neither operation changes its source.

TodoBench is free software under GPL-3.0-or-later.
