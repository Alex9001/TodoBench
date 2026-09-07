// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "storage/settings_codec.h"

#include <string>
#include <unordered_map>

class QWidget;

namespace todobench {

bool edit_settings(QWidget* parent, Settings& settings,
                   const std::unordered_map<std::string, std::string>& project_names, bool appearance = false);

}  // namespace todobench
