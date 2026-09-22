// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/workspace_scanner.h"

#include "storage/front_matter_codec.h"
#include "storage/workspace_store.h"

#include <fstream>
#include <set>
#include <map>
#include <QString>

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

namespace {
std::string normalized_path(const std::filesystem::path& path) {
    return QString::fromStdString(path.lexically_normal().generic_string()).normalized(QString::NormalizationForm_C).toStdString();
}
bool native_record_path(const std::filesystem::path& path, const std::filesystem::path& root) {
    auto directory = path.parent_path().parent_path();
    if (path.filename() == "task.md") {
        if (directory.filename() != "tasks") return false;
        directory = directory.parent_path().parent_path();
    }
    const auto projects = root / "projects";
    while (directory != projects) {
        if (directory.filename() != "projects" || directory == directory.parent_path()) return false;
        directory = directory.parent_path().parent_path();
    }
    return true;
}
void add_record_paths(std::set<std::filesystem::path>& paths, const std::filesystem::path& base, const FileTree& tree) {
    for (const auto& [relative, hash] : tree) {
        (void)hash;
        const auto path = relative == "." ? base : base / relative;
        if (path.filename() == "task.md" || path.filename() == "project.md") paths.insert(path.lexically_normal());
    }
}
void load_changed_project(WorkspaceSnapshot& snapshot, const std::filesystem::path& path) {
    auto result = parse_project_markdown(path.string(), read_file(path));
    if (const auto* error = std::get_if<CodecError>(&result)) { snapshot.diagnostics.push_back({Diagnostic::Severity::Error, path.string(), error->message}); return; }
    auto project = std::get<ProjectRecord>(std::move(result));
    snapshot.projects[project.id] = std::move(project);
}
void load_changed_task(WorkspaceSnapshot& snapshot, const std::filesystem::path& path) {
    auto result = parse_task_markdown(path.string(), read_file(path));
    if (const auto* error = std::get_if<CodecError>(&result)) { snapshot.diagnostics.push_back({Diagnostic::Severity::Error, path.string(), error->message}); return; }
    auto task = std::get<TaskRecord>(std::move(result));
    snapshot.tasks[task.id] = std::move(task);
}
}

void WorkspaceScanner::apply_changes(WorkspaceSnapshot& snapshot, const std::vector<FileOperation>& operations) const {
    std::set<std::filesystem::path> paths;
    for (const auto& operation : operations) {
        add_record_paths(paths, operation.path, operation.before);
        add_record_paths(paths, operation.path, operation.after);
        if (operation.kind == FileOperation::Kind::Move) add_record_paths(paths, operation.destination, operation.after);
    }
    std::set<std::string> keys;
    for (const auto& path : paths) keys.insert(normalized_path(path));
    std::erase_if(snapshot.tasks, [&](const auto& item) { return keys.contains(normalized_path(item.second.source_path)); });
    std::erase_if(snapshot.projects, [&](const auto& item) { return keys.contains(normalized_path(item.second.source_path)); });
    std::erase_if(snapshot.diagnostics, [&](const auto& diagnostic) { return keys.contains(normalized_path(diagnostic.path)) || diagnostic.message.starts_with("parent task") || diagnostic.message == "task hierarchy contains a cycle"; });
    for (const auto& path : paths) {
        if (!native_record_path(path, snapshot.root_path) || !std::filesystem::is_regular_file(path)) continue;
        if (path.filename() == "project.md") load_changed_project(snapshot, path);
        else load_changed_task(snapshot, path);
    }
    std::map<std::string, std::string> projects;
    for (const auto& [id, project] : snapshot.projects) projects[normalized_path(std::filesystem::path(project.source_path).parent_path())] = id;
    for (auto& [id, project] : snapshot.projects) {
        (void)id;
        const auto parent = std::filesystem::path(project.source_path).parent_path().parent_path().parent_path();
        project.parent_id = projects[normalized_path(parent)];
    }
    for (auto& [id, task] : snapshot.tasks) {
        (void)id;
        task.project_id = projects[normalized_path(std::filesystem::path(task.source_path).parent_path().parent_path().parent_path())];
    }
    if (!snapshot.projects.empty()) std::erase_if(snapshot.diagnostics, [](const auto& diagnostic) { return diagnostic.message.starts_with("workspace has no projects"); });
    validate_hierarchy(snapshot);
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
