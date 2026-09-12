// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "storage/mdbase_transfer.h"
#include "storage/mdbase_import.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace todobench::mdbase_transfer {

struct MarkdownRewriteRequest {
    std::filesystem::path source_collection_root; // frozen copy root (for source-relative resolution)
    std::filesystem::path dest_workspace_root;    // staged workspace root (for dest path allocation)
    TransferPreview* preview{nullptr};            // preview with source→dest maps (mutated with rewritten bodies)
    // Maps source asset rel -> dest asset rel (for attachment copy)
    std::unordered_map<std::string, std::string> asset_copy_map;
    TransferCancellation* cancellation{nullptr};
};

struct MarkdownRewriteResult {
    bool ok{true};
    std::vector<TransferDiagnostic> diagnostics;
    // per-record rewritten bodies keyed by source_path
    std::unordered_map<std::string, std::string> rewritten_body_by_source;
    size_t copied_asset_count{0};
};

// Rewrite inline Markdown links/images, reference definitions, and wikilinks/embeds using the fixed preview dest map.
// Copies referenced in-collection assets into owning task's assets/ (byte-exact, collision-free names).
// Only touches recognized link spans; preserves whitespace/labels/anchors/line endings outside spans.
// Every rewritten link must resolve inside the completed output (verified).
MarkdownRewriteResult rewrite_markdown_links(const MarkdownRewriteRequest& req);

} // namespace todobench::mdbase_transfer
