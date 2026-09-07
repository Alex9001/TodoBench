// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/front_matter_codec.h"

#include <QTest>

using namespace todobench;

class FrontMatterTest final : public QObject {
    Q_OBJECT
private slots:
    void taskRoundTripPreservesBodyAndUnknownField();
    void projectRoundTrip();
    void futureSchemaIsRejected();
};

void FrontMatterTest::taskRoundTripPreservesBodyAndUnknownField() {
    const std::string source = "---\nschema_version: 1\nkind: task\nid: 123e4567-e89b-12d3-a456-426614174000\ntitle: Review\nparent_id: null\nstatus: todo\nprevious_open_status: todo\npriority: high\ntags: [release, work]\ndue: null\nrecurrence: null\nreminders: []\norder: 1024\ncreated_at: now\nupdated_at: now\ncompleted_at: null\nrevision: rev\ncustom_field: preserved\n---\n\n# Notes\n\n- [ ] Verify\n";
    const auto parsed = parse_task_markdown("task.md", source);
    QVERIFY(std::holds_alternative<TaskRecord>(parsed));
    const auto task = std::get<TaskRecord>(parsed);
    QCOMPARE(QString::fromStdString(task.body), QString("\n# Notes\n\n- [ ] Verify\n"));
    QCOMPARE(QString::fromStdString(task.unknown_fields.at("custom_field")), QString("preserved"));
    const auto encoded = serialize_task_markdown(task);
    QVERIFY(encoded.find("# Notes") != std::string::npos);
    QVERIFY(encoded.find("custom_field") != std::string::npos);
}

void FrontMatterTest::futureSchemaIsRejected() {
    const auto result = parse_task_markdown("future.md", "---\nschema_version: 2\nkind: task\n---\n");
    QVERIFY(std::holds_alternative<CodecError>(result));
}

void FrontMatterTest::projectRoundTrip() {
    const std::string source = "---\nkind: project\nid: 123e4567-e89b-12d3-a456-426614174001\ndisplay_name: Work\norder: 1024\narchived: false\n---\n";
    const auto parsed = parse_project_markdown("project.md", source);
    QVERIFY(std::holds_alternative<ProjectRecord>(parsed));
    const auto project = std::get<ProjectRecord>(parsed);
    QCOMPARE(QString::fromStdString(project.display_name), QString("Work"));
    QVERIFY(serialize_project_markdown(project).find("display_name: Work") != std::string::npos);
}

QTEST_MAIN(FrontMatterTest)
#include "test_front_matter.moc"
