# TodoBench: implementation plan for Luna

Execution review, 2026-09-10: the current UI/settings/onboarding changes were independently reviewed and corrected. The living verification record is [progress.md](progress.md); left-pane checkbox acceptance and screenshot evidence are tracked in [left-pane-checkbox-handoff.md](left-pane-checkbox-handoff.md). This review does not mark the entire cross-platform product specification complete.

## 1. Handoff, decisions, and working rules

Build a native desktop task manager for Linux, Windows, and macOS. Its workspace is an ordinary directory containing Markdown task files, attachments, and readable settings. Exporting and importing a workspace uses standard `.7z` archives.

The user will launch Luna through a separate application. Do not attempt to launch another implementing agent.

### Confirmed product decisions

| Area | Decision |
| --- | --- |
| Task storage | One Markdown file per task, grouped into project folders |
| Portability | The complete workspace directory is portable and can be archived as `.7z` |
| Text format | No `todo.txt` compatibility requirement |
| Projects | Nested projects; each task has one home project |
| Cross-project organization | Tags, filters, and saved views |
| Task details | Visual editor plus Markdown source mode |
| Supported formatting | Standard Markdown formatting; no arbitrary fonts or text colors in stored notes |
| Task-list appearance | Automatic formatting rules; no manual styling of individual rows |
| Task features | Notes, checklists, independent subtasks, attachments, priorities, due dates, recurrence, reminders |
| Task states | To do, In progress, Waiting, Done, Cancelled |
| Parent completion | Ask when unfinished subtasks remain |
| Recurrence | Fixed calendar schedules and intervals after completion |
| Missed repetitions | Keep one overdue occurrence; advance without generating a backlog |
| Recurring identity | Reuse the task and keep completion history |
| Background operation | Reminders work while the app runs in the tray |
| External changes | Support external editors and folder-sync tools |
| Preferences | One shared interface and keyboard setup stored with the workspace |
| Keyboard presets | Browser and Total Commander, with customization |
| Browser tabs | Ctrl+T opens a view; Ctrl+N creates a task |
| Dependency updates | Dependabot with guarded auto-merge |
| Code quality | Mandatory cyclomatic and cognitive complexity gates |
| First-run onboarding | Guided create/open flow, realistic selectable workflows (simple list, home move, client delivery, product launch, guided tour) or empty Inbox; preview, direct destination, appearance/controls and final review; no writes until Finish |

### Reference projects

Use these repositories as design and engineering references:

- `/home/user/Documents/CODE/CyberSnapper`: native menu and toolbar organization, adjustable panes, toolbar customization, and visual spacing.
- `/home/user/Documents/CODE/whodis`: platform-native Qt styling, packaging, CI, and Dependabot automation.

Their existing helper processes serve their particular applications. TodoBench should use one application process with background worker threads.

### Luna's execution contract

1. Read this entire plan before changing application code.
2. Create `progress.md` before implementation begins.
3. Implement stages in dependency order.
4. Announce role changes in the progress log using `Role: Architect`, `Role: Implementer`, `Role: Reviewer`, `Role: QA`, and `Role: Release Engineer`. These are stages of Luna's work, not claims of independent reviewers.
5. After each stage, review the diff, run its acceptance checks, fix failures, and update progress.
6. Continue between stages without requesting routine implementation approval.
7. Do not replace specified behavior with placeholders or mark a feature complete because its controls exist.
8. Do not weaken tests, lint thresholds, or required CI checks to obtain a passing result.
9. Record unavailable checks as `not run`, with the reason. Distinguish implementation completion from platform validation.
10. Do not publish releases, push release tags, or launch another implementing agent as part of this handoff.

Create `progress.md` with this structure:

```markdown
# TodoBench implementation progress

Current stage:
Current role:
Next concrete action:

| Stage | Status | Implementation evidence | Validation evidence | Blocker |
| --- | --- | --- | --- | --- |

## Work log

### YYYY-MM-DD — Stage ID — Role
- Changes:
- Commands and results:
- Review findings and fixes:
- Remaining work:
```

Allowed stage statuses are `pending`, `in_progress`, `blocked`, and `done`. Keep this file as the specification and `progress.md` as the execution record.

## 2. Product and persistence contracts

### 2.1 Workspace layout

Use a small directory for each task so its attachments move with it:

```text
My Workspace/
  settings.json
  projects/
    work--<project-id>/
      project.md
      tasks/
        review-release--<task-id>/
          task.md
          assets/
            <asset-id>-screenshot.png
      projects/
        website--<project-id>/
          project.md
          tasks/
            ...
  .todobench/
    history/
    trash/
    conflicts/
    recovery/
```

Rules:

- IDs are UUIDs and remain stable across renames, moves, archive round trips, and recurring completions.
- Names before IDs are readable slugs. Renaming a task title does not automatically rename its directory.
- Project Rename updates its display name; provide a separate **Rename folder to match** operation.
- Task membership comes from its containing project directory.
- Subtask relationships come from task IDs, not nested task directories.
- Moving a parent task through the app moves its entire subtask branch.
- Moving a subtask to another project detaches it from its former parent and moves its descendants with it.
- An Inbox project is created with every new workspace.
- Index only active task directories. History, trash, and conflict copies must not become ordinary tasks.
- Unknown files remain untouched and travel with workspace exports.

There is no authoritative database. An in-memory index supplies the application's views and filters.

### 2.2 Task and project Markdown

Use YAML front matter followed by the Markdown body. Parse YAML with `yaml-cpp`; do not write a custom YAML parser.

A task contains:

```yaml
---
schema_version: 1
kind: task
id: "<uuid>"
title: "Review the release"
parent_id: null
status: todo
previous_open_status: todo
priority: normal
tags: ["release", "work"]
due: null
recurrence: null
reminders: []
order: 1024
created_at: "2026-09-04T20:00:00Z"
updated_at: "2026-09-04T20:00:00Z"
completed_at: null
revision: "<uuid>"
---

Notes and checklists go here.
```

Define these values:

- `status`: `todo`, `in_progress`, `waiting`, `done`, `cancelled`.
- `priority`: `none`, `low`, `normal`, `high`, `urgent`; default `normal`.
- `parent_id`: another active task in the same project, or `null`.
- `previous_open_status`: the state restored when reopening a completed task.
- `order`: integer ordering among siblings, initially spaced by 1024.
- `revision`: a new UUID after a successful mutation. Actual disk conflict checks use content hashes, not this field alone.

Additional structured fields:

| Field | Contract |
| --- | --- |
| `due` | `null`, or date, optional local time, and an IANA timezone |
| `recurrence` | Enabled flag, calendar/after-completion mode, interval, unit, applicable weekday/month-day fields, checklist-reset flag, and last completion identifier |
| `reminders` | Stable reminder IDs and offsets before the due time |
| Project metadata | Schema version, kind, UUID, display name, order, archived flag |

Store project metadata in `project.md`. Project icons live in workspace settings, keyed by stable project ID.

Preserve unknown front-matter fields when editing recognized fields. Preserve the Markdown body exactly when only metadata changes. Reading or selecting a task must not rewrite it.

Future schema versions open read-only with an explanation. Invalid files appear in workspace diagnostics with their paths and remain untouched.

### 2.3 Settings and portability

Use one readable, versioned `settings.json` containing:

- Workspace name and timezone.
- Tag display names and colors.
- Project icon assignments.
- Formatting rules and their order.
- Saved views and open view tabs.
- Theme, density, fonts, splitter proportions, columns, and toolbar arrangement.
- Keyboard preset and overrides.
- Reminder preferences.
- Archive resource limits.

Settings are shared across computers, as requested. Clamp restored window dimensions to the current screen.

Keep only machine-operational state outside the workspace: last successfully opened workspace and recent paths, process locks, disposable caches, notification-delivery receipts, and OS startup registration. These must not be needed to recover tasks or reconstruct preferences.

Starting at login is an explicit option on each computer. Importing a workspace must not silently register the app for startup.

### 2.4 Saving, external edits, and recovery

All filesystem mutations go through one storage service.

- Autosave after 500 ms of editing inactivity.
- Flush before switching workspaces, exporting, or quitting.
- Show `Saving`, `Saved`, or a specific error.
- Preserve unsaved text when a write fails.
- Use `QSaveFile` with direct-write fallback disabled. Its atomic replacement protects against partially written files; it does not provide coordination with external writers.
- Compare the current disk hash with the version loaded for editing before committing.
- Retain recovery copies for pending edits and multi-file operations.
- Use a journal for branch moves, project moves, tag renames, recurring branch resets, and trash/restore operations.
- On restart, inspect incomplete journals before enabling further writes to affected records.

External-change handling:

1. Watch project/task directories and the actively edited files.
2. Re-establish watches after replacements.
3. Reconcile on application activation and with a lightweight periodic scan.
4. Reload clean records when they change externally.
5. If a dirty record also changes externally, preserve both versions and stop autosaving that record.
6. Present a native comparison dialog: use disk version, use local version, or edit a merged result. Retain the losing version in conflicts/recovery.
7. Recognize duplicate task IDs, including sync-generated conflict copies. Show the conflicting records and offer resolution or **Import as a separate task**.
8. Surface broken parent relationships and cycles; do not silently drop affected tasks from the interface.
9. Treat synced concurrent edits as conflict detection and recovery, not distributed locking.

Use a local lock per workspace to prevent two local writers. A second local opening should offer read-only access or focus the existing instance.

### 2.5 Trash, history, and attachments

- Delete moves task bundles to workspace trash and supports undo.
- Deleting a parent includes its descendants and shows the affected count.
- Restore returns items to their original project when possible; otherwise use Inbox and report that relocation.
- Permanent deletion is available only through the trash interface with confirmation.
- Do not automatically empty trash or completion history.

Attachments:

- Copy selected or dropped files into the task's `assets` directory.
- Use unique filenames and ordinary relative Markdown links.
- Pasted images become local attachments.
- Moving a task bundle preserves its internal attachment links.
- Render local images; external images remain placeholders unless explicitly opened outside the app.
- Opening a file attachment requires an explicit user action.
- Retain assets referenced by completion history.

History snapshots record completed recurring occurrences, including the relevant task branch and attachment bytes. Snapshot active content only; do not recursively copy earlier history.

### 2.6 Window and interaction layout

Use a native `QMainWindow` with this organization:

```text
Operating-system title bar
Native application menu
Wide primary action toolbar + compact secondary actions

┌──────────────────────────────┬─────────────────────────────────┐
│ Project / saved-view tabs    │ Selected task title and metadata│
│ Title search and filter chips│ Markdown formatting toolbar     │
│ List controls and quick add  │ Visual | Source | History       │
│                              │                                 │
│ Task tree / task list        │ Task detail editor              │
│                              │                                 │
└──────────────────────────────┴─────────────────────────────────┘

Status bar; optional function-key button strip
```

Defaults:

- Initial window: approximately 1280 × 820 logical pixels.
- Minimum usable window: 900 × 600.
- Horizontal splitter: 50/50 initially; draggable and remembered.
- Preserve the active platform's Qt style.
- Offer System, Light, Dark, Midnight, Blue, Green, Brown, Amber, Purple, Rose, and Paper themes; System is the default. Named presets must render consistently regardless of desktop skin.
- Appearance settings provide a live preview, color pickers, editable hex values, per-role and per-preset reset, and portable per-preset overrides. Cancel discards changes. Light must be explicitly light.
- Offer Compact and Comfortable density; Comfortable is the default.
- Use platform fonts by default.
- Primary buttons have text and icons, a minimum height of 36 logical pixels, and a minimum width of 88.
- Secondary buttons have accessible names, hover tooltips, and current shortcut labels.
- Toolbar overflow must preserve access to every action.

Menus:

- **File:** create/open/switch workspace, import/export archive, import Markdown, open workspace folder, quit.
- **Edit:** context-aware undo/redo, clipboard actions, find.
- **Task:** new task/subtask, complete/reopen, state, priority, tags, move, duplicate, trash.
- **Project:** create, rename, archive, choose icon, open folder.
- **View:** tabs, saved views, filters, columns, sorting, pane arrangement, density.
- **Tools:** formatting rules, settings, conflict resolution, trash.
- **Help:** local help, shortcut reference, about.

Settings use a native dialog with pages for Workspace, Appearance, Layout, Tags, Projects, Formatting Rules, Keyboard, Reminders, and Archives.

Layout settings include pane orientation, pane visibility, splitter reset, column visibility/order/width, row density, task-title wrapping, toolbar order, and toolbar visibility.

### 2.7 Task list and detail behavior

Use a model/view task tree, not a separate widget for every row.

Default columns:

- Completion/state indicator.
- Title.
- Priority.
- Due date.
- Tags.
- Project, visible by default in views spanning projects.

Provide:

- Inline task creation and title editing.
- Expand/collapse subtasks.
- Multiple selection and bulk commands.
- Drag-and-drop reparenting and ordering.
- Context menus using the same actions as menus and toolbars.
- Indicators for notes, attachments, recurrence, reminders, and unfinished descendants.
- Sorting by manual order, priority, due date, title, created time, or updated time.

Manual drag ordering is enabled only in manual sort. Otherwise explain that the active sort determines placement.

Quick creation uses the active project when exactly one is selected; otherwise it uses Inbox. It does not infer task metadata from arbitrary search words. If a newly created task does not match the current filter, keep its detail open with a clear **Outside this view** indicator and a **Show Task** action.

The detail pane includes title, state, project breadcrumb, tags, priority, due date/time, recurrence, and reminder controls. Secondary metadata can collapse to preserve editing space.

### 2.8 Visual and source editors

Implement a native visual editor and a native Markdown source editor over one edit-session buffer.

Toolbar commands:

- Heading levels 1–3 and normal paragraph.
- Bold, italic, strikethrough.
- Bulleted and numbered lists.
- Checklists.
- Block quote.
- Inline code and fenced code block.
- Link.
- Image/attachment.
- Table insertion and row/column operations.

Source mode edits the Markdown body. Front matter remains represented by the task metadata controls; external editors may edit the complete file.

Use `QTextEdit`/`QTextDocument` for native visual editing and `QPlainTextEdit` for source. Use `cmark-gfm` to inspect Markdown structure and verify supported conversions rather than implementing a Markdown parser.

Preservation requirements:

- Switching modes without editing must not change the file.
- Formatting syntax may normalize after an actual visual edit, but content and supported semantics must survive.
- Detect constructs the visual editor cannot preserve. Keep those notes editable in source mode and provide a read-only preview instead of silently converting them.
- Preserve links, image paths, code contents, task checkboxes, table contents, and list nesting.
- Undo/redo must continue to work across autosaves.
- Editor shortcuts must not trigger task deletion or other list commands.

### 2.9 Filtering and saved views

Filtering must operate on the in-memory index, with no disk access per keystroke.

Provide visible controls for:

- Title words.
- Tags, with Any/All matching.
- Project selection and inclusion of subprojects.
- Task states.
- Priority.
- Due-date ranges.

Defaults:

- Title matching is case-insensitive and Unicode-normalized.
- Unquoted words are ANDed.
- Quoted phrases match contiguous title text.
- Multiple selected tags use Any unless changed to All.
- Selected projects are ORed; their combined result is ANDed with other filters.
- Include subprojects by default.
- Default task-state filter includes To do, In progress, and Waiting.
- Show active filters as removable chips and provide **Clear Filters**.

Support an optional typed syntax for `tag:`, `project:`, `status:`, `priority:`, and `due:`. Both typed syntax and picker controls must compile to the same filter representation. Invalid syntax shows an inline error and retains the last valid results.

Tabs represent views, not independent copies of tasks:

- Keep an unclosable All Tasks tab.
- Ctrl+T opens a searchable project/saved-view picker.
- A view remembers its filters, sort, selected task, and scroll position.
- Closing a view never deletes tasks or projects.
- A project picker can open a project view or a custom view spanning multiple projects.

Performance acceptance on a documented development machine:

- With 10,000 tasks indexed, title filtering reaches visible results within 100 ms at the 95th percentile.
- Target filter computation below 50 ms.
- Update externally changed active records within approximately one second of receiving the filesystem event.
- Initial scanning happens off the UI thread with progress; the window remains interactive.
- Do not claim unlimited scale or scan all Markdown bodies on every keystroke.

### 2.10 Automatic formatting rules

Provide a native rule editor with name, enabled state, conditions, appearance, ordering, and live preview.

Conditions:

- Project, including descendants.
- Tag membership.
- Task state.
- Priority.
- Title text.
- Overdue, due today, or due within a specified number of days.

Appearance properties:

- Foreground color.
- Background tint.
- Bold.
- Italic.
- Strikethrough.
- Optional supplementary badge from a bundled icon set.

Evaluate rules from top to bottom. The first matching rule that sets a particular property owns that property; later rules can supply other unset properties.

Default rules:

1. Done: muted text and strikethrough.
2. Cancelled: muted text and strikethrough, with the cancelled indicator.
3. Open and overdue: red emphasis.
4. Open and due today: amber emphasis.
5. Urgent: bold text.

Selection, keyboard focus, and readable contrast override decorative colors. Tag chips retain their configured colors. Rules never change stored task state or edit note formatting.

### 2.11 Keyboard presets and conflict handling

Every operation has a stable command ID. Menus, toolbars, context menus, and keyboard bindings invoke the same command implementation.

`Primary` means Ctrl on Linux/Windows and Command on macOS. Preserve platform-standard text editing and menu behavior.

| Command | Browser preset | Total Commander preset |
| --- | --- | --- |
| New task | Primary+N | Shift+F4 |
| New subtask | Primary+Alt+N | Primary+Alt+N |
| New project | Primary+Shift+N | F7 |
| Open view tab | Primary+T | Primary+T |
| Close view tab | Primary+W | Primary+W |
| Restore closed view | Primary+Shift+T | Primary+Shift+T |
| Next/previous view | Ctrl+Tab / Ctrl+Shift+Tab | Ctrl+Tab / Ctrl+Shift+Tab |
| Focus task search | Primary+L | Alt+F7 |
| Complete/reopen | Space in task list | Space in task list |
| Rename title | F2 | Shift+F6 |
| Refresh workspace | F5 | F2 |
| Read task preview | Primary+Enter | F3 |
| Focus task editor | Enter in task list | F4 |
| Duplicate task | Primary+D in task list | F5 |
| Move task | Primary+Shift+M | F6 |
| Move to trash | Delete in task list | F8 |
| Help | F1 | F1 |
| Quit application | Primary+Q | Primary+Q |

Additional rules:

- Browser is the initial preset.
- Primary+F searches the focused editor when editing notes; otherwise it focuses task search.
- Escape dismisses transient UI or cancels the current inline edit.
- Text fields consume ordinary editing keys before list commands.
- The Total Commander preset enables an optional bottom function-key strip by default.
- F10 retains platform menu activation behavior.
- Changing presets updates menu labels, tooltips, and the function-key strip immediately.
- Custom bindings are stored as overrides.
- Detect conflicts within overlapping contexts and require resolution before applying changes.
- Reset one binding or the whole preset without changing task data.

### 2.12 Completion, recurrence, and reminders

#### Completion

If unfinished descendants remain, present:

- Complete the whole branch.
- Complete only this task.
- Cancel.

Show unfinished checklist items in the same decision when applicable. Completing only the parent leaves unfinished descendants visible and independently actionable.

#### Recurrence

Support:

- Every N days.
- Every N weeks on selected weekdays.
- Every N months on a specified day.
- Every N years on a specified month/day.
- N days/weeks/months/years after completion.

Rules:

- One outstanding occurrence per recurring task.
- Fixed schedules retain their calendar anchor.
- On completion, skip missed slots and select the next future occurrence.
- After-completion schedules use the completion date/time.
- Clamp unavailable month days to the month's last day; February 29 becomes February 28 in non-leap years.
- Store timezone identifiers and test daylight-saving transitions.
- For ambiguous local times choose the earlier occurrence; for nonexistent times move forward through the transition gap.
- Date-only tasks become overdue after their due day in the workspace timezone.

On recurring completion:

1. Resolve the parent-completion decision.
2. Save a completion-history snapshot.
3. Keep task IDs, notes, tags, attachments, and project membership.
4. Advance the due date.
5. Return the recurring task to To do.
6. Reset its embedded checklist by default; expose a recurrence setting to disable that reset.
7. For a repeating parent branch, reset completed descendants for the next cycle. Preserve unfinished descendants when the user chose parent-only completion; leave cancelled descendants cancelled.
8. Commit the operation through the recovery journal.

A recurring branch has one recurrence owner. Prevent overlapping recurrence rules on a task and its ancestors/descendants, with an explanation in the recurrence editor.

Provide **Complete and stop repeating**. Cancelling a task disables its recurrence. Undo must restore the prior occurrence and history state.

#### Reminders

- Reminders are offsets before a due time; a due date is required.
- Date-only tasks use 09:00 in the workspace timezone as the reminder anchor.
- Allow multiple reminders per task.
- Support snooze for 10 minutes, one hour, and tomorrow at 09:00.
- Deliver notifications while the process is running, including in the tray.
- Maintain an in-app reminder list because desktop notification visibility depends on system settings.
- After sleep or restart, summarize missed reminders instead of emitting an unbounded sequence of notifications.
- Deduplicate delivery per device and occurrence.
- Closing the window hides to tray when a usable tray is present.
- If the tray is unavailable, keep the window accessible and explain the available close behavior.
- Explicit Quit flushes changes and stops reminders.
- Reminders operate for the currently open workspace.

### 2.13 `.7z` import and export

Use `libarchive` with LZMA support to produce and read genuine `.7z` archives. Verify interoperability with the standalone 7-Zip tool.

Export:

1. Flush pending edits and finish local transactions.
2. Choose a destination outside the workspace.
3. Create a stable staging snapshot while local mutations are paused.
4. Verify that source files did not change during snapshot creation; retry or report a changed workspace.
5. Include settings, projects, tasks, attachments, history, trash, conflicts, and other persisted files.
6. Write a temporary archive, verify it can be read, and rename it to the chosen destination.
7. Show progress and allow cancellation without leaving a final-looking partial archive.

Import:

- Accept an archive containing the workspace root directly or inside one enclosing directory.
- Extract into a new staging directory, validate the workspace, then rename it into a new destination.
- Never silently merge into or overwrite the currently open workspace.
- Preserve the existing workspace when import fails.
- Reject traversal paths, absolute paths, links, device entries, duplicate normalized paths, and platform-incompatible filenames.
- Bound actual extracted bytes and entry counts, not just declared sizes.
- Default limits: 10 GiB extracted data and 100,000 entries; expose limits in archive settings.
- Support unencrypted archives in the first release. Explain encrypted-archive handling through manual extraction and **Open Workspace**.

A workspace manually archived and extracted by 7-Zip must remain usable without an application-specific conversion step.

## 3. Architecture, quality gates, and delivery

### 3.1 Implementation stack

Use:

- C++20.
- CMake 3.24 or newer and Ninja.
- Qt 6.8 minimum; pin release SDKs to Qt 6.11.2.
- Qt Core, Gui, Widgets, Svg, Concurrent, Test, and Network for local instance communication.
- `yaml-cpp` for front matter.
- `cmark-gfm` for Markdown structure and conversion checks.
- `libarchive` with LZMA support for archives.
- Python for development and packaging checks only.

Use vcpkg manifest mode for native third-party libraries. Initial `builtin-baseline`:

```text
04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4
```

Keep Qt as the separately pinned SDK. Avoid building a second copy of Qt through the native dependency manifest.

Separate:

- **Domain:** task/project models, hierarchy, filtering, formatting rules, recurrence.
- **Storage:** codecs, scanning, hashes, saves, transactions, history, trash, archives.
- **Application services:** commands, edit sessions, workspace lifecycle, reminders.
- **Widgets:** window composition, views, delegates, dialogs, editor surfaces.
- **Platform integration:** tray, startup registration, notifications, local instance handling.

The domain layer must not depend on Widgets. Widgets must not write files directly. Filesystem work and archive operations run off the GUI thread; Qt item models are updated on the GUI thread.

Minimum internal contracts:

| Contract | Responsibility |
| --- | --- |
| `TaskRecord` / `ProjectRecord` | Parsed domain data with stable IDs |
| `WorkspaceSnapshot` | Indexed records and diagnostics |
| `TaskPatch` | Explicit mutation of selected fields |
| `SaveResult` | Saved, conflict, or error, with relevant revision/path |
| `FilterSpec` | One shared representation for UI filters and typed filters |
| `FormattingRule` | Conditions and optional appearance properties |
| `EditSession` | Original bytes/hash, current buffer, mode, dirty state |
| `CommandRegistry` | Command IDs, actions, shortcuts, contexts, enabled states |
| `WorkspaceStore` | Serialized mutations and filesystem reconciliation |
| `Clock` / notification adapter | Deterministic scheduling tests and platform delivery |

### 3.2 Mandatory complexity lint

**These are blocking acceptance criteria, not advisory checks.**

- Cyclomatic complexity: **maximum 15 per function**.
- Cognitive complexity: **maximum 25 per function**.
- Enforce cyclomatic complexity with pinned **Lizard 1.24.0**.
- Enforce C++ cognitive complexity with clang-tidy's `readability-function-cognitive-complexity`.
- Cover handwritten application source, headers, and tests. Include Python project scripts in Lizard coverage.
- Exclude only third-party source and generated/build output.

The underlying cyclomatic command is:

```bash
python -m lizard --no-gitignore \
  -l cpp -l python \
  -C 15 -i 0 -w \
  native/src native/tests scripts
```

Configure clang-tidy with:

```yaml
Checks: '-*,readability-function-cognitive-complexity'
WarningsAsErrors: 'readability-function-cognitive-complexity'
CheckOptions:
  readability-function-cognitive-complexity.Threshold: 25
```

Implement a `scripts/check-quality.py` entry point that:

- Verifies tools and the compilation database exist.
- Enumerates the expected handwritten source files.
- Runs both complexity checks.
- Fails if source discovery unexpectedly returns no application functions.
- Reports file, function, observed complexity, and limit.
- Returns nonzero if either gate fails.

Luna must not:

- Raise thresholds.
- Add complexity suppressions or whitelist files.
- Exclude difficult application modules.
- Hide failures with `continue-on-error`, `|| true`, or warning allowances.
- Move complex logic into large lambdas, macros, or generated-looking files to evade checks.

Refactor around cohesive operations and testable domain functions. Keep window construction separate from persistence, recurrence, and filtering.

Add a small gate-verification test that proves:

- A function at the cyclomatic limit passes.
- A function above it fails.
- A function exceeding cognitive complexity fails independently.
- Missing tools and empty source discovery fail clearly.

### 3.3 Local validation commands

Provide these working entry points:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
python scripts/check-quality.py --build-dir build/dev
```

Provide a sanitizer preset for Linux and a release preset. Document dependency setup and exact output paths.

The standard stage gate is:

1. Build affected targets.
2. Run relevant tests.
3. Pass complexity lint.
4. Review the diff for unintended file writes, weak error handling, and command-context mistakes.
5. Record results in `progress.md`.

### 3.4 CI and Dependabot

Required CI must include:

- Native build and tests on Linux, Windows, and macOS.
- Complexity lint.
- Linux AddressSanitizer and UndefinedBehaviorSanitizer runs.
- Archive interoperability tests.
- Workflow lint.
- A final required gate that fails when any required upstream job fails or is unexpectedly skipped.

Pin third-party GitHub Actions to commit SHAs with readable version comments.

Configure monthly Dependabot updates for:

- `github-actions`.
- `pip`, including development lint dependencies.
- `vcpkg`.

GitHub's vcpkg integration updates the manifest's `builtin-baseline`; treat those pull requests as native dependency-set changes requiring review.

Guarded auto-merge:

- Verify the pull request author and Dependabot metadata.
- Permit eligible patch updates and explicitly identified security-update groups.
- Keep major updates and native baseline updates manual.
- Queue squash auto-merge only after required checks and repository rules can enforce the gate.
- Never check out or execute pull-request code in a privileged `pull_request_target` job.
- Document the repository settings required for protected-branch enforcement.
- Do not claim auto-merge is active until those settings are actually configured.

Use whodis's automation as the reference, removing its unrelated Go and container jobs.

### 3.5 Release packages

Prepare packages for x64 and ARM64 on all three operating systems:

- Linux: AppImage and portable archive.
- Windows: per-user installer and portable ZIP.
- macOS: architecture-specific application bundles in DMG/ZIP packages.

Bundle required Qt plugins and native runtime dependencies. Include third-party notices and license texts.

Produce checksums, dependency inventories/SBOMs, and build provenance using the whodis packaging approach.

Every packaged build must smoke-test:

- Application startup.
- Create/open workspace.
- Save and reopen a task.
- Archive export/import.
- Editor image rendering.
- Tray capability handling.

Use `TodoBench` as the application name and MIT as the project license, following whodis. Code copied from reference projects retains applicable notices. These are planner-selected defaults.

Signing credentials, store submissions, a website, and an application self-updater are outside this implementation handoff.

## 4. Implementation stages and acceptance tests

Every stage begins with an implementation role and ends with Reviewer and QA roles. A stage cannot be marked done while its required checks fail.

| Stage | Lead role | Implementation and required exit evidence |
| --- | --- | --- |
| **S0 — Foundation and gates** | Architect | Create project structure, CMake presets, dependency manifests, progress log, test targets, quality wrapper, and initial CI. Build a minimal native window. Prove complexity gates reject deliberately failing fixtures. |
| **S1 — Workspace format** | Implementer | Implement task/project codecs, settings, scanning, stable IDs, hierarchy validation, and diagnostics. Round-trip representative files without changing untouched bodies or losing unknown fields. |
| **S2 — Reliable storage** | Implementer | Add serialized saves, hashes, autosave support, recovery journals, trash/restore, external watching, and conflict copies. Demonstrate write-failure and interrupted-operation recovery. |
| **S3 — Native shell and task commands** | Implementer | Build menus, toolbar, splitter, task model/view, metadata pane, quick add, project hierarchy, and bulk operations. Create, modify, move, complete, trash, restore, and reopen tasks through actual storage. |
| **S4 — Markdown editor and assets** | Implementer | Add visual/source modes, formatting commands, tables, checklists, local images, attachment insertion, and preservation checks. Pass the editor corpus and moving-task attachment tests. |
| **S5 — Filtering and views** | Implementer | Implement compiled filters, filter chips, project/saved-view tabs, state preservation, sorting, and the 10,000-task benchmark. Record latency and test-machine details. |
| **S6 — Appearance and controls** | Implementer | Add automatic rule editing, tag colors, project icons, shared layout settings, keyboard presets, overrides, conflict detection, and the optional function-key strip. Verify every action from keyboard and menus. |
| **S7 — Completion history and recurrence** | Implementer | Implement completion decisions, stable recurring identity, snapshots, branch resets, schedule calculations, and undo. Pass fixed-clock tests for missed dates, month ends, leap years, and timezone transitions. |
| **S8 — Reminders and desktop lifecycle** | Implementer | Add tray behavior, optional startup registration, notification adapters, snooze, missed-reminder summaries, and delivery deduplication. Verify close, quit, restart, sleep/resume, and unavailable-tray behavior. |
| **S9 — Portable archives** | Implementer | Implement staged `.7z` export/import, progress, cancellation, validation, limits, and interoperability. Prove a restored workspace retains task identities, attachments, settings, history, and trash. |
| **S10 — Platform packaging** | Release Engineer | Complete platform CI and packaging. Validate installed/portable layouts and bundled dependencies. Record actual OS/architecture results and remaining external validation honestly. |
| **S11 — Final acceptance** | Reviewer / QA | Execute the end-to-end scenarios below, resolve defects, rerun required gates, capture native UI evidence, and finish user/build documentation. Report remaining limitations explicitly. |

### Required acceptance scenarios

#### Persistence and external tools

- Create nested projects and tasks; close and reopen the app.
- Edit metadata and Markdown using an external editor.
- Replace files using an editor's temporary-file/rename save pattern.
- Delete or move an externally edited task.
- Introduce a duplicate task ID and a sync conflict copy.
- Change a file while the app has unsaved edits.
- Simulate a full disk or denied write.
- Interrupt a multi-file operation and restart.
- Verify all competing recoverable versions remain available.

#### Task workflows

- Complete a parent with unfinished descendants using each offered choice.
- Reopen a task and restore its prior open state.
- Move and duplicate complete branches.
- Filter out a newly created task and use **Show Task**.
- Archive a project without deleting its files.
- Trash and restore a branch whose original parent/project has changed.

#### Editor

- Round-trip headings, nested lists, tables, code fences, links, images, and checklists.
- Switch visual/source modes without editing and confirm byte preservation.
- Change only metadata and confirm the Markdown body is unchanged.
- Open unsupported markup without silently losing it.
- Move tasks containing image and attachment links.
- Verify typing Delete, Space, and formatting shortcuts never triggers list actions.

#### Filtering and controls

- Combine title phrases, tags, projects, status, and due filters.
- Verify Any/All tag matching and nested project inclusion.
- Confirm invalid typed filters keep the last valid results.
- Exercise browser tab creation, cycling, closure, and restoration.
- Test both presets and conflicting custom bindings.
- Run keyboard-only task creation, editing, completion, moving, and recovery.

#### Recurrence and reminders

- Complete overdue weekly tasks after several missed weeks.
- Complete a scheduled task early.
- Repeat monthly tasks on the 29th–31st.
- Test leap years and both daylight-saving transitions.
- Complete a recurring parent using whole-branch and parent-only behavior.
- Undo recurring completion and inspect history.
- Restart and resume after missed reminders without duplicate notification storms.
- Verify explicit Quit stops reminders.

#### Archives and presentation

- Export, extract with 7-Zip, and open the extracted workspace.
- Archive manually with 7-Zip and import through the app.
- Cancel export/import and verify the original workspace remains usable.
- Reject malformed and unsafe archive entries.
- Test System, Light, and Dark themes at 100%, 150%, and 200% scaling.
- Test narrow windows, long titles, many tags, toolbar overflow, and keyboard focus.
- Inspect packaged builds on each supported platform.

### Definition of done

The implementation is complete only when the specified workflows work through real persistence, required complexity lint passes, relevant automated tests pass, and platform validation is accurately recorded. No placeholder controls, silent conversion losses, concealed lint failures, or fabricated test results remain.
