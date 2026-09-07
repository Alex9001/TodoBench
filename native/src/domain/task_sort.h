// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "domain/model.h"

#include <string>
#include <vector>

namespace todobench {

enum class TaskSort { Manual, Title, Priority, Due, Created, Updated };

std::string to_string(TaskSort sort);
bool parse_task_sort(const std::string& value, TaskSort& sort);
std::vector<TaskRecord> sort_tasks(std::vector<TaskRecord> tasks, TaskSort sort);

}  // namespace todobench
