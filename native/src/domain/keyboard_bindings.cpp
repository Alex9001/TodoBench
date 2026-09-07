// SPDX-License-Identifier: GPL-3.0-or-later
#include "domain/keyboard_bindings.h"

#include <algorithm>
#include <map>

namespace todobench {
namespace {

bool contexts_overlap(const std::string& left, const std::string& right) {
    return left == "global" || right == "global" || left == right;
}

}  // namespace

std::vector<BindingConflict> find_binding_conflicts(const std::vector<KeyBinding>& bindings) {
    std::vector<BindingConflict> conflicts;
    for (size_t index = 0; index < bindings.size(); ++index) {
        for (size_t other = index + 1; other < bindings.size(); ++other) {
            const auto& left = bindings[index];
            const auto& right = bindings[other];
            if (left.shortcut != right.shortcut || !contexts_overlap(left.context, right.context)) continue;
            auto found = std::find_if(conflicts.begin(), conflicts.end(), [&left](const BindingConflict& conflict) {
                return conflict.shortcut == left.shortcut && conflict.context == left.context;
            });
            if (found == conflicts.end()) {
                conflicts.push_back({left.shortcut, left.context, {left.command_id, right.command_id}});
            } else if (std::find(found->command_ids.begin(), found->command_ids.end(), right.command_id) == found->command_ids.end()) {
                found->command_ids.push_back(right.command_id);
            }
        }
    }
    return conflicts;
}

std::vector<KeyBinding> browser_bindings() {
    return {{"task.create", "Ctrl+N", "task_list"},
            {"task.new_subtask", "Ctrl+Alt+N", "task_list"},
            {"task.duplicate", "Ctrl+D", "task_list"},
            {"task.move", "Ctrl+Shift+M", "task_list"},
            {"task.trash", "Delete", "task_list"},
            {"view.open", "Ctrl+T", "global"},
            {"view.close", "Ctrl+W", "global"},
            {"task.complete", "Space", "task_list"},
            {"workspace.refresh", "F5", "global"}};
}

std::vector<KeyBinding> total_commander_bindings() {
    return {{"task.create", "Shift+F4", "task_list"},
            {"task.new_subtask", "Ctrl+Alt+N", "task_list"},
            {"task.duplicate", "F5", "task_list"},
            {"task.move", "F6", "task_list"},
            {"task.trash", "F8", "task_list"},
            {"view.open", "Ctrl+T", "global"},
            {"view.close", "Ctrl+W", "global"},
            {"task.complete", "Space", "task_list"},
            {"workspace.refresh", "F2", "global"}};
}

}  // namespace todobench
