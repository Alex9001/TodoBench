// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/history_store.h"

#include "storage/front_matter_codec.h"

#include <QSaveFile>

#include <algorithm>
#include <fstream>

namespace todobench {

HistoryStore::HistoryStore(std::filesystem::path root) : root_(std::move(root)) {}

HistoryResult HistoryStore::snapshot_branch(const std::vector<TaskRecord>& tasks, const std::string& completion_id) const {
    if (tasks.empty()) return {false, {}, "cannot snapshot an empty task branch"};
    const auto directory = root_ / ".todobench" / "history" / completion_id;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return {false, directory, error.message()};
    for (const auto& task : tasks) {
        const auto source = std::filesystem::path(task.source_path);
        const auto file = directory / (source.parent_path().filename().string() + "--" + task.id + ".md");
        QSaveFile output(QString::fromStdString(file.string()));
        if (!output.open(QIODevice::WriteOnly)) return {false, file, output.errorString().toStdString()};
        const auto content = serialize_task_markdown(task);
        if (output.write(QByteArray::fromStdString(content)) != static_cast<qint64>(content.size()) || !output.commit()) {
            return {false, file, output.errorString().toStdString()};
        }
    }
    return {true, directory, {}};
}

std::vector<HistoryEntry> HistoryStore::list_for_task(const std::string& task_id) const {
    std::vector<HistoryEntry> entries;
    if (task_id.empty() || root_.empty()) return entries;
    const auto history = root_ / ".todobench" / "history";
    std::error_code error;
    if (!std::filesystem::exists(history, error)) return entries;
    const auto suffix = "--" + task_id + ".md";
    for (std::filesystem::directory_iterator iterator(history, error), end; iterator != end && !error;
         iterator.increment(error)) {
        if (!iterator->is_directory(error)) continue;
        for (std::filesystem::directory_iterator file(iterator->path(), error), file_end; file != file_end && !error;
             file.increment(error)) {
            const auto name = file->path().filename().string();
            if (name.size() < suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
                continue;
            }
            HistoryEntry entry;
            entry.completion_id = iterator->path().filename().string();
            entry.path = file->path().string();
            entry.label = entry.completion_id;
            std::ifstream input(file->path(), std::ios::binary);
            entry.markdown = {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            entries.push_back(std::move(entry));
        }
    }
    std::sort(entries.begin(), entries.end(), [](const HistoryEntry& left, const HistoryEntry& right) {
        return left.completion_id > right.completion_id;
    });
    return entries;
}

}  // namespace todobench
