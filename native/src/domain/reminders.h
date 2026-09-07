// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>

#include <string>
#include <unordered_set>
#include <vector>

namespace todobench {

struct ReminderOffset {
    std::string id;
    int minutes_before{0};
};

struct ScheduledReminder {
    std::string task_id;
    std::string occurrence_id;
    std::string reminder_id;
    QDateTime due_at;
    QDateTime fire_at;
};

struct MissedReminderSummary {
    int count{0};
    QDateTime earliest;
    QDateTime latest;
};

std::vector<ScheduledReminder> schedule_reminders(const std::string& task_id,
                                                   const std::string& occurrence_id,
                                                   const QDateTime& due_at,
                                                   const std::vector<ReminderOffset>& reminders);
std::string reminder_delivery_key(const ScheduledReminder& reminder);
std::vector<ScheduledReminder> undelivered_reminders(const std::vector<ScheduledReminder>& reminders,
                                                     const std::unordered_set<std::string>& delivered_keys);
MissedReminderSummary summarize_missed_reminders(const std::vector<ScheduledReminder>& reminders,
                                                 const QDateTime& now);

}  // namespace todobench
