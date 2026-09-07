# TodoBench workspace format v1

This document and `fixtures/workspace-v1/` define the public interoperability
interface for TodoBench 0.1.0. No bulk migration is required. A workspace is an
ordinary directory, not an application database. Other desktop/mobile clients
may edit it using this contract. Synchronization and automatic merging are not
part of v1.

## Tree and identity

```
settings.json
projects/<project-folder>/project.md
projects/<project-folder>/tasks/<task-folder>/task.md
projects/<project-folder>/tasks/<task-folder>/assets/<attachment>
projects/<project-folder>/projects/<child-project-folder>/...
.todobench/{history,trash,conflicts,recovery}/...
```

Folder names are human-readable hints, not identifiers. TodoBench creates
`slug--uuid` names; readers must accept other portable folder names. Project and
task `id` values are stable UUID strings, conventionally lowercase and without
braces. Never regenerate IDs on import, rename, or move. Duplicate IDs are errors,
not instructions to replace another record. Readers must report invalid UUIDs.

A task's containing `tasks` directory determines its project. Tasks, including
subtasks, each have their own sibling folder. `parent_id` identifies a task in
the same project; null/empty means a root task. Reject missing parents and
cycles. Moving a branch means moving every member's whole task folder, including
attachments, and updating the branch root's parent relationship. Project nesting
is determined by the containing `projects` directory; project `parent_id` is a
redundant hint. Folder membership takes precedence over that hint.

Order is a signed 64-bit integer, ascending among siblings. Default 1024; new
records conventionally use gaps of 1024. Ties use ID lexicographic order. Do not
renumber other records just to inspect or open a workspace.

## Text envelope and ownership

New Markdown and JSON output uses UTF-8 without BOM and LF. Readers accept a
UTF-8 BOM and CRLF. Front matter begins with a complete `---` line at the start
(after an optional BOM) and ends at the next complete `---` line. A closing
line may terminate at EOF. `---suffix` is not a delimiter. The YAML root must
be a mapping with unique scalar keys, including nested mappings. Maximum
metadata nesting is 64. JSON uses strict JSON syntax with unique keys.

Malformed metadata, invalid recognized types/values, and unsupported schema
versions must produce diagnostics and leave the source untouched. Missing
`schema_version` defaults to 1 for existing files; any explicit value other than
the integer 1 is unsupported. Never silently downgrade a newer document.

The Markdown body consists of **every byte after the closing delimiter's line
ending**. Metadata-only edits must preserve it byte-for-byte, including blank
lines, CRLF, trailing spaces, HTML, and final-newline presence. This preservation
rule takes precedence over canonical LF output. Only actual body edits may
normalize body text. Opening files is not an edit. TodoBench's Visual editor
supports a subset of Markdown; unsupported constructs remain editable in Source.

Recognized metadata may be reserialized; YAML comments, quoting, and key order
are not stable. Unknown keys and their complete nested values must survive
recognized-field edits. `x-<application>` mappings are recommended extensions,
not a mandatory naming rule. Unknown fields in recurrence/reminders and nested
settings objects belong to their originating client. Preserve them on edits to
other fields; deleting a whole record or explicitly clearing a schedule removes
that record's extensions too. JSON object arrays use `id`, `command_id`, or `name`
to associate extensions with edited records; clients should keep these keys
unique. Opaque files belong to their producer: preserve them, do not interpret
or delete them opportunistically.

## Task front matter

| Field | Type / supported values | Missing default / meaning |
| --- | --- | --- |
| `schema_version` | integer 1 | 1 |
| `kind` | string `task` | required |
| `id` | UUID string | required; stable identity |
| `title` | string | empty |
| `parent_id` | UUID string or null | null; root task |
| `status` | `todo`, `in_progress`, `waiting`, `done`, `cancelled` | `todo` |
| `previous_open_status` | same status vocabulary | `todo`; status to restore on reopening |
| `priority` | `none`, `low`, `normal`, `high`, `urgent` | `normal` |
| `tags` | sequence of strings | empty sequence |
| `due` | ISO date string `YYYY-MM-DD` or null | null |
| `recurrence` | mapping described below or null | null |
| `reminders` | sequence of mappings described below | empty sequence |
| `order` | signed 64-bit integer | 1024 |
| `created_at` | UTC ISO-8601 timestamp string | empty for legacy files |
| `updated_at` | UTC ISO-8601 timestamp string | empty for legacy files |
| `completed_at` | UTC ISO-8601 timestamp string or null | null |
| `revision` | opaque string, normally a fresh UUID | empty for legacy files |

Timestamp output uses milliseconds and `Z`. Preserve creation time. A task
mutation updates `updated_at` and replaces `revision`; revision is not a counter
or a conflict-resolution clock. Completing sets `completed_at`; reopening clears
it. `project_id`, `source_path`, and `source_hash` are in-memory data and must not
be emitted as TodoBench fields.

### Scheduling

Only date-only due scheduling is supported. Quoted and unquoted ISO date scalars
are equivalent. Date/time mappings from earlier planning documents are not v1.
Reminder delivery uses 09:00 in the workspace's configured IANA timezone (UTC
fallback), minus each reminder's offset. Recurrence calculations are date-based;
TodoBench anchors them at 09:00 UTC internally.

| Recurrence field | Type / values | Default |
| --- | --- | --- |
| `enabled` | boolean | false |
| `mode` | `fixed_calendar`, `after_completion` | `fixed_calendar` |
| `unit` | `days`, `weeks`, `months`, `years` | `days` |
| `interval` | integer 1–10000 | 1 |
| `weekdays` | sequence of integers 1–7 (Monday–Sunday) | empty; ordinary weekly interval |
| `month_day` | integer 0–31 | 0; use anchor day |
| `month` | integer 0–12 | 0; use anchor month |
| `reset_checklist` | boolean | true |

Fixed-calendar recurrence advances from the prior due date, skipping missed
occurrences through the completion date. Completion-relative recurrence advances
from completion. Month/year dates clamp to the last valid day. Weekly selected
days use Monday-based weeks. Completing a recurring task preserves its ID,
snapshots history, advances due, clears completion time, and reopens as `todo`.
With checklist reset enabled, `- [x]` and `- [X]` are changed to `- [ ]` in the
body. Stopping recurrence explicitly clears its mapping.

A reminder has `id` (stable nonempty string, normally UUID) and `minutes_before`
(nonnegative integer, up to 35791394). Missing offsets mean zero in the UI;
entries missing ID or offset are not delivered. IDs must be unique within a
task. Delivery identity is `task_id:task_id@YYYY-MM-DD:reminder_id`. Persisted
`delivered_reminder_keys` prevent duplicate delivery; snoozes map those same
keys to ISO-8601 timestamps. Done/cancelled tasks do not deliver reminders.

## Project front matter

| Field | Type / values | Default |
| --- | --- | --- |
| `schema_version` | integer 1 | 1 |
| `kind` | `project` | required |
| `id` | UUID string | required |
| `parent_id` | UUID string or null | null; containing folder is authoritative |
| `display_name` | string | empty |
| `order` | signed 64-bit integer | 1024 |
| `archived` | boolean | false |

Project notes use the same exact-body rule as task notes. Projects have no
TodoBench revision/timestamp fields in v1; compare content hashes for stale saves.

## Workspace settings

`settings.json` is a JSON object. Missing settings may use defaults; existing
invalid settings must never be replaced with defaults. All values below are
workspace-owned, including UI preferences. Preserve unknown nested values.

| Key | Type / values | Default |
| --- | --- | --- |
| `schema_version` | integer 1 | 1 |
| `workspace_name` | string | empty |
| `timezone` | IANA timezone string | `UTC` |
| `theme` | `system`, `light`, `dark`, `midnight`, `blue`, `green`, `brown`, `amber`, `purple`, `rose`, `paper` | `system` |
| `theme_overrides` | object: theme ID → color-role → color string | `{}` |
| `density` | `comfortable`, `compact` | `comfortable` |
| `keyboard_preset` | `browser`, `total_commander` | `browser` |
| `window_width` | integer pixels, minimum 900 | 1280 |
| `window_height` | integer pixels, minimum 600 | 820 |
| `task_pane_width` | integer pixels, minimum 300 | 500 |
| `toolbar_visible` | boolean | true |
| `saved_views` | array of saved-view objects | `[]` |
| `open_view_tabs` | array of open-tab objects | `[]` |
| `active_view_tab` | nonnegative integer, zero-based index | 0 |
| `delivered_reminder_keys` | array of strings | `[]` |
| `snoozed_reminder_until` | object: delivery key → ISO timestamp | `{}` |
| `formatting_rules` | ordered array of formatting-rule objects | `[]` (UI supplies defaults when empty) |
| `tag_colors` | object: tag → color string | `{}` |
| `project_icons` | object: project UUID → icon path string | `{}` |
| `keyboard_overrides` | array of binding objects | `[]` |

Saved view: `name` string (empty entries ignored), `filter_expression` string
(default empty), `sort` string (default `manual`; also `title`, `priority`, `due`,
`created`, `updated`). Open tabs add `selected_task_id` string (empty),
`scroll_value` integer (0), and `all_tasks` boolean (false). Filters use the
TodoBench filter syntax (`native/src/domain/filter.cpp`); another client may retain
an expression without evaluating it.

Keyboard binding: `command_id` string (empty entries ignored), `shortcut`
Qt portable key-sequence string (empty), `context` string (`global` by default).
Recognized IDs are `task.create`, `task.new_subtask`, `task.duplicate`, `task.move`,
`task.trash`, `task.complete` (context `task_list`), and `view.open`,
`view.close`, `workspace.refresh` (context `global`).

Formatting rule: `name` string (empty entries ignored), `enabled` boolean (true),
`project_id`, `tag`, `title_contains` strings (empty), optional `status` and
`priority` using task vocabulary, optional `overdue` boolean. `appearance` is an
object of optional `foreground`/`background` color strings and `bold`, `italic`,
`strikethrough` booleans. An absent optional predicate does not constrain matches;
an absent appearance value leaves that style unchanged. Rules apply in order.
Colors use Qt color syntax; `#RRGGBB` is the portable recommended representation.
Theme color roles are `window`, `surface`, `alternate`, `text`, `muted`,
`button`, `button_text`, `accent`, `selected_text`, `border`, and `link`.
Custom project icon paths may not be usable on another machine; preserve them.

Machine preferences (recent workspaces, login/tray preferences, instance state)
use platform-local Qt settings, outside the workspace and outside exports.

## Attachments and support material

Links resolve relative to the containing Markdown file. Prefer `assets/name`
and URL-encode spaces/special characters in links. Moving or exporting a task
must retain its whole folder. Never rewrite attachment bytes. Clients should
avoid absolute links for portability; remote links do not imply remote files
belong in the export.

`.todobench/history/<completion-id>/` contains prior task Markdown snapshots.
Trash directories retain moved task folders and `manifest.json`, whose v1
`items` contain `task_id`, `original`, and `stored` paths. Recovery journal JSON
contains `id`, `operation`, `phase` (`started` or `completed`), and `paths`.
Conflicts retain competing versions. These are opaque recovery material to
external editors: include them in snapshots, but do not execute paths or replay
operations from untrusted imported material. No automatic merge is specified.

## Reads, writes, and competing editors

1. Read each file once in binary mode; parse and compute SHA-256 from those same
   bytes. Inspecting must not rewrite files.
2. Coordinate local writers using `.todobench/workspace.lock` (Qt `QLockFile`
   format). TodoBench holds this while a workspace is open; another TodoBench
   process opens read-only. Independent clients should edit while TodoBench is
   closed or honor this lock. Do not delete a live lock. This is local writer
   coordination, not a distributed synchronization protocol.
3. Immediately before replacing an existing task, project, or settings file,
   reread it and compare SHA-256 with the loaded baseline. A revision alone is
   insufficient. A mismatch or deletion is a conflict. Without cooperation from
   every writer, hash checks cannot eliminate the final check/replace race.
4. Clean external changes reload. When local changes compete, leave the current
   disk file intact, retain both versions in memory/recovery/conflicts, and ask
   for explicit resolution. Projects/settings retain disk and local copies in
   `.todobench/conflicts`. Do not treat failed parsing as an empty record.
5. Write a temporary file in the same directory, check the complete write, then
   atomically replace the target (`QSaveFile` in TodoBench). Update the in-memory
   hash only after success. Task mutations update revision and modification
   time. Multi-file operations are not atomic; recovery material must survive.

## Export and import

An export is a standard `.7z` snapshot of the complete workspace tree: attachments,
unknown files, history, trash, conflicts, and recovery included. Exclude only the
transient `.todobench/workspace.lock`. Export outside the workspace. Do not follow
symlinks. Coordinate writers for a consistent snapshot; this is not live sync.

Imports accept contents directly at archive root (including an optional `./`
directory entry), or exactly one enclosing directory with no discarded siblings.
The root must have `settings.json` and a `projects` directory. Validate/extract
into a fresh staging directory alongside the destination. Commit by rename only
after a clean archive EOF, successful data reads/checksums/close, path validation,
and successful filesystem writes. The destination must not already exist.
Failure removes staging and leaves the destination untouched.

Archive paths are relative UTF-8 names. `/` is canonical; legacy `\\` separators
are accepted and normalized. Reject absolute/drive/UNC paths, `..`, symbolic and
hard links, devices and other special files, NUL/control characters, `<>:"|?*`,
trailing spaces/dots, and Windows reserved device names (`CON`, `PRN`, `AUX`,
`NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`, including extensions). Reject duplicate paths
under Unicode normalization/case folding and file/directory conflicts. Extraction
must never overwrite an earlier entry. Readers enforce configurable entry and
extracted-byte limits; defaults are 100,000 entries and 10 GiB of extracted bytes.

## Reference verification

`fixtures/workspace-v1/` includes BOM/CRLF project notes, mixed-line-ending task
notes, a binary attachment, scheduling, nested extensions, and opaque support
files. `scripts/reference-interop.py` uses independent Python/PyYAML and 7-Zip to
extract TodoBench's export, edit metadata, repack, import/save through TodoBench,
and independently verify identities, extension values, bodies, and attachments.
Run it via `ctest --preset dev -R reference_interop`. Malformed metadata and unsafe
archive cases are exercised by `test_interoperability` and `test_archive`.
