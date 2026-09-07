// SPDX-License-Identifier: GPL-3.0-or-later
#include "domain/markdown_validation.h"

#ifdef TODOBENCH_HAS_CMARK
#include <cmark-gfm.h>
#include <cmark-gfm-core-extensions.h>
#endif

#include <cctype>
#include <string>

namespace todobench {

#ifdef TODOBENCH_HAS_CMARK
namespace {

bool has_unsupported_node(cmark_node* node, std::string& message) {
    for (auto* current = node; current != nullptr; current = cmark_node_next(current)) {
        const auto* type = cmark_node_get_type_string(current);
        if (type == nullptr) continue;
        const std::string node_type(type);
        if (node_type == "html_block" || node_type == "html_inline"
            || node_type == "custom_block" || node_type == "custom_inline") {
            message = "visual editing is disabled for raw or custom Markdown constructs";
            return true;
        }
        if (has_unsupported_node(cmark_node_first_child(current), message)) return true;
    }
    return false;
}

}  // namespace
#endif

#ifndef TODOBENCH_HAS_CMARK
namespace {

bool contains_raw_html(const std::string& markdown) {
    for (size_t offset = 0; offset < markdown.size(); ++offset) {
        if (markdown[offset] != '<') continue;
        const auto closing = markdown.find('>', offset + 1);
        if (closing == std::string::npos || closing == offset + 1) continue;
        const auto first = markdown[offset + 1];
        if (first == '/' || first == '!' || std::isalpha(static_cast<unsigned char>(first))) return true;
    }
    return false;
}

}  // namespace
#endif

MarkdownValidationResult validate_markdown_for_visual(const std::string& markdown) {
#ifdef TODOBENCH_HAS_CMARK
    cmark_gfm_core_extensions_ensure_registered();
    auto* parser = cmark_parser_new(CMARK_OPT_DEFAULT);
    for (const auto* name : {"table", "strikethrough", "tasklist", "autolink"}) {
        if (auto* extension = cmark_find_syntax_extension(name); extension != nullptr) {
            cmark_parser_attach_syntax_extension(parser, extension);
        }
    }
    cmark_parser_feed(parser, markdown.data(), markdown.size());
    auto* document = cmark_parser_finish(parser);
    std::string message;
    const auto unsupported = document == nullptr || has_unsupported_node(document, message);
    cmark_node_free(document);
    cmark_parser_free(parser);
    if (unsupported) return {false, message.empty() ? "Markdown cannot be parsed for visual editing" : message};
    return {true, {}};
#else
    if (contains_raw_html(markdown)) {
        return {false, "visual editing is disabled for raw HTML; use Source mode for this document"};
    }
    return {true, "cmark-gfm is unavailable; using the Qt Markdown visual subset"};
#endif
}

}  // namespace todobench
