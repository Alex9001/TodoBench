// SPDX-License-Identifier: GPL-3.0-or-later
#include "domain/project_filter_labels.h"
#include "domain/filter.h"

#include <QString>
#include <QUuid>
#include <algorithm>
#include <unordered_set>

namespace todobench {
namespace {
std::string normalized(const std::string& value) {
    return QString::fromStdString(value).normalized(QString::NormalizationForm_KC).toCaseFolded().toStdString();
}

std::string project_path(const std::string& id,
                         const std::unordered_map<std::string, ProjectRecord>& projects) {
    std::string path;
    std::unordered_set<std::string> visited;
    auto current = id;
    while (!current.empty() && visited.insert(current).second) {
        const auto found = projects.find(current);
        if (found == projects.end()) break;
        const auto name = found->second.display_name.empty() ? "Unnamed project" : found->second.display_name;
        path = name + (path.empty() ? "" : " / " + path);
        current = found->second.parent_id;
    }
    return path;
}

void append_token(std::string& expression, const std::string& token) {
    if (!expression.empty()) expression += ' ';
    // Keep the field outside the quotes: project:"Client Work".
    if (token.starts_with("project:")) expression += "project:" + quote_filter_token(token.substr(8));
    else expression += quote_filter_token(token);
}
}  // namespace

ProjectFilterLabels::ProjectFilterLabels(const std::unordered_map<std::string, ProjectRecord>& projects) {
    std::vector<std::pair<std::string, std::string>> paths;
    for (const auto& [id, project] : projects) paths.emplace_back(project_path(id, projects), id);
    std::sort(paths.begin(), paths.end());
    for (const auto& [path, id] : paths) add_label(id, path);
}

std::string ProjectFilterLabels::add_label(const std::string& id, const std::string& name) {
    auto label = name;
    int suffix = 2;
    while (ids_.contains(normalized(label))) label = name + " (" + std::to_string(suffix++) + ")";
    labels_[id] = label;
    ids_[normalized(label)] = id;
    return label;
}

std::string ProjectFilterLabels::display_expression(const std::string& expression) {
    std::string result;
    for (auto token : filter_token_values(expression)) {
        if (token.starts_with("project:")) {
            const auto id = token.substr(8);
            const auto found = labels_.find(id);
            if (found != labels_.end()) token = "project:" + found->second;
            else if (!QUuid(QString::fromStdString(id)).isNull()) token = "project:" + add_label(id, "Missing project");
        }
        append_token(result, token);
    }
    return result;
}

std::string ProjectFilterLabels::canonical_expression(const std::string& expression) const {
    std::string result;
    for (auto token : filter_token_values(expression)) {
        if (token.starts_with("project:")) {
            const auto found = ids_.find(normalized(token.substr(8)));
            if (found != ids_.end()) token = "project:" + found->second;
        }
        append_token(result, token);
    }
    return result;
}

}  // namespace todobench
