// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "storage/mdbase_transfer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace todobench::mdbase_transfer {

// ---- Inspection: fold of collection discovery via Rust bridge ----

struct DiscoveredTypeInfo {
    std::string type_name;        // e.g. "task" / "note" / "project"
    std::string definition_path;  // definition file path within collection (e.g. "_types/task.md")
    std::string source_path;      // duplicate of definition_path for tests
    int version{1};
    std::string display_name_field;  // collection.display.name_field if present
    bool has_unique_triple{false};
    std::vector<std::string> link_fields; // keys under collection.links
    size_t record_count{0};              // enumerated via query
    QJsonObject raw_frontmatter;         // x-todobench etc for own-profile detection
};

struct CollectionInspection {
    bool valid{false}; // false when collection-level config/type errors
    std::filesystem::path collection_root; // same as snapshot frozen root
    QJsonObject config;                        // mdbase.yaml settings as JSON
    std::string timezone;                      // settings.timezone or "UTC"
    std::string id_field;                      // settings.id_field (usually "id")
    std::vector<DiscoveredTypeInfo> types;
    // Exact record paths discovered for each type (deduped across types, and untyped).
    std::unordered_map<std::string, std::vector<std::string>> type_to_paths; // type_name -> rel paths (forward slashes)
    std::vector<std::string> untyped_paths;       // Markdown files not matching any selected type
    std::vector<std::string> all_record_paths;    // deduped union (sorted)
    std::vector<TransferDiagnostic> diagnostics;
    bool is_own_profile{false};
    int own_profile_version{0};
    std::string spec_version; // from config
};

// ---- Mapping: per-type field selectors + typed status/priority mapping ----

struct FieldSelector {
    // Source field: JSON pointer internally ("/summary" for top-level "summary", "/meta/nested" ...)
    // UI picks string field path; we store pointer like "/summary". Empty => not mapped.
    std::string json_pointer;  // e.g. "/summary" (empty means explicitly unmapped)
    bool is_set{false};        // true when selector participates (allows missing vs unmapped distinction)
};

struct TypeMapping {
    std::string type_name;
    bool selected{false};            // include records of this type in import
    bool as_task{false};             // true => its records become TaskRecord; false => Project
    bool is_untyped_bucket{false};   // the synthetic "Untyped records" selection

    // Field mappings (all optional except id: we allow unmapped to trigger deterministic fallback)
    FieldSelector title_field;        // for task: title ; for project: display_name
    FieldSelector id_field;           // collection.id_field override per type (optional)
    FieldSelector status_field;       // e.g. "/state" for foreign
    FieldSelector priority_field;     // e.g. "/prio"
    FieldSelector tags_field;
    FieldSelector due_field;
    FieldSelector created_field;
    FieldSelector updated_field;
    FieldSelector completed_field;
    FieldSelector recurrence_field;
    FieldSelector reminders_field;
    FieldSelector order_field;
    FieldSelector archived_field;     // project only

    // Relationship selectors:
    // - project_mode: none | link | id_ref | string_label (distinct labels create projects)
    std::string project_mode{"none"};
    FieldSelector project_field;      // when mode != none: pointer to field holding value

    // Parent selector
    std::string parent_mode{"none"}; // none | link | id_ref
    FieldSelector parent_field;
};

struct StatusValueMap {
    // User maps every observed non-null scalar to one of five native values.
    // Key is JSON-serialized scalar (e.g. "\"DONE\"" for string DONE, "3" for numeric 3, "false")
    // Value is native string: todo/in_progress/waiting/done/cancelled
    std::unordered_map<std::string, std::string> scalar_to_native;

    // Helper to lookup.
    std::optional<std::string> lookup(const QJsonValue& v) const;
    void put_for_json_value(const QJsonValue& v, const std::string& native);
};

struct PriorityValueMap {
    std::unordered_map<std::string, std::string> scalar_to_native; // none/low/normal/high/urgent
    std::optional<std::string> lookup(const QJsonValue& v) const;
    void put_for_json_value(const QJsonValue& v, const std::string& native);
};

// Aggregate mapping across selected types.
struct ImportMapping {
    // Per type_name (including synthetic "" for untyped bucket)
    std::unordered_map<std::string, TypeMapping> type_mappings;

    StatusValueMap status_map;
    PriorityValueMap priority_map;

    bool accept_empty_title_as_filename_stem{false}; // explicit acceptance shown in preview
    bool create_empty_workspace_ack{false};          // for 0-record import

    // Exclusions: relative paths explicitly excluded (even when type selected)
    std::unordered_set<std::string> excluded_paths;

    // Per-record choices made after the first preview. Absence means use the
    // type mapping; a present empty value explicitly clears the relationship.
    // Non-empty values are deterministic native IDs or a "label:<name>"
    // synthetic-project choice already shown by the preview.
    std::unordered_map<std::string, std::string> record_project_overrides;
    std::unordered_map<std::string, std::string> record_parent_overrides;

    // ID handling policy is shared: preserve valid UUIDs otherwise generate; distinct per dest kind.
    // No exposed policy enum required — ImportMapping carries the concrete maps.

    // Helpers
    void ensure_type(const std::string& type_name);
    TypeMapping* find_type(const std::string& type_name);
    const TypeMapping* find_type(const std::string& type_name) const;
};

// ---- Preview: fixed snapshot identity, deterministic IDs/path map ----

struct PreviewRecord {
    std::string source_path; // collection-relative forward slashes
    std::vector<std::string> source_types; // mdbase membership (may be multiple)
    bool is_untyped{false};
    bool is_task{true}; // true => TaskRecord, false => ProjectRecord (resolved from TypeMapping.as_task)

    // Original persisted fields (raw JSON frontmatter + body bytes hash)
    QJsonObject persisted;        // raw persisted frontmatter
    QJsonObject effective;        // read_defaults applied
    std::string body;
    std::string persisted_hash_hex; // sha256 of original file bytes

    // Native conversion (when not blocked)
    std::string native_id;          // fixed UUID chosen for this preview (stable per snapshot)
    std::string native_title;       // mapped value (preview shows filename stem fallback)
    std::string native_status_native{"todo"}; // native word
    std::string native_priority_native{"normal"};
    std::vector<std::string> native_tags;
    std::optional<std::string> native_due_iso; // date YYYY-MM-DD when valid
    std::string native_created_iso;            // explicit or fixed import timestamp
    std::string native_updated_iso;
    std::optional<std::string> native_completed_iso;
    std::string native_project_choice;   // project name/id or native project id after graph
    std::string native_parent_choice;    // parent native id (after graph)
    long long native_order{1024};
    bool native_archived{false};
    // Whether raw source value was missing/null -> default applied (vs mapped present value)
    std::vector<std::string> fallback_notes; // e.g. "status defaulted to todo (missing)", "priority defaulted"
    std::vector<std::string> inactive_notes; // schedules retained as metadata

    bool blocked{false};
    std::vector<TransferDiagnostic> issues; // per-record blocking diagnostics
};

struct TransferPreview {
    // Identity: snapshot that was used (kept alive by caller)
    std::filesystem::path snapshot_id_source_root; // for staleness check
    std::string snapshot_inventory_hash_hex;        // fingerprint of inventory (for requires-rescan)
    bool snapshot_still_current{true};

    CollectionInspection inspection; // frozen copy of inspection used to build preview

    std::vector<PreviewRecord> records;
    // Summary counts
    size_t selected_record_count{0};
    size_t to_task_count{0};
    size_t to_project_count{0};
    uint64_t estimated_output_bytes{0};

    // Warnings and blocking errors (UI disable Finish while any blocking exists)
    std::vector<TransferDiagnostic> warnings;
    std::vector<TransferDiagnostic> blocking_errors;

    // Deterministic dest path allocation (native folder layout) keyed by native_id
    std::unordered_map<std::string, std::string> native_id_to_dest_rel; // native_id -> "projects/.../task.md"

    // ID map: source_path -> native_id (and id_field source identity map)
    std::unordered_map<std::string, std::string> source_path_to_native_id;
    // (source kind -> (source identity value string -> native_id)) — used for reference resolution in T05
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> source_id_value_to_native;

    // Needs explicit acceptance before import allowed
    bool requires_empty_ack_when_zero{false};
    bool has_unmapped_observed_values{false};

    bool has_blocking_errors() const { return !blocking_errors.empty(); }
};

// ---- Service entry points ----

// 1. Inspect: snapshot must be alive; reads frozen_copy_root via bridge.
//    Honors spec_version 0.3.0; unsupported/newer/older -> valid==false with diagnostics.
//    Uses bridge read/validate/get_types/list_types/query; never executes views/workflows.
CollectionInspection inspect_import_source(const TransferSnapshot& snapshot,
                                          TransferCancellation* cancellation = nullptr);

// 2. Build a preview from a snapshot + inspection + mapping. Deterministic IDs/path map.
//    Returns a preview including warnings/blocking_errors; caller keeps snapshot alive.
//    A mapping change rebuilds preview; generated native IDs stay fixed for that snapshot.
TransferPreview build_transfer_preview(const TransferSnapshot& snapshot,
                                       const CollectionInspection& inspection,
                                       const ImportMapping& mapping,
                                       TransferCancellation* cancellation = nullptr);

// Auto-fill for own-profile v1 (todobench-export).
ImportMapping make_own_profile_mapping(const CollectionInspection& inspection);

// Helpers used by mapping UI: observed values for a type/field (deduped scalars), and field suggestions.
std::vector<QJsonValue> observed_field_values(const CollectionInspection& inspection,
                                               const TransferSnapshot& snapshot,
                                               const std::string& type_name,
                                               const std::string& json_pointer);

// Stable hash for snapshot staleness (inventory hash).
std::string snapshot_identity_hash(const TransferSnapshot& snapshot);

// Forward path for staged import execution (T06): build TransferResult from fixed preview.
TransferResult execute_import(const TransferSnapshot& snapshot,
                              const TransferPreview& preview,
                              const std::filesystem::path& destination_workspace,
                              TransferLimits limits = {},
                              TransferCancellation* cancellation = nullptr,
                              TransferProgress progress = {});

} // namespace todobench::mdbase_transfer
