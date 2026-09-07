// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/icons.h"
#include "app/markdown_editor.h"

#include "domain/markdown_validation.h"

#include <QAction>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTextCharFormat>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextImageFormat>
#include <QTextListFormat>
#include <QToolBar>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>

namespace todobench {
namespace {

bool paste_image_from_mime(const QMimeData* source, const std::function<bool(const QImage&)>& importer) {
    if (source == nullptr || !source->hasImage() || !importer) return false;
    const auto image = qvariant_cast<QImage>(source->imageData());
    return !image.isNull() && importer(image);
}

void select_last(QTextEdit* editor, const QString& text) {
    if (editor == nullptr || text.isEmpty()) return;
    const auto found = editor->document()->find(text, editor->textCursor().position(), QTextDocument::FindBackward);
    if (!found.isNull()) editor->setTextCursor(found);
}

void select_last(QPlainTextEdit* editor, const QString& text) {
    if (editor == nullptr || text.isEmpty()) return;
    const auto found = editor->document()->find(text, editor->textCursor().position(), QTextDocument::FindBackward);
    if (!found.isNull()) editor->setTextCursor(found);
}

void wrap_source_selection(QPlainTextEdit* editor, const QString& before, const QString& after,
                           const QString& placeholder = "text") {
    auto cursor = editor->textCursor();
    if (cursor.hasSelection()) {
        cursor.insertText(before + cursor.selectedText() + after);
        editor->setTextCursor(cursor);
        return;
    }
    cursor.insertText(before + placeholder + after);
    editor->setTextCursor(cursor);
    select_last(editor, placeholder);
}

void apply_character_format(QTextEdit* editor, const QTextCharFormat& format) {
    auto cursor = editor->textCursor();
    if (cursor.hasSelection()) cursor.mergeCharFormat(format);
    editor->mergeCurrentCharFormat(format);
}

QString selected_visual_text(const QTextCursor& cursor) {
    auto text = cursor.selectedText();
    text.replace(QChar::ParagraphSeparator, '\n');
    return text;
}

void insert_visual_markdown(QTextEdit* editor, const QString& markdown, const QString& placeholder = {}) {
    auto cursor = editor->textCursor();
    cursor.insertMarkdown(markdown);
    editor->setTextCursor(cursor);
    select_last(editor, placeholder);
}

void apply_visual_inline(QTextEdit* editor, const QTextCharFormat& format, const QString& markdown_before,
                         const QString& placeholder) {
    if (editor->textCursor().hasSelection()) {
        apply_character_format(editor, format);
        return;
    }
    insert_visual_markdown(editor, markdown_before + placeholder + markdown_before, placeholder);
}

void apply_visual_list(QTextEdit* editor, QTextListFormat::Style style, QTextBlockFormat::MarkerType marker) {
    auto cursor = editor->textCursor();
    QTextListFormat format;
    format.setStyle(style);
    cursor.createList(format);
    if (marker != QTextBlockFormat::MarkerType::NoMarker) {
        auto block = cursor.blockFormat();
        block.setMarker(marker);
        cursor.setBlockFormat(block);
    }
    if (!cursor.block().text().trimmed().isEmpty()) {
        editor->setTextCursor(cursor);
        return;
    }
    cursor.insertText("item");
    editor->setTextCursor(cursor);
    select_last(editor, "item");
}

void insert_source_prefixed_line(QPlainTextEdit* editor, const QString& prefix, const QString& placeholder) {
    auto cursor = editor->textCursor();
    const auto selected = cursor.selectedText();
    cursor.insertText("\n" + prefix + (selected.isEmpty() ? placeholder : selected));
    editor->setTextCursor(cursor);
    if (selected.isEmpty()) select_last(editor, placeholder);
}

void insert_source_code(QPlainTextEdit* editor) {
    auto cursor = editor->textCursor();
    auto selected = cursor.selectedText();
    selected.replace(QChar::ParagraphSeparator, '\n');
    if (selected.isEmpty()) {
        cursor.insertText("\n```\ncode\n```\n");
        editor->setTextCursor(cursor);
        select_last(editor, "code");
        return;
    }
    if (selected.contains('\n')) cursor.insertText("```\n" + selected + "\n```\n");
    else cursor.insertText("`" + selected + "`");
    editor->setTextCursor(cursor);
}

void insert_visual_code(QTextEdit* editor) {
    auto cursor = editor->textCursor();
    const auto selected = selected_visual_text(cursor);
    if (!selected.isEmpty()) {
        if (selected.contains('\n')) insert_visual_markdown(editor, "```\n" + selected + "\n```\n");
        else insert_visual_markdown(editor, "`" + selected + "`");
        return;
    }
    cursor.movePosition(QTextCursor::EndOfBlock);
    editor->setTextCursor(cursor);
    insert_visual_markdown(editor, "\n\n```\ncode\n```\n", "code");
}

class VisualTextEdit final : public QTextEdit {
public:
    explicit VisualTextEdit(QWidget* parent = nullptr) : QTextEdit(parent) {}
    std::function<bool(const QImage&)> paste_image;
    std::filesystem::path base_directory;

protected:
    void insertFromMimeData(const QMimeData* source) override {
        if (paste_image_from_mime(source, paste_image)) return;
        QTextEdit::insertFromMimeData(source);
    }

    QVariant loadResource(int type, const QUrl& name) override {
        if (type != QTextDocument::ImageResource) return QTextEdit::loadResource(type, name);
        if (name.scheme() == "http" || name.scheme() == "https") return QImage();
        QString file = name.isLocalFile() ? name.toLocalFile() : name.toString();
        if (!name.isLocalFile() && !base_directory.empty()) {
            file = QString::fromStdString((base_directory / name.toString().toStdString()).string());
        }
        const QImage image(file);
        return image.isNull() ? QImage() : image;
    }
};

class SourceTextEdit final : public QPlainTextEdit {
public:
    explicit SourceTextEdit(QWidget* parent = nullptr) : QPlainTextEdit(parent) {}
    std::function<bool(const QImage&)> paste_image;

protected:
    void insertFromMimeData(const QMimeData* source) override {
        if (paste_image_from_mime(source, paste_image)) return;
        QPlainTextEdit::insertFromMimeData(source);
    }
};

}  // namespace

MarkdownEditor::MarkdownEditor(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* toolbar = new QToolBar(this);
    toolbar_ = toolbar;
    toolbar->setIconSize(QSize(20, 20));
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->setObjectName("markdownFormatting");
    auto add_button = [this, toolbar](const QString& label, const QString& tooltip, const QString& icon, auto callback) {
        auto* button = toolbar->addAction(label);
        button->setToolTip(tooltip);
        set_action_icon(button, icon);
        connect(button, &QAction::triggered, this, callback);
    };
    add_button("H1", "Heading 1", "heading-1", [this] { insert_heading(1); });
    add_button("H2", "Heading 2", "heading-2", [this] { insert_heading(2); });
    add_button("H3", "Heading 3", "heading-3", [this] { insert_heading(3); });
    toolbar->addSeparator();
    add_button("B", "Bold", "bold", [this] { insert_bold(); });
    add_button("I", "Italic", "italic", [this] { insert_italic(); });
    add_button("S", "Strikethrough", "strikethrough", [this] { insert_strikethrough(); });
    toolbar->addSeparator();
    add_button("List", "Bulleted list", "list", [this] { insert_bullets(); });
    add_button("1. List", "Numbered list", "list-ordered", [this] { insert_numbered(); });
    add_button("Checklist", "Checklist", "list-checks", [this] { insert_checklist(); });
    toolbar->addSeparator();
    add_button("Quote", "Block quote", "quote", [this] { insert_quote(); });
    add_button("Code", "Fenced code", "code", [this] { insert_code(); });
    toolbar->addSeparator();
    add_button("Link", "Insert link", "link", [this] { insert_link(); });
    add_button("Image", "Insert image", "image", [this] { insert_image(); });
    add_button("Table", "Insert table", "table", [this] { insert_table(); });
    layout->addWidget(toolbar);

    tabs_ = new QTabWidget(this);
    auto* visual = new VisualTextEdit(tabs_);
    auto* source = new SourceTextEdit(tabs_);
    visual_ = visual;
    source_ = source;
    source_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    visual->paste_image = [this](const QImage& image) { return paste_image(image); };
    source->paste_image = [this](const QImage& image) { return paste_image(image); };
    tabs_->addTab(visual_, "Visual");
    tabs_->addTab(source_, "Source");
    auto* history_page = new QWidget(tabs_);
    auto* history_layout = new QHBoxLayout(history_page);
    history_list_ = new QListWidget(history_page);
    history_view_ = new QPlainTextEdit(history_page);
    history_view_->setReadOnly(true);
    history_layout->addWidget(history_list_, 1);
    history_layout->addWidget(history_view_, 2);
    tabs_->addTab(history_page, "History");
    layout->addWidget(tabs_);
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        visual_to_source_if_dirty();
        if (index == 0) source_to_visual();
        apply_editable_state();
    });
    connect(visual_, &QTextEdit::textChanged, this, [this] {
        if (!updating_) visual_dirty_ = true;
        update_dirty_state();
    });
    connect(source_, &QPlainTextEdit::textChanged, this, [this] {
        if (!updating_) source_dirty_ = true;
        update_dirty_state();
    });
    connect(history_list_, &QListWidget::currentRowChanged, this, [this](int row) { show_history_row(row); });
}

void MarkdownEditor::set_markdown(const std::string& markdown) {
    updating_ = true;
    original_markdown_ = markdown;
    source_->setPlainText(QString::fromStdString(markdown));
    original_source_text_ = source_->toPlainText();
    const auto validation = validate_markdown_for_visual(markdown);
    visual_supported_ = validation.supported;
    visual_->setToolTip(QString::fromStdString(validation.message));
    visual_->setMarkdown(QString::fromStdString(markdown));
    source_dirty_ = false;
    visual_dirty_ = false;
    apply_editable_state();
    update_dirty_state();
    updating_ = false;
}

std::string MarkdownEditor::markdown() const {
    if (!visual_dirty_ && source_->toPlainText() == original_source_text_) return original_markdown_;
    if (!visual_dirty_) return source_->toPlainText().toStdString();
    return visual_->toMarkdown().toStdString();
}

bool MarkdownEditor::is_dirty() const { return markdown() != original_markdown_; }

void MarkdownEditor::mark_clean() {
    original_markdown_ = markdown();
    if (!visual_dirty_) original_source_text_ = source_->toPlainText();
    update_dirty_state();
}

void MarkdownEditor::set_editable(bool editable) {
    editable_ = editable;
    apply_editable_state();
}

void MarkdownEditor::set_task_directory(const std::filesystem::path& directory) {
    task_directory_ = directory;
    auto* visual = static_cast<VisualTextEdit*>(visual_);
    visual->base_directory = directory;
    visual->document()->setBaseUrl(QUrl::fromLocalFile(QString::fromStdString(directory.string()) + "/"));
}

void MarkdownEditor::set_image_importer(std::function<std::string(const QImage&)> importer) {
    image_importer_ = std::move(importer);
}

void MarkdownEditor::set_history(const std::vector<HistoryEntry>& entries) {
    history_entries_ = entries;
    history_list_->clear();
    for (const auto& entry : history_entries_) history_list_->addItem(QString::fromStdString(entry.label));
    if (history_list_->count() > 0) history_list_->setCurrentRow(0);
    else history_view_->clear();
}

bool MarkdownEditor::has_note_focus() const {
    return visual_->hasFocus() || source_->hasFocus() || history_view_->hasFocus() || history_list_->hasFocus();
}

void MarkdownEditor::find_in_notes() {
    bool accepted = false;
    const auto query = QInputDialog::getText(this, "Find in notes", "Find", QLineEdit::Normal, {}, &accepted);
    if (!accepted || query.isEmpty()) return;
    if (tabs_->currentIndex() == 1) source_->find(query);
    else if (tabs_->currentIndex() == 0) visual_->find(query);
    else history_view_->find(query);
}

void MarkdownEditor::visual_to_source_if_dirty() {
    if (!visual_dirty_) return;
    updating_ = true;
    source_->setPlainText(visual_->toMarkdown());
    source_dirty_ = false;
    visual_dirty_ = false;
    update_dirty_state();
    updating_ = false;
}

void MarkdownEditor::source_to_visual() {
    updating_ = true;
    const auto markdown = source_->toPlainText().toStdString();
    const auto validation = validate_markdown_for_visual(markdown);
    visual_supported_ = validation.supported;
    visual_->setToolTip(QString::fromStdString(validation.message));
    visual_->setMarkdown(QString::fromStdString(markdown));
    source_dirty_ = false;
    visual_dirty_ = false;
    apply_editable_state();
    update_dirty_state();
    updating_ = false;
}

void MarkdownEditor::update_dirty_state() {
    setProperty("dirty", is_dirty());
    if (!updating_) emit edited();
}

void MarkdownEditor::show_history_row(int row) {
    if (row < 0 || row >= static_cast<int>(history_entries_.size())) {
        history_view_->clear();
        return;
    }
    history_view_->setPlainText(QString::fromStdString(history_entries_[static_cast<size_t>(row)].markdown));
}

bool MarkdownEditor::paste_image(const QImage& image) {
    if (!can_format() || !image_importer_ || image.isNull()) return false;
    const auto link = image_importer_(image);
    if (link.empty()) return false;
    const auto markdown = QString("![](%1)").arg(QString::fromStdString(link));
    if (tabs_->currentIndex() == 1) insert_into_source(markdown);
    else {
        QTextImageFormat format;
        format.setName(QString::fromStdString(link));
        visual_->textCursor().insertImage(format);
    }
    return true;
}

bool MarkdownEditor::can_format() const {
    return editable_ && (tabs_->currentIndex() == 1
        || (tabs_->currentIndex() == 0 && visual_supported_));
}

void MarkdownEditor::apply_editable_state() {
    toolbar_->setEnabled(can_format());
    source_->setReadOnly(!editable_);
    visual_->setReadOnly(!editable_ || !visual_supported_);
}

void MarkdownEditor::insert_bold() {
    if (!can_format()) return;
    if (tabs_->currentIndex() == 1) {
        wrap_source_selection(source_, "**", "**");
        source_dirty_ = true;
    } else {
        QTextCharFormat format;
        format.setFontWeight(visual_->fontWeight() >= QFont::Bold ? QFont::Normal : QFont::Bold);
        apply_visual_inline(visual_, format, "**", "text");
        visual_dirty_ = true;
    }
    update_dirty_state();
}

void MarkdownEditor::insert_italic() {
    if (!can_format()) return;
    if (tabs_->currentIndex() == 1) {
        wrap_source_selection(source_, "*", "*");
        source_dirty_ = true;
    } else {
        QTextCharFormat format;
        format.setFontItalic(!visual_->fontItalic());
        apply_visual_inline(visual_, format, "*", "text");
        visual_dirty_ = true;
    }
    update_dirty_state();
}

void MarkdownEditor::insert_bullets() {
    if (!can_format()) return;
    if (tabs_->currentIndex() == 1) {
        insert_source_prefixed_line(source_, "- ", "item");
        source_dirty_ = true;
    } else {
        apply_visual_list(visual_, QTextListFormat::ListDisc, QTextBlockFormat::MarkerType::NoMarker);
        visual_dirty_ = true;
    }
    update_dirty_state();
}

void MarkdownEditor::insert_checklist() {
    if (!can_format()) return;
    if (tabs_->currentIndex() == 1) {
        insert_source_prefixed_line(source_, "- [ ] ", "item");
        source_dirty_ = true;
    } else {
        apply_visual_list(visual_, QTextListFormat::ListDisc, QTextBlockFormat::MarkerType::Unchecked);
        visual_dirty_ = true;
    }
    update_dirty_state();
}

void MarkdownEditor::insert_into_source(const QString& text) {
    if (!can_format()) return;
    source_->insertPlainText(text);
    source_dirty_ = true;
    update_dirty_state();
}

void MarkdownEditor::insert_strikethrough() {
    if (!can_format()) return;
    if (tabs_->currentIndex() == 1) {
        wrap_source_selection(source_, "~~", "~~");
        source_dirty_ = true;
        update_dirty_state();
        return;
    }
    auto format = visual_->currentCharFormat();
    format.setFontStrikeOut(!format.fontStrikeOut());
    apply_visual_inline(visual_, format, "~~", "text");
    visual_dirty_ = true;
    update_dirty_state();
}

void MarkdownEditor::insert_heading(int level) {
    if (!can_format()) return;
    const auto marks = QString(std::clamp(level, 1, 3), QChar('#'));
    if (tabs_->currentIndex() == 1) {
        insert_source_prefixed_line(source_, marks + " ", "Heading");
        source_dirty_ = true;
        update_dirty_state();
        return;
    }
    auto cursor = visual_->textCursor();
    if (cursor.block().text().trimmed().isEmpty()) cursor.insertText("Heading");
    auto format = cursor.blockFormat();
    format.setHeadingLevel(std::clamp(level, 1, 3));
    cursor.setBlockFormat(format);
    visual_->setTextCursor(cursor);
    visual_dirty_ = true;
    update_dirty_state();
}

void MarkdownEditor::insert_numbered() {
    if (!can_format()) return;
    if (tabs_->currentIndex() == 1) {
        insert_source_prefixed_line(source_, "1. ", "item");
        source_dirty_ = true;
    } else {
        apply_visual_list(visual_, QTextListFormat::ListDecimal, QTextBlockFormat::MarkerType::NoMarker);
        visual_dirty_ = true;
    }
    update_dirty_state();
}

void MarkdownEditor::insert_quote() {
    if (!can_format()) return;
    if (tabs_->currentIndex() == 1) {
        insert_source_prefixed_line(source_, "> ", "quote");
        source_dirty_ = true;
        update_dirty_state();
        return;
    }
    auto cursor = visual_->textCursor();
    cursor.movePosition(QTextCursor::StartOfBlock);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    auto text = cursor.selectedText().trimmed();
    const auto placeholder = text.isEmpty();
    if (placeholder) text = "quote";
    cursor.insertMarkdown("> " + text + "\n");
    visual_->setTextCursor(cursor);
    if (placeholder) select_last(visual_, "quote");
    visual_dirty_ = true;
    update_dirty_state();
}

void MarkdownEditor::insert_code() {
    if (!can_format()) return;
    if (tabs_->currentIndex() == 1) {
        insert_source_code(source_);
        source_dirty_ = true;
    } else {
        insert_visual_code(visual_);
        visual_dirty_ = true;
    }
    update_dirty_state();
}

void MarkdownEditor::insert_link() {
    if (!can_format()) return;
    if (tabs_->currentIndex() == 1) {
        wrap_source_selection(source_, "[", "](url)", "text");
        source_dirty_ = true;
        update_dirty_state();
        return;
    }
    bool accepted = false;
    const auto url = QInputDialog::getText(this, "Insert link", "URL", QLineEdit::Normal, "https://", &accepted);
    if (!accepted || url.trimmed().isEmpty()) return;
    auto cursor = visual_->textCursor();
    const auto label = cursor.hasSelection() ? cursor.selectedText() : url.trimmed();
    insert_visual_markdown(visual_, "[" + label + "](" + url.trimmed() + ")");
    visual_dirty_ = true;
    update_dirty_state();
}

void MarkdownEditor::insert_image() {
    if (!can_format()) return;
    if (tabs_->currentIndex() == 1) {
        insert_into_source("![](assets/image.png)");
        return;
    }
    const auto file = QFileDialog::getOpenFileName(this, "Insert image", {}, "Images (*.png *.jpg *.jpeg *.webp *.bmp)");
    if (file.isEmpty()) return;
    paste_image(QImage(file));
    visual_dirty_ = true;
    update_dirty_state();
}

void MarkdownEditor::insert_table() {
    if (!can_format()) return;
    const auto table = QString("\n| Column | Column |\n| --- | --- |\n| text | text |\n");
    if (tabs_->currentIndex() == 1) {
        insert_into_source(table);
        return;
    }
    insert_visual_markdown(visual_, table, "text");
    visual_dirty_ = true;
    update_dirty_state();
}

}  // namespace todobench
