// SPDX-License-Identifier: GPL-3.0-or-later
#include "domain/model.h"

#include <regex>

namespace todobench {

std::string to_string(TaskStatus status) {
    switch (status) {
    case TaskStatus::Todo: return "todo";
    case TaskStatus::InProgress: return "in_progress";
    case TaskStatus::Waiting: return "waiting";
    case TaskStatus::Done: return "done";
    case TaskStatus::Cancelled: return "cancelled";
    }
    return "todo";
}

std::string to_string(Priority priority) {
    switch (priority) {
    case Priority::None: return "none";
    case Priority::Low: return "low";
    case Priority::Normal: return "normal";
    case Priority::High: return "high";
    case Priority::Urgent: return "urgent";
    }
    return "normal";
}

bool parse_task_status(const std::string& value, TaskStatus& status) {
    if (value == "todo") status = TaskStatus::Todo;
    else if (value == "in_progress") status = TaskStatus::InProgress;
    else if (value == "waiting") status = TaskStatus::Waiting;
    else if (value == "done") status = TaskStatus::Done;
    else if (value == "cancelled") status = TaskStatus::Cancelled;
    else return false;
    return true;
}

bool parse_priority(const std::string& value, Priority& priority) {
    if (value == "none") priority = Priority::None;
    else if (value == "low") priority = Priority::Low;
    else if (value == "normal") priority = Priority::Normal;
    else if (value == "high") priority = Priority::High;
    else if (value == "urgent") priority = Priority::Urgent;
    else return false;
    return true;
}

bool is_valid_uuid(const std::string& value) {
    static const std::regex pattern(
        R"(^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$)");
    return std::regex_match(value, pattern);
}

}  // namespace todobench
