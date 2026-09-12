// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/task_presentation.h"

#include "domain/formatting_rules.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDate>
#include <QDropEvent>
#include <QFontMetrics>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QResizeEvent>
#include <QStandardItem>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QStyleOptionButton>
#include <QVariantMap>

#include <algorithm>
#include <unordered_set>

namespace todobench {
namespace {

constexpr int kTitleColumn = 0;
constexpr int kStatusColumn = 1;
constexpr int kPriorityColumn = 2;
constexpr int kDueColumn = 3;
constexpr int kTagsColumn = 4;
constexpr int kProjectColumn = 5;
constexpr int kCompletionSize = 22;
constexpr int kMenuWidth = 26;
constexpr int kIconGap = 6;

QString status_label(const TaskStatus status) {
    switch (status) {
    case TaskStatus::Todo: return "To do";
    case TaskStatus::InProgress: return "In progress";
    case TaskStatus::Waiting: return "Waiting";
    case TaskStatus::Done: return "Done";
    case TaskStatus::Cancelled: return "Cancelled";
    }
    return "To do";
}

QString priority_label(const Priority priority) {
    switch (priority) {
    case Priority::None: return "None";
    case Priority::Low: return "Low";
    case Priority::Normal: return "Normal";
    case Priority::High: return "High";
    case Priority::Urgent: return "Urgent";
    }
    return "Normal";
}

QColor status_color(const TaskStatus status) {
    switch (status) {
    case TaskStatus::Todo: return QColor("#78909c");
    case TaskStatus::InProgress: return QColor("#1976d2");
    case TaskStatus::Waiting: return QColor("#ed6c02");
    case TaskStatus::Done: return QColor("#2e7d32");
    case TaskStatus::Cancelled: return QColor("#757575");
    }
    return {};
}

QString project_name(const WorkspaceSnapshot& snapshot, const std::string& project_id) {
    const auto found = snapshot.projects.find(project_id);
    return found == snapshot.projects.end() ? QString::fromStdString(project_id)
                                             : QString::fromStdString(found->second.display_name);
}

QString tags_label(const std::vector<std::string>& tags) {
    QStringList values;
    for (const auto& tag : tags) values << QString::fromStdString(tag);
    return values.join(", ");
}

bool is_project_context(const std::string& context, const TaskRecord& task,
                        const WorkspaceSnapshot& snapshot) {
    if (context.empty()) return false;
    if (context == task.project_id) return true;
    const auto name = project_name(snapshot, task.project_id).toStdString();
    return context == name;
}

QVariantMap progress_for(const TaskRecord& task, const WorkspaceSnapshot& snapshot,
                         const TaskProgressMap* progress) {
    if (progress != nullptr) {
        const auto found = progress->find(task.id);
        const auto counts = found == progress->end() ? TaskProgress{} : found->second;
        return {{"completed", counts.completed}, {"total", counts.total}};
    }
    int total = 0;
    int complete = 0;
    for (const auto& [id, candidate] : snapshot.tasks) {
        Q_UNUSED(id);
        if (candidate.parent_id != task.id) continue;
        ++total;
        if (candidate.status == TaskStatus::Done) ++complete;
    }
    return {{"completed", complete}, {"total", total}};
}

QString list_metadata(const TaskRecord& task, const WorkspaceSnapshot& snapshot,
                      const std::string& project_context, const QVariantMap& progress) {
    QStringList values;
    const int total = progress.value("total").toInt();
    if (total > 0) values << QString("Subtasks: %1/%2").arg(progress.value("completed").toInt()).arg(total);
    if (task.status != TaskStatus::Todo && task.status != TaskStatus::Done) {
        values << "Status: " + status_label(task.status);
    }
    if (task.priority != Priority::None && task.priority != Priority::Normal) {
        values << "Priority: " + priority_label(task.priority);
    }
    const auto due = QDate::fromString(QString::fromStdString(task.due_yaml), Qt::ISODate);
    if (due.isValid()) values << "Due " + due.toString(Qt::ISODate);
    if (!task.tags.empty()) {
        QStringList tags;
        for (const auto& tag : task.tags) tags << "#" + QString::fromStdString(tag);
        values << tags.join(' ');
    }
    if (!is_project_context(project_context, task, snapshot)) {
        const auto project = project_name(snapshot, task.project_id);
        if (!project.isEmpty()) values << "Project: " + project;
    }
    return values.join("  ·  ");
}

QRect completion_rect_for(const QRect& row) {
    return QRect(row.left() + 5, row.center().y() - kCompletionSize / 2, kCompletionSize, kCompletionSize);
}

QRect menu_rect_for(const QRect& row) {
    return QRect(row.right() - kMenuWidth - 3, row.top() + 4, kMenuWidth, row.height() - 8);
}

int title_height(const QFont& font, const QString& title, const int width) {
    const QFontMetrics metrics(font);
    return std::max(metrics.height(), metrics.boundingRect(QRect(0, 0, std::max(width, 60), 10000),
        Qt::TextWordWrap | Qt::TextExpandTabs, title).height());
}

void paint_completion_control(QPainter* painter, const QStyleOptionViewItem& option, const QRect& rect,
                              const TaskStatus status, const bool done) {
    const auto* style = option.widget != nullptr ? option.widget->style() : QApplication::style();
    QStyleOptionButton checkbox;
    checkbox.palette = option.palette;
    checkbox.state = (option.state & QStyle::State_Enabled) | QStyle::State_Off;
    const QSize size(style->pixelMetric(QStyle::PM_IndicatorWidth, &checkbox, option.widget),
                     style->pixelMetric(QStyle::PM_IndicatorHeight, &checkbox, option.widget));
    checkbox.rect = QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter, size, rect);

    painter->save();
    // Let the application style supply the normal unchecked checkbox inset.
    // Paint the accents explicitly so native styles cannot discard status colors.
    style->drawPrimitive(QStyle::PE_IndicatorCheckBox, &checkbox, painter, option.widget);
    const QColor control = status_color(done ? TaskStatus::Done : status);
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(control, 1));
    painter->setBrush(Qt::NoBrush);
    const QRectF border = QRectF(checkbox.rect).adjusted(0.5, 0.5, -0.5, -0.5);
    painter->drawRect(border);
    if (done) {
        painter->setPen(QPen(control, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPolygonF tick{
            QPointF(border.left() + border.width() * 0.22, border.top() + border.height() * 0.52),
            QPointF(border.left() + border.width() * 0.43, border.top() + border.height() * 0.73),
            QPointF(border.left() + border.width() * 0.79, border.top() + border.height() * 0.27)};
        painter->drawPolyline(tick);
    }
    painter->restore();
}

struct TitleContentGeometry {
    QRect icon;
    QRect title;
};

QSize small_icon_size(const QStyleOptionViewItem& option) {
    const auto* style = option.widget != nullptr ? option.widget->style() : QApplication::style();
    const int size = style->pixelMetric(QStyle::PM_SmallIconSize, &option, option.widget);
    return {std::max(1, size), std::max(1, size)};
}

TitleContentGeometry title_content_geometry(const QStyleOptionViewItem& option, const QModelIndex& index,
                                            const QRect& row, const int top_padding, const int bottom_padding) {
    const auto complete = completion_rect_for(row);
    const auto menu = menu_rect_for(row);
    const auto icon = index.data(Qt::DecorationRole).value<QIcon>();
    const auto icon_size = small_icon_size(option);
    const int icon_advance = icon.isNull() ? 0 : icon_size.width() + kIconGap;
    const int base_width = menu.left() - complete.right() - 12;
    const auto title = QRect(complete.right() + 7 + icon_advance, row.top() + top_padding,
        std::max(20, base_width - icon_advance), std::max(1, row.height() - top_padding - bottom_padding));
    const auto icon_rect = icon.isNull() ? QRect{}
        : QRect(complete.right() + 7, row.center().y() - icon_size.height() / 2, icon_size.width(), icon_size.height());
    return {icon_rect, title};
}

void paint_title_icon(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index,
                      const QRect& rect) {
    const auto icon = index.data(Qt::DecorationRole).value<QIcon>();
    if (icon.isNull() || !rect.isValid()) return;
    const auto mode = option.state.testFlag(QStyle::State_Selected) ? QIcon::Selected : QIcon::Normal;
    icon.paint(painter, rect, Qt::AlignCenter, mode);
}

class TaskListDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (index.column() != kTitleColumn) return QStyledItemDelegate::sizeHint(option, index);
        const auto font_value = index.data(Qt::FontRole);
        const auto font = font_value.isValid() ? font_value.value<QFont>() : option.font;
        const auto* view = qobject_cast<const QTreeView*>(parent());
        int depth = 0;
        for (auto ancestor = index.parent(); ancestor.isValid(); ancestor = ancestor.parent()) ++depth;
        const int fallback_width = view != nullptr ? std::max(60, view->columnWidth(index.column())
            - view->indentation() * (depth + 1)) : 420;
        const int width = option.rect.width() > 0 ? option.rect.width() : fallback_width;
        const auto geometry = title_content_geometry(option, index, QRect(0, 0, width, 34), 6, 4);
        const int title = title_height(font, index.data(TaskTitleRole).toString(), geometry.title.width());
        const auto metadata = index.data(TaskMetadataRole).toString();
        const int metadata_height = metadata.isEmpty() ? 0 : QFontMetrics(option.font).height() + 3;
        return {width, std::max(34, title + metadata_height + 14)};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& original, const QModelIndex& index) const override {
        QStyleOptionViewItem option(original);
        initStyleOption(&option, index);
        const auto selected = option.state.testFlag(QStyle::State_Selected);
        const auto row = option.rect;
        const auto background = index.data(Qt::BackgroundRole).value<QBrush>();
        const QColor fill = selected ? option.palette.color(QPalette::Highlight)
            : background.style() == Qt::NoBrush ? option.palette.color(QPalette::Base) : background.color();
        painter->save();
        painter->fillRect(row, fill);

        const auto complete = completion_rect_for(row);
        const bool done = index.data(TaskCompletionRole).toBool();
        const auto status = static_cast<TaskStatus>(index.data(TaskStatusRole).toInt());
        paint_completion_control(painter, option, complete, status, done);

        const auto geometry = title_content_geometry(option, index, row, 6, 4);
        paint_title_icon(painter, option, index, geometry.icon);

        const auto menu = menu_rect_for(row);
        painter->setPen(selected ? option.palette.color(QPalette::HighlightedText) : option.palette.color(QPalette::Mid));
        painter->setFont(option.font);
        painter->drawText(menu, Qt::AlignCenter, "⋯");

        const auto title_font_value = index.data(Qt::FontRole);
        QFont title_font = title_font_value.isValid() ? title_font_value.value<QFont>() : option.font;
        painter->setFont(title_font);
        const QString title = index.data(TaskTitleRole).toString();
        const int text_height = title_height(title_font, title, geometry.title.width());
        const auto title_box = QRect(geometry.title.left(), geometry.title.top(), geometry.title.width(), text_height);
        const auto foreground = index.data(Qt::ForegroundRole).value<QBrush>();
        painter->setPen(selected ? option.palette.color(QPalette::HighlightedText)
            : foreground.style() == Qt::NoBrush ? option.palette.color(QPalette::Text) : foreground.color());
        painter->drawText(title_box, Qt::TextWordWrap | Qt::TextExpandTabs, title);

        const auto metadata = index.data(TaskMetadataRole).toString();
        if (!metadata.isEmpty()) {
            QFont meta_font = option.font;
            meta_font.setPointSizeF(std::max(7.0, meta_font.pointSizeF() - 1.0));
            painter->setFont(meta_font);
            painter->setPen(selected ? option.palette.color(QPalette::HighlightedText)
                : option.palette.color(QPalette::Text));
            const auto meta_rect = QRect(geometry.title.left(), title_box.bottom() + 3, geometry.title.width(), QFontMetrics(meta_font).height());
            painter->drawText(meta_rect, Qt::TextSingleLine, QFontMetrics(meta_font).elidedText(metadata, Qt::ElideRight, meta_rect.width()));
        }
        if (option.state.testFlag(QStyle::State_HasFocus)) {
            painter->setPen(QPen(option.palette.color(QPalette::Highlight), 1));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(row.adjusted(1, 1, -1, -1));
        }
        painter->restore();
    }
};

class TaskTableDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(std::max(32, size.height()));
        return size;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& original, const QModelIndex& index) const override {
        QStyleOptionViewItem option(original);
        initStyleOption(&option, index);
        const auto title = index.data(TaskTitleRole).toString();
        option.text.clear();
        option.icon = QIcon{};
        const auto* style = option.widget != nullptr ? option.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &option, painter, option.widget);
        painter->save();
        painter->setFont(option.font);
        const auto complete = completion_rect_for(option.rect);
        paint_completion_control(painter, option, complete, static_cast<TaskStatus>(index.data(TaskStatusRole).toInt()),
            index.data(TaskCompletionRole).toBool());
        const auto geometry = title_content_geometry(option, index, option.rect, 0, 0);
        paint_title_icon(painter, option, index, geometry.icon);
        const auto menu = menu_rect_for(option.rect);
        const auto foreground = index.data(Qt::ForegroundRole).value<QBrush>();
        painter->setPen(option.state.testFlag(QStyle::State_Selected) ? option.palette.color(QPalette::HighlightedText)
            : foreground.style() == Qt::NoBrush ? option.palette.color(QPalette::Text) : foreground.color());
        painter->drawText(menu, Qt::AlignCenter, "⋯");
        painter->drawText(geometry.title, Qt::AlignVCenter | Qt::TextSingleLine,
            QFontMetrics(option.font).elidedText(title, Qt::ElideRight, geometry.title.width()));
        painter->restore();
    }
};

void apply_formatting(QStandardItem* title, const TaskRecord& task, const Settings& settings) {
    const auto appearance = evaluate_formatting_rules(task, settings.formatting_rules, QDate::currentDate());
    if (appearance.foreground) title->setForeground(QColor(QString::fromStdString(*appearance.foreground)));
    if (appearance.background) title->setBackground(QColor(QString::fromStdString(*appearance.background)));
    auto font = title->font();
    if (appearance.bold) font.setBold(*appearance.bold);
    if (appearance.italic) font.setItalic(*appearance.italic);
    if (appearance.strikethrough) font.setStrikeOut(*appearance.strikethrough);
    title->setFont(font);
    const auto icon = settings.project_icons.find(task.project_id);
    if (icon != settings.project_icons.end() && !icon->second.empty()) title->setIcon(QIcon(QString::fromStdString(icon->second)));
}

QStandardItem* tags_item_for(const TaskRecord& task, const Settings& settings) {
    auto* tags = new QStandardItem(tags_label(task.tags));
    for (const auto& tag : task.tags) {
        const auto color = settings.tag_colors.find(tag);
        if (color == settings.tag_colors.end()) continue;
        const QColor background(QString::fromStdString(color->second));
        if (!background.isValid()) continue;
        tags->setBackground(background);
        tags->setForeground(background.lightness() < 128 ? Qt::white : Qt::black);
        break;
    }
    return tags;
}

void assign_common_roles(const QList<QStandardItem*>& row, const TaskRecord& task, const QVariantMap& progress) {
    const auto id = QString::fromStdString(task.id);
    for (auto* item : row) {
        item->setData(id, TaskIdRole);
        item->setData(static_cast<int>(task.status), TaskStatusRole);
        item->setData(progress, TaskProgressRole);
        item->setData(task.status == TaskStatus::Done, TaskCompletionRole);
        if (!item->data(Qt::AccessibleTextRole).isValid()) item->setData(item->text(), Qt::AccessibleTextRole);
    }
}

}  // namespace

TaskProgressMap count_task_progress(const WorkspaceSnapshot& snapshot) {
    TaskProgressMap progress;
    for (const auto& [id, task] : snapshot.tasks) {
        Q_UNUSED(id);
        if (task.parent_id.empty()) continue;
        auto& counts = progress[task.parent_id];
        ++counts.total;
        if (task.status == TaskStatus::Done) ++counts.completed;
    }
    return progress;
}

QList<QStandardItem*> make_task_row(const TaskRecord& task, const Settings& settings,
                                    const WorkspaceSnapshot& snapshot, const std::string& project_context,
                                    const TaskProgressMap* task_progress) {
    const auto progress = progress_for(task, snapshot, task_progress);
    auto* title = new QStandardItem(QString::fromStdString(task.title));
    title->setFlags(title->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
    title->setData(QString::fromStdString(task.title), TaskTitleRole);
    title->setData(static_cast<int>(task.status), TaskStatusRole);
    title->setData(task.status == TaskStatus::Done, TaskCompletionRole);
    title->setData(progress, TaskProgressRole);
    const auto metadata = list_metadata(task, snapshot, project_context, progress);
    title->setData(metadata, TaskMetadataRole);
    title->setData(QString("%1%2%3").arg(QString::fromStdString(task.title), metadata.isEmpty() ? QString{} : QString("\n") + metadata,
        QString("\n") + (task.status == TaskStatus::Done ? "Reopen task" : "Complete task")), Qt::ToolTipRole);
    title->setData(QString("%1. %2").arg(status_label(task.status), QString::fromStdString(task.title)), Qt::AccessibleTextRole);

    title->setData(task.status == TaskStatus::Done ? "Space: Reopen task. Enter: show details. Shift+F10: task menu."
        : "Space: Complete task. Enter: show details. Shift+F10: task menu.", Qt::AccessibleDescriptionRole);
    apply_formatting(title, task, settings);

    auto* state = new QStandardItem(status_label(task.status));
    state->setForeground(status_color(task.status));
    state->setToolTip("Status: " + status_label(task.status));
    auto* priority = new QStandardItem(priority_label(task.priority));
    priority->setToolTip("Priority: " + priority_label(task.priority));
    const auto due = QDate::fromString(QString::fromStdString(task.due_yaml), Qt::ISODate).toString(Qt::ISODate);
    auto* due_item = new QStandardItem(due);
    due_item->setToolTip(due.isEmpty() ? QString{} : "Due " + due);
    auto* tags = tags_item_for(task, settings);
    auto* project = new QStandardItem(project_name(snapshot, task.project_id));
    QList<QStandardItem*> row{title, state, priority, due_item, tags, project};
    assign_common_roles(row, task, progress);
    return row;
}

void append_task_tree(QStandardItem* parent, const std::string& parent_id,
                      const std::unordered_map<std::string, std::vector<const TaskRecord*>>& children,
                      const Settings& settings, const WorkspaceSnapshot& snapshot,
                      const std::string& project_context, const TaskProgressMap* progress) {
    if (parent == nullptr) return;
    const auto found = children.find(parent_id);
    if (found == children.end()) return;
    for (const auto* task : found->second) {
        if (task == nullptr) continue;
        const auto row = make_task_row(*task, settings, snapshot, project_context, progress);
        parent->appendRow(row);
        append_task_tree(row.front(), task->id, children, settings, snapshot, project_context, progress);
    }
}

TaskTreeView::TaskTreeView(QWidget* parent) : QTreeView(parent) {
    list_delegate_ = new TaskListDelegate(this);
    table_delegate_ = new TaskTableDelegate(this);
    setItemDelegateForColumn(kTitleColumn, list_delegate_);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setExpandsOnDoubleClick(false);
    setUniformRowHeights(false);
    setContextMenuPolicy(Qt::DefaultContextMenu);
    connect(header(), &QHeaderView::sectionResized, this, [this](const int logical, const int, const int size) {
        if (layout_ == "table" && logical == kTitleColumn && size < 280) header()->resizeSection(kTitleColumn, 280);
    });
    set_layout("list");
}

void TaskTreeView::setModel(QAbstractItemModel* model) {
    QTreeView::setModel(model);
    set_layout(layout_, hidden_columns_);
}

void TaskTreeView::set_layout(const QString& requested, const std::vector<int>& hidden_columns) {
    layout_ = requested.compare("table", Qt::CaseInsensitive) == 0 ? "table" : "list";
    const bool table = layout_ == "table";
    if (table) hidden_columns_ = hidden_columns;
    setHeaderHidden(!table);
    setRootIsDecorated(true);
    setItemsExpandable(true);
    setUniformRowHeights(table);
    setItemDelegateForColumn(kTitleColumn, table ? table_delegate_ : list_delegate_);
    const int columns = model() != nullptr ? model()->columnCount() : 6;
    for (int column = 0; column < columns; ++column) {
        const bool hidden = !table ? column != kTitleColumn
            : std::find(hidden_columns_.begin(), hidden_columns_.end(), column) != hidden_columns_.end();
        setColumnHidden(column, hidden);
    }
    if (!table) {
        setColumnHidden(kTitleColumn, false);
        header()->setStretchLastSection(false);
        header()->setSectionResizeMode(kTitleColumn, QHeaderView::Stretch);
    } else {
        setColumnHidden(kTitleColumn, false);
        header()->setStretchLastSection(false);
        header()->setSectionResizeMode(kTitleColumn, QHeaderView::Interactive);
        header()->resizeSection(kTitleColumn, std::max(280, header()->sectionSize(kTitleColumn)));
        for (int column = 1; column < columns; ++column) {
            if (!isColumnHidden(column)) header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
        }
    }
    viewport()->update();
}

QString TaskTreeView::layout() const { return layout_; }

QModelIndex TaskTreeView::task_index_at(const QPoint& position) const {
    const auto index = indexAt(position);
    return index.isValid() ? index.siblingAtColumn(kTitleColumn) : QModelIndex{};
}

QModelIndex TaskTreeView::task_index_for_id(const QString& task_id) const {
    if (task_id.isEmpty() || model() == nullptr || model()->rowCount() == 0) return {};
    const auto matches = model()->match(model()->index(0, kTitleColumn), TaskIdRole, task_id, 1,
        Qt::MatchExactly | Qt::MatchRecursive);
    return matches.isEmpty() ? QModelIndex{} : matches.front().siblingAtColumn(kTitleColumn);
}

QModelIndex TaskTreeView::current_task_index_after_selection(const QModelIndex& index) {
    const auto task_id = index.data(TaskIdRole).toString();
    if (task_id.isEmpty()) return {};
    if (selectionModel()->isSelected(index)) {
        selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
    } else {
        setCurrentIndex(index);
    }
    if (currentIndex().isValid() && currentIndex().data(TaskIdRole).toString() != task_id) return {};
    return task_index_for_id(task_id);
}

QRect TaskTreeView::completion_rect(const QModelIndex& index) const {
    return completion_rect_for(visualRect(index.siblingAtColumn(kTitleColumn)));
}

QRect TaskTreeView::menu_rect(const QModelIndex& index) const {
    return menu_rect_for(visualRect(index.siblingAtColumn(kTitleColumn)));
}

QRect TaskTreeView::disclosure_rect(const QModelIndex& index) const {
    const auto row = visualRect(index.siblingAtColumn(kTitleColumn));
    return QRect(row.left() - indentation(), row.top(), indentation() + 2, row.height());
}

void TaskTreeView::mousePressEvent(QMouseEvent* event) {
    const auto index = task_index_at(event->position().toPoint());
    if (event->button() == Qt::LeftButton && index.isValid()) {
        if (completion_rect(index).contains(event->position().toPoint())) {
            const auto current = current_task_index_after_selection(index);
            if (current.isValid()) emit completionRequested(current);
            event->accept();
            return;
        }
        if (menu_rect(index).contains(event->position().toPoint())) {
            const auto current = current_task_index_after_selection(index);
            if (current.isValid()) emit menuRequested(current, event->globalPosition().toPoint());
            event->accept();
            return;
        }
    }
    press_pos_ = event->position().toPoint();
    press_task_id_ = index.data(TaskIdRole).toString();
    QTreeView::mousePressEvent(event);
}

void TaskTreeView::mouseMoveEvent(QMouseEvent* event) {
    if ((event->buttons() & Qt::LeftButton) && dragEnabled() && !press_task_id_.isEmpty()) {
        const auto distance = (event->position().toPoint() - press_pos_).manhattanLength();
        if (distance < QApplication::startDragDistance() * 2) return;
    }
    QTreeView::mouseMoveEvent(event);
}

void TaskTreeView::mouseDoubleClickEvent(QMouseEvent* event) {
    const auto index = task_index_at(event->position().toPoint());
    if (index.isValid() && event->button() == Qt::LeftButton) {
        if (completion_rect(index).contains(event->position().toPoint())
            || menu_rect(index).contains(event->position().toPoint())
            || disclosure_rect(index).contains(event->position().toPoint())) {
            event->accept();
            return;
        }
        const auto current = current_task_index_after_selection(index);
        if (current.isValid()) emit detailsRequested(current);
        event->accept();
        return;
    }
    QTreeView::mouseDoubleClickEvent(event);
}

void TaskTreeView::resizeEvent(QResizeEvent* event) {
    QTreeView::resizeEvent(event);
    if (layout_ == "list") doItemsLayout();
}

void TaskTreeView::keyPressEvent(QKeyEvent* event) {
    if (completion_key_enabled_ && event->key() == Qt::Key_Space && currentIndex().isValid()) {
        emit completionRequested(currentIndex().siblingAtColumn(kTitleColumn));
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && currentIndex().isValid()) {
        emit detailsRequested(currentIndex().siblingAtColumn(kTitleColumn));
        event->accept();
        return;
    }
    QTreeView::keyPressEvent(event);
}

void TaskTreeView::contextMenuEvent(QContextMenuEvent* event) {
    const auto index = event->reason() == QContextMenuEvent::Keyboard
        ? currentIndex().siblingAtColumn(kTitleColumn) : task_index_at(event->pos());
    const auto target = index.isValid() ? index : currentIndex().siblingAtColumn(kTitleColumn);
    if (target.isValid()) {
        const auto current = current_task_index_after_selection(target);
        if (!current.isValid()) { event->accept(); return; }
        const auto global = event->reason() == QContextMenuEvent::Keyboard
            ? viewport()->mapToGlobal(visualRect(current).center()) : event->globalPos();
        emit menuRequested(current, global);
        event->accept();
        return;
    }
    QTreeView::contextMenuEvent(event);
}

void TaskTreeView::startDrag(const Qt::DropActions actions) {
    drag_source_id_ = !press_task_id_.isEmpty() ? press_task_id_ : currentIndex().data(TaskIdRole).toString();
    QTreeView::startDrag(actions);
}

void TaskTreeView::dropEvent(QDropEvent* event) {
    if (event->source() != this || !drop_handler) {
        event->ignore();
        return;
    }
    const auto accepted = drop_handler(task_index_for_id(drag_source_id_), task_index_at(event->position().toPoint()),
        static_cast<int>(dropIndicatorPosition()));
    drag_source_id_.clear();
    if (accepted) event->acceptProposedAction();
    else event->ignore();
}

}  // namespace todobench
