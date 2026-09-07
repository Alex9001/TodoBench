// SPDX-License-Identifier: GPL-3.0-or-later
#include "domain/recurrence.h"

#include <QTimeZone>

#include <algorithm>

namespace todobench {
namespace {

QDateTime add_units(const QDateTime& value, RecurrenceUnit unit, int interval) {
    switch (unit) {
    case RecurrenceUnit::Days: return value.addDays(interval);
    case RecurrenceUnit::Weeks: return value.addDays(interval * 7);
    case RecurrenceUnit::Months: return value.addMonths(interval);
    case RecurrenceUnit::Years: return value.addYears(interval);
    }
    return value;
}

QDate clamped_date(int year, int month, int day) {
    const auto valid_month = std::clamp(month, 1, 12);
    const auto last_day = QDate(year, valid_month, 1).daysInMonth();
    return {year, valid_month, std::clamp(day, 1, last_day)};
}

QDateTime fixed_next(const RecurrenceRule& rule, const QDateTime& anchor) {
    if (rule.unit == RecurrenceUnit::Weeks && !rule.weekdays.empty()) {
        const auto anchor_week = anchor.date().addDays(1 - anchor.date().dayOfWeek());
        auto candidate = anchor.addDays(1);
        for (int guard = 0; guard < 3660; ++guard) {
            const auto week = anchor_week.daysTo(candidate.date()) / 7;
            const auto selected_week = week % rule.interval == 0;
            const auto selected_day = rule.weekdays.contains(static_cast<Qt::DayOfWeek>(candidate.date().dayOfWeek()));
            if (selected_week && selected_day) return candidate;
            candidate = candidate.addDays(1);
        }
        return {};
    }
    if (rule.unit == RecurrenceUnit::Months && rule.month_day > 0) {
        const auto next = anchor.addMonths(rule.interval);
        return QDateTime(clamped_date(next.date().year(), next.date().month(), rule.month_day), anchor.time(), anchor.timeZone());
    }
    if (rule.unit == RecurrenceUnit::Years && rule.month > 0 && rule.month_day > 0) {
        const auto year = anchor.date().year() + rule.interval;
        return QDateTime(clamped_date(year, rule.month, rule.month_day), anchor.time(), anchor.timeZone());
    }
    return add_units(anchor, rule.unit, rule.interval);
}

}  // namespace

QDateTime skip_missed_fixed(const RecurrenceRule& rule, QDateTime candidate, const QDateTime& completion) {
    if (!candidate.isValid()) return {};
    const auto horizon = completion.isValid() ? completion : candidate;
    int guard = 0;
    while (candidate.isValid() && candidate.date() <= horizon.date() && guard < 4096) {
        candidate = fixed_next(rule, candidate);
        ++guard;
    }
    return candidate;
}

QDateTime next_occurrence(const RecurrenceRule& rule, const QDateTime& anchor, const QDateTime& completion) {
    if (!rule.enabled || rule.interval < 1) return {};
    if (rule.mode == RecurrenceMode::AfterCompletion) {
        const auto base = completion.isValid() ? completion : anchor;
        return base.isValid() ? add_units(base, rule.unit, rule.interval) : QDateTime{};
    }
    if (!anchor.isValid()) return {};
    const auto candidate = fixed_next(rule, anchor);
    if (!completion.isValid()) return candidate;
    return skip_missed_fixed(rule, candidate, completion);
}

}  // namespace todobench
