// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <yaml-cpp/yaml.h>
namespace todobench {
void validate_schedule(const YAML::Node& metadata);
}
