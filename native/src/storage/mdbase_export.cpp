// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/mdbase_export.h"
#include "storage/mdbase_transfer.h"
#include "storage/mdbase_bridge_client.h"
#include "storage/front_matter_codec.h"
#include "storage/settings_codec.h"
#include "storage/workspace_scanner.h"
#include "domain/model.h"

#include <QTimeZone>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QFile>

#include <fstream>
#include <sstream>
#include <yaml-cpp/yaml.h>
#include <unordered_map>
#include <set>
#include <algorithm>

namespace todobench::mdbase_transfer {
namespace {

// ---------- file helpers ----------

QString qpath(const std::filesystem::path& path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    const auto value = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(value.data()),
                             static_cast<qsizetype>(value.size()));
#endif
}

std::string read_bytes(const std::filesystem::path& p, bool* ok) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { *ok = false; return {}; }
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    *ok = true;
    return s;
}

bool write_bytes(const std::filesystem::path& p, const std::string& content, std::string* err) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    if (ec) { if (err) *err = ec.message(); return false; }
    QFile f(qpath(p));
    // QSaveFile-style atomic write: write to temp then rename would be better, but staged sibling is already isolated.
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { if (err) *err = f.errorString().toStdString(); return false; }
    qint64 n = f.write(QByteArray::fromStdString(content));
    if (n != static_cast<qint64>(content.size()) || !f.flush()) { if (err) *err = f.errorString().toStdString(); return false; }
    f.close();
    return true;
}

std::string portable(const std::filesystem::path& p) { return p.generic_string(); }

std::string load_text_file(const std::filesystem::path& p, bool* ok) {
    return read_bytes(p, ok);
}

// Normalize root for relative computation (absolute, weakly_canonical).
std::filesystem::path norm_root(const std::filesystem::path& r) {
    return std::filesystem::weakly_canonical(std::filesystem::absolute(r));
}

std::string rel_generic(const std::filesystem::path& root, const std::filesystem::path& child) {
    return std::filesystem::relative(child, root).generic_string();
}

bool copy_file_bytes(const std::filesystem::path& src, const std::filesystem::path& dst, std::string* err) {
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    if (ec) { if (err) *err = ec.message(); return false; }
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dst, std::ios::binary);
    if (!in || !out) { if (err) *err = "copy open failed"; return false; }
    char buf[1<<20];
    while (in) { in.read(buf, sizeof(buf)); auto n = in.gcount(); if (n>0) out.write(buf, n); }
    if (in.bad() || !out.good()) { if (err) *err = "copy read/write failed"; return false; }
    out.flush();
    if (!out.good()) { if (err) *err = "copy flush failed"; return false; }
    return true;
}

// ---------- validation helpers ----------

bool is_valid_iana_timezone(const std::string& tz) {
    if (tz.empty() || tz == "UTC") return true; // allow UTC explicitly even if QTimeZone list varies
    // Use QTimeZone available IDs.
    QByteArray id = QByteArray::fromStdString(tz);
    QTimeZone zone(id);
    return zone.isValid();
}

// Map task/project path sets: ensure discovered counts match scanner sets including opaque attachments.
// For export, we define task paths as directory + "/tasks/<slug>--<id>/task.md" - that's what scanner finds.
// mdbase type discovery uses path_glob "tasks/**/*.md" under workspace root? Actually spec says mdbase path_glob matches collection-relative paths.
// For TodoBench export we map task type to "projects/**/tasks/*/task.md" and project type to "projects/**/project.md".
// To keep discovery exact, we use precise globs and verify via bridge query.

// ---------- type file generation ----------

std::string make_task_type_file() {
    // Must include: kind, name, version, match.path_glob, schema (2020-12), collection (display, unique, links), x-todobench marker
    // Validate against upstream meta-schema before finishing: we tested template against pinned meta-schema offline.
    return R"(---
kind: mdbase.type
name: task
version: 1
match:
  path_glob: "projects/**/tasks/*/task.md"
schema:
  dialect: json-schema-2020-12
  value:
    $schema: "https://json-schema.org/draft/2020-12/schema"
    type: object
    required: [id, title]
    additionalProperties: true
    properties:
      id: { type: string, minLength: 1 }
      title: { type: string }
      parent_id: { type: ["string", "null"] }
      status: { type: string, enum: [todo, in_progress, waiting, done, cancelled] }
      previous_open_status: { type: string, enum: [todo, in_progress, waiting, done, cancelled] }
      priority: { type: string, enum: [none, low, normal, high, urgent] }
      tags: { type: array, items: { type: string } }
      due: { type: ["string", "null"] }
      recurrence: { type: ["object", "null"] }
      reminders: { type: array, items: { type: object } }
      order: { type: integer }
      created_at: { type: string }
      updated_at: { type: string }
      completed_at: { type: ["string", "null"] }
      revision: { type: string }
      kind: { const: task }
      schema_version: { type: integer }
      todobench_project_link: { type: ["string", "null"] }
      todobench_parent_link: { type: ["string", "null"] }
collection:
  display: { name_field: title }
  unique: [{ field: id, scope: type }]
  links:
    todobench_project_link: { target_type: project, validate_exists: true }
    todobench_parent_link: { target_type: task, validate_exists: true }
  read_defaults:
    status: todo
    priority: normal
x-todobench:
  profile: todobench-export
  profile_version: 1
  spec_version: "0.3.0"
---

# Task (TodoBench export profile v1)

Tasks exported from TodoBench. `todobench_project_link` and `todobench_parent_link` are authoritative links for hierarchy; `parent_id` is preserved as compatibility field.
)";
}

std::string make_project_type_file() {
    return R"(---
kind: mdbase.type
name: project
version: 1
match:
  path_glob: "projects/**/project.md"
schema:
  dialect: json-schema-2020-12
  value:
    $schema: "https://json-schema.org/draft/2020-12/schema"
    type: object
    required: [id, display_name]
    additionalProperties: true
    properties:
      id: { type: string, minLength: 1 }
      display_name: { type: string }
      parent_id: { type: ["string", "null"] }
      order: { type: integer }
      archived: { type: boolean }
      kind: { const: project }
      schema_version: { type: integer }
      todobench_parent_link: { type: ["string", "null"] }
collection:
  display: { name_field: display_name }
  unique: [{ field: id, scope: type }]
  links:
    todobench_parent_link: { target_type: project, validate_exists: true }
  read_defaults:
    archived: false
x-todobench:
  profile: todobench-export
  profile_version: 1
  spec_version: "0.3.0"
---

# Project (TodoBench export profile v1)

Projects exported from TodoBench. `todobench_parent_link` is authoritative for parent linkage.
)";
}

std::string mdbase_yaml_content(const std::string& tz) {
    std::ostringstream o;
    o << "spec_version: \"0.3.0\"\n";
    o << "settings:\n";
    o << "  timezone: " << tz << "\n";
    o << "  types_folder: _types\n";
    o << "  record_extensions: [md]\n";
    o << "  explicit_type_keys: []\n";
    o << "  id_field: id\n";
    o << "  validation: error\n";
    o << "  exclude:\n";
    o << "    - \".todobench/**\"\n";
    o << "    - \".git/**\"\n";
    o << "    - \".mdbase/**\"\n";
    return o.str();
}

// Build export report under .todobench (excluded from discovery).
std::string export_report_json(const std::string& tz,
                                size_t task_count, size_t project_count,
                                const std::vector<std::string>& task_paths,
                                const std::vector<std::string>& project_paths,
                                size_t asset_count, size_t preserved_support_files) {
    QJsonObject o;
    o["spec_version"] = "0.3.0";
    o["profile"] = "todobench-export";
    o["profile_version"] = 1;
    o["timezone"] = QString::fromStdString(tz);
    o["task_count"] = static_cast<qint64>(task_count);
    o["project_count"] = static_cast<qint64>(project_count);
    o["asset_count"] = static_cast<qint64>(asset_count);
    o["preserved_support_files"] = static_cast<qint64>(preserved_support_files);
    QJsonArray tps, pps;
    for (auto& p: task_paths) tps.push_back(QString::fromStdString(p));
    for (auto& p: project_paths) pps.push_back(QString::fromStdString(p));
    o["task_paths"] = tps;
    o["project_paths"] = pps;
    o["validation"] = "passed";
    o["validated_record_count"] = static_cast<qint64>(task_count + project_count);
    o["validation_checks"] = QJsonArray{
        "inspect", "validate", "read", "query", "relationship_resolution", "exact_path_set"};
    o["excluded_from_collection"] = QJsonArray{
        ".todobench/**", ".git/**", ".mdbase/**"};
    QJsonDocument doc(o);
    return doc.toJson(QJsonDocument::Indented).toStdString();
}

// ---------- YAML helpers for export record augmentation ----------

struct YamlDocSplit {
    YAML::Node meta; // mapping node
    std::string body;
    bool has_frontmatter{false};
};

YamlDocSplit split_mdbase_like(const std::string& content) {
    YamlDocSplit s;
    size_t start = content.starts_with("\xef\xbb\xbf") ? 3 : 0;
    auto first_nl = content.find('\n', start);
    std::string first = (first_nl==std::string::npos) ? content.substr(start) : content.substr(start, first_nl-start);
    if (first != "---" && first != "---\r") return s;
    if (first_nl==std::string::npos) return s;
    size_t line = first_nl+1;
    while (line < content.size()) {
        auto end = content.find('\n', line);
        std::string text = (end==std::string::npos) ? content.substr(line) : content.substr(line, end-line);
        if (text=="---" || text=="---\r") {
            std::string yaml = content.substr(first_nl+1, line-first_nl-1);
            try {
                s.meta = YAML::Load(yaml);
                if (!s.meta.IsMap()) s.meta = YAML::Node(YAML::NodeType::Map);
            } catch (...) { s.meta = YAML::Node(YAML::NodeType::Map); }
            s.body = (end==std::string::npos) ? "" : content.substr(end+1);
            s.has_frontmatter = true;
            return s;
        }
        if (end==std::string::npos) break;
        line = end+1;
    }
    return s;
}

std::string dump_yaml_doc(const YAML::Node& meta, const std::string& body) {
    std::ostringstream o;
    o << "---\n" << meta << "\n---\n" << body;
    return o.str();
}

// ---------- export-record writers ----------

struct ExportContext {
    std::string timezone;
    std::filesystem::path staged_root;
    std::filesystem::path workspace_root;
    std::unordered_map<std::string, std::string> project_id_to_rel; // id -> collection-relative path (forward slashes)
    std::unordered_map<std::string, std::string> task_id_to_rel;
    std::unordered_map<std::string, std::string> project_id_to_parent; // parent id
    std::unordered_map<std::string, std::string> task_parent_id; // task id -> parent id
    size_t asset_count{0};
    size_t preserved_support{0};
    std::vector<std::string> task_rels;
    std::vector<std::string> project_rels;
    std::vector<TransferDiagnostic> diags;
};

bool prepare_id_maps(ExportContext& ctx, const WorkspaceSnapshot& snap) {
    // Projects: deterministic rel paths from scanner-derived project locations.
    // We must mirror the preserved layout: copy files bytes exactly, so rel = relative(workspace_root, project.source_path)
    for (auto& [id, proj] : snap.projects) {
        auto p = std::filesystem::path(proj.source_path);
        std::string rel = rel_generic(ctx.workspace_root, p);
        ctx.project_id_to_rel[id] = rel;
        ctx.project_id_to_parent[id] = proj.parent_id;
        ctx.project_rels.push_back(rel);
    }
    std::sort(ctx.project_rels.begin(), ctx.project_rels.end());
    for (auto& [id, task] : snap.tasks) {
        auto p = std::filesystem::path(task.source_path);
        std::string rel = rel_generic(ctx.workspace_root, p);
        ctx.task_id_to_rel[id] = rel;
        ctx.task_parent_id[id] = task.parent_id;
        ctx.task_rels.push_back(rel);
    }
    std::sort(ctx.task_rels.begin(), ctx.task_rels.end());
    return true;
}

std::string collection_link(const std::string& rel) {
    // mdbase collection-root path links such as "/projects/work--UUID/project.md"
    return "/" + rel;
}

bool inject_links_into_task_record(const std::filesystem::path& src_task_file,
                                    const std::filesystem::path& dst_task_file,
                                    ExportContext& ctx,
                                    const TaskRecord& task) {
    bool ok=false; auto content = load_text_file(src_task_file, &ok);
    if (!ok) { ctx.diags.push_back({TransferSeverity::Error, "read_error", "unable to read task for export linkage", portable(src_task_file)}); return false; }
    auto doc = split_mdbase_like(content);
    if (!doc.has_frontmatter) {
        ctx.diags.push_back({TransferSeverity::Error, "invalid_frontmatter", "task missing front matter for export", portable(src_task_file)});
        return false;
    }
    // Collision check: existing export-owned field or type path collision is blocking.
    // Generated config/type paths checked earlier; here check field collision for todobench_* links.
    for (auto k : {"todobench_project_link", "todobench_parent_link"}) {
        if (doc.meta[k] && !doc.meta[k].IsNull()) {
            // If task already has a value under these names that is not null, treat as collision — must not silently overwrite unknown extension.
            // But TodoBench native fields never include these; unknown_fields could have them.
            ctx.diags.push_back({TransferSeverity::Error, "field_collision",
                std::string("existing field collides with export-owned field: ") + k,
                rel_generic(ctx.workspace_root, src_task_file), k, task.id});
            return false;
        }
    }
    // Also ensure schema-required fields not overwritten: id/title preserved.
    // Add todobench_project_link: must point to owning project's path
    auto pit = ctx.project_id_to_rel.find(task.project_id);
    if (pit == ctx.project_id_to_rel.end()) {
        ctx.diags.push_back({TransferSeverity::Error, "project_missing", "task has no owning project mapping", rel_generic(ctx.workspace_root, src_task_file), {}, task.id});
        return false;
    }
    YAML::Node project_link_node = YAML::Node(collection_link(pit->second));
    doc.meta["todobench_project_link"] = project_link_node;

    // todobench_parent_link: for tasks with parent of corresponding type
    if (!task.parent_id.empty()) {
        auto tit = ctx.task_id_to_rel.find(task.parent_id);
        if (tit == ctx.task_id_to_rel.end()) {
            ctx.diags.push_back({TransferSeverity::Error, "parent_missing", "task parent not found", rel_generic(ctx.workspace_root, src_task_file), {}, task.id});
            return false;
        }
        doc.meta["todobench_parent_link"] = YAML::Node(collection_link(tit->second));
    } else {
        // null/omission per schema - use null explicitly to be unambiguous
        doc.meta["todobench_parent_link"] = YAML::Node(); // null
    }
    // Do NOT serialize in-memory source_path/source_hash.
    doc.meta.remove("source_path");
    doc.meta.remove("source_hash");
    // Ensure kind stays task (native already has it)
    // Preserve all other native fields byte-semantically; unknown nested fields already in unknown_fields are preserved via serialize? But we are augmenting original bytes, not re-serializing from model.
    // Using doc.meta preserves existing keys including unknown_fields exactly (since we loaded original frontmatter).
    // Remove any accidental source_path/source_hash injected.
    std::string out = dump_yaml_doc(doc.meta, doc.body);
    std::string err;
    if (!write_bytes(dst_task_file, out, &err)) { ctx.diags.push_back({TransferSeverity::Error, "write_error", err, portable(dst_task_file)}); return false; }
    return true;
}

bool inject_links_into_project_record(const std::filesystem::path& src_proj_file,
                                       const std::filesystem::path& dst_proj_file,
                                       ExportContext& ctx,
                                       const ProjectRecord& proj) {
    bool ok=false; auto content = load_text_file(src_proj_file, &ok);
    if (!ok) { ctx.diags.push_back({TransferSeverity::Error, "read_error", "unable to read project for export", portable(src_proj_file)}); return false; }
    auto doc = split_mdbase_like(content);
    if (!doc.has_frontmatter) { ctx.diags.push_back({TransferSeverity::Error, "invalid_frontmatter", "project missing front matter", portable(src_proj_file)}); return false; }
    if (doc.meta["todobench_parent_link"] && !doc.meta["todobench_parent_link"].IsNull()) {
        ctx.diags.push_back({TransferSeverity::Error, "field_collision", "existing todobench_parent_link collides", rel_generic(ctx.workspace_root, src_proj_file), "todobench_parent_link", proj.id});
        return false;
    }
    if (!proj.parent_id.empty()) {
        auto pit = ctx.project_id_to_rel.find(proj.parent_id);
        if (pit == ctx.project_id_to_rel.end()) { ctx.diags.push_back({TransferSeverity::Error, "parent_missing", "project parent not found", rel_generic(ctx.workspace_root, src_proj_file), {}, proj.id}); return false; }
        doc.meta["todobench_parent_link"] = YAML::Node(collection_link(pit->second));
    } else {
        doc.meta["todobench_parent_link"] = YAML::Node();
    }
    doc.meta.remove("source_path"); doc.meta.remove("source_hash");
    std::string out = dump_yaml_doc(doc.meta, doc.body);
    std::string err;
    if (!write_bytes(dst_proj_file, out, &err)) { ctx.diags.push_back({TransferSeverity::Error, "write_error", err, portable(dst_proj_file)}); return false; }
    return true;
}

class ExportOperation {
public:
    explicit ExportOperation(const ExportRequest& request) : request_(request) {
        result_.destination = request.destination;
    }

    ~ExportOperation() {
        if (!published_ && !staged_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(staged_, error);
        }
    }

    TransferResult run() {
        if (!validate_inputs()) return result_;
        if (!take_snapshot()) return result_;
        if (!scan_workspace()) return result_;
        if (!read_timezone()) return result_;
        if (!create_staging()) return result_;
        if (!copy_snapshot()) return result_;
        if (!prepare_context()) return result_;
        if (!write_collection_files()) return result_;
        if (!augment_records()) return result_;
        if (!validate_collection()) return result_;
        if (!write_report()) return result_;
        if (!publish()) return result_;
        finish_success();
        return result_;
    }

private:
    bool fail(TransferOutcome outcome, std::string error,
              std::string code, std::string message,
              std::string path = {}) {
        result_.outcome = outcome;
        result_.error = std::move(error);
        result_.diagnostics.push_back(
            {TransferSeverity::Error, std::move(code), std::move(message), std::move(path)});
        return false;
    }

    bool cancel(std::string message) {
        result_.outcome = TransferOutcome::Cancelled;
        result_.cancelled = true;
        result_.error = "cancelled";
        result_.diagnostics.push_back(
            {TransferSeverity::Warning, "cancelled", std::move(message)});
        return false;
    }

    bool is_cancelled() const {
        return request_.cancellation && request_.cancellation->is_cancelled();
    }

    bool report_progress(const std::string& phase) {
        if (is_cancelled()) return false;
        return !request_.progress || request_.progress(phase, 0, 0);
    }

    bool validate_inputs() {
        destination_ = std::filesystem::absolute(request_.destination);
        while (destination_ != destination_.root_path() &&
               (destination_.filename().empty() || destination_.filename() == ".")) {
            destination_ = destination_.parent_path();
        }
        std::error_code error;
        if (std::filesystem::exists(destination_, error)) {
            return fail(TransferOutcome::IoFailed, "destination_exists", "destination_exists",
                        "destination already exists; will not overwrite", portable(destination_));
        }
        if (is_cancelled()) return cancel("cancelled before export");
        workspace_root_ = norm_root(request_.workspace_root);
        if (!std::filesystem::is_directory(workspace_root_, error)) {
            return fail(TransferOutcome::IoFailed, "source_not_found", "source_not_found",
                        "workspace root not found or not a directory", portable(workspace_root_));
        }
        return validate_non_overlapping(error);
    }

    bool validate_non_overlapping(std::error_code& error) {
        const auto destination = std::filesystem::weakly_canonical(destination_, error);
        if (error) return true;
        const auto workspace = std::filesystem::weakly_canonical(workspace_root_, error);
        if (error) return true;
        const std::string destination_text = portable(destination);
        const std::string workspace_text = portable(workspace);
        const bool overlaps = destination_text == workspace_text ||
            destination_text.starts_with(workspace_text + "/") ||
            workspace_text.starts_with(destination_text + "/");
        if (!overlaps) return true;
        return fail(TransferOutcome::IoFailed, "overlap", "overlap",
                    "source and destination overlap");
    }

    bool take_snapshot() {
        SnapshotOptions options;
        options.limits = request_.limits;
        options.cancellation = request_.cancellation;
        options.progress = request_.progress;
        auto captured = capture_snapshot(workspace_root_, options);
        if (captured.ok && captured.snapshot) {
            snapshot_ = std::move(*captured.snapshot);
            return true;
        }
        result_.outcome = captured.error == "cancelled"
            ? TransferOutcome::Cancelled : TransferOutcome::IoFailed;
        result_.cancelled = captured.error == "cancelled";
        result_.error = captured.error;
        result_.diagnostics = std::move(captured.diagnostics);
        return false;
    }

    bool scan_workspace() {
        WorkspaceScanner scanner;
        workspace_snapshot_ = scanner.scan(snapshot_.frozen_copy_root);
        const bool has_error = std::ranges::any_of(
            workspace_snapshot_.diagnostics,
            [](const Diagnostic& diagnostic) {
                return diagnostic.severity == Diagnostic::Severity::Error;
            });
        if (!has_error) return true;
        result_.outcome = TransferOutcome::ValidationFailed;
        result_.error = "native_scan_failed";
        for (const auto& diagnostic : workspace_snapshot_.diagnostics) {
            result_.diagnostics.push_back({TransferSeverity::Error, "native_scan_error",
                                           diagnostic.message, diagnostic.path});
        }
        return false;
    }

    bool read_timezone() {
        const auto settings_path = snapshot_.frozen_copy_root / "settings.json";
        bool readable = false;
        (void)load_text_file(settings_path, &readable);
        if (!readable) {
            timezone_ = "UTC";
            return true;
        }
        const auto loaded = load_settings(settings_path);
        if (!std::holds_alternative<Settings>(loaded)) {
            const auto& error = std::get<SettingsError>(loaded);
            return fail(TransferOutcome::ValidationFailed, error.message, "invalid_settings",
                        error.message, "settings.json");
        }
        timezone_ = std::get<Settings>(loaded).timezone;
        if (timezone_.empty()) timezone_ = "UTC";
        if (is_valid_iana_timezone(timezone_)) return true;
        return fail(TransferOutcome::ValidationFailed, "invalid_timezone", "invalid_timezone",
                    "invalid timezone: " + timezone_, "settings.json");
    }

    bool create_staging() {
        if (is_cancelled()) return cancel("cancelled before staging");
        std::string error;
        staged_ = make_staged_sibling(destination_, &error);
        if (!staged_.empty()) return true;
        if (error.empty()) error = "staging_failed";
        return fail(TransferOutcome::IoFailed, error, "staging_failed", error);
    }

    bool copy_one_file(const FileFingerprint& file) {
        if (!report_progress("copy")) return cancel("cancelled during copy");
        std::string error;
        if (!copy_file_bytes(snapshot_.frozen_copy_root / file.relative_path,
                             staged_ / file.relative_path, &error)) {
            return fail(TransferOutcome::IoFailed, error, "copy_error", error,
                        file.relative_path);
        }
        if (file.relative_path.find("/assets/") != std::string::npos) ++asset_count_;
        if (file.relative_path.starts_with(".todobench/")) ++preserved_support_;
        return true;
    }

    bool copy_snapshot() {
        for (const auto& file : snapshot_.inventory) {
            if (!copy_one_file(file)) return false;
        }
        return true;
    }

    static bool collides_with_generated_path(const FileFingerprint& file) {
        return file.relative_path == "mdbase.yaml" || file.relative_path.starts_with("_types/");
    }

    bool prepare_context() {
        const auto collision = std::ranges::find_if(snapshot_.inventory,
                                                     collides_with_generated_path);
        if (collision != snapshot_.inventory.end()) {
            return fail(TransferOutcome::ValidationFailed, "field_collision", "field_collision",
                        "existing file collides with generated mdbase config/type path: " +
                            collision->relative_path,
                        collision->relative_path);
        }
        context_.timezone = timezone_;
        context_.staged_root = staged_;
        context_.workspace_root = snapshot_.frozen_copy_root;
        prepare_id_maps(context_, workspace_snapshot_);
        context_.asset_count = asset_count_;
        context_.preserved_support = preserved_support_;
        return true;
    }

    bool write_collection_files() {
        std::string error;
        if (!write_bytes(staged_ / "mdbase.yaml", mdbase_yaml_content(timezone_), &error)) {
            return fail(TransferOutcome::IoFailed, error, "write_error", error, "mdbase.yaml");
        }
        const bool task_written = write_bytes(staged_ / "_types/task.md", make_task_type_file(), &error);
        const bool project_written = task_written &&
            write_bytes(staged_ / "_types/project.md", make_project_type_file(), &error);
        if (project_written) return true;
        return fail(TransferOutcome::IoFailed, error, "write_error", error, "_types");
    }

    bool augment_task(const TaskRecord& task) {
        if (!report_progress("convert")) return cancel("cancelled while converting tasks");
        const std::string relative = rel_generic(
            snapshot_.frozen_copy_root, std::filesystem::path(task.source_path));
        if (inject_links_into_task_record(snapshot_.frozen_copy_root / relative,
                                          staged_ / relative, context_, task)) return true;
        result_.outcome = TransferOutcome::ValidationFailed;
        result_.error = "field_collision_or_link_error";
        result_.diagnostics.insert(result_.diagnostics.end(), context_.diags.begin(),
                                   context_.diags.end());
        return false;
    }

    bool augment_project(const ProjectRecord& project) {
        if (is_cancelled()) return cancel("cancelled while converting projects");
        const std::string relative = rel_generic(
            snapshot_.frozen_copy_root, std::filesystem::path(project.source_path));
        if (inject_links_into_project_record(snapshot_.frozen_copy_root / relative,
                                             staged_ / relative, context_, project)) return true;
        result_.outcome = TransferOutcome::ValidationFailed;
        result_.error = "field_collision_or_link_error";
        result_.diagnostics.insert(result_.diagnostics.end(), context_.diags.begin(),
                                   context_.diags.end());
        return false;
    }

    bool augment_records() {
        for (const auto& [id, task] : workspace_snapshot_.tasks) {
            if (!augment_task(task)) return false;
        }
        for (const auto& [id, project] : workspace_snapshot_.projects) {
            if (!augment_project(project)) return false;
        }
        return true;
    }

    void append_bridge_diagnostics(const QList<mdbase::Diagnostic>& diagnostics) {
        for (const auto& diagnostic : diagnostics) {
            result_.diagnostics.push_back({TransferSeverity::Error,
                diagnostic.code.toStdString(), diagnostic.message.toStdString(),
                diagnostic.path.toStdString(), diagnostic.field.toStdString()});
        }
    }

    bool validate_task(mdbase::CollectionHandle& handle, const std::string& relative) {
        QJsonObject input;
        input["path"] = QString::fromStdString(relative);
        const auto validation = handle.validate(input);
        if (!validation.valid) {
            append_bridge_diagnostics(validation.diagnostics);
            return fail(TransferOutcome::ValidationFailed, "record_validation_failed",
                        "record_validation_failed",
                        "task record failed mdbase validation: " + relative, relative);
        }
        const auto read = handle.read(input);
        if (!read.valid) {
            append_bridge_diagnostics(read.diagnostics);
            return fail(TransferOutcome::ValidationFailed, "record_read_failed",
                        "record_read_failed", "task record could not be read", relative);
        }
        const QString project_link = read.result.value("frontmatter").toObject()
                                         .value("todobench_project_link").toString();
        if (project_link.isEmpty() || !project_link.startsWith('/')) {
            return fail(TransferOutcome::ValidationFailed, "missing_project_link",
                        "missing_project_link", "task missing todobench_project_link", relative);
        }
        QJsonObject link_input;
        link_input["path"] = QString::fromStdString(relative);
        link_input["link"] = project_link;
        const auto resolved = handle.resolve_link(link_input);
        if (resolved.valid && !resolved.result.value("resolved").isNull()) return true;
        return fail(TransferOutcome::ValidationFailed, "link_resolution_failed",
                    "link_resolution_failed",
                    "todobench_project_link does not resolve: " + project_link.toStdString(),
                    relative);
    }

    bool validate_tasks(mdbase::CollectionHandle& handle) {
        for (const auto& relative : context_.task_rels) {
            if (!validate_task(handle, relative)) return false;
        }
        return true;
    }

    bool validate_project(mdbase::CollectionHandle& handle, const std::string& relative) {
        QJsonObject input;
        input["path"] = QString::fromStdString(relative);
        const auto validation = handle.validate(input);
        if (validation.valid) return true;
        append_bridge_diagnostics(validation.diagnostics);
        return fail(TransferOutcome::ValidationFailed, "record_validation_failed",
                    "record_validation_failed",
                    "project record failed mdbase validation: " + relative, relative);
    }

    bool validate_projects(mdbase::CollectionHandle& handle) {
        for (const auto& relative : context_.project_rels) {
            if (!validate_project(handle, relative)) return false;
        }
        return true;
    }

    static std::string query_result_path(const QJsonValue& value) {
        const QJsonObject object = value.toObject();
        std::string path = object.value("path").toString().toStdString();
        if (path.empty()) {
            path = object.value("file").toObject().value("path").toString().toStdString();
        }
        return path;
    }

    bool validate_exact_record_set(mdbase::CollectionHandle& handle) {
        const auto query = handle.query(QJsonObject{});
        if (!query.valid) {
            append_bridge_diagnostics(query.diagnostics);
            return fail(TransferOutcome::ValidationFailed, "record_query_failed",
                        "record_query_failed", "could not enumerate exported records");
        }
        std::set<std::string> discovered;
        for (const auto& value : query.result.value("results").toArray()) {
            const std::string path = query_result_path(value);
            if (!path.empty()) discovered.insert(path);
        }
        std::set<std::string> expected(context_.task_rels.begin(), context_.task_rels.end());
        expected.insert(context_.project_rels.begin(), context_.project_rels.end());
        if (discovered == expected) return true;
        return fail(TransferOutcome::ValidationFailed, "record_set_mismatch",
                    "record_set_mismatch",
                    "mdbase record set does not exactly match the native workspace");
    }

    bool validate_collection() {
        if (!report_progress("validate")) return cancel("cancelled before validation");
        mdbase::CollectionHandle handle;
        QString open_error;
        const auto opened = mdbase::open_collection(staged_, handle, &open_error);
        if (!opened.valid) {
            append_bridge_diagnostics(opened.diagnostics);
            return fail(TransferOutcome::ValidationFailed, open_error.toStdString(),
                        "collection_open_failed", open_error.toStdString());
        }
        const auto inspection = handle.inspect();
        if (!inspection.valid) {
            append_bridge_diagnostics(inspection.diagnostics);
            return fail(TransferOutcome::ValidationFailed, "inspect_failed", "inspect_failed",
                        "exported collection inspection failed");
        }
        if (!validate_tasks(handle)) return false;
        if (!validate_projects(handle)) return false;
        if (!validate_exact_record_set(handle)) return false;
        handle = mdbase::CollectionHandle{};
        return true;
    }

    bool write_report() {
        const std::string report = export_report_json(
            timezone_, context_.task_rels.size(), context_.project_rels.size(),
            context_.task_rels, context_.project_rels, context_.asset_count,
            context_.preserved_support);
        std::string error;
        if (!write_bytes(staged_ / ".todobench/mdbase_export_report.json", report, &error)) {
            return fail(TransferOutcome::IoFailed, "report_write_failed", "report_write_failed",
                        error, ".todobench/mdbase_export_report.json");
        }
        result_.report_path = destination_ / ".todobench/mdbase_export_report.json";
        return true;
    }

    bool publish() {
        if (is_cancelled()) return cancel("cancelled before publication");
        std::vector<TransferDiagnostic> diagnostics;
        if (!snapshot_unchanged(snapshot_, &diagnostics)) {
            result_.outcome = TransferOutcome::IoFailed;
            result_.error = "source_changed";
            result_.diagnostics = std::move(diagnostics);
            result_.diagnostics.push_back({TransferSeverity::Error, "source_changed",
                                           "source changed since snapshot; rescan required"});
            return false;
        }
        const auto published = publish_staged(
            staged_, destination_, &snapshot_, request_.cancellation);
        if (published.ok) {
            published_ = true;
            return true;
        }
        result_.outcome = published.error == "cancelled"
            ? TransferOutcome::Cancelled : TransferOutcome::IoFailed;
        result_.cancelled = published.error == "cancelled";
        result_.error = published.error;
        result_.diagnostics = published.diagnostics;
        return false;
    }

    void finish_success() {
        result_.outcome = TransferOutcome::Succeeded;
        result_.destination = destination_;
        result_.record_count = context_.task_rels.size() + context_.project_rels.size();
        result_.asset_count = context_.asset_count;
        result_.diagnostics.insert(result_.diagnostics.end(), context_.diags.begin(),
                                   context_.diags.end());
    }

    const ExportRequest& request_;
    TransferResult result_;
    std::filesystem::path destination_;
    std::filesystem::path workspace_root_;
    std::filesystem::path staged_;
    TransferSnapshot snapshot_;
    WorkspaceSnapshot workspace_snapshot_;
    std::string timezone_{"UTC"};
    ExportContext context_;
    size_t asset_count_{0};
    size_t preserved_support_{0};
    bool published_{false};
};

} // anonymous namespace

TransferResult export_workspace(const ExportRequest& request) {
    try {
        return ExportOperation(request).run();
    } catch (const std::exception& error) {
        TransferResult result;
        result.destination = request.destination;
        result.outcome = TransferOutcome::IoFailed;
        result.error = error.what();
        result.diagnostics.push_back(
            {TransferSeverity::Error, "exception", error.what()});
        return result;
    }
}

} // namespace todobench::mdbase_transfer
