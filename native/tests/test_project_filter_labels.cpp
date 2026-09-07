// SPDX-License-Identifier: GPL-3.0-or-later
#include "domain/project_filter_labels.h"
#include "domain/filter_session.h"
#include <QTest>

using namespace todobench;

namespace {
ProjectRecord project(std::string id, std::string name, std::string parent = {}) {
    ProjectRecord result;
    result.id = std::move(id);
    result.display_name = std::move(name);
    result.parent_id = std::move(parent);
    return result;
}
}

class ProjectFilterLabelsTest final : public QObject {
    Q_OBJECT
private slots:
    void displaysNamesAndRetainsIds();
    void distinguishesNestedAndDuplicateNames();
    void handlesQuotesAndTokenRemoval();
    void retainsMissingProjectScope();
    void followsRename();
    void preservesOtherFiltersAndErrors();
};

void ProjectFilterLabelsTest::displaysNamesAndRetainsIds() {
    ProjectFilterLabels labels({{"id", project("id", "Client Work")}});
    const auto display = labels.display_expression("project:id tag:qa");
    QCOMPARE(display, "project:\"Client Work\" tag:qa");
    QCOMPARE(labels.canonical_expression(display + " status:todo"), "project:id tag:qa status:todo");
    QCOMPARE(labels.canonical_expression("project:\"client work\""), "project:id");
}

void ProjectFilterLabelsTest::distinguishesNestedAndDuplicateNames() {
    ProjectFilterLabels labels({{"a", project("a", "Work")}, {"b", project("b", "Work")},
                               {"c", project("c", "Launch", "a")}, {"d", project("d", "Launch")}});
    QCOMPARE(labels.display_expression("project:c"), "project:\"Work / Launch\"");
    QCOMPARE(labels.display_expression("project:b"), "project:\"Work (2)\"");
    QCOMPARE(labels.canonical_expression(labels.display_expression("project:a project:b project:c project:d")),
             "project:a project:b project:c project:d");
}

void ProjectFilterLabelsTest::handlesQuotesAndTokenRemoval() {
    ProjectFilterLabels labels({{"id", project("id", "My \"quoted\" \\ project")}});
    const auto display = labels.display_expression("project:id \"release notes\" tag:qa");
    QCOMPARE(labels.canonical_expression(display), "project:id \"release notes\" tag:qa");
    FilterSession session;
    QVERIFY(session.update(display));
    QVERIFY(session.remove_token(2));
    QCOMPARE(labels.canonical_expression(session.expression()), "project:id \"release notes\"");
}

void ProjectFilterLabelsTest::retainsMissingProjectScope() {
    ProjectFilterLabels labels;
    const std::string expression = "project:13483b91-8b05-4eda-a68e-251a1b322b29";
    const auto display = labels.display_expression(expression);
    QCOMPARE(display, "project:\"Missing project\"");
    QCOMPARE(labels.canonical_expression(display), expression);
    TaskRecord unrelated;
    unrelated.project_id = "another-project";
    QVERIFY(!matches_filter(unrelated, compile_filter(labels.canonical_expression(display)).spec));
}

void ProjectFilterLabelsTest::followsRename() {
    ProjectFilterLabels old_labels({{"id", project("id", "Old name")}});
    const auto saved = old_labels.canonical_expression("project:\"Old name\" tag:qa");
    ProjectFilterLabels renamed({{"id", project("id", "New name")}});
    QCOMPARE(renamed.display_expression(saved), "project:\"New name\" tag:qa");
    QCOMPARE(renamed.canonical_expression(renamed.display_expression(saved)), saved);
}

void ProjectFilterLabelsTest::preservesOtherFiltersAndErrors() {
    ProjectFilterLabels labels({{"id", project("id", "Work")}});
    const auto canonical = labels.canonical_expression("project:Work \"release notes\" tag:qa status:todo");
    const auto compiled = compile_filter(canonical);
    QCOMPARE(compiled.spec.title_terms, std::vector<std::string>{"release notes"});
    QCOMPARE(compiled.spec.tags, std::vector<std::string>{"qa"});
    QVERIFY(!compile_filter(labels.canonical_expression("project:Work status:invalid")).error.empty());
    QCOMPARE(labels.canonical_expression("project:Unknown"), "project:Unknown");
}

QTEST_GUILESS_MAIN(ProjectFilterLabelsTest)
#include "test_project_filter_labels.moc"
