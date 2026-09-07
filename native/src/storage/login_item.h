// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace todobench {

bool login_item_enabled();
bool set_login_item_enabled(bool enabled, std::string& error);

}  // namespace todobench
