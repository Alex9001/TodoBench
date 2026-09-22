// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/workspace_store.h"

#include "storage/front_matter_codec.h"
#include "storage/directory_names.h"
#include "storage/markdown_links.h"
#include "storage/transactional_storage.h"
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
    const auto project_directory = allocate_directory(root / "projects", "Inbox", "project");
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
    return storage_command(root_, [&](CommandTransaction& transaction) {
        if (transaction.exists(task.source_path)) return SaveResult{SaveStatus::Conflict, task.source_path, "task already exists"};
        const auto content = serialize_task_markdown(task);
        transaction.write(task.source_path, content);
        task.source_hash = hash_bytes(content);
        return SaveResult{SaveStatus::Saved, task.source_path, {}};
    });
}

std::string WorkspaceStore::hash_bytes(const std::string& bytes) {
    return QCryptographicHash::hash(QByteArray::fromStdString(bytes), QCryptographicHash::Sha256).toHex().toStdString();
}

SaveResult WorkspaceStore::move_task_branch(const std::vector<TaskRecord>& tasks,
                                            const std::filesystem::path& destination_tasks) const {
    return storage_command(root_, [&](CommandTransaction& transaction) {
        DirectoryMoves moves;
        SaveResult result{SaveStatus::Saved, destination_tasks.string(), {}};
        std::set<std::string> occupied;
        for (const auto& entry : transaction.children(destination_tasks)) occupied.insert(directory_key(entry.filename().string()));
        for (const auto& task : tasks) {
            const auto source = std::filesystem::path(task.source_path).parent_path();
            const auto bytes = transaction.read(task.source_path);
            if (hash_bytes(bytes) != task.source_hash) return SaveResult{SaveStatus::Conflict, task.source_path, "task changed before move"};
            auto destination = destination_tasks / allocate_directory_name(task.title, "task", occupied);
            moves.emplace_back(source, destination);
            result.moved_paths.emplace_back(task.source_path, (destination / "task.md").string());
        }
        rewrite_workspace_links(root_, moves);
        for (const auto& [source, destination] : moves) transaction.move(source, destination);
        return result;
    });
}

SaveResult WorkspaceStore::save_project(const ProjectRecord& project) const {
    return storage_command(root_, [&](CommandTransaction& transaction) {
        const auto path = std::filesystem::path(project.source_path);
        const auto exists = transaction.exists(path);
        const auto current = exists ? transaction.read(path) : std::string{};
        if ((!project.source_hash.empty() && hash_bytes(current) != project.source_hash) || (project.source_hash.empty() && exists)) {
            std::string message;
            const bool retained = retain_conflict(root_, project.id, current, serialize_project_markdown(project), message);
            return SaveResult{retained ? SaveStatus::Conflict : SaveStatus::Error, path.string(), message};
        }
        auto destination = path.parent_path();
        if (exists && path.parent_path() != root_) {
            const auto parsed = parse_project_markdown(path.string(), current);
            if (const auto* previous = std::get_if<ProjectRecord>(&parsed))
                if (directory_base(previous->display_name, "project") != directory_base(project.display_name, "project"))
                    destination = allocate_directory(destination.parent_path(), project.display_name, "project", destination);
        }
        transaction.write(path, serialize_project_markdown(project), project.source_hash);
        const auto new_path = destination / "project.md";
        if (destination != path.parent_path()) {
            rewrite_workspace_links(root_, {{path.parent_path(), destination}});
            transaction.move(path.parent_path(), destination);
        }
        return SaveResult{SaveStatus::Saved, new_path.string(), {}};
    });
}

SaveResult WorkspaceStore::save_task(const TaskRecord& task) const {
    return storage_command(root_, [&](CommandTransaction& transaction) {
        const auto path = std::filesystem::path(task.source_path);
        const auto exists = transaction.exists(path);
        const auto current = exists ? transaction.read(path) : std::string{};
        if ((!task.source_hash.empty() && hash_bytes(current) != task.source_hash) || (task.source_hash.empty() && exists))
            return SaveResult{SaveStatus::Conflict, path.string(), "task changed on disk while it was being edited"};
        auto destination = path.parent_path();
        if (exists && path.parent_path() != root_) {
            const auto parsed = parse_task_markdown(path.string(), current);
            if (const auto* previous = std::get_if<TaskRecord>(&parsed))
                if (directory_base(previous->title, "task") != directory_base(task.title, "task"))
                    destination = allocate_directory(destination.parent_path(), task.title, "task", destination);
        }
        transaction.write(path, serialize_task_markdown(task), task.source_hash);
        const auto new_path = destination / "task.md";
        if (destination != path.parent_path()) {
            rewrite_workspace_links(root_, {{path.parent_path(), destination}});
            transaction.move(path.parent_path(), destination);
        }
        return SaveResult{SaveStatus::Saved, new_path.string(), {}};
    });
}

} // namespace todobench
