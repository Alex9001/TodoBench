// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "storage/workspace_store.h"
#include <functional>
namespace todobench {
// Publish only a complete new workspace; never populate an existing nonempty folder.
SaveResult create_staged_workspace(const std::filesystem::path& root,
    const std::function<void(const std::filesystem::path&)>& populate);
}
