// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/task_schedule_editor.h"

#include <QTest>

using namespace todobench;

class TaskScheduleEditorTest final : public QObject {
    Q_OBJECT
private slots:
    void summarizesRecurrenceRules();
    void summarizesReminderOffsets();
    void malformedMetadataFailsClosed();
};

void TaskScheduleEditorTest::summarizesRecurrenceRules() {
    const std::string weekly = "enabled: true\nmode: fixed_calendar\ninterval: 2\nunit: weeks\nweekdays: [1, 5]\n";
    QCOMPARE(recurrence_summary(weekly), QString("Every 2 weeks on Mon, Fri"));
    const std::string completion = "enabled: true\nmode: after_completion\ninterval: 1\nunit: days\n";
    QCOMPARE(recurrence_summary(completion), QString("After completion: every day"));
}

void TaskScheduleEditorTest::summarizesReminderOffsets() {
    QCOMPARE(reminders_summary("[{id: due, minutes_before: 0}]"), QString("At the due time"));
    QCOMPARE(reminders_summary("[{id: hour, minutes_before: 60}]"), QString("1 hour before"));
    QCOMPARE(reminders_summary("[{id: hour, minutes_before: 60}, {id: day, minutes_before: 1440}]"),
             QString("2 reminders"));
}

void TaskScheduleEditorTest::malformedMetadataFailsClosed() {
    QCOMPARE(recurrence_summary("not: [valid"), QString("Does not repeat"));
    QCOMPARE(reminders_summary("not: [valid"), QString("No reminders"));
}

QTEST_MAIN(TaskScheduleEditorTest)
#include "test_task_schedule_editor.moc"
