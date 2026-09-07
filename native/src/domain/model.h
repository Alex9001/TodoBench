// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace todobench {

enum class TaskStatus { Todo, InProgress, Waiting, Done, Cancelled };
enum class Priority { None, Low, Normal, High, Urgent };

std::string to_string(TaskStatus status);
std::string to_string(Priority priority);
bool parse_task_status(const std::string& value, TaskStatus& status);
bool parse_priority(const std::string& value, Priority& priority);

struct TaskRecord {
    int schema_version{1};
    std::string id;
    std::string title;
    std::string project_id;
    std::string parent_id;
    TaskStatus status{TaskStatus::Todo};
    TaskStatus previous_open_status{TaskStatus::Todo};
    Priority priority{Priority::Normal};
    std::vector<std::string> tags;
    std::string due_yaml;
    std::string recurrence_yaml;
    std::string reminders_yaml{"[]"};
    long long order{1024};
    std::string created_at;
    std::string updated_at;
    std::string completed_at;
    std::string revision;
    std::string body;
    std::unordered_map<std::string, std::string> unknown_fields;
    std::string source_hash;
    std::string source_path;
};

struct ProjectRecord {
    int schema_version{1};
    std::string id;
    std::string parent_id;
    std::string display_name;
    long long order{1024};
    bool archived{false};
    std::string body;
    std::string source_hash;
    std::unordered_map<std::string, std::string> unknown_fields;
    std::string source_path;
};

struct Diagnostic {
    enum class Severity { Warning, Error };
    Severity severity{Severity::Error};
    std::string path;
    std::string message;
};

struct WorkspaceSnapshot {
    std::string root_path;
    std::unordered_map<std::string, ProjectRecord> projects;
    std::unordered_map<std::string, TaskRecord> tasks;
    std::vector<Diagnostic> diagnostics;
};

bool is_valid_uuid(const std::string& value);

}  // namespace todobench
