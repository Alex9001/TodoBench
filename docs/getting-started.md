# Getting started with TodoBench

[Download TodoBench](https://alex9001.github.io/TodoBench/#download) · [Installation help](packaging.md) · [Back to the overview](../README.md)

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

## Tasks, layouts, and details

Each tab starts in **List**, with a completion control beside the task title and metadata underneath. The disclosure arrow expands subtasks; **Space** completes or reopens the current task. Use the row's **…** menu, right-click, or **Shift+F10** for status, move, duplicate, subtask, and trash commands. **Status** offers all five states; completing a repeating task or a task with unfinished subtasks uses the same behavior from every entry point.

Use the active tab's header for **Add task**, **Filter**, **Sort**, **List / Table**, and **Show / Hide details**. Table keeps comparison columns, with **Columns** choosing which metadata to show. Switching layouts retains selection, filters, sorting, and expanded subtasks. Multiple selection reveals **Change selected…**, with all five statuses, priorities, and Trash. Bulk completion affects selected tasks only and advances repeating tasks; bulk Trash includes descendants. Each bulk command is one undo entry. These choices and the details pane width are remembered with the workspace.

The task title is editable at the top of details. Project and parent links navigate above it; property controls open their pickers nearby. Notes retain **Visual**, **Source**, and **History**, with save feedback beside the editor. **Subtasks** and **Attachments** show counts and their own Add controls. Activate an item with **Enter** or a double-click; attachment menus offer Open, Copy Markdown link, and Open containing folder.

Closing details keeps it closed as you select other tasks. **Enter**, a double-click on a task title, or **Show details** opens it again. Workspace commands remain in the workspace toolbar and **File** menu; settings, appearance, keyboard preferences, and help remain in the application menus.

## Convert to and from mdbase

Use **File → Export as mdbase…** to convert the open workspace into a new mdbase v0.3 collection folder. Choose a path that does not exist and is outside the workspace. The dialog calculates attachment and source-size estimates in the background. TodoBench saves pending edits first, preserves supporting files, validates the completed collection, and publishes it only after validation succeeds.

Use **File → Import from mdbase…** with or without another workspace open. Choose a v0.3 collection, select its types and individual records, map its fields plus every observed status and priority, review the exact result, and choose a new workspace folder. The source scan runs in the background and can be cancelled. Per-record project and parent choices resolve relationship conflicts in the review. Import never merges into an existing folder. The source collection is retained byte-for-byte under the new workspace's import provenance. See [mdbase transfer v1](mdbase-transfer-v1.md) for the complete mapping and preservation rules.

## Readable project filters

Project tabs and saved views show names in the search box, for example `project:Tutorial` or `project:"Tutorial / Launch example"`. Edit these names alongside title words and tags. Nested projects use their full path, and identical paths receive a numbered suffix. Saved filters retain internal project IDs so a rename preserves their scope. A deleted project's filter reads “Missing project” and continues to match no tasks.

## Themes and custom colors

Use **View > Appearance** for System, Light, Dark, Midnight, Blue, Green, Brown, Amber, Purple, Rose, and Paper. Light has an explicitly white editor; System restores the desktop appearance captured when the application starts. Named presets use Qt's Fusion widget style so their colors remain consistent under desktop skins.

Choose **View > Appearance > Customize theme** or **Edit > Settings > Appearance** to adjust backgrounds, text, buttons, selection, borders, and links. Click a swatch for the color picker or enter `#RRGGBB`; leave a field blank to inherit its preset. Each preset remembers its own overrides in the workspace's `settings.json`. The preview updates immediately, flags low text/selection contrast, and offers individual and whole-preset resets. **OK** applies; **Cancel** discards changes. Task formatting rules and tag colors remain separate.


## Check a workspace for problems

**Help → Workspace Diagnostics** reports the last workspace scan. It checks task
and project metadata, duplicate IDs, and subtask relationships. A healthy workspace
shows **No workspace problems found**, with its folder and loaded project/task
counts. If no workspace is open, the dialog explains how to open one first.

When problems are found, expand **Show Details** to see their paths and messages.
Duplicate tasks retain the **Import as a separate task** recovery action. After
editing files with another tool, use **File → Refresh** to scan again. Diagnostics
does not inspect attachment contents or verify backups.

## Undo and folder names

Use **Edit → Undo** or **Ctrl+Z** (**Cmd+Z** on macOS) outside text fields to undo
the latest task or project action. **Edit → Redo** uses the platform's standard
redo shortcut. Inside text fields, these shortcuts always belong to the editor,
even when its text history is empty.

Task creation, duplication, edits, attachments, completion, recurrence, bulk
changes, reordering, moves, deletion, and restoration share one timeline with
project creation, renaming, and archiving. Consecutive autosaves in the same detail
editing session form one action. Selecting another task, leaving the detail pane,
explicitly saving, or performing another command ends that group. Pending edits
must save successfully before workspace undo or redo can run.

History lasts for the current session, keeps at most 100 actions (within a bounded
backup budget), survives refresh, and clears when a different workspace opens.
Restarting the application starts an empty history. Settings, view preferences,
and whole-workspace import/export are outside undo. If external changes conflict
with undo, TodoBench stops at that action and explains which file needs attention.

New folders use readable names such as `projects/inbox/tasks/buy-milk/`, with
`-2`, `-3`, and subsequent suffixes for collisions. Saving a changed title also
renames its folder and updates local Markdown links. IDs remain stable. Existing
workspaces are not bulk-renamed when opened.


## Daily views and search

**View → Today**, **Overdue**, and **Upcoming** also appear in **Open tab…**.
They show unfinished tasks; Upcoming covers tomorrow through the next seven days.
Dates follow the workspace time zone and update when the day changes. These views
save relative filters, so reopening a Today tab does not pin it to yesterday.
Search words match task titles and Markdown notes across the current view.
Search does not index attachment contents. Advanced filters accept `due:today`,
`due:overdue`, and `due:upcoming` alongside existing tags and states.

## Manage projects and recover tasks

**Project → Rename / Archive** uses the project scoped by the current filter or
asks which project to use. Empty projects work without selecting a task.
**Project → Archived Projects…** restores archived projects, even after restart.
Archived projects and their descendants are excluded from normal task views and
reminders; `include_archived:true` includes them in an advanced filter.

**Task → Restore from Trash…** shows task titles, deletion times, and the original
project names before restoration. Existing folders are preserved if names collide.
When an external edit conflicts with your draft, the dialog displays both full
copies, including properties, alongside a merged-notes editor. Saving merged notes
keeps the draft's properties. If the disk copy changes again during review, reopen
the comparison before choosing a version.

## Review and snooze reminders

**Task → Reminders** shows a persistent count and list of due and missed reminders.
Open a task, snooze its reminder, or dismiss it. Clicking a desktop notification
opens this list. A reminder can be snoozed after it has already fired; snoozes and
dismissals survive restart. Completed, cancelled, and archived tasks do not notify.
Reminders run while TodoBench is running; missed reminders appear when its workspace
next opens. Due times use 09:00 in the workspace time zone.

Launching TodoBench with a workspace folder while it is already running opens that
folder in the existing window, using the same unsaved-edit checks as File → Open.
