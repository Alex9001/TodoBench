# TodoBench

TodoBench is a native C++20/Qt desktop task manager whose workspace is an ordinary directory of Markdown files.

## Development setup

Requirements:

- CMake 3.24+
- Ninja
- Qt 6.8+ (CI/release SDK: Qt 6.8.3)
- vcpkg with `VCPKG_ROOT` set
- Python 3.10+
- `pip install -r requirements-dev.txt` (Lizard and PyYAML)
- 7-Zip (`7z` or `7zz`) for independent interoperability tests
- `clang-tidy` for the cognitive-complexity gate

Configure and build with vcpkg manifest mode:

```bash
cmake --preset dev -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build --preset dev
ctest --preset dev --output-on-failure
python scripts/check-quality.py --build-dir build/dev
```

The first configure may install the manifest dependencies. Qt is intentionally not part of `vcpkg.json`; provide the separately pinned Qt SDK through the normal CMake package path.

## Running TodoBench

Start without an argument to choose or create a workspace from the native File menu, or open one directly:

```bash
./build/dev/TodoBench /path/to/workspace
```

The main window keeps project/view tabs, instant filters, and the hierarchical task list on the left. The selected task's project, state, priority, tags, due date, recurrence, reminders, and Visual/Source/History Markdown editor are on the right. Interface layout, colors, project icons, timezone, and Browser or Total Commander keysets are stored in the workspace settings file.

Sanitizer and release configurations are available as `asan` and `release` presets. Build output is under `build/<preset>` and the compile database is `build/<preset>/compile_commands.json`.

## First launch and sample workspaces

On first launch, a guided setup lets you create a workspace or open one you already have. You can return through **Help > Getting started**; **File > New Workspace** starts at the sample chooser.

Choose a starting point, preview its tasks, select the workspace folder, choose a theme and keyboard preset, and review everything before creating files:

| Starting point | What you get |
| --- | --- |
| Start empty | An empty Inbox for your own work |
| Everyday to-dos | 7 tasks for shopping, appointments, friends and household jobs |
| Move into a new home | 15 tasks for bookings, packing, address changes and the first night |
| Team project | 12 tasks for drafting, reviewing and handing off a team handbook |
| WordPress client build | 15 tasks for business intake, assets, staging, manual construction and checks |
| Client website build | 20 tasks for business intake, site direction, scaffolding, implementation, checks and launch |
| Website care and local search | 19 tasks for GMB corrections, weekly posting, client approval, search reports and agreed content updates |
| Guided feature tour | 13 exercises covering notes, filters, subtasks, recurrence, attachments, backup and task states |

Samples use fictional people and ordinary working notes. Dates are relative to creation day, and sample reminders are off. The first task explains the scenario. All examples are editable Markdown files you can keep, change or move to Trash; exporting the workspace includes their attachments. Updated starting points apply to newly created samples. Existing workspaces keep their contents.

Files are written **directly into your chosen folder**, without adding a folder based on the workspace name. New workspaces require an empty or new directory. Nothing is created on cancellation, and failed sample creation leaves no partially populated workspace.

TodoBench remembers the last successfully opened folder and reopens it on launch, including saved tabs and filters. An explicitly supplied command-line folder takes precedence. If the last folder is missing or disconnected, setup explains the problem and lets you locate it or create another workspace. Recent folder paths are machine-local; the tasks and workspace preferences stay in your workspace folder.

## Open and restore tabs

Click **Open tab…** beside the task tabs to choose a project or saved view. Closing a tab leaves its project and tasks intact, so it remains available in this menu. Open views have a checkmark; selecting one switches to its tab. Nested projects show their full path. **Custom view…** opens the advanced tab dialog.

## Readable project filters

Project tabs and saved views show names in the search box, for example `project:Tutorial` or `project:"Tutorial / Launch example"`. Edit these names alongside title words and tags. Nested projects use their full path, and identical paths receive a numbered suffix. Saved filters retain internal project IDs so a rename preserves their scope. A deleted project's filter reads “Missing project” and continues to match no tasks.

## Themes and custom colors

Use **View > Appearance** for System, Light, Dark, Midnight, Blue, Green, Brown, Amber, Purple, Rose, and Paper. Light has an explicitly white editor; System restores the desktop appearance captured when the application starts. Named presets use Qt's Fusion widget style so their colors remain consistent under desktop skins.

Choose **View > Appearance > Customize theme** or **Edit > Settings > Appearance** to adjust backgrounds, text, buttons, selection, borders, and links. Click a swatch for the color picker or enter `#RRGGBB`; leave a field blank to inherit its preset. Each preset remembers its own overrides in the workspace's `settings.json`. The preview updates immediately, flags low text/selection contrast, and offers individual and whole-preset resets. **OK** applies; **Cancel** discards changes. Task formatting rules and tag colors remain separate.

## Workspace format

A workspace contains `settings.json` and a `projects/` tree. Each project has `project.md`; each task has its own directory containing `task.md` and optional `assets/`. YAML front matter is parsed with yaml-cpp and the Markdown body is retained independently, so metadata-only saves do not rewrite note content.

The authoritative [workspace v1 specification](docs/workspace-format-v1.md) and
[fixtures](docs/fixtures/workspace-v1) explain how other clients can safely edit
workspaces. See [build and installation instructions](docs/packaging.md).

## License

Copyright © 2026 TodoBench contributors. TodoBench is free software under
**GPL-3.0-or-later**: you may redistribute and modify it under version 3 of the
GNU General Public License, or any later version. It comes without any warranty.
See [LICENSE](LICENSE) and [third-party notices](packaging/THIRD_PARTY_NOTICES.md).
Release source and build instructions accompany the binaries.
