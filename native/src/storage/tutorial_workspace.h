// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "storage/workspace_store.h"

namespace todobench {

// Build in a sibling staging directory, then publish only the complete workspace.
SaveResult create_tutorial_workspace(const std::filesystem::path& root, const std::string& name);

}  // namespace todobench
