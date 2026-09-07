// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "domain/model.h"

#include <QDate>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace todobench {

enum class TagMatch { Any, All };

struct FilterSpec {
    std::string title_query;
    std::vector<std::string> title_terms;
    std::vector<std::string> tags;
    TagMatch tag_match{TagMatch::Any};
    std::vector<std::string> project_ids;
    std::vector<TaskStatus> statuses;
    std::vector<Priority> priorities;
    std::optional<QDate> due_from;
    std::optional<QDate> due_to;
    bool include_subprojects{true};
};

struct FilterCompileResult {
    FilterSpec spec;
    std::string error;
};

FilterCompileResult compile_filter(const std::string& expression);
std::vector<std::string> filter_tokens(const std::string& expression);
std::vector<std::string> filter_token_values(const std::string& expression);
std::string quote_filter_token(const std::string& token);
bool matches_filter(const TaskRecord& task, const FilterSpec& spec);
bool matches_filter(const TaskRecord& task, const FilterSpec& spec,
                    const std::unordered_map<std::string, ProjectRecord>& projects);

}  // namespace todobench
