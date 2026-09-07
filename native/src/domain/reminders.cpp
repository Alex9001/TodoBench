// SPDX-License-Identifier: GPL-3.0-or-later
#include "domain/reminders.h"

#include <algorithm>

namespace todobench {

std::vector<ScheduledReminder> schedule_reminders(const std::string& task_id,
                                                   const std::string& occurrence_id,
                                                   const QDateTime& due_at,
                                                   const std::vector<ReminderOffset>& reminders) {
    std::vector<ScheduledReminder> scheduled;
    if (!due_at.isValid()) return scheduled;
    for (const auto& reminder : reminders) {
        if (reminder.id.empty() || reminder.minutes_before < 0) continue;
        scheduled.push_back({task_id, occurrence_id, reminder.id, due_at,
                             due_at.addSecs(-reminder.minutes_before * 60)});
    }
    std::sort(scheduled.begin(), scheduled.end(), [](const ScheduledReminder& left, const ScheduledReminder& right) {
        if (left.fire_at != right.fire_at) return left.fire_at < right.fire_at;
        return left.reminder_id < right.reminder_id;
    });
    return scheduled;
}

std::string reminder_delivery_key(const ScheduledReminder& reminder) {
    return reminder.task_id + ":" + reminder.occurrence_id + ":" + reminder.reminder_id;
}

std::vector<ScheduledReminder> undelivered_reminders(const std::vector<ScheduledReminder>& reminders,
                                                     const std::unordered_set<std::string>& delivered_keys) {
    std::vector<ScheduledReminder> pending;
    for (const auto& reminder : reminders) {
        if (!delivered_keys.contains(reminder_delivery_key(reminder))) pending.push_back(reminder);
    }
    return pending;
}

MissedReminderSummary summarize_missed_reminders(const std::vector<ScheduledReminder>& reminders,
                                                 const QDateTime& now) {
    MissedReminderSummary summary;
    for (const auto& reminder : reminders) {
        if (!reminder.fire_at.isValid() || reminder.fire_at > now) continue;
        if (summary.count == 0) summary.earliest = reminder.fire_at;
        summary.latest = reminder.fire_at;
        ++summary.count;
    }
    return summary;
}

}  // namespace todobench
