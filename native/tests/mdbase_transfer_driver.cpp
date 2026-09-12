// SPDX-License-Identifier: GPL-3.0-or-later
// Small process boundary used by the independent TypeScript interoperability test.
#include "app/workspace_controller.h"
#include "storage/mdbase_export.h"
#include "storage/mdbase_import.h"
#include "storage/mdbase_transfer.h"
#include "storage/workspace_scanner.h"
#include "storage/workspace_store.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <filesystem>
#include <iostream>
#include <string>

using namespace todobench;
using namespace todobench::mdbase_transfer;

namespace {

QString qpath(const std::filesystem::path& path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    const auto value = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(value.data()),
                             static_cast<qsizetype>(value.size()));
#endif
}

std::string relative_path(const std::filesystem::path& path,
                          const std::filesystem::path& root) {
    return std::filesystem::relative(path, root).generic_string();
}

bool write_manifest(const std::filesystem::path& path, const QJsonObject& manifest) {
    QFile file(qpath(path));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::cerr << "cannot write manifest\n";
        return false;
    }
    const QByteArray bytes = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.flush()) {
        std::cerr << "cannot finish manifest\n";
        return false;
    }
    return true;
}

QJsonObject read_manifest(const std::filesystem::path& path) {
    QFile file(qpath(path));
    if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) return {};
    return document.object();
}

bool valid_import_arguments(const std::string& child_id, const std::string& parent_id,
                            const std::filesystem::path& workspace) {
    return !child_id.empty() && !parent_id.empty() && !std::filesystem::exists(workspace);
}

bool imported_workspace_matches(const WorkspaceSnapshot& imported,
                                const std::string& child_id,
                                const std::string& parent_id) {
    if (imported.projects.size() != 1 || imported.tasks.size() != 2) return false;
    const auto child = imported.tasks.find(child_id);
    return child != imported.tasks.end() && child->second.title == "Oracle edited child" &&
           child->second.status == TaskStatus::Waiting && child->second.parent_id == parent_id;
}

int export_fixture(const std::filesystem::path& workspace,
                   const std::filesystem::path& collection,
                   const std::filesystem::path& manifest_path) {
    if (std::filesystem::exists(workspace) || std::filesystem::exists(collection)) {
        std::cerr << "fixture destinations must not exist\n";
        return 2;
    }
    const auto created = WorkspaceStore::create_workspace(workspace, "Oracle interoperability");
    if (created.status != SaveStatus::Saved) {
        std::cerr << "workspace creation failed: " << created.message << '\n';
        return 1;
    }

    WorkspaceController controller;
    std::string error;
    if (!controller.open_workspace(workspace, error) || controller.snapshot().projects.empty()) {
        std::cerr << "workspace open failed: " << error << '\n';
        return 1;
    }
    const std::string project_id = controller.snapshot().projects.begin()->first;
    std::string parent_id;
    std::string child_id;
    if (!controller.create_task(project_id, "Oracle parent", parent_id, error) ||
        !controller.create_subtask(parent_id, "Oracle child", child_id, error)) {
        std::cerr << "fixture task creation failed: " << error << '\n';
        return 1;
    }

    const auto& snapshot = controller.snapshot();
    const auto parent_it = snapshot.tasks.find(parent_id);
    const auto child_it = snapshot.tasks.find(child_id);
    const auto project_it = snapshot.projects.find(project_id);
    if (parent_it == snapshot.tasks.end() || child_it == snapshot.tasks.end() ||
        project_it == snapshot.projects.end()) {
        std::cerr << "fixture records missing after creation\n";
        return 1;
    }

    ExportRequest request;
    request.workspace_root = workspace;
    request.destination = collection;
    const TransferResult result = export_workspace(request);
    if (result.outcome != TransferOutcome::Succeeded) {
        std::cerr << "export failed: " << result.error << '\n';
        return 1;
    }

    QJsonObject manifest;
    manifest["parent_id"] = QString::fromStdString(parent_id);
    manifest["child_id"] = QString::fromStdString(child_id);
    manifest["project_id"] = QString::fromStdString(project_id);
    manifest["parent_path"] = QString::fromStdString(relative_path(parent_it->second.source_path, workspace));
    manifest["child_path"] = QString::fromStdString(relative_path(child_it->second.source_path, workspace));
    manifest["project_path"] = QString::fromStdString(relative_path(project_it->second.source_path, workspace));
    manifest["expected_record_count"] = 3;
    return write_manifest(manifest_path, manifest) ? 0 : 1;
}

int import_and_check(const std::filesystem::path& collection,
                     const std::filesystem::path& workspace,
                     const std::filesystem::path& manifest_path) {
    const QJsonObject manifest = read_manifest(manifest_path);
    const std::string child_id = manifest.value("child_id").toString().toStdString();
    const std::string parent_id = manifest.value("parent_id").toString().toStdString();
    if (!valid_import_arguments(child_id, parent_id, workspace)) {
        std::cerr << "invalid import-check arguments\n";
        return 2;
    }

    auto snapshot_result = capture_snapshot(collection);
    if (!snapshot_result.ok || !snapshot_result.snapshot) {
        std::cerr << "snapshot failed: " << snapshot_result.error << '\n';
        return 1;
    }
    const CollectionInspection inspection = inspect_import_source(*snapshot_result.snapshot);
    if (!inspection.valid || !inspection.is_own_profile) {
        std::cerr << "exported collection was not recognized as the TodoBench profile\n";
        return 1;
    }
    const ImportMapping mapping = make_own_profile_mapping(inspection);
    const TransferPreview preview = build_transfer_preview(*snapshot_result.snapshot, inspection, mapping);
    if (preview.has_blocking_errors() || preview.selected_record_count != 3) {
        std::cerr << "import preview failed or returned the wrong exact record count\n";
        return 1;
    }
    const TransferResult result = execute_import(*snapshot_result.snapshot, preview, workspace);
    if (result.outcome != TransferOutcome::Succeeded) {
        std::cerr << "import failed: " << result.error << '\n';
        return 1;
    }

    WorkspaceScanner scanner;
    const WorkspaceSnapshot imported = scanner.scan(workspace);
    if (!imported_workspace_matches(imported, child_id, parent_id)) {
        std::cerr << "external title/status/parent semantics were not preserved\n";
        return 1;
    }
    std::cout << "native re-import verified\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (argc != 5) {
        std::cerr << "usage: mdbase_transfer_driver <export-fixture|import-check> <source> <destination> <manifest>\n";
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "export-fixture") {
        return export_fixture(argv[2], argv[3], argv[4]);
    }
    if (mode == "import-check") {
        return import_and_check(argv[2], argv[3], argv[4]);
    }
    std::cerr << "unknown mode\n";
    return 2;
}
