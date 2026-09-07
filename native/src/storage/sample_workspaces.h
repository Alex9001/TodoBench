// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "storage/workspace_store.h"
#include <QJsonArray>
#include <QJsonObject>
namespace todobench {
struct WorkspaceRecipe {
    std::string name{"My Tasks"};
    std::string workflow{"simple"};
    std::string theme{"system"};
    std::string keyboard{"browser"};
};
const QJsonArray& sample_workflows();
QJsonObject sample_workflow(const std::string& id);
SaveResult create_sample_workspace(const std::filesystem::path& root, const WorkspaceRecipe& recipe);
}
