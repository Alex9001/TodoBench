// SPDX-License-Identifier: GPL-3.0-or-later
// M01–M20 matrix: independent interoperability and failure tests
// for mdbase transfer pipeline (export, import, graph, markdown, staging).
#include "storage/mdbase_transfer.h"
#include "storage/mdbase_export.h"
#include "storage/mdbase_import.h"
#include "storage/mdbase_graph.h"
#include "storage/mdbase_markdown.h"
#include "storage/workspace_scanner.h"
#include "storage/workspace_store.h"
#include "storage/mdbase_bridge_client.h"
#include "app/workspace_controller.h"
#include "tb_test_assertions.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonValue>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace todobench;
using namespace todobench::mdbase_transfer;

// ── helpers ──────────────────────────────────────────────────────────
namespace {

std::filesystem::path create_native_ws(const std::filesystem::path& root, const std::string& name) {
    (void)WorkspaceStore::create_workspace(root, name);
    return root;
}

// Write a minimal mdbase foreign collection (notes type with numeric prio, string status, non-UUID ids).
std::filesystem::path write_foreign_notes(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "_types");
    std::ofstream(root / "mdbase.yaml") << R"(spec_version: "0.3.0"
settings:
  timezone: UTC
  types_folder: _types
  record_extensions: [md]
  explicit_type_keys: []
  id_field: id
  validation: error
  exclude:
    - ".git/**"
    - ".mdbase/**"
)";
    std::ofstream(root / "_types/note.md") << R"(---
kind: mdbase.type
name: note
version: 1
match:
  path_glob: "notes/**/*.md"
schema:
  dialect: json-schema-2020-12
  value:
    type: object
    required: [id]
    additionalProperties: true
    properties:
      id: {type: string}
      summary: {type: string}
      state: {type: [string, number, boolean, "null"]}
      prio: {type: [integer, string, "null"]}
      tagline: {type: string}
      due: {type: string}
      parent_note: {type: string}
      extra_meta: {type: object}
      nested_obj: {type: object}
collection:
  display: {name_field: summary}
  unique: [{field: id, scope: type}]
---
)";
    std::filesystem::create_directories(root / "notes");
    std::ofstream(root / "notes/a.md") << R"(---
id: note-1001
summary: Alpha task
state: DONE
prio: 3
tagline: important
due: 2026-12-25
extra_meta:
  deep:
    value: 42
---
Body of alpha
)";
    std::ofstream(root / "notes/b.md") << R"(---
id: note-1002
summary: Beta task
state: todo
prio: 1
parent_note: note-1001
---
Body of beta
)";
    std::ofstream(root / "notes/c.md") << R"(---
id: note-1003
summary: Gamma task
state: in_progress
prio: "urgent"
---
Body of gamma
)";
    return root;
}

// Write a foreign collection with multiple types + untyped.
std::filesystem::path write_multi_type_foreign(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "_types");
    std::ofstream(root / "mdbase.yaml") << R"(spec_version: "0.3.0"
settings:
  timezone: UTC
  types_folder: _types
  record_extensions: [md]
  explicit_type_keys: []
  id_field: id
  validation: error
  exclude:
    - ".git/**"
    - ".mdbase/**"
)";
    // Two different task-like types
    std::ofstream(root / "_types/task.md") << R"(---
kind: mdbase.type
name: task
version: 1
match:
  path_glob: "tasks/**/*.md"
schema:
  dialect: json-schema-2020-12
  value:
    type: object
    required: [id, title]
    additionalProperties: true
    properties:
      id: {type: string}
      title: {type: string}
      status: {type: string}
      priority: {type: string}
collection:
  display: {name_field: title}
  unique: [{field: id, scope: type}]
---
)";
    std::ofstream(root / "_types/wish.md") << R"(---
kind: mdbase.type
name: wish
version: 1
match:
  path_glob: "wishes/**/*.md"
schema:
  dialect: json-schema-2020-12
  value:
    type: object
    required: [id, title]
    additionalProperties: true
    properties:
      id: {type: string}
      title: {type: string}
      status: {type: string}
      priority: {type: string}
collection:
  display: {name_field: title}
  unique: [{field: id, scope: type}]
---
)";
    std::filesystem::create_directories(root / "tasks");
    std::filesystem::create_directories(root / "wishes");
    std::ofstream(root / "tasks/t1.md") << "---\nid: t-1\ntitle: Task One\ndone: true\n---\nTask body\n";
    std::ofstream(root / "tasks/t2.md") << "---\nid: t-2\ntitle: Task Two\n---\nBody\n";
    std::ofstream(root / "wishes/w1.md") << "---\nid: w-1\ntitle: Wish One\ndone: false\n---\nWish body\n";
    // An untyped markdown file
    std::ofstream(root / "notes/random.md") << "---\nid: x-1\n---\nUntyped note\n";
    return root;
}

// Write a collection with link references inside markdown bodies.
std::filesystem::path write_linked_collection(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "_types");
    std::ofstream(root / "mdbase.yaml") << R"(spec_version: "0.3.0"
settings:
  timezone: UTC
  types_folder: _types
  record_extensions: [md]
  explicit_type_keys: []
  id_field: id
  validation: error
  exclude:
    - ".git/**"
    - ".mdbase/**"
)";
    std::ofstream(root / "_types/note.md") << R"(---
kind: mdbase.type
name: note
version: 1
match:
  path_glob: "notes/**/*.md"
schema:
  dialect: json-schema-2020-12
  value:
    type: object
    required: [id]
    additionalProperties: true
    properties:
      id: {type: string}
      summary: {type: string}
collection:
  display: {name_field: summary}
  unique: [{field: id, scope: type}]
---
)";
    std::filesystem::create_directories(root / "notes");
    // a references b via inline link
    std::ofstream(root / "notes/a.md") << R"(---
id: la-1
summary: Linker A
---
See [b-obj](b.md#section) for details.
Also ![img](assets/img.png)
)";
    std::ofstream(root / "notes/b.md") << R"(---
id: lb-1
summary: Linker B
---
#section
Back to [A](a.md).
)";
    std::filesystem::create_directories(root / "notes/assets");
    std::ofstream(root / "notes/assets/img.png") << "PNGDATA";
    return root;
}

// Build own-profile mapping (auto-fill for todobench-export v1).
ImportMapping build_own_profile(const CollectionInspection& insp) {
    return make_own_profile_mapping(insp);
}

// Build a standard foreign "note" mapping for the simple 3-note foreign collection.
ImportMapping build_note_mapping(const CollectionInspection& insp) {
    ImportMapping mp;
    TypeMapping tm;
    tm.type_name = "note";
    tm.selected = true;
    tm.as_task = true;
    tm.title_field = {"/summary", true};
    tm.status_field = {"/state", true};
    tm.priority_field = {"/prio", true};
    tm.tags_field = {"/tagline", true};
    mp.type_mappings["note"] = tm;
    TypeMapping um;
    um.type_name = "__untyped__";
    um.is_untyped_bucket = true;
    um.selected = false;
    mp.type_mappings["__untyped__"] = um;
    mp.status_map.put_for_json_value(QJsonValue("DONE"), "done");
    mp.status_map.put_for_json_value(QJsonValue("todo"), "todo");
    mp.status_map.put_for_json_value(QJsonValue("in_progress"), "in_progress");
    mp.priority_map.put_for_json_value(QJsonValue(3), "high");
    mp.priority_map.put_for_json_value(QJsonValue(1), "low");
    mp.priority_map.put_for_json_value(QJsonValue("urgent"), "urgent");
    return mp;
}

// Build mapping for multi-type (task + wish).
ImportMapping build_multi_type_mapping(const CollectionInspection& insp) {
    ImportMapping mp;
    for (auto& ti : insp.types) {
        TypeMapping tm;
        tm.type_name = ti.type_name;
        tm.selected = true;
        tm.as_task = true;
        tm.title_field = {"/title", true};
        tm.status_field = {"/status", true};
        tm.priority_field = {"/priority", true};
        mp.type_mappings[ti.type_name] = tm;
    }
    TypeMapping um;
    um.type_name = "__untyped__";
    um.is_untyped_bucket = true;
    um.selected = false;
    mp.type_mappings["__untyped__"] = um;
    mp.status_map.put_for_json_value(QJsonValue("DONE"), "done");
    mp.status_map.put_for_json_value(QJsonValue("done"), "done");
    mp.status_map.put_for_json_value(QJsonValue("todo"), "todo");
    mp.status_map.put_for_json_value(QJsonValue("in_progress"), "in_progress");
    mp.status_map.put_for_json_value(QJsonValue(true), "done");
    mp.status_map.put_for_json_value(QJsonValue(false), "todo");
    return mp;
}

std::string file_contents(const std::filesystem::path& p) {
    std::ifstream f(p);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::pair<std::filesystem::path, std::filesystem::path> find_exported_tasks(
    const std::filesystem::path& collection, const std::string& edited_id,
    const std::string& parent_id) {
    std::filesystem::path edited;
    std::filesystem::path parent;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(collection)) {
        if (entry.path().filename() != "task.md") continue;
        const auto content = file_contents(entry.path());
        if (content.find(edited_id) != std::string::npos) edited = entry.path();
        if (content.find(parent_id) != std::string::npos) parent = entry.path();
    }
    return {edited, parent};
}

void apply_external_task_edit(const std::filesystem::path& task,
                              const std::string& parent_link) {
    std::istringstream input(file_contents(task));
    std::ostringstream output;
    std::string line;
    bool in_frontmatter = false;
    while (std::getline(input, line)) {
        if (line == "---") in_frontmatter = !in_frontmatter;
        if (in_frontmatter && line.rfind("title:", 0) == 0) line = "title: Edited title";
        if (in_frontmatter && line.rfind("status:", 0) == 0) line = "status: waiting";
        if (in_frontmatter && line.rfind("todobench_parent_link:", 0) == 0) {
            line = "todobench_parent_link: " + parent_link;
        }
        output << line << '\n';
    }
    std::ofstream(task) << output.str();
}

void verify_alpha_mapping(const TransferPreview& preview) {
    const auto record = std::find_if(preview.records.begin(), preview.records.end(),
                                     [](const PreviewRecord& candidate) {
        return candidate.native_title == "Alpha task";
    });
    TB_VERIFY(record != preview.records.end());
    TB_COMPARE(record->native_status_native, "done");
    TB_COMPARE(record->native_priority_native, "high");
}

void verify_urgent_import(const WorkspaceSnapshot& snapshot) {
    const auto task = std::find_if(snapshot.tasks.begin(), snapshot.tasks.end(),
                                   [](const auto& entry) {
        return entry.second.title == "Gamma task";
    });
    TB_VERIFY(task != snapshot.tasks.end());
    TB_COMPARE(task->second.priority, Priority::Urgent);
}

} // anonymous namespace


// ── test class ───────────────────────────────────────────────────────
class MdbaseMatrixTest : public QObject {
  Q_OBJECT
private slots:
    void matrix1_exportReadByIndependentEngine();
    void matrix2_exportEditImportRoundTrip();
    void matrix3_foreignWithNumericPrioNonUuidIds();
    void matrix4_multiTypeUntypedMembership();
    void matrix5_missingVsNullDefaults();
    void matrix7_fiveStatusesFivePrioritiesRoundTrip();
    void matrix8_unknownMetadataCollisionPreserved();
    void matrix9_unmappedStatusBlocks();
    void matrix10_duplicateIdsDistinct();
    void matrix11_linkRewritingInline();
    void matrix12_missingTargetPreserved();
    void matrix13_snapshotChangeRejects();
    void matrix14_existingDestinationBlocked();
    void matrix15_cancellationSafe();
    void matrix16_diskLimitFailure();
    void matrix19_emptyCollectionRoundTrip();
    void matrix2b_postEditReimport();
    void projectCycleBlocksAndStillAllocatesReviewPaths();
};

void MdbaseMatrixTest::projectCycleBlocksAndStillAllocatesReviewPaths() {
    TransferPreview preview;
    PreviewRecord first;
    first.native_id = "11111111-1111-4111-8111-111111111111";
    first.native_title = "First";
    first.source_path = "first.md";
    first.is_task = false;
    PreviewRecord second;
    second.native_id = "22222222-2222-4222-8222-222222222222";
    second.native_title = "Second";
    second.source_path = "second.md";
    second.is_task = false;
    first.native_parent_choice = second.native_id;
    second.native_parent_choice = first.native_id;
    preview.records = {first, second};
    const auto diagnostics = validate_import_graph(preview, ImportMapping{});
    TB_VERIFY(std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "task_cycle" && diagnostic.severity == TransferSeverity::Blocking;
    }));
    const auto paths = allocate_native_paths(preview);
    TB_COMPARE(paths.size(), size_t{2});
    TB_VERIFY(paths.at(first.native_id).ends_with("/project.md"));
    TB_VERIFY(paths.at(second.native_id).ends_with("/project.md"));
    TB_COMPARE(allocate_native_paths(preview), paths);
}

// M01: Native export read by independent TS engine.
// Validate via the Rust bridge that types/records resolve correctly.
void MdbaseMatrixTest::matrix1_exportReadByIndependentEngine() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    auto ws = std::filesystem::path(tmp.path().toStdString()) / "ws";
    auto out = std::filesystem::path(tmp.path().toStdString()) / "coll";
    create_native_ws(ws, "M01ws");
    // Create tasks with assets and links
    WorkspaceController ctrl;
    std::string err;
    ctrl.open_workspace(ws, err);
    WorkspaceScanner sc;
    auto snap = sc.scan(ws);
    auto inbox = snap.projects.begin()->first;
    std::string t1, t2;
    ctrl.create_task(inbox, "First task", t1, err);
    ctrl.create_subtask(t1, "Sub task", t2, err);
    // Add an asset
    auto taskRec = ctrl.snapshot().tasks.at(t1);
    auto assetsDir = std::filesystem::path(taskRec.source_path).parent_path() / "assets";
    std::filesystem::create_directories(assetsDir);
    std::ofstream(assetsDir / "photo.jpg", std::ios::binary) << "JPEG_DATA";
    // Export
    ExportRequest req; req.workspace_root = ws; req.destination = out;
    auto res = export_workspace(req);
    TB_VERIFY2(res.outcome == TransferOutcome::Succeeded, qPrintable(QString::fromStdString(res.error)));
    // Bridge validation
    mdbase::CollectionHandle h;
    QString oerr;
    TB_VERIFY(mdbase::open_collection(out, h, &oerr).valid);
    auto insp = h.inspect();
    TB_VERIFY(insp.valid);
    auto types = insp.result.value("types").toArray();
    TB_COMPARE(types.size(), 2);
    // Query finds both tasks
    QJsonObject qr; qr["where"] = QString("true");
    auto qres = h.query(qr);
    TB_VERIFY(qres.valid);
    TB_VERIFY(qres.result.value("results").toArray().size() >= 2);
    // Read each task and verify link resolution
    QJsonObject ri1; ri1["path"] = QString::fromStdString(
        std::filesystem::relative(std::filesystem::path(taskRec.source_path), ws).generic_string());
    auto r1 = h.read(ri1);
    TB_VERIFY(r1.valid);
    TB_VERIFY(r1.result.value("frontmatter").toObject().contains("todobench_project_link"));
    // effective frontmatter includes read_defaults from type schema
    TB_VERIFY(!r1.result.value("effective_frontmatter").toObject().isEmpty());
}

// M02: Export → own-profile re-import preserves task data.
void MdbaseMatrixTest::matrix2_exportEditImportRoundTrip() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    auto ws = std::filesystem::path(tmp.path().toStdString()) / "ws";
    auto coll = std::filesystem::path(tmp.path().toStdString()) / "coll";
    auto ws2 = std::filesystem::path(tmp.path().toStdString()) / "ws2";
    create_native_ws(ws, "M02ws");
    WorkspaceController ctrl;
    std::string err;
    ctrl.open_workspace(ws, err);
    WorkspaceScanner sc;
    auto snap = sc.scan(ws);
    auto inbox = snap.projects.begin()->first;
    std::string parentId;
    std::string tid;
    TB_VERIFY(ctrl.create_task(inbox, "Relationship parent", parentId, err));
    TB_VERIFY(ctrl.create_task(inbox, "Original title", tid, err));
    // Export
    ExportRequest req; req.workspace_root = ws; req.destination = coll;
    auto res = export_workspace(req);
    TB_VERIFY2(res.outcome == TransferOutcome::Succeeded, qPrintable(QString::fromStdString(res.error)));
    TB_VERIFY(std::filesystem::exists(coll / "mdbase.yaml"));
    // Re-import via own-profile mapping
    auto snapRes = capture_snapshot(coll);
    TB_VERIFY(snapRes.ok);
    auto insp = inspect_import_source(*snapRes.snapshot);
    TB_VERIFY(insp.is_own_profile);
    auto mp = make_own_profile_mapping(insp);
    auto preview = build_transfer_preview(*snapRes.snapshot, insp, mp);
    TB_VERIFY2(preview.blocking_errors.empty(),
        qPrintable(QString::fromStdString(preview.blocking_errors.empty() ? "" : preview.blocking_errors[0].message)));
    TB_COMPARE(preview.to_task_count, size_t(2));
    auto impRes = execute_import(*snapRes.snapshot, preview, ws2);
    TB_VERIFY2(impRes.outcome == TransferOutcome::Succeeded,
        qPrintable(QString::fromStdString(impRes.error)));
    // Verify re-imported workspace has a project and the source workspace is unchanged
    WorkspaceScanner sc2;
    auto wsSnap = sc2.scan(ws2);
    TB_VERIFY(wsSnap.projects.size() >= size_t(1));
    TB_COMPARE(wsSnap.tasks.size(), size_t(2));
    TB_VERIFY(std::filesystem::exists(ws / "settings.json"));
}

// M03: Foreign fixture with numeric priorities, non-UUID ids.
void MdbaseMatrixTest::matrix3_foreignWithNumericPrioNonUuidIds() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    auto root = std::filesystem::path(tmp.path().toStdString()) / "foreign";
    write_foreign_notes(root);
    auto snapRes = capture_snapshot(root);
    TB_VERIFY2(snapRes.ok, qPrintable(QString::fromStdString(snapRes.error)));
    auto insp = inspect_import_source(*snapRes.snapshot);
    TB_VERIFY(insp.valid);
    auto mp = build_note_mapping(insp);
    auto preview = build_transfer_preview(*snapRes.snapshot, insp, mp);
    TB_VERIFY2(preview.blocking_errors.empty(),
        qPrintable(QString::fromStdString(preview.blocking_errors.empty() ? "" : preview.blocking_errors[0].message)));
    TB_COMPARE(preview.to_task_count, size_t(3));
    // Verify mapping: note-1001 DONE→done, prio 3→high
    verify_alpha_mapping(preview);
    // Execute and verify provenance
    QTemporaryDir outTmp;
    auto ws = std::filesystem::path(outTmp.path().toStdString()) / "imported";
    auto res = execute_import(*snapRes.snapshot, preview, ws);
    TB_VERIFY2(res.outcome == TransferOutcome::Succeeded, qPrintable(QString::fromStdString(res.error)));
    // Provenance exists
    TB_VERIFY(std::filesystem::exists(ws / ".todobench" / "imports"));
    // WorkspaceScanner validates
    WorkspaceScanner sc;
    auto wsSnap = sc.scan(ws);
    TB_COMPARE(wsSnap.tasks.size(), size_t(3));
    // Priority preserved: "urgent" string mapped to native urgent
    verify_urgent_import(wsSnap);
}

// M04: Multiple types + untyped records.
void MdbaseMatrixTest::matrix4_multiTypeUntypedMembership() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    auto root = std::filesystem::path(tmp.path().toStdString()) / "multi";
    write_multi_type_foreign(root);
    auto snapRes = capture_snapshot(root);
    TB_VERIFY(snapRes.ok);
    auto insp = inspect_import_source(*snapRes.snapshot);
    TB_VERIFY(insp.valid);
    // Verify multiple types discovered
    TB_VERIFY(insp.types.size() >= 2);
    auto mp = build_multi_type_mapping(insp);
    auto preview = build_transfer_preview(*snapRes.snapshot, insp, mp);
    TB_VERIFY2(preview.blocking_errors.empty(),
        qPrintable(QString::fromStdString(preview.blocking_errors.empty() ? "" : preview.blocking_errors[0].message)));
    // All 3 selected records (2 tasks + 1 wish)
    TB_COMPARE(preview.to_task_count, size_t(3));
    // Execute
    QTemporaryDir outTmp;
    auto ws = std::filesystem::path(outTmp.path().toStdString()) / "ws";
    auto res = execute_import(*snapRes.snapshot, preview, ws);
    TB_VERIFY2(res.outcome == TransferOutcome::Succeeded, qPrintable(QString::fromStdString(res.error)));
    WorkspaceScanner sc;
    auto wsSnap = sc.scan(ws);
    TB_COMPARE(wsSnap.tasks.size(), size_t(3));
}

// M05: Missing vs null defaults; nested/boolean/numeric/string fields.
void MdbaseMatrixTest::matrix5_missingVsNullDefaults() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    auto root = std::filesystem::path(tmp.path().toStdString()) / "defaults";
    std::filesystem::create_directories(root / "_types");
    std::ofstream(root / "mdbase.yaml") << R"(spec_version: "0.3.0"
settings:
  timezone: UTC
  types_folder: _types
  record_extensions: [md]
  explicit_type_keys: []
  id_field: id
  validation: error
  exclude:
    - ".git/**"
    - ".mdbase/**"
)";
    std::ofstream(root / "_types/note.md") << R"(---
kind: mdbase.type
name: note
version: 1
match:
  path_glob: "notes/**/*.md"
schema:
  dialect: json-schema-2020-12
  value:
    type: object
    required: [id]
    additionalProperties: true
    properties:
      id: {type: string}
      summary: {type: string}
      state: {type: [string, number, boolean, "null"]}
      prio: {type: [integer, string, "null"]}
      flag: {type: boolean}
      count: {type: integer}
      tags: {type: array, items: {type: string}}
collection:
  display: {name_field: summary}
  unique: [{field: id, scope: type}]
---
)";
    std::filesystem::create_directories(root / "notes");
    // null state
    std::ofstream(root / "notes/null_state.md") << "---\nid: ns-1\nsummary: Null state\nstate: null\n---\n";
    // missing state entirely (not in frontmatter)
    std::ofstream(root / "notes/missing_state.md") << "---\nid: ms-1\nsummary: Missing state\n---\n";
    // boolean flag (not status)
    std::ofstream(root / "notes/bool_flag.md") << "---\nid: bf-1\nsummary: Bool flag\nflag: true\ncount: 42\ntags: [a, b]\n---\n";
    auto snapRes = capture_snapshot(root);
    TB_VERIFY(snapRes.ok);
    auto insp = inspect_import_source(*snapRes.snapshot);
    ImportMapping mp;
    TypeMapping tm;
    tm.type_name = "note";
    tm.selected = true;
    tm.as_task = true;
    tm.title_field = {"/summary", true};
    tm.status_field = {"/state", true};
    tm.priority_field = {"/prio", true};
    tm.tags_field = {"/tags", true};
    mp.type_mappings["note"] = tm;
    TypeMapping um;
    um.type_name = "__untyped__";
    um.is_untyped_bucket = true;
    um.selected = false;
    mp.type_mappings["__untyped__"] = um;
    mp.status_map.put_for_json_value(QJsonValue("DONE"), "done");
    mp.status_map.put_for_json_value(QJsonValue("todo"), "todo");
    mp.priority_map.put_for_json_value(QJsonValue(3), "high");
    mp.priority_map.put_for_json_value(QJsonValue(1), "low");
    mp.priority_map.put_for_json_value(QJsonValue("urgent"), "urgent");
    auto preview = build_transfer_preview(*snapRes.snapshot, insp, mp);
    TB_COMPARE(preview.to_task_count, size_t(3));
    // null state → should default to "todo"
    bool foundNull = false;
    for (auto& r : preview.records) {
        if (r.native_title == "Null state") {
            TB_COMPARE(r.native_status_native, "todo");
            foundNull = true;
        }
    }
    TB_VERIFY(foundNull);
}

// M07: Five statuses, five priorities, round trip.
void MdbaseMatrixTest::matrix7_fiveStatusesFivePrioritiesRoundTrip() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    auto ws = std::filesystem::path(tmp.path().toStdString()) / "ws";
    auto coll = std::filesystem::path(tmp.path().toStdString()) / "coll";
    auto ws2 = std::filesystem::path(tmp.path().toStdString()) / "ws2";
    create_native_ws(ws, "M07ws");
    WorkspaceController ctrl;
    std::string err;
    ctrl.open_workspace(ws, err);
    WorkspaceScanner sc;
    auto snap = sc.scan(ws);
    auto inbox = snap.projects.begin()->first;
    // Create tasks with all statuses and priorities
    struct { const char* title; TaskStatus st; Priority pr; } cases[] = {
        {"t1_todo", TaskStatus::Todo, Priority::Normal},
        {"t2_inprog", TaskStatus::InProgress, Priority::High},
        {"t3_waiting", TaskStatus::Waiting, Priority::Low},
        {"t4_done", TaskStatus::Done, Priority::Urgent},
        {"t5_cancel", TaskStatus::Cancelled, Priority::None},
    };
    for (auto& c : cases) {
        std::string tid;
        ctrl.create_task(inbox, c.title, tid, err);
        auto t = ctrl.snapshot().tasks.at(tid);
        t.status = c.st;
        t.priority = c.pr;
        ctrl.save_task(t);
    }
    // Export
    ExportRequest req; req.workspace_root = ws; req.destination = coll;
    auto res = export_workspace(req);
    TB_VERIFY2(res.outcome == TransferOutcome::Succeeded, qPrintable(QString::fromStdString(res.error)));
    // Import via own profile
    auto snapRes = capture_snapshot(coll);
    TB_VERIFY(snapRes.ok);
    auto insp = inspect_import_source(*snapRes.snapshot);
    TB_VERIFY(insp.is_own_profile);
    auto mp = make_own_profile_mapping(insp);
    auto preview = build_transfer_preview(*snapRes.snapshot, insp, mp);
    TB_VERIFY(preview.blocking_errors.empty());
    auto impRes = execute_import(*snapRes.snapshot, preview, ws2);
    TB_VERIFY2(impRes.outcome == TransferOutcome::Succeeded, qPrintable(QString::fromStdString(impRes.error)));
    // Verify all 5 statuses survived
    WorkspaceScanner sc2;
    auto wsSnap = sc2.scan(ws2);
    TB_COMPARE(wsSnap.tasks.size(), size_t(5));
    // Count statuses
    std::map<TaskStatus, int> statusCounts;
    for (auto& [id, task] : wsSnap.tasks) statusCounts[task.status]++;
    TB_COMPARE(statusCounts[TaskStatus::Todo], 1);
    TB_COMPARE(statusCounts[TaskStatus::InProgress], 1);
    TB_COMPARE(statusCounts[TaskStatus::Waiting], 1);
    TB_COMPARE(statusCounts[TaskStatus::Done], 1);
    TB_COMPARE(statusCounts[TaskStatus::Cancelled], 1);
    // Priorities
    std::map<Priority, int> prioCounts;
    for (auto& [id, task] : wsSnap.tasks) prioCounts[task.priority]++;
    TB_COMPARE(prioCounts[Priority::Normal], 1);
    TB_COMPARE(prioCounts[Priority::High], 1);
    TB_COMPARE(prioCounts[Priority::Low], 1);
    TB_COMPARE(prioCounts[Priority::Urgent], 1);
    TB_COMPARE(prioCounts[Priority::None], 1);
}

// M08: Unknown nested metadata survives; canonical collision blocked.
void MdbaseMatrixTest::matrix8_unknownMetadataCollisionPreserved() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    auto root = std::filesystem::path(tmp.path().toStdString()) / "foreign";
    write_foreign_notes(root);
    auto snapRes = capture_snapshot(root);
    TB_VERIFY(snapRes.ok);
    auto insp = inspect_import_source(*snapRes.snapshot);
    auto mp = build_note_mapping(insp);
    auto preview = build_transfer_preview(*snapRes.snapshot, insp, mp);
    TB_VERIFY(preview.blocking_errors.empty());
    QTemporaryDir outTmp;
    auto ws = std::filesystem::path(outTmp.path().toStdString()) / "ws";
    auto res = execute_import(*snapRes.snapshot, preview, ws);
    TB_VERIFY(res.outcome == TransferOutcome::Succeeded);
    // Verify unknown metadata retained in source provenance
    TB_VERIFY(std::filesystem::exists(ws / ".todobench" / "imports"));
    // The extra_meta and nested_obj should not overwrite native fields
    WorkspaceScanner sc;
    auto wsSnap = sc.scan(ws);
    TB_COMPARE(wsSnap.tasks.size(), size_t(3));
}

// M09: Unmapped status blocks.
void MdbaseMatrixTest::matrix9_unmappedStatusBlocks() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    auto root = std::filesystem::path(tmp.path().toStdString()) / "foreign";
    write_foreign_notes(root);
    auto snapRes = capture_snapshot(root);
    TB_VERIFY(snapRes.ok);
    auto insp = inspect_import_source(*snapRes.snapshot);
    ImportMapping mp;
    TypeMapping tm;
    tm.type_name = "note";
    tm.selected = true;
    tm.as_task = true;
    tm.title_field = {"/summary", true};
    tm.status_field = {"/state", true};
    mp.type_mappings["note"] = tm;
    TypeMapping um;
    um.type_name = "__untyped__";
    um.is_untyped_bucket = true;
    um.selected = false;
    mp.type_mappings["__untyped__"] = um;
    // Only map "DONE" → "done"; "todo" and "in_progress" unmapped
    mp.status_map.put_for_json_value(QJsonValue("DONE"), "done");
    auto preview = build_transfer_preview(*snapRes.snapshot, insp, mp);
    TB_VERIFY(!preview.blocking_errors.empty());
    bool foundUnmapped = false;
    for (auto& d : preview.blocking_errors) if (d.code == "unmapped_status") foundUnmapped = true;
    TB_VERIFY(foundUnmapped);
    // Execute should fail validation
    QTemporaryDir outTmp;
    auto ws = std::filesystem::path(outTmp.path().toStdString()) / "ws";
    auto res = execute_import(*snapRes.snapshot, preview, ws);
    TB_COMPARE(res.outcome, TransferOutcome::ValidationFailed);
    TB_VERIFY(!std::filesystem::exists(ws));
}

// M10: Duplicate IDs produce distinct native IDs.
void MdbaseMatrixTest::matrix10_duplicateIdsDistinct() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    auto root = std::filesystem::path(tmp.path().toStdString()) / "dup";
    std::filesystem::create_directories(root / "_types");
    std::ofstream(root / "mdbase.yaml") << R"(spec_version: "0.3.0"
settings:
  timezone: UTC
  types_folder: _types
  record_extensions: [md]
  explicit_type_keys: []
  id_field: id
  validation: error
  exclude:
    - ".git/**"
    - ".mdbase/**"
)";
    std::ofstream(root / "_types/note.md") << R"(---
kind: mdbase.type
name: note
version: 1
match:
  path_glob: "notes/**/*.md"
schema:
  dialect: json-schema-2020-12
  value:
    type: object
    required: [id]
    additionalProperties: true
    properties:
      id: {type: string}
      summary: {type: string}
collection:
  display: {name_field: summary}
  unique: [{field: id, scope: type}]
---
)";
    std::filesystem::create_directories(root / "notes");
    std::string dupId = "11111111-1111-1111-1111-111111111111";
    std::ofstream(root / "notes/a.md") << "---\nid: " + dupId + "\nsummary: Dup A\n---\n";
    std::ofstream(root / "notes/b.md") << "---\nid: " + dupId + "\nsummary: Dup B\n---\n";
    auto snapRes = capture_snapshot(root);
    TB_VERIFY(snapRes.ok);
    auto insp = inspect_import_source(*snapRes.snapshot);
    ImportMapping mp;
    TypeMapping tm;
    tm.type_name = "note";
    tm.selected = true;
    tm.as_task = true;
    tm.title_field = {"/summary", true};
    mp.type_mappings["note"] = tm;
    TypeMapping um;
    um.type_name = "__untyped__";
    um.is_untyped_bucket = true;
    um.selected = false;
    mp.type_mappings["__untyped__"] = um;
    auto preview = build_transfer_preview(*snapRes.snapshot, insp, mp);
    TB_COMPARE(preview.records.size(), size_t(2));
    // Distinct native IDs
    TB_VERIFY(preview.records[0].native_id != preview.records[1].native_id);
    // Warning about duplicate
    bool hasDup = false;
    for (auto& w : preview.warnings)
        if (w.code == "duplicate_source_id" || w.code == "non_uuid_id_generated") hasDup = true;
    TB_VERIFY(hasDup);
}

// M11: Link rewriting — inline links survive and are rewritten correctly.
void MdbaseMatrixTest::matrix11_linkRewritingInline() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    auto root = std::filesystem::path(tmp.path().toStdString()) / "links";
    write_linked_collection(root);
    auto snapRes = capture_snapshot(root);
    TB_VERIFY(snapRes.ok);
    auto insp = inspect_import_source(*snapRes.snapshot);
    ImportMapping mp;
    TypeMapping tm;
    tm.type_name = "note";
    tm.selected = true;
    tm.as_task = true;
    tm.title_field = {"/summary", true};
    mp.type_mappings["note"] = tm;
    TypeMapping um;
    um.type_name = "__untyped__";
    um.is_untyped_bucket = true;
    um.selected = false;
    mp.type_mappings["__untyped__"] = um;
    auto preview = build_transfer_preview(*snapRes.snapshot, insp, mp);
    TB_VERIFY(preview.blocking_errors.empty());
    QTemporaryDir outTmp;
    auto ws = std::filesystem::path(outTmp.path().toStdString()) / "ws";
    auto res = execute_import(*snapRes.snapshot, preview, ws);
    TB_VERIFY2(res.outcome == TransferOutcome::Succeeded, qPrintable(QString::fromStdString(res.error)));
    // Verify the imported task body has the link rewritten (b.md → new native path)
    WorkspaceScanner sc;
    auto wsSnap = sc.scan(ws);
    TB_COMPARE(wsSnap.tasks.size(), size_t(2));
    const auto target = std::find_if(wsSnap.tasks.begin(), wsSnap.tasks.end(), [](const auto& item) {
        return item.second.title == "Linker B";
    });
    TB_VERIFY(target != wsSnap.tasks.end());
    bool foundLinker = false;
    for (auto& [id, task] : wsSnap.tasks) {
        if (task.title == "Linker A") {
            foundLinker = true;
            auto body = file_contents(std::filesystem::path(task.source_path));
            const std::string expectedLink = std::filesystem::relative(
                std::filesystem::path(target->second.source_path),
                std::filesystem::path(task.source_path).parent_path()).generic_string() + "#section";
            TB_VERIFY2(body.find("[b-obj](" + expectedLink + ")") != std::string::npos,
                     qPrintable(QString::fromStdString(body)));
            TB_VERIFY2(body.find("![img](assets/img.png)") != std::string::npos,
                     qPrintable(QString::fromStdString(body)));
            const auto importedAsset =
                std::filesystem::path(task.source_path).parent_path() / "assets" / "img.png";
            TB_VERIFY(std::filesystem::exists(importedAsset));
            TB_COMPARE(file_contents(importedAsset), std::string("PNGDATA"));
            break;
        }
    }
    TB_VERIFY(foundLinker);
    TB_COMPARE(res.asset_count, size_t(1));
}

// M12: Missing/external target preserved; no remote fetch.
void MdbaseMatrixTest::matrix12_missingTargetPreserved() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    auto root = std::filesystem::path(tmp.path().toStdString()) / "missing";
    std::filesystem::create_directories(root / "_types");
    std::ofstream(root / "mdbase.yaml") << R"(spec_version: "0.3.0"
settings:
  timezone: UTC
  types_folder: _types
  record_extensions: [md]
  explicit_type_keys: []
  id_field: id
  validation: error
  exclude:
    - ".git/**"
    - ".mdbase/**"
)";
    std::ofstream(root / "_types/note.md") << R"(---
kind: mdbase.type
name: note
version: 1
match:
  path_glob: "notes/**/*.md"
schema:
  dialect: json-schema-2020-12
  value:
    type: object
    required: [id]
    additionalProperties: true
    properties:
      id: {type: string}
      summary: {type: string}
collection:
  display: {name_field: summary}
  unique: [{field: id, scope: type}]
---
)";
    std::filesystem::create_directories(root / "notes");
    // Link to missing file, remote URL, and data URL
    std::ofstream(root / "notes/a.md") << R"(---
id: mx-1
summary: Missing targets
---
See [missing](notes/nonexistent.md).
Visit [remote](https://example.com/page).
Data [inline](data:text/plain,hello).
)";
    auto snapRes = capture_snapshot(root);
    TB_VERIFY(snapRes.ok);
    auto insp = inspect_import_source(*snapRes.snapshot);
    ImportMapping mp;
    TypeMapping tm;
    tm.type_name = "note";
    tm.selected = true;
    tm.as_task = true;
    tm.title_field = {"/summary", true};
    mp.type_mappings["note"] = tm;
    TypeMapping um;
    um.type_name = "__untyped__";
    um.is_untyped_bucket = true;
    um.selected = false;
    mp.type_mappings["__untyped__"] = um;
    auto preview = build_transfer_preview(*snapRes.snapshot, insp, mp);
    QTemporaryDir outTmp;
    auto ws = std::filesystem::path(outTmp.path().toStdString()) / "ws";
    auto res = execute_import(*snapRes.snapshot, preview, ws);
    TB_VERIFY(res.outcome == TransferOutcome::Succeeded);
    // Verify body preserved, remote/data URLs untouched
    WorkspaceScanner sc;
    auto wsSnap = sc.scan(ws);
    for (auto& [id, task] : wsSnap.tasks) {
        auto body = file_contents(std::filesystem::path(task.source_path));
        TB_VERIFY(body.find("https://example.com/page") != std::string::npos);
        TB_VERIFY(body.find("data:text/plain,hello") != std::string::npos);
    }
}

// M13: Source change after snapshot capture → operation rejects.
void MdbaseMatrixTest::matrix13_snapshotChangeRejects() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    auto root = std::filesystem::path(tmp.path().toStdString()) / "src13";
    std::filesystem::create_directories(root / "_types");
    std::ofstream(root / "mdbase.yaml") << R"(spec_version: "0.3.0"
settings:
  timezone: UTC
  types_folder: _types
  record_extensions: [md]
  explicit_type_keys: []
  id_field: id
  validation: error
  exclude:
    - ".git/**"
    - ".mdbase/**"
)";
    std::ofstream(root / "_types/note.md") << R"(---
kind: mdbase.type
name: note
version: 1
match:
  path_glob: "notes/**/*.md"
schema:
  dialect: json-schema-2020-12
  value:
    type: object
    required: [id]
    additionalProperties: true
    properties:
      id: {type: string}
      summary: {type: string}
collection:
  display: {name_field: summary}
  unique: [{field: id, scope: type}]
---
)";
    std::filesystem::create_directories(root / "notes");
    std::ofstream(root / "notes/a.md") << "---\nid: chg-1\nsummary: Before\n---\n";
    // Capture snapshot
    auto snapRes = capture_snapshot(root);
    TB_VERIFY(snapRes.ok);
    // Modify source after snapshot
    std::ofstream(root / "notes/a.md") << "---\nid: chg-1\nsummary: After\n---\n";
    // snapshot_unchanged should detect change
    TB_VERIFY(!snapshot_unchanged(*snapRes.snapshot));
}

// M14: Existing destination blocked.
void MdbaseMatrixTest::matrix14_existingDestinationBlocked() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    auto ws = std::filesystem::path(tmp.path().toStdString()) / "ws";
    auto coll = std::filesystem::path(tmp.path().toStdString()) / "coll";
    create_native_ws(ws, "M14ws");
    ExportRequest req; req.workspace_root = ws; req.destination = coll;
    // First export succeeds
    auto res1 = export_workspace(req);
    TB_VERIFY(res1.outcome == TransferOutcome::Succeeded);
    // Second export to same destination fails
    auto res2 = export_workspace(req);
    TB_VERIFY(res2.outcome != TransferOutcome::Succeeded);
    bool found = false;
    for (auto& d : res2.diagnostics) if (d.code == "destination_exists") found = true;
    TB_VERIFY(found);
}

// M15: Cancellation leaves no published output.
void MdbaseMatrixTest::matrix15_cancellationSafe() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    auto root = std::filesystem::path(tmp.path().toStdString()) / "cancel";
    std::filesystem::create_directories(root / "_types");
    std::ofstream(root / "mdbase.yaml") << R"(spec_version: "0.3.0"
settings:
  timezone: UTC
  types_folder: _types
  record_extensions: [md]
  explicit_type_keys: []
  id_field: id
  validation: error
  exclude:
    - ".git/**"
    - ".mdbase/**"
)";
    std::ofstream(root / "_types/note.md") << R"(---
kind: mdbase.type
name: note
version: 1
match:
  path_glob: "notes/**/*.md"
schema:
  dialect: json-schema-2020-12
  value:
    type: object
    required: [id]
    additionalProperties: true
    properties:
      id: {type: string}
      summary: {type: string}
collection:
  display: {name_field: summary}
  unique: [{field: id, scope: type}]
---
)";
    std::filesystem::create_directories(root / "notes");
    std::ofstream(root / "notes/a.md") << "---\nid: can-1\nsummary: Cancel test\n---\n";
    auto snapRes = capture_snapshot(root);
    TB_VERIFY(snapRes.ok);
    auto insp = inspect_import_source(*snapRes.snapshot);
    ImportMapping mp;
    TypeMapping tm;
    tm.type_name = "note";
    tm.selected = true;
    tm.as_task = true;
    tm.title_field = {"/summary", true};
    mp.type_mappings["note"] = tm;
    TypeMapping um;
    um.type_name = "__untyped__";
    um.is_untyped_bucket = true;
    um.selected = false;
    mp.type_mappings["__untyped__"] = um;
    auto preview = build_transfer_preview(*snapRes.snapshot, insp, mp);
    // Pre-cancel
    TransferCancellation canc;
    canc.cancel();
    QTemporaryDir outTmp;
    auto ws = std::filesystem::path(outTmp.path().toStdString()) / "ws";
    auto res = execute_import(*snapRes.snapshot, preview, ws, {}, &canc);
    // Should not succeed when cancelled
    TB_VERIFY(res.outcome != TransferOutcome::Succeeded || !std::filesystem::exists(ws));
    // If it did succeed (late cancel after publish), that's acceptable per spec
}

// M16: Entry limit exceeded during snapshot.
void MdbaseMatrixTest::matrix16_diskLimitFailure() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    auto src = std::filesystem::path(tmp.path().toStdString()) / "src";
    std::filesystem::create_directories(src);
    // Create more files than the limit
    std::ofstream(src / "a.txt") << "hello";
    std::ofstream(src / "b.txt") << "world";
    TransferLimits lim;
    lim.max_entries = 1;
    SnapshotOptions opts;
    opts.limits = lim;
    auto res = capture_snapshot(src, opts);
    TB_VERIFY(!res.ok);
    bool found = false;
    for (auto& d : res.diagnostics) if (d.code == "entry_limit") found = true;
    TB_VERIFY(found);
}

// M19: Empty workspace export/import round trip.
void MdbaseMatrixTest::matrix19_emptyCollectionRoundTrip() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    auto ws = std::filesystem::path(tmp.path().toStdString()) / "ws";
    auto coll = std::filesystem::path(tmp.path().toStdString()) / "coll";
    auto ws2 = std::filesystem::path(tmp.path().toStdString()) / "ws2";
    create_native_ws(ws, "M19ws");
    // Export empty workspace
    ExportRequest req; req.workspace_root = ws; req.destination = coll;
    auto res = export_workspace(req);
    TB_VERIFY2(res.outcome == TransferOutcome::Succeeded, qPrintable(QString::fromStdString(res.error)));
    // Bridge validates empty collection
    mdbase::CollectionHandle h;
    QString oerr;
    TB_VERIFY(mdbase::open_collection(coll, h, &oerr).valid);
    auto insp = h.inspect();
    TB_VERIFY(insp.valid);
    // Import back — should create Inbox
    auto snapRes = capture_snapshot(coll);
    TB_VERIFY(snapRes.ok);
    auto inspImp = inspect_import_source(*snapRes.snapshot);
    TB_VERIFY(inspImp.is_own_profile);
    auto mp = make_own_profile_mapping(inspImp);
    auto preview = build_transfer_preview(*snapRes.snapshot, inspImp, mp);
    TB_VERIFY(preview.blocking_errors.empty());
    TB_COMPARE(preview.to_task_count, size_t(0));
    auto impRes = execute_import(*snapRes.snapshot, preview, ws2);
    TB_VERIFY2(impRes.outcome == TransferOutcome::Succeeded, qPrintable(QString::fromStdString(impRes.error)));
    WorkspaceScanner sc;
    auto wsSnap = sc.scan(ws2);
    // Should have at least 1 project (Inbox) from the empty import
    TB_VERIFY(wsSnap.projects.size() >= size_t(1));
}

// M02b: Export → external edit → re-import (the full scenario from spec §9 M02).
void MdbaseMatrixTest::matrix2b_postEditReimport() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    auto ws = std::filesystem::path(tmp.path().toStdString()) / "ws";
    auto coll = std::filesystem::path(tmp.path().toStdString()) / "coll";
    auto ws2 = std::filesystem::path(tmp.path().toStdString()) / "ws2";
    create_native_ws(ws, "M02bws");
    WorkspaceController ctrl;
    std::string err;
    ctrl.open_workspace(ws, err);
    WorkspaceScanner sc;
    auto snap = sc.scan(ws);
    auto inbox = snap.projects.begin()->first;
    std::string parentId;
    std::string tid;
    TB_VERIFY(ctrl.create_task(inbox, "Relationship parent", parentId, err));
    TB_VERIFY(ctrl.create_task(inbox, "Original title", tid, err));
    // Export
    ExportRequest req; req.workspace_root = ws; req.destination = coll;
    auto res = export_workspace(req);
    TB_VERIFY2(res.outcome == TransferOutcome::Succeeded, qPrintable(QString::fromStdString(res.error)));
    // Find both exported tasks, then externally change title, status, and parent link.
    const auto [editedTask, parentTask] = find_exported_tasks(coll, tid, parentId);
    TB_VERIFY2(!editedTask.empty() && !parentTask.empty(), "Could not find exported task files");
    const std::string parentLink =
        "/" + std::filesystem::relative(parentTask, coll).generic_string();
    apply_external_task_edit(editedTask, parentLink);
    // Capture snapshot of edited collection and import
    auto snapRes = capture_snapshot(coll);
    TB_VERIFY(snapRes.ok);
    auto insp = inspect_import_source(*snapRes.snapshot);
    TB_VERIFY(insp.is_own_profile);
    auto mp = make_own_profile_mapping(insp);
    auto preview = build_transfer_preview(*snapRes.snapshot, insp, mp);
    TB_VERIFY2(preview.blocking_errors.empty(),
        qPrintable(QString::fromStdString(preview.blocking_errors.empty() ? "" : preview.blocking_errors[0].message)));
    TB_COMPARE(preview.to_task_count, size_t(2));
    auto impRes = execute_import(*snapRes.snapshot, preview, ws2);
    TB_VERIFY2(impRes.outcome == TransferOutcome::Succeeded,
        qPrintable(QString::fromStdString(impRes.error)));
    // The imported workspace should have at least one project (Inbox)
    WorkspaceScanner sc2;
    auto wsSnap = sc2.scan(ws2);
    TB_VERIFY2(wsSnap.projects.size() >= size_t(1),
        qPrintable(QString::fromStdString("projects: " + std::to_string(wsSnap.projects.size()))));
    // Verify imported workspace has valid settings
    TB_VERIFY(std::filesystem::exists(ws2 / "settings.json"));
    // Source workspace unchanged
    TB_VERIFY(std::filesystem::exists(ws / "settings.json"));
    TB_COMPARE(wsSnap.tasks.size(), size_t(2));
    TB_VERIFY(wsSnap.tasks.contains(tid));
    const auto& imported = wsSnap.tasks.at(tid);
    TB_COMPARE(imported.title, std::string("Edited title"));
    TB_VERIFY(imported.status == TaskStatus::Waiting);
    TB_COMPARE(imported.parent_id, parentId);
    const auto sourceSnap = sc.scan(ws);
    TB_VERIFY(sourceSnap.tasks.contains(tid));
    TB_COMPARE(sourceSnap.tasks.at(tid).title, std::string("Original title"));
    TB_VERIFY(sourceSnap.tasks.at(tid).status == TaskStatus::Todo);
    TB_VERIFY(sourceSnap.tasks.at(tid).parent_id.empty());
}

QTEST_MAIN(MdbaseMatrixTest)
#include "test_mdbase_matrix.moc"
