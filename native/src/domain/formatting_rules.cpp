// SPDX-License-Identifier: GPL-3.0-or-later
#include "domain/formatting_rules.h"

#include <QString>

#include <algorithm>

namespace todobench {
namespace {

bool has_tag(const TaskRecord& task, const std::string& tag) {
    return std::find(task.tags.begin(), task.tags.end(), tag) != task.tags.end();
}

bool is_overdue(const TaskRecord& task, const QDate& today) {
    if (task.status == TaskStatus::Done || task.status == TaskStatus::Cancelled || task.due_yaml.empty() || task.due_yaml == "null") return false;
    const auto due = QDate::fromString(QString::fromStdString(task.due_yaml).trimmed(), Qt::ISODate);
    return due.isValid() && due < today;
}

bool matches_optional(bool condition, bool actual) {
    return condition == actual;
}

void claim_if_unset(std::optional<std::string>& target, const std::optional<std::string>& value) {
    if (!target && value) target = value;
}

void claim_if_unset(std::optional<bool>& target, const std::optional<bool>& value) {
    if (!target && value) target = value;
}

}  // namespace

bool formatting_rule_matches(const FormattingRule& rule, const TaskRecord& task, const QDate& today) {
    if (!rule.enabled) return false;
    if (!rule.project_id.empty() && rule.project_id != task.project_id) return false;
    if (!rule.tag.empty() && !has_tag(task, rule.tag)) return false;
    if (rule.status && *rule.status != task.status) return false;
    if (rule.priority && *rule.priority != task.priority) return false;
    if (!rule.title_contains.empty() && QString::fromStdString(task.title).toCaseFolded().indexOf(QString::fromStdString(rule.title_contains).toCaseFolded()) < 0) return false;
    if (rule.overdue && !matches_optional(*rule.overdue, is_overdue(task, today))) return false;
    return true;
}

FormattingAppearance evaluate_formatting_rules(const TaskRecord& task,
                                               const std::vector<FormattingRule>& rules,
                                               const QDate& today) {
    FormattingAppearance result;
    for (const auto& rule : rules) {
        if (!formatting_rule_matches(rule, task, today)) continue;
        claim_if_unset(result.foreground, rule.appearance.foreground);
        claim_if_unset(result.background, rule.appearance.background);
        claim_if_unset(result.bold, rule.appearance.bold);
        claim_if_unset(result.italic, rule.appearance.italic);
        claim_if_unset(result.strikethrough, rule.appearance.strikethrough);
    }
    return result;
}

std::vector<FormattingRule> default_formatting_rules() {
    FormattingRule done{"Done", true, {}, {}, TaskStatus::Done, std::nullopt, {}, std::nullopt,
                        {std::nullopt, std::nullopt, std::nullopt, std::nullopt, true}};
    FormattingRule cancelled{"Cancelled", true, {}, {}, TaskStatus::Cancelled, std::nullopt, {}, std::nullopt,
                             {std::nullopt, std::nullopt, std::nullopt, std::nullopt, true}};
    FormattingRule overdue{"Open and overdue", true, {}, {}, std::nullopt, std::nullopt, {}, true,
                           {std::optional<std::string>("#b00020"), std::nullopt, std::nullopt, std::nullopt, std::nullopt}};
    FormattingRule urgent{"Urgent", true, {}, {}, std::nullopt, Priority::Urgent, {}, std::nullopt,
                          {std::nullopt, std::nullopt, std::optional<bool>(true), std::nullopt, std::nullopt}};
    return {std::move(done), std::move(cancelled), std::move(overdue), std::move(urgent)};
}

}  // namespace todobench
