// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "domain/model.h"

#include <filesystem>

namespace todobench {

class WorkspaceScanner {
public:
    WorkspaceSnapshot scan(const std::filesystem::path& root) const;

private:
    static void scan_project_tree(const std::filesystem::path& directory, WorkspaceSnapshot& snapshot,
                                  const std::string& parent_project_id = {});
    static void validate_hierarchy(WorkspaceSnapshot& snapshot);
};

}  // namespace todobench
