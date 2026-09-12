# mdbase Transfer v1

Status: **implemented in TodoBench 0.1.2**

This document describes TodoBench's in-app mdbase import and export commands. They convert between TodoBench's native Markdown workspace format and the [mdbase specification v0.3.0](https://github.com/mdbase-dev/mdbase-spec) collection format.

## Overview

Two new File-menu commands:

- **Export as mdbase…** — converts the current workspace into a new mdbase collection directory.
- **Import from mdbase…** — converts an existing mdbase collection into a new TodoBench workspace.

Both commands leave the source data unchanged. The export creates a complete collection outside the workspace. The import creates a brand-new workspace directory.

## Requirements

- A TodoBench build that includes mdbase transfer support.
- No external tools, network, or developer runtimes are required while importing or exporting. The Rust bridge is statically linked into TodoBench. The TypeScript oracle is a development-only test dependency and is not shipped.

## Export

### What gets exported

- All tasks, subtasks, projects, notes, and attachments — including completed, cancelled, and archived items.
- Asset files (images, attachments) are preserved byte-for-byte.
- Markdown body content is preserved byte-for-byte.
- Native metadata (due dates, recurrence rules, order, timestamps, revisions, unknown fields) is preserved.
- Support files (settings.json, history, trash, conflicts, recovery) are included as supporting material.

### What mdbase record discovery excludes

- `.todobench/` support files, including the export report.
- `.git/` version-control files.
- `.mdbase/` collection cache files.

These exclusions stop supporting Markdown from being discovered as task or project records. Regular supporting files are still copied into the export. Transient `.mdbase` cache files and `.todobench/workspace.lock` are omitted from the snapshot.

### Generated collection profile

The exported collection uses `spec_version: "0.3.0"` with these settings:

```yaml
spec_version: "0.3.0"
settings:
  timezone: <workspace timezone>
  types_folder: _types
  record_extensions: [md]
  explicit_type_keys: []
  id_field: id
  validation: error
  exclude:
    - ".todobench/**"
```

Two type definitions are generated in `_types/`:

- **`task.md`** — path glob `projects/**/tasks/*/task.md`, fields include id, title, status, priority, tags, due, created/updated/completed timestamps, order, recurrence, reminders, parent_id, and all unknown fields.
- **`project.md`** — path glob `projects/**/project.md`, fields include id, display_name, archived, order, parent_id, and all unknown fields.

Each type includes:
- `collection.display.name_field` for display name.
- `collection.unique` for ID uniqueness within each type.
- `collection.links` for `todobench_project_link` and `todobench_parent_link` (relationship links).
- `x-todobench` extension marking this as TodoBench export profile v1.

### Validation gate

Before publication, the exported collection is opened through the mdbase bridge and validated:
- Config, type definitions, and record schemas are valid.
- IDs are unique within each type.
- Relationship targets resolve correctly.
- Record path sets match the source workspace exactly.

### Export report

Written to `.todobench/mdbase_export_report.json` inside the exported collection. Contains profile version, spec version, source/output counts, exclusions, warnings, and validation results.

## Import

### Source requirements

- `mdbase.yaml` with `spec_version: "0.3.0"`.
- Type definitions are discovered from the configured types folder when present; records without a matching type are exposed in an explicit **Untyped records** bucket.
- Regular Markdown files matching configured record extensions.

### Import flow

1. **Source**: Choose the collection root. TodoBench snapshots and inspects it on a cancellable background worker, validates the version, and shows path-specific diagnostics.
2. **Records and fields**: Select which types and individual records to import (task, project, untyped). Configure field mappings for each selected type. Status and priority values must be explicitly mapped.
3. **Review and destination**: Preview the converted workspace — projects, tasks, statuses, due dates, relationships, link rewrites, and the new workspace path. Each record has project and parent choices so a single conflict can be repaired or cleared without changing the whole type mapping. Graph validation reruns after each override. Blocking errors must be resolved before proceeding.
4. **Progress**: Phase and processed/total counts with cancellation support.
5. **Result**: Success/failure, destination path, report, and option to open the new workspace.

### Field mapping

| Target | Source | Default |
|--------|--------|---------|
| Task title / project name | User-selected string field | Filename stem (requires acknowledgement) |
| Body | Record's Markdown body | — |
| ID | Configured identity field + canonical source path | Generated UUID for non-UUID or absent IDs |
| Status | User maps every observed scalar to: todo, in_progress, waiting, done, cancelled | Missing/null → todo |
| Priority | User maps observed scalars to: none, low, normal, high, urgent | Missing/null → normal |
| Tags | String → one tag; list of strings | Missing/null → empty list |
| Due date | Valid date preserved; date-time uses collection timezone | — |
| Created/updated | Mapped when valid; fixed import timestamp when absent | — |
| Completion | Preserved for completed states; absent otherwise | — |
| Project | Linked project, ID reference, string label, or none | none → Inbox |
| Parent task/project | Link or ID reference | none → root |

### Own-profile auto-fill

Collections exported by TodoBench (detected via `x-todobench.profile`) automatically fill the field mappings. The user can still review and override.

### Foreign collections

Collections from other mdbase tools are fully supported. The user configures mappings for foreign field names and status/priority values. Unsupported or inactive features (like TodoBench-specific recurrence or reminders) are preserved as source metadata in provenance.

### Identity and graph rules

- Valid source UUIDs are preserved when unique for the destination kind.
- Non-UUID or absent IDs receive generated UUIDs.
- Duplicate source IDs produce distinct native IDs with a warning.
- Graph validation: cycles, missing parents, and cross-project parents are blocking errors.
- The import creates synthetic projects for string labels and ensures every task has exactly one home project.

### Link rewriting

- Inline links `[text](target)`, images `![alt](target)`, reference definitions, and wikilinks are rewritten to point to new native paths.
- Links inside fenced/indented code and inline code are not rewritten.
- Remote URLs, data URLs, and unsupported markup are preserved unchanged.
- Missing or external targets are preserved with a warning.
- Shared attachments are byte-copied into the owning task's `assets/` directory.

### Provenance

The import retains complete source material:
- `.todobench/imports/<uuid>/source/` — byte-for-byte copy of the original collection.
- `.todobench/imports/<uuid>/report.json` — source root, snapshot hash, record mapping, blocking decisions.
- `settings.json` — generated with validated timezone (defaults to UTC).

## Cancellation and failure

- Cancelling during scan/copy/conversion/validation cleans up only temporary paths.
- Cancelling after publication succeeds is a no-op (the destination is already complete).
- Failed imports leave no published partial output.
- Export failure prevents publication; the destination remains non-existent.
- Concurrent transfers are blocked per window.

## Limitations

- **No merge or update-in-place**: Import always creates a new workspace. There is no mode to merge into an existing workspace.
- **No live mdbase storage**: This is a conversion bridge, not a live-storage adapter. The native format remains the primary storage.
- **Recurrence and reminders**: These are TodoBench-specific features. Foreign collections retain them as inactive metadata. Only TodoBench's own export profile maps them correctly.
- **Date precision**: Date-time values lose time-of-day precision when the source timezone differs from the import timezone.
- **Foreign wikilinks**: Converted to standard Markdown links. The original is retained in provenance.
- **Platform**: The automated transfer suite runs on every release target. Hands-on packaged UI transfer has been completed on Linux only.

## Testing

The transfer pipeline is tested via:
- `test_mdbase_export` — empty workspace, nested projects, asset preservation, destination collision.
- `test_mdbase_transfer` — foreign mapping, duplicate IDs, graph validation, own-profile round trip, title acknowledgement, snapshot limits.
- `test_mdbase_matrix` plus the transfer/UI suites — the M01–M19 scenarios covering export validation, round trip, foreign fixtures, defaults, statuses/priorities, metadata preservation, link rewriting, source change detection, cancellation, limits, empty collections, and UI state.
- `mdbase_oracle_interop` — independent TypeScript oracle validation.
- `scripts/mdbase-oracle-verify.py` — standalone oracle proof script.

## Security

- Source collections are never modified.
- Temporary copies are owned by the operation and cleaned up on failure/cancellation.
- No network access during transfer.
- No execution of stored views, workflows, or actions from the source collection.
- Path traversal, symlinks, and special files are rejected.
