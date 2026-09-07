// SPDX-License-Identifier: GPL-3.0-or-later
#include "domain/filter_session.h"

#include <sstream>

namespace todobench {

bool FilterSession::update(const std::string& expression) {
    const auto compiled = compile_filter(expression);
    if (!compiled.error.empty()) {
        error_ = compiled.error;
        return false;
    }
    tokens_ = filter_tokens(expression);
    active_filter_ = compiled.spec;
    expression_ = expression;
    error_.clear();
    return true;
}

bool FilterSession::remove_token(size_t index) {
    if (index >= tokens_.size()) return false;
    tokens_.erase(tokens_.begin() + static_cast<std::ptrdiff_t>(index));
    std::ostringstream expression;
    for (size_t token_index = 0; token_index < tokens_.size(); ++token_index) {
        if (token_index > 0) expression << ' ';
        expression << tokens_[token_index];
    }
    return update(expression.str());
}

std::vector<std::string> FilterSession::tokens() const { return tokens_; }

void FilterSession::clear() {
    tokens_.clear();
    active_filter_ = {};
    expression_.clear();
    error_.clear();
}

}  // namespace todobench
