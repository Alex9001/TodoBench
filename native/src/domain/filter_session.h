// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "domain/filter.h"

#include <string>
#include <vector>

namespace todobench {

class FilterSession final {
public:
    bool update(const std::string& expression);
    bool remove_token(size_t index);
    void clear();

    std::vector<std::string> tokens() const;

    const FilterSpec& active_filter() const { return active_filter_; }
    const std::string& expression() const { return expression_; }
    const std::string& error() const { return error_; }

private:
    std::vector<std::string> tokens_;
    FilterSpec active_filter_;
    std::string expression_;
    std::string error_;
};

}  // namespace todobench
