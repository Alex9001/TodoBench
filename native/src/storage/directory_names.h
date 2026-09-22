// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <set>
#include <string>

namespace todobench {
std::string directory_key(const std::string& value);
std::string directory_base(const std::string& title, const std::string& kind);
std::string allocate_directory_name(const std::string& title, const std::string& kind, std::set<std::string>& occupied);
std::filesystem::path allocate_directory(const std::filesystem::path& parent, const std::string& title,
                                          const std::string& kind, const std::filesystem::path& exclude = {});
} // namespace todobench
