// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>

#include <set>

namespace todobench {

enum class RecurrenceMode { FixedCalendar, AfterCompletion };
enum class RecurrenceUnit { Days, Weeks, Months, Years };

struct RecurrenceRule {
    bool enabled{false};
    RecurrenceMode mode{RecurrenceMode::FixedCalendar};
    int interval{1};
    RecurrenceUnit unit{RecurrenceUnit::Days};
    std::set<Qt::DayOfWeek> weekdays;
    int month_day{0};
    int month{0};
    bool reset_checklist{true};
};

QDateTime next_occurrence(const RecurrenceRule& rule, const QDateTime& anchor, const QDateTime& completion);

}  // namespace todobench
