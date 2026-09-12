// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/main_window.h"
#include "app/task_presentation.h"
#include "app/icons.h"
#include "app/task_schedule_editor.h"

#include <QActionGroup>
#include <QApplication>
#include <QCalendarWidget>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDirIterator>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QToolBar>
#include <algorithm>

namespace todobench {
namespace {
QToolButton* action_button(QAction* action, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setFocusPolicy(Qt::StrongFocus);
    return button;
}

QToolButton* menu_button(const QString& text, QMenu* menu, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setAccessibleName(text);
    button->setMenu(menu);
    button->setPopupMode(QToolButton::InstantPopup);
    button->setFocusPolicy(Qt::StrongFocus);
    return button;
}

QMenu* property_menu(QWidget* content, QWidget* parent) {
    auto* menu = new QMenu(parent);
    auto* action = new QWidgetAction(menu);
    action->setDefaultWidget(content);
    menu->addAction(action);
    return menu;
}

QWidget* section_header(QLabel*& heading, const QString& text, QAction* add, QWidget* parent) {
    auto* widget = new QWidget(parent);
    auto* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    heading = new QLabel(text, widget);
    auto font = heading->font();
    font.setBold(true);
    heading->setFont(font);
    layout->addWidget(heading, 1);
    layout->addWidget(action_button(add, widget));
    return widget;
}

void fit_section_list(QListWidget* list) {
    const auto height = std::max(24, list->sizeHintForRow(0)) * list->count() + list->frameWidth() * 2 + 4;
    list->setFixedHeight(std::min(96, height));
    list->setVisible(list->count() > 0);
}

QListWidget* section_list(const QString& name, QWidget* parent) {
    auto* list = new QListWidget(parent);
    list->setObjectName(name);
    list->setAccessibleName(name == "subtasksList" ? "Subtasks" : "Attachments");
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setTextElideMode(Qt::ElideRight);
    list->setMaximumHeight(96);
    list->hide();
    return list;
}
}

QWidget* MainWindow::create_task_header(QWidget* parent) {
    auto* header = new QWidget(parent);
    header->setObjectName("activeViewActions");
    auto* layout = new QToolBar(header);
    auto* header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->addWidget(layout);
    auto* add = action_button(new_task_action_, header);
    add->setText("Add task");
    layout->addWidget(add);
    auto* filter = new QToolButton(header);
    filter->setText("Filter");
    filter->setAccessibleName("Filter tasks");
    connect(filter, &QToolButton::clicked, this, [this] { filter_edit_->setFocus(); });
    layout->addWidget(filter);
    layout->addWidget(menu_button("Sort", sort_menu_, header));
    auto* spacer = new QWidget(header);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(spacer);
    QMenu* view_menu = nullptr;
    for (auto* action : menuBar()->actions()) {
        if (action->text() == "&View") view_menu = action->menu();
    }
    auto* layouts = view_menu->addMenu("Task layout");
    auto* group = new QActionGroup(this);
    list_action_ = layouts->addAction("List");
    table_action_ = layouts->addAction("Table");
    list_action_->setObjectName("listLayout");
    table_action_->setObjectName("tableLayout");
    for (auto* action : {list_action_, table_action_}) {
        action->setCheckable(true);
        group->addAction(action);
        layout->addWidget(action_button(action, header));
    }
    list_action_->setChecked(true);
    connect(list_action_, &QAction::triggered, this, [this] { set_task_layout("list"); });
    connect(table_action_, &QAction::triggered, this, [this] { set_task_layout("table"); });
    columns_menu_ = layouts->addMenu("Table columns");
    const QStringList columns{"Status", "Priority", "Due date", "Tags", "Project"};
    for (int column = 1; column <= columns.size(); ++column) {
        auto* action = columns_menu_->addAction(columns[column - 1]);
        action->setCheckable(true);
        action->setChecked(true);
        action->setData(column);
        connect(action, &QAction::triggered, this, [this, column](bool visible) {
            auto& hidden = view_tab_states_[active_view_index_].hidden_columns;
            std::erase(hidden, column);
            if (!visible) hidden.push_back(column);
            apply_view_layout();
            persist_settings();
        });
    }
    auto* columns_button = menu_button("Columns", columns_menu_, header);
    columns_button->setObjectName("tableColumnsButton");
    auto* columns_control = layout->addWidget(columns_button);
    columns_control->setObjectName("tableColumnsControl");
    auto* show_details_action = layouts->addAction("Open task details");
    show_details_action->setObjectName("showDetails");
    connect(show_details_action, &QAction::triggered, this, [this] { show_details(); });
    details_action_ = layouts->addAction("Hide details");
    details_action_->setObjectName("toggleDetails");
    details_action_->setCheckable(true);
    details_action_->setChecked(true);
    connect(details_action_, &QAction::triggered, this, [this](bool visible) { set_details_visible(visible); });
    auto* details = action_button(details_action_, header);
    details->setObjectName("toggleDetailsButton");
    layout->addWidget(details);
    return header;
}

QToolButton* MainWindow::create_due_button(QWidget* parent) {
    auto* content = new QWidget(parent);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(4, 4, 4, 4);
    due_calendar_ = new QCalendarWidget(content);
    due_calendar_->setObjectName("taskDueCalendar");
    due_calendar_->setAccessibleName("Due date calendar");
    due_calendar_->setFocusPolicy(Qt::StrongFocus);
    due_calendar_->setVerticalHeaderFormat(QCalendarWidget::NoVerticalHeader);
    due_calendar_->setSelectedDate(QDate::currentDate());
    layout->addWidget(due_calendar_);
    auto* clear = new QPushButton("Clear due date", content);
    clear->setObjectName("clearDueDate");
    clear->setAccessibleName("Clear due date");
    clear->setFocusPolicy(Qt::StrongFocus);
    layout->addWidget(clear, 0, Qt::AlignRight);
    auto* menu = property_menu(content, parent);
    menu->setObjectName("taskDueMenu");
    due_button_ = menu_button("Due date…", menu, parent);
    due_button_->setObjectName("taskDueButton");
    due_button_->setCheckable(true);
    connect(menu, &QMenu::aboutToShow, this, [this, clear] {
        update_property_buttons();
        const auto date = due_date_.isValid() ? due_date_ : QDate::currentDate();
        due_calendar_->setSelectedDate(date);
        due_calendar_->setCurrentPage(date.year(), date.month());
        clear->setEnabled(due_date_.isValid() && !read_only_);
        QTimer::singleShot(0, due_calendar_, [this] { due_calendar_->setFocus(); });
    });
    connect(menu, &QMenu::aboutToHide, this, [this] { update_property_buttons(); });
    const auto commit = [this, menu](const QDate& date) {
        set_due_date(date);
        menu->close();
    };
    connect(due_calendar_, &QCalendarWidget::clicked, this, commit);
    connect(due_calendar_, &QCalendarWidget::activated, this, commit);
    connect(clear, &QPushButton::clicked, this, [this, menu] {
        set_due_date({});
        menu->close();
    });
    return due_button_;
}

QWidget* MainWindow::create_task_properties(QWidget* parent) {
    auto* strip = new QWidget(parent);
    auto* grid = new QGridLayout(strip);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(3, 1);
    status_edit_ = new QComboBox(strip);
    status_edit_->setObjectName("taskStatus");
    status_edit_->setAccessibleName("Status");
    status_edit_->addItems({"To do", "In progress", "Waiting", "Done", "Cancelled"});
    priority_edit_ = new QComboBox(strip);
    priority_edit_->setObjectName("taskPriority");
    priority_edit_->setAccessibleName("Priority");
    priority_edit_->addItems({"None", "Low", "Normal", "High", "Urgent"});
    auto* status_label = new QLabel("Status", strip);
    status_label->setBuddy(status_edit_);
    auto* priority_label = new QLabel("Priority", strip);
    priority_label->setBuddy(priority_edit_);
    grid->addWidget(status_label, 0, 0);
    grid->addWidget(status_edit_, 0, 1);
    due_button_ = create_due_button(strip);
    grid->addWidget(due_button_, 0, 2);
    grid->addWidget(priority_label, 1, 0);
    grid->addWidget(priority_edit_, 1, 1);
    connect(status_edit_, &QComboBox::activated, this, [this](int index) { change_current_status(static_cast<TaskStatus>(index)); });
    connect(priority_edit_, &QComboBox::currentIndexChanged, this, [this] { schedule_autosave(); });

    auto* tags_content = new QWidget;
    auto* tags_layout = new QFormLayout(tags_content);
    tags_edit_ = new QLineEdit(tags_content);
    tags_edit_->setObjectName("taskTags");
    tags_edit_->setAccessibleName("Tags, separated by commas");
    tags_edit_->setPlaceholderText("comma-separated tags");
    tags_layout->addRow("Tags", tags_edit_);
    auto* tags_menu = property_menu(tags_content, strip);
    tags_button_ = menu_button("Tags", tags_menu, strip);
    tags_button_->setObjectName("taskTagsButton");
    connect(tags_menu, &QMenu::aboutToShow, this, [this] { QTimer::singleShot(0, tags_edit_, [this] { tags_edit_->setFocus(); }); });
    connect(tags_edit_, &QLineEdit::returnPressed, tags_menu, &QMenu::close);
    connect(tags_edit_, &QLineEdit::textEdited, this, [this] { schedule_autosave(); update_property_buttons(); });
    grid->addWidget(tags_button_, 1, 2);
    recurrence_value_ = new QLabel(strip);
    reminders_value_ = new QLabel(strip);
    recurrence_value_->hide();
    reminders_value_->hide();
    recurrence_button_ = new QPushButton("Repeat…", strip);
    recurrence_button_->setObjectName("scheduleRecurrence");
    recurrence_button_->setAccessibleName("Recurrence");
    reminders_button_ = new QPushButton("Reminders…", strip);
    reminders_button_->setObjectName("scheduleReminders");
    reminders_button_->setAccessibleName("Reminders");
    grid->addWidget(recurrence_button_, 2, 0, 1, 2);
    grid->addWidget(reminders_button_, 2, 2, 1, 2);
    connect(recurrence_button_, &QPushButton::clicked, this, [this] { edit_task_recurrence(); });
    connect(reminders_button_, &QPushButton::clicked, this, [this] { edit_task_reminders(); });
    return strip;
}

void MainWindow::create_detail_pane() {
    detail_stack_ = new QStackedWidget(splitter_);
    detail_stack_->setObjectName("detailStack");
    detail_stack_->setMinimumWidth(360);
    auto* empty = new QWidget(detail_stack_);
    auto* empty_layout = new QVBoxLayout(empty);
    empty_layout->addStretch();
    empty_detail_label_ = new QLabel("Open or create a workspace to begin", empty);
    empty_detail_label_->setObjectName("emptyDetailMessage");
    empty_detail_label_->setAlignment(Qt::AlignCenter);
    empty_detail_label_->setWordWrap(true);
    empty_layout->addWidget(empty_detail_label_);
    auto* start = new QPushButton("Get started…", empty);
    start->setObjectName("getStartedButton");
    connect(start, &QPushButton::clicked, this, [this] { show_onboarding(); });
    empty_layout->addWidget(start, 0, Qt::AlignCenter);
    empty_layout->addStretch();
    detail_stack_->addWidget(empty);
    auto* detail = new QWidget(detail_stack_);
    auto* layout = new QVBoxLayout(detail);
    layout->setContentsMargins(14, 8, 10, 6);
    layout->setSpacing(6);
    auto* navigation = new QHBoxLayout;
    project_value_ = new QLabel(detail);
    project_value_->setObjectName("taskNavigation");
    project_value_->setWordWrap(true);
    project_value_->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    navigation->addWidget(project_value_, 1);
    auto* menu = new QMenu(detail);
    connect(menu, &QMenu::aboutToShow, this, [this, menu] { populate_task_menu(menu); });
    details_menu_button_ = menu_button("Task actions", menu, detail);
    navigation->addWidget(details_menu_button_);
    auto* close = new QToolButton(detail);
    close->setText("×");
    close->setObjectName("closeDetailsButton");
    close->setAccessibleName("Close details");
    close->setToolTip("Close details");
    close->setFocusPolicy(Qt::StrongFocus);
    connect(close, &QToolButton::clicked, this, [this] { set_details_visible(false); task_view_->setFocus(); });
    navigation->addWidget(close);
    layout->addLayout(navigation);
    connect(project_value_, &QLabel::linkActivated, this, [this](const QString& link) {
        const auto id = link.section(':', 1).toStdString();
        if (link.startsWith("task:")) { select_task(id); show_details(); }
        else {
            const auto project = controller_.snapshot().projects.find(id);
            if (project != controller_.snapshot().projects.end()) open_view_tab({project->second.display_name, "project:" + id});
        }
    });
    auto* title_row = new QHBoxLayout;
    auto* complete = action_button(complete_action_, detail);
    complete->setToolButtonStyle(Qt::ToolButtonIconOnly);
    complete->setAccessibleName("Complete or reopen task");
    title_row->addWidget(complete);
    title_edit_ = new QLineEdit(detail);
    title_edit_->setObjectName("taskTitle");
    title_edit_->setAccessibleName("Task title");
    title_edit_->setPlaceholderText("What needs to be done?");
    auto title_font = title_edit_->font();
    title_font.setPointSize(title_font.pointSize() + 5);
    title_font.setBold(true);
    title_edit_->setFont(title_font);
    title_row->addWidget(title_edit_, 1);
    layout->addLayout(title_row);
    connect(title_edit_, &QLineEdit::textEdited, this, [this] { schedule_autosave(); });
    layout->addWidget(create_task_properties(detail));
    auto* notes_row = new QHBoxLayout;
    notes_row->addWidget(new QLabel("Notes", detail));
    save_feedback_ = new QLabel("Saved", detail);
    save_feedback_->setObjectName("taskSaveFeedback");
    save_feedback_->setAccessibleName("Save status");
    notes_row->addStretch();
    notes_row->addWidget(save_feedback_);
    layout->addLayout(notes_row);
    markdown_editor_ = new MarkdownEditor(detail);
    markdown_editor_->set_image_importer([this](const QImage& image) { return import_pasted_image(image); });
    connect(markdown_editor_, &MarkdownEditor::edited, this, [this] { schedule_autosave(); });
    layout->addWidget(markdown_editor_, 1);
    layout->addWidget(section_header(subtasks_heading_, "Subtasks (0)", new_subtask_action_, detail));
    subtasks_list_ = section_list("subtasksList", detail);
    layout->addWidget(subtasks_list_);
    connect(subtasks_list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        select_task(item->data(Qt::UserRole).toString().toStdString());
        show_details();
    });
    layout->addWidget(section_header(attachments_heading_, "Attachments (0)", attach_action_, detail));
    attachments_list_ = section_list("attachmentsList", detail);
    layout->addWidget(attachments_list_);
    connect(attachments_list_, &QListWidget::itemActivated, this, [](QListWidgetItem* item) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(item->data(Qt::UserRole).toString()));
    });
    attachments_list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(attachments_list_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        auto* item = attachments_list_->itemAt(point);
        if (item == nullptr) item = attachments_list_->currentItem();
        if (item == nullptr) return;
        const auto path = item->data(Qt::UserRole).toString();
        const auto link = item->data(Qt::UserRole + 1).toString();
        QMenu menu(this);
        menu.addAction("Open attachment", this, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
        menu.addAction("Copy Markdown link", this, [link] { QApplication::clipboard()->setText("[Attachment](" + link + ")"); });
        menu.addAction("Open containing folder", this, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath())); });
        menu.exec(attachments_list_->viewport()->mapToGlobal(point));
    });
    detail_stack_->addWidget(detail);
    detail_stack_->setCurrentIndex(0);
}

void MainWindow::update_detail_sections() {
    subtasks_list_->clear();
    attachments_list_->clear();
    const auto found = controller_.snapshot().tasks.find(current_task_id_);
    if (found == controller_.snapshot().tasks.end()) return;
    std::vector<TaskRecord> children;
    for (const auto& [id, task] : controller_.snapshot().tasks) {
        if (task.parent_id == current_task_id_) children.push_back(task);
    }
    int completed = 0;
    for (const auto& task : sort_tasks(std::move(children), TaskSort::Manual)) {
        if (task.status == TaskStatus::Done) ++completed;
        const auto title = QString::fromStdString(task.title);
        auto* item = new QListWidgetItem(title, subtasks_list_);
        item->setData(Qt::UserRole, QString::fromStdString(task.id));
        item->setToolTip(title + " — " + QString::fromStdString(to_string(task.status)));
    }
    subtasks_heading_->setText(QString("Subtasks (%1/%2 complete)").arg(completed).arg(subtasks_list_->count()));
    fit_section_list(subtasks_list_);
    const auto directory = QString::fromStdString(std::filesystem::path(found->second.source_path).parent_path().string());
    QDirIterator files(directory + "/assets", QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto path = files.next();
        const auto relative = QDir(directory).relativeFilePath(path);
        auto* item = new QListWidgetItem(relative.mid(7), attachments_list_);
        item->setData(Qt::UserRole, path);
        item->setData(Qt::UserRole + 1, QString::fromUtf8(QUrl::toPercentEncoding(relative, "/")));
        item->setToolTip(relative);
    }
    attachments_list_->sortItems();
    attachments_heading_->setText(QString("Attachments (%1)").arg(attachments_list_->count()));
    fit_section_list(attachments_list_);
}

void MainWindow::update_property_buttons() {
    if (due_button_ == nullptr) return;
    const auto has_due_date = due_date_.isValid();
    due_button_->setText(has_due_date ? "Due: " + due_date_.toString("MMM d, yyyy") : "Due date…");
    due_button_->setChecked(has_due_date);
    const auto tags = tags_edit_->text().split(',', Qt::SkipEmptyParts);
    tags_button_->setText(tags.empty() ? "Tags…" : QString("Tags (%1)").arg(tags.size()));
    tags_button_->setToolTip(tags_edit_->text());
    if (recurrence_value_ != nullptr) {
        recurrence_button_->setText(recurrence_enabled(staged_recurrence_yaml_) ? "Repeats…" : "Repeat…");
        reminders_button_->setText(staged_reminders_yaml_ == "[]" ? "Reminders…" : "Reminders set…");
        recurrence_button_->setToolTip(recurrence_value_->text());
        reminders_button_->setToolTip(reminders_value_->text());
    }
}

void MainWindow::set_due_date(const QDate& date) {
    if (read_only_ || current_task_id_.empty() || date == due_date_) return;
    due_date_ = date;
    schedule_autosave();
    update_property_buttons();
}

void MainWindow::set_details_visible(bool visible) {
    if (detail_stack_ == nullptr) return;
    details_action_->setChecked(visible);
    details_action_->setText(visible ? "Hide details" : "Show details");
    const bool was_visible = !detail_stack_->isHidden();
    if (was_visible == visible) return;
    if (was_visible && splitter_->sizes().size() == 2) {
        settings_.details_pane_width = std::max(360, splitter_->sizes()[1]);
    }
    settings_.details_visible = visible;
    detail_stack_->setVisible(visible);
    if (visible) {
        const int available = splitter_->width() - splitter_->handleWidth();
        splitter_->setSizes({std::max(300, available - settings_.details_pane_width), settings_.details_pane_width});
    }
    if (!applying_settings_) persist_settings();
}

void MainWindow::show_details() {
    set_details_visible(true);
    if (!current_task_id_.empty()) select_task(current_task_id_);
}

void MainWindow::set_task_layout(const QString& layout) {
    if (view_tab_states_.empty()) return;
    sync_active_view();
    view_tab_states_[active_view_index_].layout = layout.toStdString();
    apply_view_layout();
    persist_settings();
}

void MainWindow::apply_view_layout() {
    if (view_tab_states_.empty()) return;
    const auto& view = view_tab_states_[active_view_index_];
    task_view_->set_layout(QString::fromStdString(view.layout), view.hidden_columns);
    const bool table = view.layout == "table";
    table_action_->setChecked(table);
    list_action_->setChecked(!table);
    columns_menu_->setEnabled(table);
    findChild<QAction*>("tableColumnsControl")->setVisible(table);
    for (auto* action : columns_menu_->actions()) {
        action->setChecked(std::find(view.hidden_columns.begin(), view.hidden_columns.end(), action->data().toInt()) == view.hidden_columns.end());
    }
}

void MainWindow::remember_expansion(const QModelIndex& index, bool expanded) {
    if (rebuilding_view_ || view_tab_states_.empty()) return;
    auto& view = view_tab_states_[active_view_index_];
    const auto id = index.data(TaskIdRole).toString().toStdString();
    std::erase(view.expanded_task_ids, id);
    if (expanded) view.expanded_task_ids.push_back(id);
    view.expansion_initialized = true;
}

void MainWindow::update_selection_bar() {
    if (selection_bar_ == nullptr) return;
    const auto count = task_view_->selectionModel()->selectedRows().size();
    selection_count_->setText(QString("%1 selected").arg(count));
    selection_bar_->setVisible(count > 1);
    bulk_wait_action_->setEnabled(controller_.is_open() && !read_only_ && count > 0);
}

void MainWindow::populate_task_menu(QMenu* menu) {
    menu->clear();
    auto* show = menu->addAction("Show details", this, [this] { show_details(); });
    show->setEnabled(!current_task_id_.empty());
    menu->addAction(complete_action_);
    const auto found = controller_.snapshot().tasks.find(current_task_id_);
    if (found != controller_.snapshot().tasks.end() && recurrence_enabled(found->second.recurrence_yaml)) menu->addAction(stop_repeating_action_);
    auto* statuses = menu->addMenu("Change status");
    statuses->setEnabled(!read_only_ && found != controller_.snapshot().tasks.end());
    const QStringList labels{"To do", "In progress", "Waiting", "Done", "Cancelled"};
    for (int i = 0; i < labels.size(); ++i) {
        auto* action = statuses->addAction(labels[i], this, [this, i] { change_current_status(static_cast<TaskStatus>(i)); });
        action->setCheckable(true);
        action->setChecked(found != controller_.snapshot().tasks.end() && found->second.status == static_cast<TaskStatus>(i));
    }
    menu->addSeparator();
    for (auto* action : {move_action_, duplicate_action_, new_subtask_action_, trash_action_}) menu->addAction(action);
}

void MainWindow::show_task_menu(const QModelIndex& index, const QPoint& position) {
    if (!index.isValid()) return;
    const auto id = index.siblingAtColumn(0).data(TaskIdRole).toString().toStdString();
    select_task(id);
    if (current_task_id_ != id) return;
    QMenu menu(this);
    populate_task_menu(&menu);
    menu.exec(position);
}

} // namespace todobench
