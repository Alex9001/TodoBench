// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/front_matter_codec.h"
#include "storage/schedule_validation.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <set>
#include "storage/workspace_store.h"

namespace todobench {
namespace {

struct DocumentParts {
    YAML::Node metadata;
    std::string body;
};

void validate_mapping(const YAML::Node& node, int depth = 0) {
    if (depth > 64) throw std::runtime_error("metadata nesting exceeds 64 levels");
    if (node.IsMap()) {
        std::set<std::string> keys;
        for (const auto& entry : node) {
            if (!entry.first.IsScalar()) throw std::runtime_error("metadata keys must be strings");
            if (!keys.insert(entry.first.as<std::string>()).second)
                throw std::runtime_error("duplicate metadata key: " + entry.first.as<std::string>());
            validate_mapping(entry.second, depth + 1);
        }
    } else if (node.IsSequence()) {
        for (const auto& entry : node) validate_mapping(entry, depth + 1);
    }
}

std::variant<DocumentParts, CodecError> split_document(const std::string& content) {
    const size_t start = content.starts_with("\xef\xbb\xbf") ? 3 : 0;
    const auto first_end = content.find('\n', start);
    const auto delimiter = content.substr(start, first_end - start);
    if (delimiter != "---" && delimiter != "---\r")
        return CodecError{"missing YAML front matter delimiter"};
    if (first_end == std::string::npos) return CodecError{"unterminated YAML front matter"};
    auto line = first_end + 1;
    while (line < content.size()) {
        const auto end = content.find('\n', line);
        const auto text = content.substr(line, end - line);
        if (text == "---" || text == "---\r") {
            auto metadata = YAML::Load(content.substr(first_end + 1, line - first_end - 1));
            if (!metadata.IsMap()) return CodecError{"front matter must be a mapping"};
            validate_mapping(metadata);
            return DocumentParts{metadata, end == std::string::npos ? "" : content.substr(end + 1)};
        }
        if (end == std::string::npos) break;
        line = end + 1;
    }
    return CodecError{"unterminated YAML front matter"};
}

std::string scalar(const YAML::Node& node, const std::string& key, const std::string& fallback = {}) {
    if (!node[key] || node[key].IsNull()) return fallback;
    return node[key].as<std::string>();
}

bool boolean(const YAML::Node& node, const std::string& key, bool fallback = false) {
    return node[key] ? node[key].as<bool>() : fallback;
}

void collect_unknown(const YAML::Node& node, const std::vector<std::string>& known,
                     std::unordered_map<std::string, std::string>& output) {
    if (!node.IsMap()) return;
    for (const auto& entry : node) {
        const auto key = entry.first.as<std::string>();
        if (std::find(known.begin(), known.end(), key) == known.end()) {
            output[key] = YAML::Dump(entry.second);
        }
    }
}

YAML::Node parse_optional_node(const std::string& value) {
    try { return YAML::Load(value); } catch (const YAML::Exception&) { return YAML::Node(); }
}

std::string dump_document(const YAML::Node& metadata, const std::string& body) {
    std::ostringstream output;
    output << "---\n" << metadata << "\n---\n" << body;
    return output.str();
}

}  // namespace

TaskParseResult parse_task_markdown(const std::string& path, const std::string& content) try {
    auto split = split_document(content);
    if (std::holds_alternative<CodecError>(split)) return std::get<CodecError>(split);
    const auto& parts = std::get<DocumentParts>(split);
    const auto& metadata = parts.metadata;
    if (scalar(metadata, "kind") != "task") return CodecError{"front matter kind is not task"};
    validate_schedule(metadata);
    TaskRecord task;
    task.schema_version = metadata["schema_version"] ? metadata["schema_version"].as<int>() : 1;
    if (task.schema_version != 1) return CodecError{"task schema version is newer than this application"};
    task.id = scalar(metadata, "id");
    task.title = scalar(metadata, "title");
    task.parent_id = scalar(metadata, "parent_id");
    if (!parse_task_status(scalar(metadata, "status", "todo"), task.status)) return CodecError{"invalid task status"};
    if (!parse_task_status(scalar(metadata, "previous_open_status", "todo"), task.previous_open_status)) return CodecError{"invalid previous_open_status"};
    if (!parse_priority(scalar(metadata, "priority", "normal"), task.priority)) return CodecError{"invalid priority"};
    if (metadata["tags"] && !metadata["tags"].IsSequence()) return CodecError{"tags must be a sequence"};
    if (metadata["tags"]) {
        for (const auto& tag : metadata["tags"]) task.tags.push_back(tag.as<std::string>());
    }
    task.due_yaml = scalar(metadata, "due", "null");
    task.recurrence_yaml = metadata["recurrence"] ? YAML::Dump(metadata["recurrence"]) : "null";
    task.reminders_yaml = metadata["reminders"] ? YAML::Dump(metadata["reminders"]) : "[]";
    task.order = metadata["order"] ? metadata["order"].as<long long>() : 1024;
    task.created_at = scalar(metadata, "created_at");
    task.updated_at = scalar(metadata, "updated_at");
    task.completed_at = scalar(metadata, "completed_at");
    task.revision = scalar(metadata, "revision");
    task.body = parts.body;
    task.source_path = path;
    task.source_hash = WorkspaceStore::hash_bytes(content);
    collect_unknown(metadata, {"schema_version", "kind", "id", "title", "parent_id", "status", "previous_open_status", "priority", "tags", "due", "recurrence", "reminders", "order", "created_at", "updated_at", "completed_at", "revision"}, task.unknown_fields);
    return task;
} catch (const std::exception& error) {
    return CodecError{error.what()};
}

ProjectParseResult parse_project_markdown(const std::string& path, const std::string& content) try {
    auto split = split_document(content);
    if (std::holds_alternative<CodecError>(split)) return std::get<CodecError>(split);
    const auto& parts = std::get<DocumentParts>(split);
    const auto& metadata = parts.metadata;
    if (scalar(metadata, "kind") != "project") return CodecError{"front matter kind is not project"};
    ProjectRecord project;
    project.schema_version = metadata["schema_version"] ? metadata["schema_version"].as<int>() : 1;
    if (project.schema_version != 1) return CodecError{"project schema version is newer than this application"};
    project.id = scalar(metadata, "id");
    project.parent_id = scalar(metadata, "parent_id");
    project.display_name = scalar(metadata, "display_name");
    project.order = metadata["order"] ? metadata["order"].as<long long>() : 1024;
    project.archived = boolean(metadata, "archived");
    project.body = parts.body;
    project.source_path = path;
    project.source_hash = WorkspaceStore::hash_bytes(content);
    collect_unknown(metadata, {"schema_version", "kind", "id", "parent_id", "display_name", "order", "archived"}, project.unknown_fields);
    return project;
} catch (const std::exception& error) {
    return CodecError{error.what()};
}

std::string serialize_task_markdown(const TaskRecord& task) {
    YAML::Node metadata;
    metadata["schema_version"] = task.schema_version;
    metadata["kind"] = "task";
    metadata["id"] = task.id;
    metadata["title"] = task.title;
    metadata["parent_id"] = task.parent_id.empty() ? YAML::Node() : YAML::Node(task.parent_id);
    metadata["status"] = to_string(task.status);
    metadata["previous_open_status"] = to_string(task.previous_open_status);
    metadata["priority"] = to_string(task.priority);
    metadata["tags"] = YAML::Node(YAML::NodeType::Sequence);
    for (const auto& tag : task.tags) metadata["tags"].push_back(tag);
    metadata["due"] = parse_optional_node(task.due_yaml);
    metadata["recurrence"] = parse_optional_node(task.recurrence_yaml);
    metadata["reminders"] = parse_optional_node(task.reminders_yaml);
    metadata["order"] = task.order;
    metadata["created_at"] = task.created_at;
    metadata["updated_at"] = task.updated_at;
    metadata["completed_at"] = task.completed_at.empty() ? YAML::Node() : YAML::Node(task.completed_at);
    metadata["revision"] = task.revision;
    for (const auto& [key, value] : task.unknown_fields) metadata[key] = parse_optional_node(value);
    return dump_document(metadata, task.body);
}

std::string serialize_project_markdown(const ProjectRecord& project) {
    YAML::Node metadata;
    metadata["schema_version"] = project.schema_version;
    metadata["kind"] = "project";
    metadata["id"] = project.id;
    metadata["parent_id"] = project.parent_id.empty() ? YAML::Node() : YAML::Node(project.parent_id);
    metadata["display_name"] = project.display_name;
    metadata["order"] = project.order;
    metadata["archived"] = project.archived;
    for (const auto& [key, value] : project.unknown_fields) metadata[key] = parse_optional_node(value);
    return dump_document(metadata, project.body);
}

}  // namespace todobench
