// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace todobench {

struct KeyBinding {
    std::string command_id;
    std::string shortcut;
    std::string context;
};

struct BindingConflict {
    std::string shortcut;
    std::string context;
    std::vector<std::string> command_ids;
};

std::vector<BindingConflict> find_binding_conflicts(const std::vector<KeyBinding>& bindings);
std::vector<KeyBinding> browser_bindings();
std::vector<KeyBinding> total_commander_bindings();

}  // namespace todobench
