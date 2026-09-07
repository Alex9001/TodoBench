// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/trash_store.h"

#include "storage/recovery_journal.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>
#include <fstream>

namespace todobench {
namespace {

struct TrashManifestEntry {
    std::string task_id;
    std::filesystem::path original;
    std::filesystem::path stored;
};

std::filesystem::path manifest_path(const std::filesystem::path& root, const std::string& id) {
    return root / id / "manifest.json";
}

bool write_manifest(const std::filesystem::path& path, const std::vector<TrashManifestEntry>& entries, std::string& error) {
    QJsonArray items;
    for (const auto& entry : entries) {
        QJsonObject item;
        item["task_id"] = QString::fromStdString(entry.task_id);
        item["original"] = QString::fromStdString(entry.original.string());
        item["stored"] = QString::fromStdString(entry.stored.string());
        items.append(item);
    }
    QJsonObject manifest;
    manifest["schema_version"] = 1;
    manifest["items"] = items;
    QSaveFile output(QString::fromStdString(path.string()));
    if (!output.open(QIODevice::WriteOnly)) { error = output.errorString().toStdString(); return false; }
    const auto bytes = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    if (output.write(bytes) != bytes.size() || !output.commit()) { error = output.errorString().toStdString(); return false; }
    return true;
}

bool read_manifest(const std::filesystem::path& path, std::vector<TrashManifestEntry>& entries, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { error = "unable to read trash manifest: " + path.string(); return false; }
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(bytes), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) { error = "invalid trash manifest: " + path.string(); return false; }
    for (const auto& value : document.object().value("items").toArray()) {
        const auto item = value.toObject();
        entries.push_back({item.value("task_id").toString().toStdString(),
                           item.value("original").toString().toStdString(),
                           item.value("stored").toString().toStdString()});
    }
    return !entries.empty();
}

std::filesystem::path unique_destination(const std::filesystem::path& directory, const std::filesystem::path& source,
                                         const std::string& id) {
    auto destination = directory / source.filename();
    if (!std::filesystem::exists(destination)) return destination;
    return directory / (source.filename().string() + "--" + id);
}

}  // namespace

TrashStore::TrashStore(std::filesystem::path workspace_root)
    : root_(std::move(workspace_root) / ".todobench" / "trash") {}

TrashResult TrashStore::move_to_trash(const std::vector<TaskRecord>& tasks) {
    if (tasks.empty()) return {TrashStatus::Error, {}, "no tasks selected", 0, 0};
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const auto bundle = root_ / id;
    std::vector<std::string> paths;
    for (const auto& task : tasks) paths.push_back(task.source_path);
    RecoveryJournal journal(root_.parent_path().parent_path());
    std::string error;
    std::string journal_id;
    if (!journal.begin("trash", paths, journal_id, error)) return {TrashStatus::Error, {}, error, 0, 0};
    std::error_code filesystem_error;
    std::filesystem::create_directories(bundle / "items", filesystem_error);
    if (filesystem_error) return {TrashStatus::Error, id, filesystem_error.message(), 0, 0};
    std::vector<TrashManifestEntry> entries;
    for (const auto& task : tasks) {
        const auto original = std::filesystem::path(task.source_path).parent_path();
        const auto stored = bundle / "items" / original.filename();
        std::filesystem::rename(original, stored, filesystem_error);
        if (filesystem_error) return {TrashStatus::Error, id, filesystem_error.message(), static_cast<int>(entries.size()), 0};
        entries.push_back({task.id, original, stored});
    }
    if (!write_manifest(bundle / "manifest.json", entries, error)) return {TrashStatus::Error, id, error, static_cast<int>(entries.size()), 0};
    if (!journal.complete(journal_id, error)) return {TrashStatus::Error, id, error, static_cast<int>(entries.size()), 0};
    return {TrashStatus::Succeeded, id, {}, static_cast<int>(entries.size()), 0};
}

TrashResult TrashStore::restore(const std::string& id, const std::filesystem::path& fallback_tasks) {
    const auto manifest = manifest_path(root_, id);
    std::vector<TrashManifestEntry> entries;
    std::string error;
    if (!read_manifest(manifest, entries, error)) return {TrashStatus::Error, id, error, 0, 0};
    std::vector<std::string> paths;
    for (const auto& entry : entries) paths.push_back(entry.stored.string());
    RecoveryJournal journal(root_.parent_path().parent_path());
    std::string journal_id;
    if (!journal.begin("restore", paths, journal_id, error)) return {TrashStatus::Error, id, error, 0, 0};
    int relocated = 0;
    std::error_code filesystem_error;
    for (auto& entry : entries) {
        auto destination = entry.original;
        if (std::filesystem::exists(destination)) {
            std::filesystem::create_directories(fallback_tasks, filesystem_error);
            destination = unique_destination(fallback_tasks, entry.stored, entry.task_id);
            ++relocated;
        } else {
            std::filesystem::create_directories(destination.parent_path(), filesystem_error);
        }
        if (filesystem_error) return {TrashStatus::Error, id, filesystem_error.message(), 0, relocated};
        std::filesystem::rename(entry.stored, destination, filesystem_error);
        if (filesystem_error) return {TrashStatus::Error, id, filesystem_error.message(), 0, relocated};
    }
    std::filesystem::remove_all(root_ / id, filesystem_error);
    if (filesystem_error) return {TrashStatus::Error, id, filesystem_error.message(), static_cast<int>(entries.size()), relocated};
    if (!journal.complete(journal_id, error)) return {TrashStatus::Error, id, error, static_cast<int>(entries.size()), relocated};
    return {TrashStatus::Succeeded, id, {}, static_cast<int>(entries.size()), relocated};
}

std::vector<TrashItem> TrashStore::list(std::string& error) const {
    std::vector<TrashItem> items;
    if (!std::filesystem::exists(root_)) return items;
    for (const auto& entry : std::filesystem::directory_iterator(root_)) {
        if (!entry.is_directory()) continue;
        std::vector<TrashManifestEntry> manifest;
        if (!read_manifest(entry.path() / "manifest.json", manifest, error)) return {};
        items.push_back({entry.path().filename().string(), static_cast<int>(manifest.size()), entry.path() / "manifest.json"});
    }
    return items;
}

}  // namespace todobench
