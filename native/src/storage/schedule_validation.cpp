// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/schedule_validation.h"
#include <QDate>
#include <QString>
#include <limits>
#include <set>
#include <stdexcept>
namespace todobench {
namespace {
void integer_range(const YAML::Node& node, const char* key, int low, int high) {
    if (!node[key]) return;
    const auto value = node[key].as<long long>();
    if (value < low || value > high) throw std::runtime_error(std::string("invalid ") + key);
}
void choice(const YAML::Node& node, const char* key, const std::set<std::string>& values) {
    if (node[key] && !values.contains(node[key].as<std::string>()))
        throw std::runtime_error(std::string("unsupported ") + key);
}
void recurrence(const YAML::Node& node) {
    if (!node || node.IsNull()) return;
    if (!node.IsMap()) throw std::runtime_error("recurrence must be a mapping or null");
    if (node["enabled"]) node["enabled"].as<bool>();
    if (node["reset_checklist"]) node["reset_checklist"].as<bool>();
    choice(node, "mode", {"fixed_calendar", "after_completion"});
    choice(node, "unit", {"days", "weeks", "months", "years"});
    integer_range(node, "interval", 1, 10000);
    integer_range(node, "month", 0, 12);
    integer_range(node, "month_day", 0, 31);
    if (!node["weekdays"]) return;
    if (!node["weekdays"].IsSequence()) throw std::runtime_error("weekdays must be a sequence");
    for (const auto& day : node["weekdays"]) {
        if (day.as<int>() < 1 || day.as<int>() > 7) throw std::runtime_error("weekday must be 1..7");
    }
}
void reminders(const YAML::Node& node) {
    if (!node) return;
    if (!node.IsSequence()) throw std::runtime_error("reminders must be a sequence");
    std::set<std::string> ids;
    for (const auto& item : node) {
        if (!item.IsMap()) throw std::runtime_error("reminder must be a mapping");
        if (item["id"] && !ids.insert(item["id"].as<std::string>()).second)
            throw std::runtime_error("duplicate reminder id");
        integer_range(item, "minutes_before", 0, std::numeric_limits<int>::max() / 60);
    }
}
}
void validate_schedule(const YAML::Node& metadata) {
    const auto due = metadata["due"];
    if (due && !due.IsNull()) {
        const auto value = QString::fromStdString(due.as<std::string>());
        if (value.size() != 10 || !QDate::fromString(value, Qt::ISODate).isValid())
            throw std::runtime_error("due must be an ISO date (YYYY-MM-DD) or null");
    }
    recurrence(metadata["recurrence"]);
    reminders(metadata["reminders"]);
}
}
