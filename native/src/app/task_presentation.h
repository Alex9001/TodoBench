// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "domain/model.h"
#include "storage/settings_codec.h"

#include <QList>
#include <QPoint>
#include <QTreeView>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class QContextMenuEvent;
class QAbstractItemDelegate;
class QAbstractItemModel;
class QKeyEvent;
class QMouseEvent;
class QDropEvent;
class QResizeEvent;
class QStandardItem;

namespace todobench {

// Every column in a task row carries the identity so selection and drag/drop
// commands can use either the list or table presentation.
constexpr int TaskIdRole = Qt::UserRole + 1;
constexpr int TaskStatusRole = Qt::UserRole + 2;
constexpr int TaskMetadataRole = Qt::UserRole + 3;
constexpr int TaskProgressRole = Qt::UserRole + 4;
constexpr int TaskCompletionRole = Qt::UserRole + 5;
constexpr int TaskTitleRole = Qt::UserRole + 6;

struct TaskProgress {
    int completed{0};
    int total{0};
};
using TaskProgressMap = std::unordered_map<std::string, TaskProgress>;

TaskProgressMap count_task_progress(const WorkspaceSnapshot& snapshot);

QList<QStandardItem*> make_task_row(const TaskRecord& task, const Settings& settings,
                                    const WorkspaceSnapshot& snapshot,
                                    const std::string& project_context = {},
                                    const TaskProgressMap* progress = nullptr);

void append_task_tree(QStandardItem* parent, const std::string& parent_id,
                      const std::unordered_map<std::string, std::vector<const TaskRecord*>>& children,
                      const Settings& settings, const WorkspaceSnapshot& snapshot,
                      const std::string& project_context = {},
                      const TaskProgressMap* progress = nullptr);

class TaskTreeView final : public QTreeView {
    Q_OBJECT

public:
    using DropHandler = std::function<bool(const QModelIndex&, const QModelIndex&, int)>;

    explicit TaskTreeView(QWidget* parent = nullptr);

    DropHandler drop_handler;

    void setModel(QAbstractItemModel* model) override;
    void set_layout(const QString& layout, const std::vector<int>& hidden_columns = {});
    [[nodiscard]] QString layout() const;
    void set_completion_key_enabled(bool enabled) { completion_key_enabled_ = enabled; }

signals:
    void completionRequested(const QModelIndex& index);
    void menuRequested(const QModelIndex& index, const QPoint& global_pos);
    void detailsRequested(const QModelIndex& index);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void startDrag(Qt::DropActions actions) override;
    void dropEvent(QDropEvent* event) override;

private:
    [[nodiscard]] QModelIndex task_index_at(const QPoint& position) const;
    [[nodiscard]] QModelIndex task_index_for_id(const QString& task_id) const;
    [[nodiscard]] QModelIndex current_task_index_after_selection(const QModelIndex& index);
    [[nodiscard]] QRect completion_rect(const QModelIndex& index) const;
    [[nodiscard]] QRect menu_rect(const QModelIndex& index) const;
    [[nodiscard]] QRect disclosure_rect(const QModelIndex& index) const;

    QString layout_{"list"};
    bool completion_key_enabled_{true};
    QString drag_source_id_;
    QString press_task_id_;
    QPoint press_pos_;
    QAbstractItemDelegate* list_delegate_{nullptr};
    QAbstractItemDelegate* table_delegate_{nullptr};
    std::vector<int> hidden_columns_;
};

}  // namespace todobench
