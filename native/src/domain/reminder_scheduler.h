// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "domain/reminders.h"

#include <QDateTime>

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace todobench {

class ReminderScheduler final {
public:
    using Deliver = std::function<bool(const ScheduledReminder&)>;

    explicit ReminderScheduler(Deliver deliver);

    void replace_schedule(std::vector<ScheduledReminder> reminders);
    size_t deliver_due(const QDateTime& now);
    bool snooze(const ScheduledReminder& reminder, const QDateTime& until);
    bool is_delivered(const ScheduledReminder& reminder) const;
    bool is_due(const ScheduledReminder& reminder, const QDateTime& now) const;
    const QDateTime* snoozed_until(const ScheduledReminder& reminder) const;
    void restore_snoozed(const std::unordered_map<std::string, QDateTime>& values);
    std::unordered_map<std::string, QDateTime> snoozed_values() const;
    MissedReminderSummary missed_summary(const QDateTime& now) const;
    void mark_delivered(const ScheduledReminder& reminder);
    void restore_delivered(const std::vector<std::string>& keys);
    std::vector<std::string> delivered_keys() const;
    const std::vector<ScheduledReminder>& schedule() const { return reminders_; }

private:
    Deliver deliver_;
    std::vector<ScheduledReminder> reminders_;
    std::unordered_set<std::string> delivered_keys_;
    std::unordered_map<std::string, QDateTime> snoozed_until_;
};

}  // namespace todobench
