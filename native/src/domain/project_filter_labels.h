// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "domain/model.h"

namespace todobench {

// The editor uses names; persisted filters continue to use stable project IDs.
class ProjectFilterLabels final {
public:
    explicit ProjectFilterLabels(const std::unordered_map<std::string, ProjectRecord>& projects = {});
    std::string display_expression(const std::string& expression);
    std::string canonical_expression(const std::string& expression) const;

private:
    std::string add_label(const std::string& id, const std::string& name);
    std::unordered_map<std::string, std::string> labels_;
    std::unordered_map<std::string, std::string> ids_;
};

}  // namespace todobench
