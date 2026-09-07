// SPDX-License-Identifier: GPL-3.0-or-later
#include "domain/task_sort.h"

#include <algorithm>
#include <cctype>
#include <tuple>

namespace todobench {
namespace {

int priority_rank(Priority priority) {
    switch (priority) {
    case Priority::Urgent: return 0;
    case Priority::High: return 1;
    case Priority::Normal: return 2;
    case Priority::Low: return 3;
    case Priority::None: return 4;
    }
    return 4;
}

std::string normalized_due(const TaskRecord& task) {
    if (task.due_yaml.empty() || task.due_yaml == "null") return "9999-12-31";
    return task.due_yaml;
}

}  // namespace

std::string to_string(TaskSort sort) {
    switch (sort) {
    case TaskSort::Manual: return "manual";
    case TaskSort::Title: return "title";
    case TaskSort::Priority: return "priority";
    case TaskSort::Due: return "due";
    case TaskSort::Created: return "created";
    case TaskSort::Updated: return "updated";
    }
    return "manual";
}

bool parse_task_sort(const std::string& value, TaskSort& sort) {
    if (value == "manual") sort = TaskSort::Manual;
    else if (value == "title") sort = TaskSort::Title;
    else if (value == "priority") sort = TaskSort::Priority;
    else if (value == "due") sort = TaskSort::Due;
    else if (value == "created") sort = TaskSort::Created;
    else if (value == "updated") sort = TaskSort::Updated;
    else return false;
    return true;
}

std::vector<TaskRecord> sort_tasks(std::vector<TaskRecord> tasks, TaskSort sort) {
    std::stable_sort(tasks.begin(), tasks.end(), [sort](const TaskRecord& left, const TaskRecord& right) {
        switch (sort) {
        case TaskSort::Manual: return std::make_tuple(left.order, left.id) < std::make_tuple(right.order, right.id);
        case TaskSort::Title: return std::make_tuple(left.title, left.id) < std::make_tuple(right.title, right.id);
        case TaskSort::Priority: return std::make_tuple(priority_rank(left.priority), left.id) < std::make_tuple(priority_rank(right.priority), right.id);
        case TaskSort::Due: return std::make_tuple(normalized_due(left), left.id) < std::make_tuple(normalized_due(right), right.id);
        case TaskSort::Created: return std::make_tuple(left.created_at, left.id) < std::make_tuple(right.created_at, right.id);
        case TaskSort::Updated: return std::make_tuple(left.updated_at, left.id) < std::make_tuple(right.updated_at, right.id);
        }
        return left.id < right.id;
    });
    return tasks;
}

}  // namespace todobench
