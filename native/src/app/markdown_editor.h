// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "storage/history_store.h"

#include <QImage>
#include <QString>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class QListWidget;
class QPlainTextEdit;
class QTabWidget;
class QTextEdit;
class QToolBar;

namespace todobench {

class MarkdownEditor final : public QWidget {
    Q_OBJECT
public:
    explicit MarkdownEditor(QWidget* parent = nullptr);

    void set_markdown(const std::string& markdown);
    std::string markdown() const;
    bool is_dirty() const;
    void mark_clean();
    bool visual_supported() const { return visual_supported_; }
    void set_editable(bool editable);
    void set_task_directory(const std::filesystem::path& directory);
    void set_image_importer(std::function<std::string(const QImage&)> importer);
    void set_history(const std::vector<HistoryEntry>& entries);
    bool has_note_focus() const;
    void find_in_notes();

    void insert_bold();
    void insert_italic();
    void insert_strikethrough();
    void insert_heading(int level);
    void insert_bullets();
    void insert_numbered();
    void insert_checklist();
    void insert_quote();
    void insert_code();
    void insert_link();
    void insert_image();
    void insert_table();

signals:
    void edited();

private:
    void visual_to_source_if_dirty();
    void source_to_visual();
    void update_dirty_state();
    void insert_into_source(const QString& text);
    void show_history_row(int row);
    bool paste_image(const QImage& image);
    void apply_editable_state();
    bool can_format() const;

    QTabWidget* tabs_{nullptr};
    QToolBar* toolbar_{nullptr};
    QTextEdit* visual_{nullptr};
    QPlainTextEdit* source_{nullptr};
    QListWidget* history_list_{nullptr};
    QPlainTextEdit* history_view_{nullptr};
    std::string original_markdown_;
    QString original_source_text_;
    std::filesystem::path task_directory_;
    std::function<std::string(const QImage&)> image_importer_;
    std::vector<HistoryEntry> history_entries_;
    bool source_dirty_{false};
    bool visual_dirty_{false};
    bool updating_{false};
    bool visual_supported_{false};
    bool editable_{true};
};

}  // namespace todobench
