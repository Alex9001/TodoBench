// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace todobench {

struct MarkdownValidationResult {
    bool supported{false};
    std::string message;
};

MarkdownValidationResult validate_markdown_for_visual(const std::string& markdown);

}  // namespace todobench
