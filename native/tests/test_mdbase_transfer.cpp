// SPDX-License-Identifier: GPL-3.0-or-later
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
#include <latch>
#include <thread>

using namespace todobench;
using namespace todobench::mdbase_transfer;

namespace {

std::filesystem::path write_foreign_collection(const std::filesystem::path& root) {
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
collection:
  display: {name_field: summary}
  unique: [{field: id, scope: type}]
---
)";
    std::filesystem::create_directories(root / "notes");
    std::ofstream(root / "notes/a.md") << R"(---
id: fk-1
summary: Foreign note A
state: DONE
prio: 3
tagline: alpha
---
Body A
)";
    std::ofstream(root / "notes/b.md") << R"(---
id: fk-2
summary: Foreign note B
state: todo
prio: 1
---
Body B
)";
    return root;
}

void verify_all_tasks(const TransferPreview& preview) {
    for (const auto& record : preview.records)
        TB_VERIFY(record.is_task);
}

std::string first_scan_error(const WorkspaceSnapshot& snapshot) {
    const auto error = std::find_if(snapshot.diagnostics.begin(), snapshot.diagnostics.end(),
                                    [](const Diagnostic& diagnostic) {
        return diagnostic.severity == Diagnostic::Severity::Error;
    });
    return error == snapshot.diagnostics.end() ? std::string{} : error->message;
}

void verify_foreign_alpha(const WorkspaceSnapshot& snapshot) {
    const auto task = std::find_if(snapshot.tasks.begin(), snapshot.tasks.end(),
                                   [](const auto& entry) {
        return entry.second.title == "Foreign note A";
    });
    TB_VERIFY(task != snapshot.tasks.end());
    TB_COMPARE(task->second.priority, Priority::High);
    TB_VERIFY(std::find(task->second.tags.begin(), task->second.tags.end(), "alpha") !=
              task->second.tags.end());
}

} // namespace

class MdbaseTransferTest : public QObject {
  Q_OBJECT
private slots:
    void foreignImportMapsCorrectly();
    void unmappedStatusBlocks();
    void duplicateIdsWarnAndDistinct();
    void emptyWorkspaceRoundTrip();
    void graphCrossProjectBlocked();
    void titleMissingRequiresAck();
    void snapshotLimitsAndSymlinkRejected();
    void snapshotRootSymlinkRejected();
    void concurrentPublishNeverReplacesWinner();
};

void MdbaseTransferTest::foreignImportMapsCorrectly() {
    QTemporaryDir collRoot;
    TB_VERIFY(collRoot.isValid());
    auto root = std::filesystem::path(collRoot.path().toStdString()) / "foreign";
    write_foreign_collection(root);
    auto snapRes = capture_snapshot(root);
    TB_VERIFY2(snapRes.ok, qPrintable(QString::fromStdString(snapRes.error)));
    auto insp = inspect_import_source(*snapRes.snapshot);
    TB_VERIFY(insp.valid);
    TB_VERIFY(!insp.all_record_paths.empty());
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
    mp.status_map.put_for_json_value(QJsonValue(QStringLiteral("DONE")), "done");
    mp.status_map.put_for_json_value(QJsonValue(QStringLiteral("todo")), "todo");
    mp.priority_map.put_for_json_value(QJsonValue(3), "high");
    mp.priority_map.put_for_json_value(QJsonValue(1), "low");
    auto preview = build_transfer_preview(*snapRes.snapshot, insp, mp);
    TB_VERIFY2(preview.blocking_errors.empty(), qPrintable(QString::fromStdString(preview.blocking_errors.empty() ? "" : preview.blocking_errors[0].message)));
    TB_COMPARE(preview.to_task_count, size_t(2));
    TB_COMPARE(preview.to_project_count, size_t(0));
    verify_all_tasks(preview);
    // Execute import
    QTemporaryDir outTmp;
    auto destWs = std::filesystem::path(outTmp.path().toStdString()) / "imported_ws";
    auto res = execute_import(*snapRes.snapshot, preview, destWs);
    TB_VERIFY2(res.outcome == TransferOutcome::Succeeded, qPrintable(QString::fromStdString(res.error)));
    WorkspaceScanner sc;
    auto wsSnap = sc.scan(destWs);
    const std::string scanError = first_scan_error(wsSnap);
    TB_VERIFY2(scanError.empty(), qPrintable(QString::fromStdString(scanError)));
    TB_COMPARE(wsSnap.tasks.size(), size_t(2));
    // Provenance retained
    TB_VERIFY(std::filesystem::exists(destWs / ".todobench" / "imports"));
    // Unknown fields not needed here; tag and priority mapped correctly
    verify_foreign_alpha(wsSnap);
}

void MdbaseTransferTest::unmappedStatusBlocks() {
    QTemporaryDir collRoot;
    TB_VERIFY(collRoot.isValid());
    auto root = std::filesystem::path(collRoot.path().toStdString()) / "foreign2";
    write_foreign_collection(root);
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
    mp.type_mappings["note"] = tm;
    TypeMapping um;
    um.type_name = "__untyped__";
    um.is_untyped_bucket = true;
    um.selected = false;
    mp.type_mappings["__untyped__"] = um;
    // Only map one status -> other remains unmapped -> blocking
    mp.status_map.put_for_json_value(QJsonValue(QStringLiteral("DONE")), "done");
    mp.priority_map.put_for_json_value(QJsonValue(3), "high");
    mp.priority_map.put_for_json_value(QJsonValue(1), "low");
    auto preview = build_transfer_preview(*snapRes.snapshot, insp, mp);
    TB_VERIFY(!preview.blocking_errors.empty());
    bool foundUnmapped = false;
    for (auto &d : preview.blocking_errors) if (d.code == "unmapped_status") foundUnmapped = true;
    TB_VERIFY(foundUnmapped);
    // execute should validation-fail
    QTemporaryDir outTmp;
    auto destWs = std::filesystem::path(outTmp.path().toStdString()) / "ws_blocked";
    auto res = execute_import(*snapRes.snapshot, preview, destWs);
    TB_COMPARE(res.outcome, TransferOutcome::ValidationFailed);
    TB_VERIFY(!std::filesystem::exists(destWs));
}

void MdbaseTransferTest::duplicateIdsWarnAndDistinct() {
    QTemporaryDir collRoot;
    TB_VERIFY(collRoot.isValid());
    auto root = std::filesystem::path(collRoot.path().toStdString()) / "dup";
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
    // Use UUIDs that are valid but duplicate
    std::string dupId = "11111111-1111-1111-1111-111111111111";
    std::ofstream(root / "notes/a.md") << "---\nid: " + dupId + "\nsummary: A\n---\nA\n";
    std::ofstream(root / "notes/b.md") << "---\nid: " + dupId + "\nsummary: B\n---\nB\n";
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
    TB_VERIFY(preview.records[0].native_id != preview.records[1].native_id);
    bool hasDupWarn = false;
    for (auto &w : preview.warnings) if (w.code == "duplicate_source_id" || w.code == "non_uuid_id_generated") hasDupWarn = true;
    if (!hasDupWarn) { for (auto &w : preview.warnings) qDebug() << qPrintable(QString::fromStdString(w.code + ": " + w.message)); }
    TB_VERIFY(hasDupWarn);
    QTemporaryDir outTmp;
    auto destWs = std::filesystem::path(outTmp.path().toStdString()) / "ws_dup";
    auto res = execute_import(*snapRes.snapshot, preview, destWs);
    TB_VERIFY(res.outcome == TransferOutcome::Succeeded);
    WorkspaceScanner sc;
    auto snap = sc.scan(destWs);
    TB_COMPARE(snap.tasks.size(), size_t(2));
}

void MdbaseTransferTest::emptyWorkspaceRoundTrip() {
    QTemporaryDir wsTmp;
    TB_VERIFY(wsTmp.isValid());
    auto ws = std::filesystem::path(wsTmp.path().toStdString()) / "ws";
    auto coll = std::filesystem::path(wsTmp.path().toStdString()) / "coll";
    TB_VERIFY(WorkspaceStore::create_workspace(ws, "EmptyRT").status == SaveStatus::Saved);
    ExportRequest req;
    req.workspace_root = ws;
    req.destination = coll;
    auto exp = export_workspace(req);
    TB_VERIFY2(exp.outcome == TransferOutcome::Succeeded, qPrintable(QString::fromStdString(exp.error)));
    // Import back via own-profile mapping
    auto snapRes = capture_snapshot(coll);
    TB_VERIFY(snapRes.ok);
    auto insp = inspect_import_source(*snapRes.snapshot);
    TB_VERIFY(insp.is_own_profile);
    auto mp = make_own_profile_mapping(insp);
    TB_VERIFY(mp.find_type("__untyped__") != nullptr);
    TB_VERIFY(mp.find_type("__untyped__")->as_task);
    auto preview = build_transfer_preview(*snapRes.snapshot, insp, mp);
    TB_VERIFY2(preview.blocking_errors.empty(), qPrintable(QString::fromStdString(preview.blocking_errors.empty() ? "" : preview.blocking_errors[0].message)));
    QTemporaryDir outTmp2;
    auto ws2 = std::filesystem::path(outTmp2.path().toStdString()) / "ws2";
    auto res = execute_import(*snapRes.snapshot, preview, ws2);
    TB_VERIFY2(res.outcome == TransferOutcome::Succeeded, qPrintable(QString::fromStdString(res.error)));
    WorkspaceScanner sc;
    auto s = sc.scan(ws2);
    // Empty workspace should still have at least one project (Inbox) after import via our own-profile empty handling
    TB_VERIFY(s.projects.size() >= size_t(1));
    TB_VERIFY(s.diagnostics.empty());
}

void MdbaseTransferTest::graphCrossProjectBlocked() {
    QTemporaryDir collRoot;
    TB_VERIFY(collRoot.isValid());
    auto root = std::filesystem::path(collRoot.path().toStdString()) / "graph";
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
      project: {type: string}
      parent: {type: string}
collection:
  display: {name_field: summary}
  unique: [{field: id, scope: type}]
---
)";
    std::filesystem::create_directories(root / "notes");
    std::ofstream(root / "notes/p1.md") << "---\nid: p1\nsummary: Task p1\nproject: projA\n---\nA\n";
    std::ofstream(root / "notes/p2.md") << "---\nid: p2\nsummary: Task p2\nproject: projB\nparent: p1\n---\nB\n";
    auto snapRes = capture_snapshot(root);
    TB_VERIFY(snapRes.ok);
    auto insp = inspect_import_source(*snapRes.snapshot);
    ImportMapping mp;
    TypeMapping tm;
    tm.type_name = "note";
    tm.selected = true;
    tm.as_task = true;
    tm.title_field = {"/summary", true};
    tm.project_mode = "string_label";
    tm.project_field = {"/project", true};
    tm.parent_mode = "id_ref";
    tm.parent_field = {"/parent", true};
    mp.type_mappings["note"] = tm;
    TypeMapping um;
    um.type_name = "__untyped__";
    um.is_untyped_bucket = true;
    um.selected = false;
    mp.type_mappings["__untyped__"] = um;
    auto preview = build_transfer_preview(*snapRes.snapshot, insp, mp);
    // Cross-project parent should be blocking (p2 parent p1 but different projects)
    bool hasCross = false;
    for (auto &d : preview.blocking_errors) if (d.code == "cross_project_parent") hasCross = true;
    TB_VERIFY(hasCross);

    const auto first = std::ranges::find_if(preview.records, [](const PreviewRecord& record) {
        return record.source_path == "notes/p1.md";
    });
    TB_VERIFY(first != preview.records.end());
    mp.record_project_overrides["notes/p2.md"] = first->native_project_choice;
    const auto repaired = build_transfer_preview(*snapRes.snapshot, insp, mp);
    const bool stillCrossed = std::ranges::any_of(
        repaired.blocking_errors, [](const TransferDiagnostic& diagnostic) {
            return diagnostic.code == "cross_project_parent";
        });
    TB_VERIFY(!stillCrossed);
}

void MdbaseTransferTest::titleMissingRequiresAck() {
    QTemporaryDir collRoot;
    TB_VERIFY(collRoot.isValid());
    auto root = std::filesystem::path(collRoot.path().toStdString()) / "titlemiss";
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
    std::ofstream(root / "notes/a.md") << "---\nid: a1\n---\nNo summary\n";
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
    bool needsAck = false;
    for (auto &d : preview.blocking_errors) if (d.code == "title_missing_requires_ack") needsAck = true;
    TB_VERIFY(needsAck);
    mp.accept_empty_title_as_filename_stem = true;
    auto preview2 = build_transfer_preview(*snapRes.snapshot, insp, mp);
    TB_VERIFY(preview2.blocking_errors.empty());
    bool hasFallback = false;
    for (auto &r : preview2.records) if (!r.fallback_notes.empty()) hasFallback = true;
    TB_VERIFY(hasFallback);
}

void MdbaseTransferTest::snapshotLimitsAndSymlinkRejected() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    auto src = std::filesystem::path(tmp.path().toStdString()) / "src";
    std::filesystem::create_directories(src);
    std::ofstream(src / "a.txt") << "hello";
    // symlink
    std::filesystem::create_symlink(src / "a.txt", src / "link.txt");
    auto res = capture_snapshot(src);
    TB_VERIFY(!res.ok);
    bool isSymlink = false;
    for (auto &d : res.diagnostics) if (d.code == "symlink_not_followed") isSymlink = true;
    TB_VERIFY(isSymlink);
    // entry limit
    std::filesystem::remove(src / "link.txt");
    TransferLimits lim;
    lim.max_entries = 1;
    SnapshotOptions opts;
    opts.limits = lim;
    std::ofstream(src / "b.txt") << "world";
    auto res2 = capture_snapshot(src, opts);
    TB_VERIFY(!res2.ok);
    bool limitHit = false;
    for (auto &d : res2.diagnostics) if (d.code == "entry_limit") limitHit = true;
    TB_VERIFY(limitHit);
}

void MdbaseTransferTest::snapshotRootSymlinkRejected() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    const auto parent = std::filesystem::path(tmp.path().toStdString());
    const auto source = parent / "source";
    const auto alias = parent / "source-alias";
    std::filesystem::create_directories(source);
    std::ofstream(source / "record.md") << "contents";
    std::error_code error;
    std::filesystem::create_directory_symlink(source, alias, error);
    if (error) QSKIP("directory symlinks are not available on this test host");
    const auto result = capture_snapshot(alias);
    TB_VERIFY(!result.ok);
    TB_COMPARE(result.error, std::string("source_symlink"));
}

void MdbaseTransferTest::concurrentPublishNeverReplacesWinner() {
    QTemporaryDir tmp;
    TB_VERIFY(tmp.isValid());
    const auto parent = std::filesystem::path(tmp.path().toStdString());
    const auto destination = parent / "published";
    std::string error;
    const auto first_stage = make_staged_sibling(destination, &error);
    TB_VERIFY2(!first_stage.empty(), error.c_str());
    const auto second_stage = make_staged_sibling(destination, &error);
    TB_VERIFY2(!second_stage.empty(), error.c_str());
    std::ofstream(first_stage / "winner.txt") << "first";
    std::ofstream(second_stage / "winner.txt") << "second";

    std::latch ready(3);
    PublishResult first;
    PublishResult second;
    std::thread first_thread([&] {
        ready.count_down();
        ready.wait();
        first = publish_staged(first_stage, destination);
    });
    std::thread second_thread([&] {
        ready.count_down();
        ready.wait();
        second = publish_staged(second_stage, destination);
    });
    ready.count_down();
    ready.wait();
    first_thread.join();
    second_thread.join();

    TB_COMPARE(static_cast<int>(first.ok) + static_cast<int>(second.ok), 1);
    TB_VERIFY(std::filesystem::exists(destination / "winner.txt"));
    std::ifstream input(destination / "winner.txt");
    std::string winner;
    input >> winner;
    TB_COMPARE(winner, first.ok ? std::string("first") : std::string("second"));
    const auto& loser = first.ok ? second : first;
    TB_COMPARE(loser.error, std::string("destination_exists"));
}

QTEST_MAIN(MdbaseTransferTest)
#include "test_mdbase_transfer.moc"
