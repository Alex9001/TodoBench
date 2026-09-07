// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "domain/model.h"

#include <filesystem>
#include <string>
#include <vector>

namespace todobench {

enum class SaveStatus { Saved, Conflict, Error };
struct SaveResult {
    SaveStatus status{SaveStatus::Error};
    std::string path;
    std::string message;
};

class WorkspaceStore {
public:
    explicit WorkspaceStore(std::filesystem::path root);
    static SaveResult create_workspace(const std::filesystem::path& root, const std::string& name, bool include_tutorial = false);
    SaveResult create_task(TaskRecord& task) const;
    SaveResult save_task(const TaskRecord& task) const;
    SaveResult save_project(const ProjectRecord& project) const;
    SaveResult move_task_branch(const std::vector<TaskRecord>& tasks,
                                const std::filesystem::path& destination_tasks) const;
    static std::string hash_bytes(const std::string& bytes);

private:
    std::filesystem::path root_;
};

}  // namespace todobench
