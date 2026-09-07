// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <vector>

namespace todobench {

std::filesystem::path last_workspace();
void remember_workspace(const std::filesystem::path& root);
std::vector<std::filesystem::path> recent_workspaces();
bool hide_to_tray_enabled();
void set_hide_to_tray_enabled(bool enabled);

}  // namespace todobench
