// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "domain/model.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace todobench {

enum class TrashStatus { Succeeded, Error, Conflict };
struct TrashResult {
    TrashStatus status{TrashStatus::Error};
    std::string id;
    std::string message;
    int affected_count{0};
    int relocated_count{0};
};

struct TrashItem {
    std::string id;
    int affected_count{0};
    std::filesystem::path manifest_path;
};

class TrashStore {
public:
    explicit TrashStore(std::filesystem::path workspace_root);

    TrashResult move_to_trash(const std::vector<TaskRecord>& tasks);
    TrashResult restore(const std::string& id, const std::filesystem::path& fallback_tasks);
    std::vector<TrashItem> list(std::string& error) const;

private:
    std::filesystem::path root_;
};

}  // namespace todobench
