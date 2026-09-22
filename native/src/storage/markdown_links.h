// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace todobench {
using LinkRewriter = std::function<std::string(const std::string&, bool)>;
std::string rewrite_link_destinations(const std::string& markdown, const LinkRewriter& rewrite, bool convert_wiki = false);
using DirectoryMoves = std::vector<std::pair<std::filesystem::path, std::filesystem::path>>;
std::filesystem::path moved_path(const std::filesystem::path& path, const DirectoryMoves& moves);
std::string rewrite_document_links(const std::string& markdown, const std::filesystem::path& source,
                                   const std::filesystem::path& root, const DirectoryMoves& moves, bool source_moves = true);
void rewrite_workspace_links(const std::filesystem::path& root, const DirectoryMoves& moves, bool source_moves = true);
} // namespace todobench
