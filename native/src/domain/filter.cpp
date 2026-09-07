// SPDX-License-Identifier: GPL-3.0-or-later
#include "domain/filter.h"

#include <QChar>
#include <QString>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace todobench {
namespace {

std::vector<std::string> tokenize(const std::string& expression) {
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false;
    for (size_t index = 0; index < expression.size(); ++index) {
        const char character = expression[index];
        if (character == '\\' && index + 1 < expression.size()
            && (expression[index + 1] == '"' || expression[index + 1] == '\\')) {
            current.push_back(expression[++index]);
            continue;
        }
        if (character == '"') {
            quoted = !quoted;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(character)) && !quoted) {
            if (!current.empty()) tokens.push_back(std::move(current));
            current.clear();
            continue;
        }
        current.push_back(character);
    }
    if (!current.empty()) tokens.push_back(std::move(current));
    return tokens;
}

std::string normalized(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()))
        .normalized(QString::NormalizationForm_KC).toCaseFolded().toUtf8().toStdString();
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool parse_date(const std::string& value, std::optional<QDate>& target) {
    const auto date = QDate::fromString(QString::fromStdString(value), Qt::ISODate);
    if (!date.isValid()) return false;
    target = date;
    return true;
}

bool parse_status_list(const std::string& value, std::vector<TaskStatus>& output) {
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        TaskStatus status;
        if (!parse_task_status(item, status)) return false;
        output.push_back(status);
    }
    return !output.empty();
}

bool parse_priority_list(const std::string& value, std::vector<Priority>& output) {
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        Priority priority;
        if (!parse_priority(item, priority)) return false;
        output.push_back(priority);
    }
    return !output.empty();
}

bool due_matches(const TaskRecord& task, const FilterSpec& spec) {
    if (!spec.due_from && !spec.due_to) return true;
    if (task.due_yaml.empty() || task.due_yaml == "null") return false;
    const auto date = QDate::fromString(QString::fromStdString(task.due_yaml).trimmed(), Qt::ISODate);
    if (!date.isValid()) return false;
    if (spec.due_from && date < *spec.due_from) return false;
    return !spec.due_to || date <= *spec.due_to;
}

bool apply_named_filter(const std::string& key, const std::string& value, FilterCompileResult& result) {
    if (key == "tag") {
        result.spec.tags.push_back(value);
        return true;
    }
    if (key == "tag_any") {
        result.spec.tag_match = TagMatch::Any;
        return true;
    }
    if (key == "tag_all") {
        result.spec.tag_match = TagMatch::All;
        return true;
    }
    if (key == "project") {
        result.spec.project_ids.push_back(value);
        return true;
    }
    if (key == "status") {
        if (!parse_status_list(value, result.spec.statuses)) result.error = "invalid status filter";
        return result.error.empty();
    }
    if (key == "priority") {
        if (!parse_priority_list(value, result.spec.priorities)) result.error = "invalid priority filter";
        return result.error.empty();
    }
    if (key == "due_from") {
        if (!parse_date(value, result.spec.due_from)) result.error = "invalid due_from date";
        return result.error.empty();
    }
    if (key == "due_to") {
        if (!parse_date(value, result.spec.due_to)) result.error = "invalid due_to date";
        return result.error.empty();
    }
    result.error = "unknown filter field: " + key;
    return false;
}

bool title_matches(const TaskRecord& task, const FilterSpec& spec) {
    const auto title = normalized(task.title);
    for (const auto& term : spec.title_terms) {
        if (title.find(normalized(term)) == std::string::npos) return false;
    }
    if (spec.title_terms.empty() && !spec.title_query.empty()) {
        return title.find(normalized(spec.title_query)) != std::string::npos;
    }
    return true;
}

bool tags_match(const TaskRecord& task, const FilterSpec& spec) {
    if (spec.tags.empty()) return true;
    const auto matches = [&task](const std::string& tag) {
        return std::any_of(task.tags.begin(), task.tags.end(), [&tag](const std::string& candidate) {
            return normalized(candidate) == normalized(tag);
        });
    };
    if (spec.tag_match == TagMatch::All) return std::all_of(spec.tags.begin(), spec.tags.end(), matches);
    return std::any_of(spec.tags.begin(), spec.tags.end(), matches);
}

void add_descendant_projects(const std::string& selected,
                             const std::unordered_map<std::string, ProjectRecord>& projects,
                             std::vector<std::string>& ids) {
    for (const auto& [project_id, project] : projects) {
        auto parent = project.parent_id;
        std::vector<std::string> visited;
        while (!parent.empty() && std::find(visited.begin(), visited.end(), parent) == visited.end()) {
            visited.push_back(parent);
            if (parent == selected) {
                ids.push_back(project_id);
                break;
            }
            const auto ancestor = projects.find(parent);
            if (ancestor == projects.end()) break;
            parent = ancestor->second.parent_id;
        }
    }
}

std::vector<std::string> resolve_project_filter(
    const std::string& selected, const std::unordered_map<std::string, ProjectRecord>& projects) {
    std::vector<std::string> ids;
    if (projects.contains(selected)) ids.push_back(selected);
    const auto query = normalized(selected);
    for (const auto& [id, project] : projects) {
        if (normalized(project.display_name) == query && !contains(ids, id)) ids.push_back(id);
    }
    if (ids.empty()) ids.push_back(selected);
    return ids;
}

}  // namespace

std::vector<std::string> filter_tokens(const std::string& expression) {
    std::vector<std::string> result;
    for (const auto& token : tokenize(expression)) {
        result.push_back(quote_filter_token(token));
    }
    return result;
}

std::vector<std::string> filter_token_values(const std::string& expression) {
    return tokenize(expression);
}

std::string quote_filter_token(const std::string& token) {
    std::string escaped;
    for (const char character : token) {
        if (character == '"' || character == '\\') escaped += '\\';
        escaped += character;
    }
    if (token.find_first_of(" \t\r\n\"") != std::string::npos) return "\"" + escaped + "\"";
    return escaped;
}

FilterCompileResult compile_filter(const std::string& expression) {
    FilterCompileResult result;
    for (const auto& token : tokenize(expression)) {
        const auto separator = token.find(':');
        if (separator == std::string::npos) {
            result.spec.title_terms.push_back(token);
            if (!result.spec.title_query.empty()) result.spec.title_query += ' ';
            result.spec.title_query += token;
            continue;
        }
        const auto key = token.substr(0, separator);
        const auto value = token.substr(separator + 1);
        if (value.empty()) {
            result.error = "filter value cannot be empty for " + key;
            return result;
        }
        if (!apply_named_filter(key, value, result)) return result;
    }
    return result;
}

bool matches_filter(const TaskRecord& task, const FilterSpec& spec) {
    if (!title_matches(task, spec)) return false;
    if (!spec.project_ids.empty() && !contains(spec.project_ids, task.project_id)) return false;
    if (!spec.statuses.empty() && std::find(spec.statuses.begin(), spec.statuses.end(), task.status) == spec.statuses.end()) return false;
    if (!spec.priorities.empty() && std::find(spec.priorities.begin(), spec.priorities.end(), task.priority) == spec.priorities.end()) return false;
    if (!tags_match(task, spec)) return false;
    return due_matches(task, spec);
}

bool matches_filter(const TaskRecord& task, const FilterSpec& spec,
                    const std::unordered_map<std::string, ProjectRecord>& projects) {
    if (spec.project_ids.empty() || !spec.include_subprojects) return matches_filter(task, spec);
    auto expanded = spec;
    expanded.project_ids.clear();
    for (const auto& selected : spec.project_ids) {
        for (const auto& resolved : resolve_project_filter(selected, projects)) {
            expanded.project_ids.push_back(resolved);
            add_descendant_projects(resolved, projects, expanded.project_ids);
        }
    }
    return matches_filter(task, expanded);
}

}  // namespace todobench
