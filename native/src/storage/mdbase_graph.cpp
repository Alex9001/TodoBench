// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/mdbase_graph.h"
#include "storage/directory_names.h"

#include "storage/mdbase_import.h"
#include "domain/model.h"

#include <QCryptographicHash>
#include <QByteArray>

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace todobench::mdbase_transfer {
namespace {
std::string deterministic_uuid(const std::string& seed){
    auto hex = QCryptographicHash::hash(QByteArray::fromStdString(seed), QCryptographicHash::Sha256).toHex().toStdString();
    std::string h = hex.substr(0,32);
    std::string uuid = h.substr(0,8)+"-"+h.substr(8,4)+"-"+h.substr(12,4)+"-"+h.substr(16,4)+"-"+h.substr(20,12);
    // Force UUID v4 / RFC4122 variant so is_valid_uuid accepts deterministic ids (constraint requires [1-5] and [89ab])
    if(uuid.size()==36){
        uuid[14]='4';
        char c = uuid[19];
        // map to 8..b variant (8,9,a,b)
        int v = 0;
        if(c>='0' && c<='9') v = c - '0';
        else if(c>='a' && c<='f') v = 10 + (c - 'a');
        else if(c>='A' && c<='F') v = 10 + (c - 'A');
        const char variant[4] = {'8','9','a','b'};
        uuid[19] = variant[v % 4];
    }
    return uuid;
}

using RecordIndex = std::unordered_map<std::string, const PreviewRecord*>;
using PathMap = std::unordered_map<std::string, std::string>;

RecordIndex index_records(const TransferPreview& preview) {
    RecordIndex index;
    for (const auto& record : preview.records) index[record.native_id] = &record;
    return index;
}

void validate_record_owner(const PreviewRecord& record, const RecordIndex& index,
                           std::vector<TransferDiagnostic>& diagnostics) {
    if (!record.is_task || record.native_project_choice.empty() ||
        record.native_project_choice.starts_with("label:")) return;
    const auto owner = index.find(record.native_project_choice);
    if (owner == index.end()) {
        diagnostics.push_back({TransferSeverity::Blocking, "project_not_found",
            "mapped project does not exist: " + record.native_project_choice,
            record.source_path, "", record.native_id});
    } else if (owner->second->is_task) {
        diagnostics.push_back({TransferSeverity::Blocking, "project_target_not_project",
            "mapped project refers to a task", record.source_path, "", record.native_id});
    }
}

void validate_record_parent(const PreviewRecord& record, const RecordIndex& index,
                            std::vector<TransferDiagnostic>& diagnostics) {
    if (record.native_parent_choice.empty()) return;
    const auto parent = index.find(record.native_parent_choice);
    if (parent == index.end()) {
        diagnostics.push_back({TransferSeverity::Blocking, "missing_parent",
            "parent does not exist: " + record.native_parent_choice,
            record.source_path, "", record.native_id});
        return;
    }
    if (record.is_task && parent->second->is_task &&
        record.native_project_choice != parent->second->native_project_choice) {
        diagnostics.push_back({TransferSeverity::Blocking, "cross_project_parent",
            "parent belongs to another project", record.source_path, "", record.native_id});
    }
}

bool parent_chain_has_cycle(const std::string& start, const RecordIndex& index) {
    std::unordered_set<std::string> chain;
    auto current = index.find(start);
    while (current != index.end()) {
        if (!chain.insert(current->first).second) return true;
        current = index.find(current->second->native_parent_choice);
    }
    return false;
}

class PathAllocator {
public:
    explicit PathAllocator(TransferPreview& preview)
        : preview_(preview), records_(index_records(preview)) {}

    PathMap allocate() {
        std::vector<PreviewRecord> ordered = preview_.records;
        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.source_path < b.source_path; });
        preview_.generated_project_names.clear();
        for (const auto& record : ordered) {
            if (!record.is_task) project_path(record.native_id);
        }
        create_inbox();
        for (const auto& record : ordered) {
            if (record.is_task) {
                const auto parent = owner_path(record.native_project_choice) + "/tasks";
                destinations_[record.native_id] = parent + "/" + allocate_directory_name(record.native_title, "task", occupied_[parent]) + "/task.md";
            }
        }
        return destinations_;
    }

private:
    std::string project_path(const std::string& id) {
        const auto cached = project_paths_.find(id);
        if (cached != project_paths_.end()) return cached->second;
        const auto found = records_.find(id);
        if (found == records_.end()) return "projects/nobody";
        // Preview allocation also runs for invalid graphs so users can review errors.
        // Break cycles here; validate_import_graph supplies the blocking diagnostic.
        if (!active_projects_.insert(id).second) return "projects/unresolved";
        const auto& record = *found->second;
        std::string parent_path = "projects";
        const auto parent = records_.find(record.native_parent_choice);
        if (parent != records_.end() && !parent->second->is_task) {
            parent_path = project_path(parent->first) + "/projects";
        }
        const auto path = parent_path + "/" + allocate_directory_name(record.native_title, "project", occupied_[parent_path]);
        project_paths_[id] = path;
        destinations_[id] = path + "/project.md";
        active_projects_.erase(id);
        return path;
    }

    void create_inbox() {
        const bool needed = std::any_of(preview_.records.begin(), preview_.records.end(),
            [](const PreviewRecord& record) {
                return record.is_task && record.native_project_choice.empty();
            });
        if (!needed) return;
        const auto id = deterministic_uuid("synthetic:inbox|" + preview_.snapshot_inventory_hash_hex);
        inbox_path_ = "projects/" + allocate_directory_name("Inbox", "project", occupied_["projects"]);
        preview_.generated_project_names[id] = "Inbox";
        project_paths_[id] = inbox_path_;
        destinations_[id] = inbox_path_ + "/project.md";
    }

    std::string label_path(const std::string& owner) {
        const auto label = owner.substr(6);
        const auto existing = label_paths_.find(label);
        if (existing != label_paths_.end()) return existing->second;
        const auto path = "projects/" + allocate_directory_name(label, "project", occupied_["projects"]);
        const auto id = deterministic_uuid("labelproj:" + label);
        preview_.generated_project_names[id] = label;
        label_paths_[label] = path;
        project_paths_[id] = path;
        destinations_[id] = path + "/project.md";
        return path;
    }

    std::string owner_path(const std::string& owner) {
        if (owner.empty()) return inbox_path_;
        if (owner.starts_with("label:")) return label_path(owner);
        const auto project = project_paths_.find(owner);
        if (project != project_paths_.end()) return project->second;
        // Invalid owners are blocked by graph validation; keep review paths deterministic.
        return inbox_path_.empty() ? "projects/unresolved" : inbox_path_;
    }

    TransferPreview& preview_;
    std::map<std::string, std::set<std::string>> occupied_;
    RecordIndex records_;
    PathMap project_paths_;
    PathMap label_paths_;
    PathMap destinations_;
    std::unordered_set<std::string> active_projects_;
    std::string inbox_path_;
};
} // namespace

std::vector<TransferDiagnostic> validate_import_graph(const TransferPreview& preview,
                                                      const ImportMapping& /*mapping*/) {
    std::vector<TransferDiagnostic> diagnostics;
    const auto index = index_records(preview);
    for (const auto& record : preview.records) {
        validate_record_owner(record, index, diagnostics);
        validate_record_parent(record, index, diagnostics);
    }
    for (const auto& record : preview.records) {
        if (parent_chain_has_cycle(record.native_id, index)) {
            diagnostics.push_back({TransferSeverity::Blocking, "task_cycle",
                "task hierarchy contains a cycle", record.source_path, "", record.native_id});
            break;
        }
    }
    return diagnostics;
}

std::unordered_map<std::string, std::string> allocate_native_paths(TransferPreview& preview) {
    return PathAllocator(preview).allocate();
}

} // namespace todobench::mdbase_transfer
