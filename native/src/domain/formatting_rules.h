// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "domain/model.h"

#include <QDate>

#include <optional>
#include <string>
#include <vector>

namespace todobench {

struct FormattingAppearance {
    std::optional<std::string> foreground;
    std::optional<std::string> background;
    std::optional<bool> bold;
    std::optional<bool> italic;
    std::optional<bool> strikethrough;
};

struct FormattingRule {
    std::string name;
    bool enabled{true};
    std::string project_id;
    std::string tag;
    std::optional<TaskStatus> status;
    std::optional<Priority> priority;
    std::string title_contains;
    std::optional<bool> overdue;
    FormattingAppearance appearance;
};

bool formatting_rule_matches(const FormattingRule& rule, const TaskRecord& task,
                             const QDate& today);
FormattingAppearance evaluate_formatting_rules(const TaskRecord& task,
                                               const std::vector<FormattingRule>& rules,
                                               const QDate& today);
std::vector<FormattingRule> default_formatting_rules();

}  // namespace todobench
