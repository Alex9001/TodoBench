// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/trash_store.h"
#include "storage/transactional_storage.h"
#include "storage/markdown_links.h"
#include "storage/archive_safety.h"
#include "storage/directory_names.h"
#include "storage/front_matter_codec.h"
#include "storage/workspace_scanner.h"
#include <QJsonArray>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <fstream>
#include <set>
#include <stdexcept>

namespace todobench {
namespace {
namespace fs = std::filesystem;
struct Entry {
    std::string task_id, project_id, title;
    fs::path original, stored;
    std::string project_name;
};
bool within(const fs::path& path, const fs::path& root) {
    const auto relative = fs::weakly_canonical(path).lexically_relative(fs::weakly_canonical(root));
    return !relative.empty() && relative != "." && *relative.begin() != "..";
}
std::string manifest_bytes(const fs::path& workspace, const std::vector<Entry>& entries) {
    QJsonArray items;
    for (const auto& entry : entries) items.append(QJsonObject{
        {"task_id", QString::fromStdString(entry.task_id)}, {"project_id", QString::fromStdString(entry.project_id)},
        {"title", QString::fromStdString(entry.title)}, {"project_name", QString::fromStdString(entry.project_name)},
        {"original", QString::fromStdString(entry.original.lexically_relative(workspace).generic_string())},
        {"stored", QString::fromStdString(entry.stored.lexically_relative(workspace).generic_string())}});
    return QJsonDocument(QJsonObject{{"schema_version", 2}, {"deleted_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}, {"items", items}}).toJson().toStdString();
}
void validate_entry(const Entry& entry, int version) {
    if (!is_valid_uuid(entry.task_id) || entry.original.empty() || entry.stored.empty())
        throw std::runtime_error("Invalid Trash item identity or path");
    if (version == 2 && (!is_valid_uuid(entry.project_id) || entry.original.is_absolute() || entry.stored.is_absolute()))
        throw std::runtime_error("Invalid version 2 Trash project identity or relative path");
}
std::vector<Entry> parse_manifest(const fs::path& bundle, const std::string& bytes) {
    const auto workspace = bundle.parent_path().parent_path().parent_path();
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(bytes), &error);
    const auto object = document.object();
    const int version = object.value("schema_version").toInt();
    if (error.error != QJsonParseError::NoError || (version != 1 && version != 2) || !object.value("items").isArray())
        throw std::runtime_error("Invalid Trash manifest: " + (bundle / "manifest.json").string());
    std::vector<Entry> entries;
    std::set<std::string> ids, stored_paths;
    for (const auto& value : object.value("items").toArray()) {
        const auto item = value.toObject();
        Entry entry{item.value("task_id").toString().toStdString(), item.value("project_id").toString().toStdString(),
            item.value("title").toString().toStdString(), item.value("original").toString().toStdString(), item.value("stored").toString().toStdString(), {}};
        entry.project_name = item.value("project_name").toString().toStdString();
        validate_entry(entry, version);
        if (!ids.insert(entry.task_id).second)
            throw std::runtime_error("Invalid or duplicate Trash item in " + bundle.string());
        entry.original = entry.original.is_absolute() ? entry.original : workspace / entry.original;
        entry.stored = entry.stored.is_absolute() ? bundle / "items" / entry.stored.filename() : workspace / entry.stored;
        if (!within(entry.stored, bundle / "items") || !stored_paths.insert(entry.stored.lexically_normal().string()).second)
            throw std::runtime_error("Unsafe Trash item path in " + bundle.string());
        if (version == 2 && !within(entry.original, workspace / "projects")) throw std::runtime_error("Unsafe original Trash path");
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) throw std::runtime_error("Empty Trash manifest: " + bundle.string());
    return entries;
}
fs::path inbox_directory(const fs::path& root, const WorkspaceSnapshot& snapshot, CommandTransaction& transaction) {
    for (const auto& [id, project] : snapshot.projects) {
        (void)id;
        if (project.parent_id.empty() && directory_key(project.display_name) == "inbox" && !project.archived)
            return fs::path(project.source_path).parent_path() / "tasks";
    }
    ProjectRecord project;
    project.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    project.display_name = "Inbox";
    const auto directory = allocate_directory(root / "projects", project.display_name, "project");
    project.source_path = (directory / "project.md").string();
    transaction.write(project.source_path, serialize_project_markdown(project));
    transaction.mkdir(directory / "tasks");
    return directory / "tasks";
}
fs::path owner_directory(const Entry& entry, const WorkspaceSnapshot& snapshot) {
    const auto owner = snapshot.projects.find(entry.project_id);
    if (owner != snapshot.projects.end()) return fs::path(owner->second.source_path).parent_path() / "tasks";
    // Legacy manifests have no project ID; accept only an existing registered project.
    if (entry.project_id.empty()) for (const auto& [id, project] : snapshot.projects) {
        (void)id;
        const auto directory = fs::path(project.source_path).parent_path() / "tasks";
        if (directory == entry.original.parent_path()) return directory;
    }
    return {};
}
bool portable_collision(const fs::path& directory, const fs::path& destination, CommandTransaction& transaction) {
    for (const auto& child : transaction.children(directory))
        if (directory_key(child.filename().string()) == directory_key(destination.filename().string())) return true;
    return false;
}
fs::path restore_destination(const Entry& entry, const WorkspaceSnapshot& snapshot, const fs::path& fallback, CommandTransaction& transaction) {
    auto directory = owner_directory(entry, snapshot);
    if (directory.empty()) directory = fallback;
    const auto parsed = parse_task_markdown((entry.stored / "task.md").string(), transaction.read(entry.stored / "task.md"));
    const auto* task = std::get_if<TaskRecord>(&parsed);
    if (!task || task->id != entry.task_id || snapshot.tasks.contains(entry.task_id))
        throw std::runtime_error("Trash task identity conflict: " + entry.stored.string());
    auto destination = directory / entry.original.filename();
    const bool portable = validate_archive_entry({destination.filename().string(), ArchiveEntryType::Directory}).valid;
    if (!portable || portable_collision(directory, destination, transaction)) destination = allocate_directory(directory, task->title, "task");
    return destination;
}
std::string owning_project_id(const TaskRecord& task, CommandTransaction& transaction) {
    const auto path = fs::path(task.source_path).parent_path().parent_path().parent_path() / "project.md";
    const auto parsed = parse_project_markdown(path.string(), transaction.read(path));
    const auto* project = std::get_if<ProjectRecord>(&parsed);
    if (!project || !is_valid_uuid(project->id)) throw std::runtime_error("Trash task has no valid owning project");
    if (!task.project_id.empty() && task.project_id != project->id) throw std::runtime_error("Task's owning project changed before Trash");
    return project->id;
}

void validate_bundle(const std::vector<Entry>& entries) {
    for (const auto& entry : entries)
        if (!fs::is_directory(entry.stored) || !fs::is_regular_file(entry.stored / "task.md"))
            throw std::runtime_error("Missing task in Trash bundle: " + entry.stored.string());
}

} // namespace

TrashStore::TrashStore(fs::path root) : root_(std::move(root) / ".todobench" / "trash") {}

TrashResult TrashStore::move_to_trash(const std::vector<TaskRecord>& tasks) {
    if (tasks.empty()) return {TrashStatus::Error, {}, "no tasks selected", 0, 0};
    const auto root = root_.parent_path().parent_path();
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const auto bundle = root_ / id;
    const auto saved = storage_command(root, [&](CommandTransaction& transaction) {
        std::vector<Entry> entries;
        std::set<std::string> occupied;
        for (const auto& task : tasks) {
            if (!within(task.source_path, root / "projects")) throw std::runtime_error("Task is outside workspace projects");
            if (WorkspaceStore::hash_bytes(transaction.read(task.source_path)) != task.source_hash) throw std::runtime_error("Task changed before Trash");
            entries.push_back({task.id, owning_project_id(task, transaction), task.title, fs::path(task.source_path).parent_path(),
                bundle / "items" / allocate_directory_name(task.title, "task", occupied), {}});
        }
        const auto snapshot = WorkspaceScanner{}.scan(root);
        for (auto& entry : entries) {
            const auto project = snapshot.projects.find(entry.project_id);
            if (project != snapshot.projects.end()) entry.project_name = project->second.display_name;
        }
        const auto bytes = manifest_bytes(root, entries);
        parse_manifest(bundle, bytes);
        transaction.write(bundle / "manifest.json", bytes); // Committed durably before the first move.
        for (const auto& entry : entries) transaction.move(entry.original, entry.stored);
        return SaveResult{SaveStatus::Saved, {}, {}};
    });
    return {saved.status == SaveStatus::Saved ? TrashStatus::Succeeded : TrashStatus::Error, id, saved.message,
        saved.status == SaveStatus::Saved ? static_cast<int>(tasks.size()) : 0, 0};
}

TrashResult TrashStore::restore(const std::string& id, const fs::path& fallback_tasks) {
    (void)fallback_tasks; // Compatibility API: fallback must be resolved from project records.
    if (id.empty() || fs::path(id).filename().string() != id || id == "." || id == "..") return {TrashStatus::Error, id, "invalid trash id", 0, 0};
    const auto root = root_.parent_path().parent_path();
    const auto bundle = root_ / id;
    int relocated = 0, affected = 0;
    const auto result = storage_command(root, [&](CommandTransaction& transaction) {
        const auto manifest = bundle / "manifest.json";
        const auto entries = parse_manifest(bundle, transaction.read(manifest));
        const auto snapshot = WorkspaceScanner{}.scan(root);
        fs::path fallback;
        DirectoryMoves moves;
        DirectoryMoves project_moves;
        std::vector<fs::path> destinations;
        for (const auto& entry : entries) {
            const auto owner = owner_directory(entry, snapshot);
            if (owner.empty() && fallback.empty()) fallback = inbox_directory(root, snapshot, transaction);
            const auto destination = restore_destination(entry, snapshot, fallback, transaction);
            if (destination != entry.original) ++relocated;
            moves.emplace_back(entry.original, destination);
            if (!owner.empty()) project_moves.emplace_back(entry.original.parent_path().parent_path(), owner.parent_path());
            destinations.push_back(destination);
            // Staging this move reserves the actual destination for subsequent restored items.
            transaction.move(entry.stored, destination);
        }
        moves.insert(moves.end(), project_moves.begin(), project_moves.end());
        rewrite_workspace_links(root, moves, false);
        for (size_t index = 0; index < entries.size(); ++index) {
            const auto path = destinations[index] / "task.md";
            const auto original = transaction.read(path);
            transaction.write(path, rewrite_document_links(original, entries[index].original / "task.md", root, moves));
        }
        transaction.rmdir(bundle / "items");
        transaction.erase_file(manifest);
        transaction.rmdir(bundle);
        affected = static_cast<int>(entries.size());
        return SaveResult{SaveStatus::Saved, {}, {}};
    });
    return {result.status == SaveStatus::Saved ? TrashStatus::Succeeded : TrashStatus::Error, id, result.message, affected, relocated};
}

std::vector<TrashItem> TrashStore::list(std::string& error) const {
    std::vector<TrashItem> result;
    error.clear();
    std::error_code filesystem_error;
    for (fs::directory_iterator it(root_, filesystem_error), end; !filesystem_error && it != end; it.increment(filesystem_error)) {
        if (!it->is_directory()) continue;
        try {
            const auto manifest = it->path() / "manifest.json";
            std::ifstream input(manifest, std::ios::binary);
            if (!input) throw std::runtime_error("Missing Trash manifest: " + manifest.string());
            const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            const auto entries = parse_manifest(it->path(), bytes);
            validate_bundle(entries);
            TrashItem item{it->path().filename().string(), static_cast<int>(entries.size()), manifest, {}, {}, {}};
            item.title = entries.front().title;
            item.deleted_at = QJsonDocument::fromJson(QByteArray::fromStdString(bytes)).object().value("deleted_at").toString().toStdString();
            for (const auto& entry : entries) {
                const auto project = entry.project_name.empty() ? entry.original.parent_path().parent_path().filename().string() : entry.project_name;
                item.preview += entry.title + " — " + project + "\n";
            }
            result.push_back(std::move(item));
        } catch (const std::exception& failure) {
            if (!error.empty()) error += '\n';
            error += failure.what();
        }
    }
    return result;
}
} // namespace todobench
