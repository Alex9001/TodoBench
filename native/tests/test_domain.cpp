// SPDX-License-Identifier: GPL-3.0-or-later
#include "domain/filter.h"
#include "domain/filter_session.h"
#include "domain/recurrence.h"
#include "domain/reminders.h"
#include "domain/reminder_scheduler.h"

#include <QDateTime>
#include <QTest>
#include <QTimeZone>

#include <algorithm>
#include <chrono>
#include <unordered_map>

using namespace todobench;

class DomainTest final : public QObject {
    Q_OBJECT
private slots:
    void typedFiltersMatchTasks();
    void invalidFilterRetainsError();
    void filterSessionRetainsLastValidResults();
    void filterSessionRemovesTokens();
    void projectFilterIncludesNestedProjects();
    void projectFilterAcceptsDisplayName();
    void largeFilterIndexStaysResponsive();
    void monthlyRecurrenceClampsMonthEnd();
    void weeklyRecurrenceSkipsMissedSlots();
    void weeklyRecurrenceHonorsIntervalAndSelectedDays();
    void yearlyRecurrenceClampsLeapDay();
    void recurrenceKeepsTimezoneAcrossDst();
    void afterCompletionUsesCompletionTime();
    void remindersScheduleAndDeduplicate();
    void missedRemindersSummarizeWithoutStorms();
    void schedulerDeliversDueItemsOnce();
    void schedulerSupportsSnooze();
    void schedulerMissedSummaryHonorsSnooze();
    void schedulerSnoozeStateRestores();
};

void DomainTest::typedFiltersMatchTasks() {
    TaskRecord task;
    task.title = "Review Release Notes";
    task.project_id = "work";
    task.tags = {"release", "qa"};
    task.status = TaskStatus::Todo;
    task.priority = Priority::High;
    task.due_yaml = "2026-09-10";
    const auto compiled = compile_filter("\"release notes\" tag:qa status:todo priority:high due_to:2026-09-30");
    QVERIFY(compiled.error.empty());
    QVERIFY(matches_filter(task, compiled.spec));
    const auto reordered = compile_filter("notes Review");
    QVERIFY(matches_filter(task, reordered.spec));
}

void DomainTest::invalidFilterRetainsError() {
    const auto compiled = compile_filter("status:unknown");
    QVERIFY(!compiled.error.empty());
}

namespace {
bool session_keeps_last_valid_filter() {
    FilterSession session;
    if (!session.update("status:todo")) return false;
    const auto active = session.active_filter();
    if (session.update("status:not-a-status")) return false;
    if (session.expression() != "status:todo") return false;
    if (session.active_filter().statuses != active.statuses) return false;
    TaskRecord todo;
    todo.title = "Keep visible";
    todo.status = TaskStatus::Todo;
    TaskRecord done;
    done.title = "Hide after valid filter";
    done.status = TaskStatus::Done;
    if (!matches_filter(todo, session.active_filter()) || matches_filter(done, session.active_filter())) return false;
    if (session.error().empty()) return false;
    session.clear();
    return session.expression().empty() && session.error().empty() && session.active_filter().statuses.empty();
}

bool session_removes_tokens() {
    FilterSession session;
    if (!session.update("\"release notes\" status:todo priority:high")) return false;
    if (session.tokens().size() != 3) return false;
    if (!session.remove_token(1)) return false;
    if (session.expression() != "\"release notes\" priority:high") return false;
    if (!session.active_filter().statuses.empty() || session.active_filter().priorities.size() != 1) return false;
    if (!session.remove_token(0)) return false;
    return session.expression() == "priority:high" && session.tokens().size() == 1;
}
}

void DomainTest::filterSessionRetainsLastValidResults() {
    QVERIFY(session_keeps_last_valid_filter());
}

void DomainTest::filterSessionRemovesTokens() {
    QVERIFY(session_removes_tokens());
}

void DomainTest::projectFilterIncludesNestedProjects() {
    ProjectRecord parent;
    parent.id = "parent";
    ProjectRecord child;
    child.id = "child";
    child.parent_id = "parent";
    ProjectRecord grandchild;
    grandchild.id = "grandchild";
    grandchild.parent_id = "child";
    const std::unordered_map<std::string, ProjectRecord> projects{{parent.id, parent}, {child.id, child}, {grandchild.id, grandchild}};

    TaskRecord nested;
    nested.project_id = "grandchild";
    const auto compiled = compile_filter("project:parent");
    QVERIFY(compiled.error.empty());
    QVERIFY(compiled.spec.include_subprojects);
    QVERIFY(matches_filter(nested, compiled.spec, projects));
    auto direct_only = compiled.spec;
    direct_only.include_subprojects = false;
    QVERIFY(!matches_filter(nested, direct_only, projects));
}

void DomainTest::projectFilterAcceptsDisplayName() {
    ProjectRecord project;
    project.id = "123e4567-e89b-12d3-a456-426614174123";
    project.display_name = "Client Work";
    TaskRecord task;
    task.project_id = project.id;
    const auto compiled = compile_filter("project:\"Client Work\"");
    QVERIFY(compiled.error.empty());
    QVERIFY(matches_filter(task, compiled.spec, {{project.id, project}}));
}

void DomainTest::largeFilterIndexStaysResponsive() {
    std::vector<TaskRecord> tasks;
    tasks.reserve(10000);
    for (int index = 0; index < 10000; ++index) {
        TaskRecord task;
        task.id = std::to_string(index);
        task.title = index % 2 == 0 ? "Release review" : "Inbox note";
        tasks.push_back(std::move(task));
    }
    const auto compiled = compile_filter("release review");
    QVERIFY(compiled.error.empty());
    std::vector<long long> samples;
    samples.reserve(20);
    for (int sample = 0; sample < 20; ++sample) {
        const auto started = std::chrono::steady_clock::now();
        size_t matches = 0;
        for (const auto& task : tasks) matches += matches_filter(task, compiled.spec) ? 1U : 0U;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        QCOMPARE(matches, size_t(5000));
        samples.push_back(elapsed);
    }
    std::sort(samples.begin(), samples.end());
    QCOMPARE(samples.size(), size_t(20));
    QVERIFY(samples[19] < 100);
}

void DomainTest::monthlyRecurrenceClampsMonthEnd() {
    RecurrenceRule rule;
    rule.enabled = true;
    rule.unit = RecurrenceUnit::Months;
    rule.month_day = 31;
    const auto next = next_occurrence(rule, QDateTime(QDate(2026, 1, 31), QTime(9, 0), QTimeZone("UTC")), {});
    QCOMPARE(next.date(), QDate(2026, 2, 28));
}

void DomainTest::remindersScheduleAndDeduplicate() {
    const auto due = QDateTime(QDate(2026, 9, 4), QTime(9, 0), QTimeZone("UTC"));
    const auto scheduled = schedule_reminders("task", "occurrence-1", due,
                                              {{"one-hour", 60}, {"at-due", 0}, {"invalid", -1}});
    QCOMPARE(scheduled.size(), size_t(2));
    QCOMPARE(QString::fromStdString(scheduled[0].reminder_id), QString("one-hour"));
    QCOMPARE(scheduled[0].fire_at, due.addSecs(-3600));
    const auto key = reminder_delivery_key(scheduled[0]);
    const auto pending = undelivered_reminders(scheduled, {key});
    QCOMPARE(pending.size(), size_t(1));
    QCOMPARE(QString::fromStdString(pending[0].reminder_id), QString("at-due"));
}

void DomainTest::missedRemindersSummarizeWithoutStorms() {
    const auto due = QDateTime(QDate(2026, 9, 4), QTime(9, 0), QTimeZone("UTC"));
    const auto scheduled = schedule_reminders("task", "occurrence-1", due, {{"early", 120}, {"late", 10}});
    const auto summary = summarize_missed_reminders(scheduled, due);
    QCOMPARE(summary.count, 2);
    QCOMPARE(summary.earliest, due.addSecs(-7200));
    QCOMPARE(summary.latest, due.addSecs(-600));
}

void DomainTest::schedulerDeliversDueItemsOnce() {
    const auto due = QDateTime(QDate(2026, 9, 4), QTime(9, 0), QTimeZone("UTC"));
    const auto reminders = schedule_reminders("task", "occurrence", due, {{"early", 60}, {"at-due", 0}});
    std::vector<std::string> delivered;
    ReminderScheduler scheduler([&delivered](const ScheduledReminder& reminder) {
        delivered.push_back(reminder.reminder_id);
        return true;
    });
    scheduler.replace_schedule(reminders);
    QCOMPARE(scheduler.deliver_due(due.addSecs(-3601)), size_t(0));
    QCOMPARE(scheduler.deliver_due(due), size_t(2));
    QCOMPARE(scheduler.deliver_due(due.addSecs(60)), size_t(0));
    QCOMPARE(delivered.size(), size_t(2));
    QCOMPARE(QString::fromStdString(delivered[0]), QString("early"));
    QCOMPARE(QString::fromStdString(delivered[1]), QString("at-due"));
}

void DomainTest::schedulerSupportsSnooze() {
    const auto due = QDateTime(QDate(2026, 9, 4), QTime(9, 0), QTimeZone("UTC"));
    const auto reminder = schedule_reminders("task", "occurrence", due, {{"one", 0}}).front();
    std::vector<std::string> delivered;
    ReminderScheduler scheduler([&delivered](const ScheduledReminder& value) {
        delivered.push_back(value.reminder_id);
        return true;
    });
    scheduler.replace_schedule({reminder});
    QVERIFY(scheduler.snooze(reminder, due.addSecs(600)));
    QCOMPARE(scheduler.deliver_due(due), size_t(0));
    QCOMPARE(scheduler.deliver_due(due.addSecs(599)), size_t(0));
    QCOMPARE(scheduler.deliver_due(due.addSecs(600)), size_t(1));
    QCOMPARE(scheduler.deliver_due(due.addSecs(601)), size_t(0));
    QCOMPARE(delivered.size(), size_t(1));
}

void DomainTest::schedulerMissedSummaryHonorsSnooze() {
    const auto due = QDateTime(QDate(2026, 9, 4), QTime(9, 0), QTimeZone("UTC"));
    const auto reminder = schedule_reminders("task", "occurrence", due, {{"one", 0}}).front();
    ReminderScheduler scheduler([](const ScheduledReminder&) { return true; });
    scheduler.replace_schedule({reminder});
    const auto snoozed_until = due.addSecs(3600);
    QVERIFY(scheduler.snooze(reminder, snoozed_until));
    QCOMPARE(scheduler.missed_summary(due.addSecs(3599)).count, 0);
    const auto summary = scheduler.missed_summary(snoozed_until);
    QCOMPARE(summary.count, 1);
    QCOMPARE(summary.earliest, snoozed_until);
    QCOMPARE(summary.latest, snoozed_until);
}

void DomainTest::schedulerSnoozeStateRestores() {
    const auto due = QDateTime(QDate(2026, 9, 4), QTime(9, 0), QTimeZone("UTC"));
    const auto reminder = schedule_reminders("task", "occurrence", due, {{"one", 0}}).front();
    ReminderScheduler source([](const ScheduledReminder&) { return true; });
    source.replace_schedule({reminder});
    const auto until = due.addSecs(600);
    QVERIFY(source.snooze(reminder, until));
    ReminderScheduler restored([](const ScheduledReminder&) { return true; });
    restored.replace_schedule({reminder});
    restored.restore_snoozed(source.snoozed_values());
    QVERIFY(restored.snoozed_until(reminder) != nullptr);
    QCOMPARE(*restored.snoozed_until(reminder), until);
    QCOMPARE(restored.deliver_due(due.addSecs(599)), size_t(0));
    QCOMPARE(restored.deliver_due(until), size_t(1));
}

void DomainTest::weeklyRecurrenceSkipsMissedSlots() {
    RecurrenceRule rule;
    rule.enabled = true;
    rule.unit = RecurrenceUnit::Weeks;
    rule.interval = 1;
    const auto due = QDateTime(QDate(2026, 8, 7), QTime(9, 0), QTimeZone("UTC"));
    const auto completion = QDateTime(QDate(2026, 9, 4), QTime(12, 0), QTimeZone("UTC"));
    const auto next = next_occurrence(rule, due, completion);
    QVERIFY(next.date() > completion.date());
    QCOMPARE(next.date().dayOfWeek(), due.date().dayOfWeek());
}

void DomainTest::weeklyRecurrenceHonorsIntervalAndSelectedDays() {
    RecurrenceRule rule;
    rule.enabled = true;
    rule.unit = RecurrenceUnit::Weeks;
    rule.interval = 2;
    rule.weekdays = {Qt::Monday, Qt::Friday};
    const auto friday = QDateTime(QDate(2026, 9, 4), QTime(9, 0), QTimeZone("UTC"));
    const auto next = next_occurrence(rule, friday, {});
    QCOMPARE(next.date(), QDate(2026, 9, 14));
    QCOMPARE(next.date().dayOfWeek(), static_cast<int>(Qt::Monday));
}

void DomainTest::yearlyRecurrenceClampsLeapDay() {
    RecurrenceRule rule;
    rule.enabled = true;
    rule.unit = RecurrenceUnit::Years;
    rule.month = 2;
    rule.month_day = 29;
    const auto next = next_occurrence(rule, QDateTime(QDate(2024, 2, 29), QTime(9, 0), QTimeZone("UTC")), {});
    QCOMPARE(next.date(), QDate(2025, 2, 28));
}

void DomainTest::recurrenceKeepsTimezoneAcrossDst() {
    RecurrenceRule rule;
    rule.enabled = true;
    rule.unit = RecurrenceUnit::Days;
    const QTimeZone new_york("America/New_York");
    QVERIFY(new_york.isValid());
    const auto anchor = QDateTime(QDate(2026, 3, 7), QTime(1, 30), new_york);
    const auto next = next_occurrence(rule, anchor, {});
    QCOMPARE(next.timeZone(), new_york);
    QVERIFY(next.isValid());
    QVERIFY(next > anchor);
}

void DomainTest::afterCompletionUsesCompletionTime() {
    RecurrenceRule rule;
    rule.enabled = true;
    rule.mode = RecurrenceMode::AfterCompletion;
    rule.unit = RecurrenceUnit::Weeks;
    const auto completion = QDateTime(QDate(2026, 9, 4), QTime(12, 0), QTimeZone("UTC"));
    QCOMPARE(next_occurrence(rule, {}, completion), completion.addDays(7));
}

QTEST_MAIN(DomainTest)
#include "test_domain.moc"
