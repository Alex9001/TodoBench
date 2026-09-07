// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "domain/model.h"

#include <filesystem>
#include <string>
#include <vector>

namespace todobench {

struct HistoryResult {
    bool success{false};
    std::filesystem::path path;
    std::string error;
};

struct HistoryEntry {
    std::string completion_id;
    std::string path;
    std::string label;
    std::string markdown;
};

class HistoryStore final {
public:
    explicit HistoryStore(std::filesystem::path root);
    HistoryResult snapshot_branch(const std::vector<TaskRecord>& tasks, const std::string& completion_id) const;
    std::vector<HistoryEntry> list_for_task(const std::string& task_id) const;

private:
    std::filesystem::path root_;
};

}  // namespace todobench
