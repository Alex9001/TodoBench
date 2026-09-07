// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/workspace_scanner.h"

#include "storage/front_matter_codec.h"
#include "storage/workspace_store.h"

#include <fstream>
#include <set>

namespace todobench {
namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool is_project_directory(const std::filesystem::path& path) {
    return std::filesystem::is_directory(path) && std::filesystem::exists(path / "project.md");
}

}  // namespace

WorkspaceSnapshot WorkspaceScanner::scan(const std::filesystem::path& root) const {
    WorkspaceSnapshot snapshot;
    snapshot.root_path = root.string();
    const auto projects = root / "projects";
    if (!std::filesystem::exists(projects)) {
        snapshot.diagnostics.push_back({Diagnostic::Severity::Error, projects.string(), "missing projects directory"});
        return snapshot;
    }
    for (const auto& entry : std::filesystem::directory_iterator(projects)) {
        if (is_project_directory(entry.path())) scan_project_tree(entry.path(), snapshot);
    }
    if (snapshot.projects.empty()) {
        snapshot.diagnostics.push_back({Diagnostic::Severity::Warning, projects.string(), "workspace has no projects; Inbox is expected for new workspaces"});
    }
    validate_hierarchy(snapshot);
    return snapshot;
}

void WorkspaceScanner::scan_project_tree(const std::filesystem::path& directory, WorkspaceSnapshot& snapshot,
                                          const std::string& parent_project_id) {
    const auto project_path = directory / "project.md";
    auto project_result = parse_project_markdown(project_path.string(), read_file(project_path));
    if (std::holds_alternative<CodecError>(project_result)) {
        snapshot.diagnostics.push_back({Diagnostic::Severity::Error, project_path.string(), std::get<CodecError>(project_result).message});
        return;
    }
    auto project = std::get<ProjectRecord>(std::move(project_result));
    if (!is_valid_uuid(project.id)) {
        snapshot.diagnostics.push_back({Diagnostic::Severity::Error, project_path.string(), "project id is not a UUID"});
        return;
    }
    if (snapshot.projects.contains(project.id)) {
        snapshot.diagnostics.push_back({Diagnostic::Severity::Error, project_path.string(), "duplicate project id"});
        return;
    }
    const auto project_id = project.id;
    project.parent_id = parent_project_id;
    snapshot.projects.emplace(project_id, std::move(project));

    const auto tasks = directory / "tasks";
    if (std::filesystem::exists(tasks)) {
        for (const auto& task_directory : std::filesystem::directory_iterator(tasks)) {
            const auto task_path = task_directory.path() / "task.md";
            if (!std::filesystem::is_directory(task_directory.path()) || !std::filesystem::exists(task_path)) continue;
            auto task_result = parse_task_markdown(task_path.string(), read_file(task_path));
            if (std::holds_alternative<CodecError>(task_result)) {
                snapshot.diagnostics.push_back({Diagnostic::Severity::Error, task_path.string(), std::get<CodecError>(task_result).message});
                continue;
            }
            auto task = std::get<TaskRecord>(std::move(task_result));
            task.project_id = project_id;
            if (!is_valid_uuid(task.id)) {
                snapshot.diagnostics.push_back({Diagnostic::Severity::Error, task_path.string(), "task id is not a UUID"});
                continue;
            }
            if (snapshot.tasks.contains(task.id)) {
                snapshot.diagnostics.push_back({Diagnostic::Severity::Error, task_path.string(), "duplicate task id"});
                continue;
            }
            snapshot.tasks.emplace(task.id, std::move(task));
        }
    }
    const auto child_projects = directory / "projects";
    if (!std::filesystem::exists(child_projects)) return;
    for (const auto& child : std::filesystem::directory_iterator(child_projects)) {
        if (is_project_directory(child.path())) scan_project_tree(child.path(), snapshot, project_id);
    }
}

void WorkspaceScanner::validate_hierarchy(WorkspaceSnapshot& snapshot) {
    std::set<std::string> reported;
    for (const auto& [id, task] : snapshot.tasks) {
        if (task.parent_id.empty()) continue;
        const auto parent = snapshot.tasks.find(task.parent_id);
        if (parent == snapshot.tasks.end()) {
            snapshot.diagnostics.push_back({Diagnostic::Severity::Error, task.source_path, "parent task does not exist: " + task.parent_id});
            continue;
        }
        if (parent->second.project_id != task.project_id) {
            snapshot.diagnostics.push_back({Diagnostic::Severity::Error, task.source_path, "parent task belongs to another project"});
            continue;
        }
        if (!reported.insert(id).second) continue;
        std::set<std::string> ancestors;
        auto current = task.parent_id;
        while (!current.empty()) {
            if (!ancestors.insert(current).second) {
                snapshot.diagnostics.push_back({Diagnostic::Severity::Error, task.source_path, "task hierarchy contains a cycle"});
                break;
            }
            const auto ancestor = snapshot.tasks.find(current);
            if (ancestor == snapshot.tasks.end()) break;
            current = ancestor->second.parent_id;
        }
    }
}

}  // namespace todobench
