// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/workspace_store.h"

#include "storage/front_matter_codec.h"
#include "storage/settings_codec.h"
#include "storage/conflict_copy.h"
#include "storage/tutorial_workspace.h"

#include <QCryptographicHash>
#include <QSaveFile>
#include <QUuid>

#include <fstream>

namespace todobench {

WorkspaceStore::WorkspaceStore(std::filesystem::path root) : root_(std::move(root)) {}

namespace {
SaveResult create_empty_workspace(const std::filesystem::path& root, const std::string& name) {
    std::error_code error;
    if (std::filesystem::exists(root, error)) {
        if (error) return {SaveStatus::Error, root.string(), error.message()};
        if (!std::filesystem::is_directory(root, error)) {
            return {SaveStatus::Error, root.string(), "workspace path is not a directory"};
        }
        if (error) return {SaveStatus::Error, root.string(), error.message()};
        if (!std::filesystem::is_empty(root, error)) {
            return {SaveStatus::Error, root.string(), "workspace directory must be empty"};
        }
        if (error) return {SaveStatus::Error, root.string(), error.message()};
    }
    std::filesystem::create_directories(root / "projects", error);
    if (error) return {SaveStatus::Error, root.string(), error.message()};
    for (const auto& directory : {"history", "trash", "conflicts", "recovery"}) {
        std::filesystem::create_directories(root / ".todobench" / directory, error);
        if (error) return {SaveStatus::Error, root.string(), error.message()};
    }
    const auto project_id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const auto project_directory = root / "projects" / ("inbox--" + project_id);
    std::filesystem::create_directories(project_directory / "tasks", error);
    if (error) return {SaveStatus::Error, project_directory.string(), error.message()};
    ProjectRecord inbox;
    inbox.id = project_id;
    inbox.display_name = "Inbox";
    const auto project_path = project_directory / "project.md";
    QSaveFile project_file(QString::fromStdString(project_path.string()));
    if (!project_file.open(QIODevice::WriteOnly)) return {SaveStatus::Error, project_path.string(), project_file.errorString().toStdString()};
    const auto project_content = serialize_project_markdown(inbox);
    if (project_file.write(QByteArray::fromStdString(project_content)) != static_cast<qint64>(project_content.size()) || !project_file.commit()) {
        return {SaveStatus::Error, project_path.string(), project_file.errorString().toStdString()};
    }
    Settings settings;
    settings.workspace_name = name;
    std::string settings_error;
    if (!save_settings(root / "settings.json", settings, settings_error)) return {SaveStatus::Error, (root / "settings.json").string(), settings_error};
    return {SaveStatus::Saved, root.string(), {}};
}

}  // namespace

SaveResult WorkspaceStore::create_workspace(const std::filesystem::path& root, const std::string& name,
                                           bool include_tutorial) {
    return include_tutorial ? create_tutorial_workspace(root, name) : create_empty_workspace(root, name);
}

SaveResult WorkspaceStore::create_task(TaskRecord& task) const {
    if (std::filesystem::exists(task.source_path)) return {SaveStatus::Conflict, task.source_path, "task already exists"};
    const auto project_directory = std::filesystem::path(task.source_path).parent_path();
    std::error_code error;
    std::filesystem::create_directories(project_directory, error);
    if (error) return {SaveStatus::Error, project_directory.string(), error.message()};
    const auto content = serialize_task_markdown(task);
    QSaveFile output(QString::fromStdString(task.source_path));
    if (!output.open(QIODevice::WriteOnly)) return {SaveStatus::Error, task.source_path, output.errorString().toStdString()};
    if (output.write(QByteArray::fromStdString(content)) != static_cast<qint64>(content.size()) || !output.commit()) {
        return {SaveStatus::Error, task.source_path, output.errorString().toStdString()};
    }
    task.source_hash = hash_bytes(content);
    return {SaveStatus::Saved, task.source_path, {}};
}

std::string WorkspaceStore::hash_bytes(const std::string& bytes) {
    const auto digest = QCryptographicHash::hash(QByteArray::fromStdString(bytes), QCryptographicHash::Sha256);
    return digest.toHex().toStdString();
}

SaveResult WorkspaceStore::move_task_branch(const std::vector<TaskRecord>& tasks,
                                            const std::filesystem::path& destination_tasks) const {
    if (tasks.empty()) return {SaveStatus::Error, destination_tasks.string(), "no tasks selected"};
    std::error_code error;
    std::filesystem::create_directories(destination_tasks, error);
    if (error) return {SaveStatus::Error, destination_tasks.string(), error.message()};
    const auto journal_root = root_ / ".todobench" / "recovery";
    std::filesystem::create_directories(journal_root, error);
    if (error) return {SaveStatus::Error, journal_root.string(), error.message()};
    std::vector<std::filesystem::path> destinations;
    for (const auto& task : tasks) {
        const auto source = std::filesystem::path(task.source_path).parent_path();
        const auto destination = destination_tasks / source.filename();
        if (std::filesystem::exists(destination)) return {SaveStatus::Conflict, destination.string(), "destination task already exists"};
        destinations.push_back(destination);
    }
    std::vector<std::filesystem::path> moved;
    for (size_t index = 0; index < tasks.size(); ++index) {
        std::filesystem::rename(std::filesystem::path(tasks[index].source_path).parent_path(), destinations[index], error);
        if (error) {
            for (size_t rollback = 0; rollback < moved.size(); ++rollback) {
                std::error_code rollback_error;
                std::filesystem::rename(moved[rollback], std::filesystem::path(tasks[rollback].source_path).parent_path(), rollback_error);
            }
            return {SaveStatus::Error, tasks[index].source_path, error.message()};
        }
        moved.push_back(destinations[index]);
    }
    return {SaveStatus::Saved, destination_tasks.string(), {}};
}

SaveResult WorkspaceStore::save_project(const ProjectRecord& project) const {
    const auto path = std::filesystem::path(project.source_path);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return {SaveStatus::Error, path.string(), error.message()};
    std::ifstream input(path, std::ios::binary);
    const std::string current((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    if (!project.source_hash.empty() && hash_bytes(current) != project.source_hash) {
        std::string message;
        const auto retained = retain_conflict(root_, project.id, current, serialize_project_markdown(project), message);
        return {retained ? SaveStatus::Conflict : SaveStatus::Error, path.string(), message};
    }
    if (project.source_hash.empty() && std::filesystem::exists(path))
        return {SaveStatus::Conflict, path.string(), "existing project must be loaded before saving"};
    QSaveFile output(QString::fromStdString(path.string()));
    if (!output.open(QIODevice::WriteOnly)) return {SaveStatus::Error, path.string(), output.errorString().toStdString()};
    const auto content = serialize_project_markdown(project);
    if (output.write(QByteArray::fromStdString(content)) != static_cast<qint64>(content.size()) || !output.commit()) {
        return {SaveStatus::Error, path.string(), output.errorString().toStdString()};
    }
    return {SaveStatus::Saved, path.string(), {}};
}

SaveResult WorkspaceStore::save_task(const TaskRecord& task) const {
    const auto path = std::filesystem::path(task.source_path);
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        if (std::filesystem::exists(path)) return {SaveStatus::Error, path.string(), "unable to read task before saving"};
    }
    const std::string current((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    if (!task.source_hash.empty() && hash_bytes(current) != task.source_hash) {
        return {SaveStatus::Conflict, path.string(), "task changed on disk while it was being edited"};
    }
    if (task.source_hash.empty() && std::filesystem::exists(path))
        return {SaveStatus::Conflict, path.string(), "existing task must be loaded before saving"};
    QSaveFile output(QString::fromStdString(path.string()));
    if (!output.open(QIODevice::WriteOnly)) return {SaveStatus::Error, path.string(), output.errorString().toStdString()};
    const auto content = serialize_task_markdown(task);
    if (output.write(QByteArray::fromStdString(content)) != static_cast<qint64>(content.size()) || !output.commit()) {
        return {SaveStatus::Error, path.string(), output.errorString().toStdString()};
    }
    return {SaveStatus::Saved, path.string(), {}};
}

}  // namespace todobench
