// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/external_reconciler.h"

#include "storage/front_matter_codec.h"
#include "storage/workspace_store.h"

#include <QSaveFile>
#include <QUuid>

#include <fstream>

namespace todobench {
namespace {

std::string read_bytes(const std::filesystem::path& path, bool& readable) {
    std::ifstream input(path, std::ios::binary);
    readable = static_cast<bool>(input);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace

ExternalReconciler::ExternalReconciler(std::filesystem::path workspace_root) : root_(std::move(workspace_root)) {}

ReconcileResult ExternalReconciler::reconcile(const TaskRecord& loaded, bool dirty) const {
    bool readable = false;
    const auto bytes = read_bytes(loaded.source_path, readable);
    if (!readable && std::filesystem::exists(loaded.source_path)) return {ExternalChange::Error, {}, {}, "unable to read externally changed task"};
    if (WorkspaceStore::hash_bytes(bytes) == loaded.source_hash) return {ExternalChange::Unchanged, {}, {}, {}};
    if (!dirty) {
        const auto parsed = parse_task_markdown(loaded.source_path, bytes);
        if (std::holds_alternative<CodecError>(parsed)) return {ExternalChange::Error, {}, {}, std::get<CodecError>(parsed).message};
        auto task = std::get<TaskRecord>(parsed);
        task.project_id = loaded.project_id;
        task.source_hash = WorkspaceStore::hash_bytes(bytes);
        return {ExternalChange::Reload, std::move(task), {}, {}};
    }
    const auto conflict_directory = root_ / ".todobench" / "conflicts";
    std::error_code error;
    std::filesystem::create_directories(conflict_directory, error);
    if (error) return {ExternalChange::Error, {}, {}, error.message()};
    const auto path = conflict_directory / (loaded.id + "--disk-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString() + ".md");
    QSaveFile output(QString::fromStdString(path.string()));
    if (!output.open(QIODevice::WriteOnly) || output.write(QByteArray::fromStdString(bytes)) != static_cast<qint64>(bytes.size()) || !output.commit()) {
        return {ExternalChange::Error, {}, {}, output.errorString().toStdString()};
    }
    return {ExternalChange::Conflict, {}, path.string(), "local edits were preserved; the disk version was copied to conflicts"};
}

}  // namespace todobench
