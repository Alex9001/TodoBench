// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <string>
namespace todobench {
// Retain both versions without modifying the contested file.
bool retain_conflict(const std::filesystem::path& root, const std::string& name,
                     const std::string& disk, const std::string& local, std::string& error);
}
