// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/task_presentation.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QSignalSpy>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QTest>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QtAlgorithms>

using namespace todobench;

namespace {
WorkspaceSnapshot snapshot_with_tasks() {
    WorkspaceSnapshot snapshot;
    snapshot.projects.emplace("work", ProjectRecord{.id = "work", .display_name = "Work"});
    TaskRecord parent;
    parent.id = "parent";
    parent.project_id = "work";
    parent.title = "A deliberately long task title that must wrap without letting its metadata take title space";
    parent.status = TaskStatus::InProgress;
    parent.priority = Priority::High;
    parent.tags = {"release", "desktop"};
    parent.due_yaml = "2026-09-12";
    snapshot.tasks.emplace(parent.id, parent);
    TaskRecord child;
    child.id = "child";
    child.project_id = "work";
    child.parent_id = parent.id;
    child.title = "Child";
    child.status = TaskStatus::Done;
    snapshot.tasks.emplace(child.id, child);
    return snapshot;
}

struct DenseTree {
    WorkspaceSnapshot snapshot;
    std::vector<TaskRecord> branch;
};

DenseTree dense_tree() {
    DenseTree result;
    result.snapshot.projects.emplace("work", ProjectRecord{.id = "work", .display_name = "Work"});
    TaskRecord root;
    root.id = "deep-0";
    root.project_id = "work";
    root.title = "A long title that must wrap cleanly while a large amount of secondary information stays below it";
    root.status = TaskStatus::InProgress;
    root.priority = Priority::Urgent;
    root.due_yaml = "2026-09-12";
    for (int index = 0; index < 40; ++index) root.tags.push_back("tag" + std::to_string(index));
    result.snapshot.tasks.emplace(root.id, root);
    result.branch.push_back(root);
    for (int depth = 1; depth <= 8; ++depth) {
        TaskRecord child;
        child.id = "deep-" + std::to_string(depth);
        child.project_id = "work";
        child.parent_id = result.branch.back().id;
        child.title = "Nested task " + std::to_string(depth) + " remains reachable after resizing and changing layouts";
        child.status = depth == 8 ? TaskStatus::Waiting : TaskStatus::Todo;
        result.snapshot.tasks.emplace(child.id, child);
        result.branch.push_back(child);
    }
    return result;
}

QModelIndex append_dense_tree(QStandardItemModel& model, const DenseTree& tree, const Settings& settings) {
    auto row = make_task_row(tree.branch.front(), settings, tree.snapshot);
    QStandardItem* parent = row.front();
    model.appendRow(row);
    for (size_t depth = 1; depth < tree.branch.size(); ++depth) {
        const auto child_row = make_task_row(tree.branch[depth], settings, tree.snapshot);
        parent->appendRow(child_row);
        parent = child_row.front();
    }
    auto deepest = model.index(0, 0);
    for (int depth = 1; depth <= 8; ++depth) deepest = model.index(0, 0, deepest);
    return deepest;
}

bool row_actions_reachable(TaskTreeView& view, const QModelIndex& root) {
    view.scrollTo(root, QAbstractItemView::PositionAtTop);
    QApplication::processEvents();
    const auto rect = view.visualRect(root);
    if (!rect.isValid()) return false;
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, QPoint(rect.left() + 13, rect.center().y()));
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, QPoint(rect.right() - 15, rect.center().y()));
    return true;
}

bool deepest_row_reachable_after_resizes(TaskTreeView& view, const QModelIndex& deepest) {
    for (const int width : {520, 360}) {
        view.resize(width, 240);
        QApplication::processEvents();
        if (view.columnWidth(0) < view.viewport()->width() - 2) return false;
        view.scrollTo(deepest, QAbstractItemView::PositionAtCenter);
        QApplication::processEvents();
        if (!view.visualRect(deepest).intersects(view.viewport()->rect())) return false;
    }
    return true;
}

bool deep_hierarchy_is_intact(const QStandardItemModel& model, const QModelIndex& root) {
    auto index = root;
    for (int depth = 0; depth < 8; ++depth) {
        if (model.rowCount(index) != 1) return false;
        index = model.index(0, 0, index);
    }
    return index.data(TaskIdRole).toString() == "deep-8";
}

bool save_presentation_screenshot(TaskTreeView& view, const QString& directory, const QString& filename) {
    return directory.isEmpty() || (QDir().mkpath(directory) && view.grab().save(directory + "/" + filename));
}

bool all_true(const std::initializer_list<bool> values) {
    return std::all_of(values.begin(), values.end(), [](const bool value) { return value; });
}

bool contains_vivid_icon(const QImage& image) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const auto pixel = image.pixelColor(x, y);
            if (pixel.red() > 240 && pixel.green() < 30 && pixel.blue() > 240) return true;
        }
    }
    return false;
}
}

class TaskPresentationTest final : public QObject {
    Q_OBJECT
private slots:
    void rowsKeepTaskIdentityAndContentFirstMetadata();
    void projectIconsRenderInBothLayouts_data();
    void projectIconsRenderInBothLayouts();
    void listHasSeparateCompletionAndMenuTargets();
    void keyboardRequestsDetailsWithoutChangingExpansion();
    void tableKeepsSelectionAndCanHideMetadata();
    void narrowListStretchesTitleAndKeepsActionsSeparate();
    void selectionRefreshUsesFreshActionIndex();
    void cancelledSelectionDoesNotInvokeActions();
    void dragHandlerIsPreservedInBothLayouts();
    void denseDeepTreeStaysReachableAcrossLayoutsAndResizes();
    void taskMenusKeepSelection_data();
    void taskMenusKeepSelection();
    void cachedProgressCountsHiddenChildrenAndRefreshes();
};

void TaskPresentationTest::projectIconsRenderInBothLayouts_data() {
    QTest::addColumn<QString>("layout");
    QTest::newRow("list") << QString("list");
    QTest::newRow("table") << QString("table");
}

void TaskPresentationTest::projectIconsRenderInBothLayouts() {
    QFETCH(QString, layout);
    const auto snapshot = snapshot_with_tasks();
    Settings settings;
    QStandardItemModel model;
    const auto row = make_task_row(snapshot.tasks.at("parent"), settings, snapshot);
    QPixmap vivid_icon(16, 16);
    vivid_icon.fill(QColor("#ff00ff"));
    row.front()->setIcon(QIcon(vivid_icon));
    model.appendRow(row);
    TaskTreeView view;
    view.setModel(&model);
    view.set_layout(layout);
    view.resize(520, 180);
    view.show();
    QApplication::processEvents();
    QVERIFY(contains_vivid_icon(view.viewport()->grab().toImage()));
}

void TaskPresentationTest::rowsKeepTaskIdentityAndContentFirstMetadata() {
    const auto snapshot = snapshot_with_tasks();
    Settings settings;
    const auto row = make_task_row(snapshot.tasks.at("parent"), settings, snapshot, "work");
    QCOMPARE(row.size(), 6);
    QCOMPARE(row.front()->data(TaskIdRole).toString(), QString("parent"));
    QCOMPARE(row.front()->data(TaskProgressRole).toMap().value("total").toInt(), 1);
    QVERIFY(row.front()->data(TaskMetadataRole).toString().contains("Status: In progress"));
    QVERIFY(row.front()->data(TaskMetadataRole).toString().contains("Priority: High"));
    QVERIFY(!row.front()->data(TaskMetadataRole).toString().contains("Project: Work"));
    QVERIFY(row.front()->data(Qt::AccessibleTextRole).toString().contains("In progress"));
    qDeleteAll(row);
}

void TaskPresentationTest::listHasSeparateCompletionAndMenuTargets() {
    const auto snapshot = snapshot_with_tasks();
    Settings settings;
    QStandardItemModel model;
    model.appendRow(make_task_row(snapshot.tasks.at("parent"), settings, snapshot));
    TaskTreeView view;
    view.setModel(&model);
    view.resize(440, 180);
    view.show();
    QApplication::processEvents();
    const auto index = model.index(0, 0);
    const auto rect = view.visualRect(index);
    QVERIFY(rect.height() > 34);
    QSignalSpy completion(&view, &TaskTreeView::completionRequested);
    QSignalSpy menu(&view, &TaskTreeView::menuRequested);
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, QPoint(rect.left() + 13, rect.center().y()));
    QCOMPARE(completion.count(), 1);
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, QPoint(rect.right() - 15, rect.center().y()));
    QCOMPARE(menu.count(), 1);
}

void TaskPresentationTest::keyboardRequestsDetailsWithoutChangingExpansion() {
    const auto snapshot = snapshot_with_tasks();
    Settings settings;
    QStandardItemModel model;
    const auto row = make_task_row(snapshot.tasks.at("parent"), settings, snapshot);
    row.front()->appendRow(make_task_row(snapshot.tasks.at("child"), settings, snapshot));
    model.appendRow(row);
    TaskTreeView view;
    view.setModel(&model);
    view.resize(440, 220);
    view.expand(model.index(0, 0));
    view.setCurrentIndex(model.index(0, 0));
    view.show();
    QApplication::processEvents();
    QSignalSpy details(&view, &TaskTreeView::detailsRequested);
    QTest::keyClick(&view, Qt::Key_Return);
    QCOMPARE(details.count(), 1);
    QVERIFY(view.isExpanded(model.index(0, 0)));
    QSignalSpy completion(&view, &TaskTreeView::completionRequested);
    view.set_completion_key_enabled(false);
    QTest::keyClick(&view, Qt::Key_Space);
    QCOMPARE(completion.count(), 0);
    view.set_completion_key_enabled(true);
    QTest::keyClick(&view, Qt::Key_Space);
    QCOMPARE(completion.count(), 1);
}

void TaskPresentationTest::tableKeepsSelectionAndCanHideMetadata() {
    const auto snapshot = snapshot_with_tasks();
    Settings settings;
    QStandardItemModel model;
    model.setHorizontalHeaderLabels({"Task", "State", "Priority", "Due", "Tags", "Project"});
    model.appendRow(make_task_row(snapshot.tasks.at("parent"), settings, snapshot));
    TaskTreeView view;
    view.setModel(&model);
    view.setCurrentIndex(model.index(0, 0));
    view.set_layout("table", {4, 5});
    QCOMPARE(view.layout(), QString("table"));
    QCOMPARE(view.currentIndex().data(TaskIdRole).toString(), QString("parent"));
    QVERIFY(!view.isColumnHidden(0));
    QVERIFY(view.isColumnHidden(4));
    QVERIFY(view.header()->sectionSize(0) >= 280);
    view.set_layout("list");
    QCOMPARE(view.currentIndex().data(TaskIdRole).toString(), QString("parent"));
    QVERIFY(view.isColumnHidden(1));
}

void TaskPresentationTest::narrowListStretchesTitleAndKeepsActionsSeparate() {
    const auto snapshot = snapshot_with_tasks();
    Settings settings;
    QStandardItemModel model;
    model.appendRow(make_task_row(snapshot.tasks.at("parent"), settings, snapshot));
    TaskTreeView view;
    view.setModel(&model);
    view.resize(300, 180);
    view.show();
    QApplication::processEvents();
    const auto index = model.index(0, 0);
    const auto rect = view.visualRect(index);
    QVERIFY(view.columnWidth(0) >= view.viewport()->width() - 2);
    QVERIFY(rect.height() > 34);
    QSignalSpy details(&view, &TaskTreeView::detailsRequested);
    QTest::mouseDClick(view.viewport(), Qt::LeftButton, {}, QPoint(rect.left() + 13, rect.center().y()));
    QCOMPARE(details.count(), 0);
    QTest::mouseDClick(view.viewport(), Qt::LeftButton, {}, QPoint(rect.right() - 15, rect.center().y()));
    QCOMPARE(details.count(), 0);
    view.set_layout("table");
    QApplication::processEvents();
    const auto table_rect = view.visualRect(index);
    QSignalSpy completion(&view, &TaskTreeView::completionRequested);
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, QPoint(table_rect.left() + 13, table_rect.center().y()));
    QCOMPARE(completion.count(), 1);
    view.header()->resizeSection(0, 100);
    QCOMPARE(view.header()->sectionSize(0), 280);
}

void TaskPresentationTest::selectionRefreshUsesFreshActionIndex() {
    const auto snapshot = snapshot_with_tasks();
    Settings settings;
    QStandardItemModel model;
    model.appendRow(make_task_row(snapshot.tasks.at("parent"), settings, snapshot));
    TaskTreeView view;
    view.setModel(&model);
    view.resize(420, 180);
    view.show();
    QApplication::processEvents();
    bool refreshed = false;
    connect(view.selectionModel(), &QItemSelectionModel::currentChanged, &view, [&] {
        if (refreshed) return;
        refreshed = true;
        model.clear();
        model.appendRow(make_task_row(snapshot.tasks.at("parent"), settings, snapshot));
    });
    QSignalSpy completion(&view, &TaskTreeView::completionRequested);
    const auto rect = view.visualRect(model.index(0, 0));
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, QPoint(rect.left() + 13, rect.center().y()));
    QCOMPARE(completion.count(), 1);
    QCOMPARE(completion.first().first().value<QModelIndex>().data(TaskIdRole).toString(), QString("parent"));
}

void TaskPresentationTest::cancelledSelectionDoesNotInvokeActions() {
    const auto snapshot = snapshot_with_tasks();
    QStandardItemModel model;
    model.appendRow(make_task_row(snapshot.tasks.at("parent"), Settings{}, snapshot));
    model.appendRow(make_task_row(snapshot.tasks.at("child"), Settings{}, snapshot));
    TaskTreeView view;
    view.setModel(&model);
    view.setCurrentIndex(model.index(0, 0));
    view.resize(420, 220);
    view.show();
    connect(view.selectionModel(), &QItemSelectionModel::currentChanged, &view, [&] {
        QSignalBlocker blocker(view.selectionModel());
        view.setCurrentIndex(model.index(0, 0));
    });
    QSignalSpy completion(&view, &TaskTreeView::completionRequested);
    QSignalSpy menu(&view, &TaskTreeView::menuRequested);
    QSignalSpy details(&view, &TaskTreeView::detailsRequested);
    for (const auto* layout : {"list", "table"}) {
        view.set_layout(layout);
        QApplication::processEvents();
        const auto rect = view.visualRect(model.index(1, 0));
        QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, QPoint(rect.left() + 13, rect.center().y()));
        QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, QPoint(rect.right() - 15, rect.center().y()));
        QTest::mouseDClick(view.viewport(), Qt::LeftButton, {}, QPoint(rect.left() + 70, rect.center().y()));
    }
    QCOMPARE(completion.count(), 0);
    QCOMPARE(menu.count(), 0);
    QCOMPARE(details.count(), 0);
    QCOMPARE(view.currentIndex().data(TaskIdRole).toString(), QString("parent"));
}

void TaskPresentationTest::dragHandlerIsPreservedInBothLayouts() {
    const auto snapshot = snapshot_with_tasks();
    Settings settings;
    QStandardItemModel model;
    model.appendRow(make_task_row(snapshot.tasks.at("parent"), settings, snapshot));
    model.appendRow(make_task_row(snapshot.tasks.at("child"), settings, snapshot));
    TaskTreeView view;
    view.setModel(&model);
    int calls = 0;
    QString source;
    QString target;
    view.drop_handler = [&](const QModelIndex& from, const QModelIndex& to, const int position) {
        ++calls;
        source = from.data(TaskIdRole).toString();
        target = to.data(TaskIdRole).toString();
        return position == 2;
    };
    const auto from = model.index(0, 0);
    const auto to = model.index(1, 0);
    view.set_layout("list");
    QVERIFY(view.drop_handler(from, to, 2));
    view.set_layout("table", {4});
    QVERIFY(view.drop_handler(from, to, 2));
    QCOMPARE(calls, 2);
    QCOMPARE(source, QString("parent"));
    QCOMPARE(target, QString("child"));
}

void TaskPresentationTest::denseDeepTreeStaysReachableAcrossLayoutsAndResizes() {
    const auto tree = dense_tree();
    Settings settings;
    QStandardItemModel model;
    const auto deepest = append_dense_tree(model, tree, settings);
    TaskTreeView view;
    view.setModel(&model);
    view.resize(360, 240);
    view.show();
    view.expandAll();
    QApplication::processEvents();
    const auto root_index = model.index(0, 0);
    const int title_column_width = view.columnWidth(0);
    const bool list_dimensions = view.visualRect(root_index).height() > 40
        && title_column_width >= view.viewport()->width() - 2;
    const auto root_metadata = root_index.data(TaskMetadataRole);
    model.itemFromIndex(root_index)->setData(QString{}, TaskMetadataRole);
    view.doItemsLayout();
    const bool metadata_keeps_width = view.columnWidth(0) == title_column_width;
    model.itemFromIndex(root_index)->setData(root_metadata, TaskMetadataRole);
    QSignalSpy completion(&view, &TaskTreeView::completionRequested);
    QSignalSpy menu(&view, &TaskTreeView::menuRequested);
    const auto screenshots = qEnvironmentVariable("TODOBENCH_PRESENTATION_SCREENSHOTS");
    const bool list_actions = row_actions_reachable(view, root_index);
    const bool list_resizes = deepest_row_reachable_after_resizes(view, deepest);
    const bool list_screenshot = save_presentation_screenshot(view, screenshots, "deep-list.png");
    view.set_layout("table", {4, 5});
    view.expandAll();
    view.resize(360, 240);
    QApplication::processEvents();
    view.scrollTo(deepest, QAbstractItemView::PositionAtCenter);
    QApplication::processEvents();
    const bool table_is_valid = view.header()->sectionSize(0) >= 280 && deep_hierarchy_is_intact(model, root_index)
        && view.visualRect(deepest).intersects(view.viewport()->rect());
    const bool table_actions = row_actions_reachable(view, root_index);
    const bool table_screenshot = save_presentation_screenshot(view, screenshots, "deep-table.png");
    QVERIFY(all_true({deepest.isValid(), root_index.data(TaskMetadataRole).toString().startsWith("Subtasks: 0/1"),
        root_index.data(TaskMetadataRole).toString().contains("#tag39"), list_dimensions, metadata_keeps_width, list_actions,
        list_resizes, list_screenshot, table_is_valid, table_actions, table_screenshot, completion.count() == 2, menu.count() == 2}));
}

void TaskPresentationTest::taskMenusKeepSelection_data() {
    QTest::addColumn<QString>("layout");
    QTest::addColumn<bool>("keyboard");
    for (const auto* layout : {"list", "table"}) {
        QTest::newRow(qPrintable(QString(layout) + "-keyboard")) << QString(layout) << true;
        QTest::newRow(qPrintable(QString(layout) + "-row-menu")) << QString(layout) << false;
    }
}

void TaskPresentationTest::taskMenusKeepSelection() {
    QFETCH(QString, layout);
    QFETCH(bool, keyboard);
    const auto snapshot = snapshot_with_tasks();
    QStandardItemModel model;
    model.appendRow(make_task_row(snapshot.tasks.at("parent"), Settings{}, snapshot));
    model.appendRow(make_task_row(snapshot.tasks.at("child"), Settings{}, snapshot));
    TaskTreeView view;
    view.setModel(&model);
    view.set_layout(layout);
    view.setSelectionMode(QAbstractItemView::ExtendedSelection);
    view.resize(480, 240);
    view.show();
    view.setCurrentIndex(model.index(0, 0));
    view.selectAll();
    QApplication::processEvents();
    QSignalSpy menu(&view, &TaskTreeView::menuRequested);
    // A keyboard context-menu event can carry a position over another row.
    const auto other = view.visualRect(model.index(1, 0));
    if (keyboard) {
        QContextMenuEvent event(QContextMenuEvent::Keyboard, other.center(),
                               view.viewport()->mapToGlobal(other.center()));
        QApplication::sendEvent(view.viewport(), &event);
    } else {
        QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, QPoint(other.right() - 15, other.center().y()));
    }
    QCOMPARE(menu.count(), 1);
    QCOMPARE(menu.first().first().value<QModelIndex>().row(), keyboard ? 0 : 1);
    QCOMPARE(view.selectionModel()->selectedRows().size(), 2);
}

void TaskPresentationTest::cachedProgressCountsHiddenChildrenAndRefreshes() {
    auto snapshot = snapshot_with_tasks();
    TaskRecord grandchild;
    grandchild.id = "grandchild";
    grandchild.parent_id = "child";
    grandchild.status = TaskStatus::Done;
    snapshot.tasks.emplace(grandchild.id, grandchild);
    auto progress = count_task_progress(snapshot);
    QStandardItemModel model;
    // Only the parent is displayed: filtered-out children still contribute.
    model.appendRow(make_task_row(snapshot.tasks.at("parent"), Settings{}, snapshot, {}, &progress));
    QCOMPARE(model.index(0, 0).data(TaskProgressRole).toMap(), (QVariantMap{{"completed", 1}, {"total", 1}}));
    QCOMPARE(progress.at("child").completed, 1);
    snapshot.tasks.at("child").status = TaskStatus::Waiting;
    progress = count_task_progress(snapshot);
    model.clear();
    model.appendRow(make_task_row(snapshot.tasks.at("parent"), Settings{}, snapshot, {}, &progress));
    QCOMPARE(model.index(0, 0).data(TaskProgressRole).toMap(), (QVariantMap{{"completed", 0}, {"total", 1}}));
    model.appendRow(make_task_row(grandchild, Settings{}, snapshot, {}, &progress));
    QCOMPARE(model.index(1, 0).data(TaskProgressRole).toMap(), (QVariantMap{{"completed", 0}, {"total", 0}}));
}

QTEST_MAIN(TaskPresentationTest)
#include "test_task_presentation.moc"
