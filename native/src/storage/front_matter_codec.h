// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "domain/model.h"

#include <string>
#include <variant>

namespace todobench {

struct CodecError {
    std::string message;
};

using TaskParseResult = std::variant<TaskRecord, CodecError>;
using ProjectParseResult = std::variant<ProjectRecord, CodecError>;

TaskParseResult parse_task_markdown(const std::string& path, const std::string& content);
ProjectParseResult parse_project_markdown(const std::string& path, const std::string& content);
std::string serialize_task_markdown(const TaskRecord& task);
std::string serialize_project_markdown(const ProjectRecord& project);

}  // namespace todobench
