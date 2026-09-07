// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "domain/model.h"

#include <filesystem>
#include <string>

namespace todobench {

enum class ExternalChange { Unchanged, Reload, Conflict, Error };
struct ReconcileResult {
    ExternalChange change{ExternalChange::Error};
    TaskRecord disk_task;
    std::string conflict_path;
    std::string message;
};

class ExternalReconciler {
public:
    explicit ExternalReconciler(std::filesystem::path workspace_root);
    ReconcileResult reconcile(const TaskRecord& loaded, bool dirty) const;

private:
    std::filesystem::path root_;
};

}  // namespace todobench
