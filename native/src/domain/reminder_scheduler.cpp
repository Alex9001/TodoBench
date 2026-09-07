// SPDX-License-Identifier: GPL-3.0-or-later
#include "domain/reminder_scheduler.h"

#include <algorithm>

#include <vector>

namespace todobench {

ReminderScheduler::ReminderScheduler(Deliver deliver) : deliver_(std::move(deliver)) {}

void ReminderScheduler::replace_schedule(std::vector<ScheduledReminder> reminders) {
    reminders_ = std::move(reminders);
    std::sort(reminders_.begin(), reminders_.end(), [](const ScheduledReminder& left, const ScheduledReminder& right) {
        return left.fire_at < right.fire_at;
    });
    const auto is_active = [this](const std::string& key) {
        return std::any_of(reminders_.begin(), reminders_.end(), [&key](const ScheduledReminder& reminder) {
            return reminder_delivery_key(reminder) == key;
        });
    };
    for (auto iterator = delivered_keys_.begin(); iterator != delivered_keys_.end();) {
        if (!is_active(*iterator)) iterator = delivered_keys_.erase(iterator);
        else ++iterator;
    }
    for (auto iterator = snoozed_until_.begin(); iterator != snoozed_until_.end();) {
        if (!is_active(iterator->first)) iterator = snoozed_until_.erase(iterator);
        else ++iterator;
    }
}

void ReminderScheduler::restore_delivered(const std::vector<std::string>& keys) {
    delivered_keys_.insert(keys.begin(), keys.end());
}

std::vector<std::string> ReminderScheduler::delivered_keys() const {
    return {delivered_keys_.begin(), delivered_keys_.end()};
}

void ReminderScheduler::mark_delivered(const ScheduledReminder& reminder) {
    delivered_keys_.insert(reminder_delivery_key(reminder));
    snoozed_until_.erase(reminder_delivery_key(reminder));
}

bool ReminderScheduler::snooze(const ScheduledReminder& reminder, const QDateTime& until) {
    if (!until.isValid() || delivered_keys_.contains(reminder_delivery_key(reminder))) return false;
    snoozed_until_[reminder_delivery_key(reminder)] = until;
    return true;
}

bool ReminderScheduler::is_delivered(const ScheduledReminder& reminder) const {
    return delivered_keys_.contains(reminder_delivery_key(reminder));
}

bool ReminderScheduler::is_due(const ScheduledReminder& reminder, const QDateTime& now) const {
    if (is_delivered(reminder)) return false;
    const auto* snoozed = snoozed_until(reminder);
    return (snoozed == nullptr ? reminder.fire_at : *snoozed) <= now;
}

const QDateTime* ReminderScheduler::snoozed_until(const ScheduledReminder& reminder) const {
    const auto found = snoozed_until_.find(reminder_delivery_key(reminder));
    return found == snoozed_until_.end() ? nullptr : &found->second;
}

void ReminderScheduler::restore_snoozed(const std::unordered_map<std::string, QDateTime>& values) {
    snoozed_until_ = values;
}

std::unordered_map<std::string, QDateTime> ReminderScheduler::snoozed_values() const {
    return snoozed_until_;
}

size_t ReminderScheduler::deliver_due(const QDateTime& now) {
    size_t delivered = 0;
    for (const auto& reminder : reminders_) {
        const auto key = reminder_delivery_key(reminder);
        const auto snoozed = snoozed_until_.find(key);
        const auto effective_fire = snoozed == snoozed_until_.end() ? reminder.fire_at : snoozed->second;
        if (!effective_fire.isValid() || effective_fire > now || delivered_keys_.contains(key)) continue;
        if (deliver_ && deliver_(reminder)) {
            mark_delivered(reminder);
            ++delivered;
        }
    }
    return delivered;
}

MissedReminderSummary ReminderScheduler::missed_summary(const QDateTime& now) const {
    std::vector<ScheduledReminder> pending;
    for (const auto& reminder : reminders_) {
        if (delivered_keys_.contains(reminder_delivery_key(reminder))) continue;
        const auto snoozed = snoozed_until_.find(reminder_delivery_key(reminder));
        if (snoozed != snoozed_until_.end()) {
            auto deferred = reminder;
            deferred.fire_at = snoozed->second;
            pending.push_back(std::move(deferred));
        } else {
            pending.push_back(reminder);
        }
    }
    return summarize_missed_reminders(pending, now);
}

}  // namespace todobench
