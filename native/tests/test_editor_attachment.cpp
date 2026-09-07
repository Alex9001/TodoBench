// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/markdown_editor.h"
#include "domain/markdown_validation.h"
#include "storage/attachment_store.h"

#include <QApplication>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTest>
#include <QTextEdit>
#include <QTextCursor>
#include <QToolBar>

#include <filesystem>
#include <fstream>

using namespace todobench;

class EditorAttachmentTest final : public QObject {
    Q_OBJECT
private slots:
    void historyPreservesPendingEdits_data();
    void historyPreservesPendingEdits();
    void formattingCannotMutateProtectedNotes_data();
    void formattingCannotMutateProtectedNotes();
    void sourceModePreservesUntouchedMarkdown();
    void sourceEditsAreRetainedWhenSwitchingModes();
    void sourceFormattingActionsInsertMarkdown();
    void sourceFormattingWrapsSelection();
    void attachmentImportUsesSafeRelativeAssetLink();
    void attachmentRejectsUnsafeExtension();
    void visualValidationFailsClosedWhenUnsupported();
    void sourceTypingPreservesInsertedText();
    void visualTypingPreservesInsertedText();
    void ordinaryMarkdownEnablesVisualEditing();
    void markCleanClearsDirtyAfterEdit();
    void historyTabShowsSnapshotMarkdown();
    void importBytesWritesSafePngAttachment();
    void visualChecklistRoundTripsThroughSource();
    void visualCodeInsertsWithoutSelection();
    void visualCodeWrapsSelection();
    void sourceChecklistSurvivesVisualTab();
    void visualQuoteAndHeadingRoundTrip();
    void visualBoldInsertsWithoutSelection();
};

void EditorAttachmentTest::historyPreservesPendingEdits_data() {
    QTest::addColumn<int>("mode");
    QTest::newRow("visual") << 0;
    QTest::newRow("source") << 1;
}

void EditorAttachmentTest::historyPreservesPendingEdits() {
    QFETCH(int, mode);
    MarkdownEditor editor;
    editor.set_markdown("original");
    auto* tabs = editor.findChild<QTabWidget*>();
    tabs->setCurrentIndex(mode);
    if (mode == 0) editor.findChild<QTextEdit*>()->setPlainText("pending edits");
    else editor.findChild<QPlainTextEdit*>()->setPlainText("pending edits");
    const auto pending = editor.markdown();
    tabs->setCurrentIndex(2);
    QCOMPARE(editor.markdown(), pending);
    tabs->setCurrentIndex(0);
    QCOMPARE(editor.markdown(), pending);
    tabs->setCurrentIndex(1);
    QCOMPARE(editor.markdown(), pending);
    QVERIFY(editor.is_dirty());
}

void EditorAttachmentTest::formattingCannotMutateProtectedNotes_data() {
    QTest::addColumn<int>("mode");
    QTest::addColumn<bool>("editable");
    QTest::newRow("readonly-visual") << 0 << false;
    QTest::newRow("readonly-source") << 1 << false;
    QTest::newRow("history") << 2 << true;
}

void EditorAttachmentTest::formattingCannotMutateProtectedNotes() {
    QFETCH(int, mode);
    QFETCH(bool, editable);
    MarkdownEditor editor;
    editor.set_markdown("original");
    editor.findChild<QTabWidget*>()->setCurrentIndex(mode);
    editor.set_editable(editable);
    editor.insert_heading(2);
    editor.insert_bold();
    editor.insert_italic();
    editor.insert_strikethrough();
    editor.insert_bullets();
    editor.insert_numbered();
    editor.insert_checklist();
    editor.insert_quote();
    editor.insert_code();
    editor.insert_link();
    editor.insert_image();
    editor.insert_table();
    QCOMPARE(editor.markdown(), std::string("original"));
    QVERIFY(!editor.is_dirty());
    QVERIFY(!editor.findChild<QToolBar*>()->isEnabled());
}

void EditorAttachmentTest::sourceModePreservesUntouchedMarkdown() {
    MarkdownEditor editor;
    const std::string markdown = "# Heading\n\nUnknown   spacing\n- [ ] item\n";
    editor.set_markdown(markdown);
    auto* tabs = editor.findChild<QTabWidget*>();
    QVERIFY(tabs != nullptr);
    QCOMPARE(editor.markdown(), markdown);
    tabs->setCurrentIndex(1);
    QCOMPARE(editor.markdown(), markdown);
    QVERIFY(!editor.is_dirty());
}

void EditorAttachmentTest::sourceEditsAreRetainedWhenSwitchingModes() {
    MarkdownEditor editor;
    editor.set_markdown("plain text");
    auto* tabs = editor.findChild<QTabWidget*>();
    auto* source = editor.findChild<QPlainTextEdit*>();
    QVERIFY(tabs != nullptr);
    QVERIFY(source != nullptr);
    tabs->setCurrentIndex(1);
    source->setPlainText("**edited**");
    QCOMPARE(editor.markdown(), std::string("**edited**"));
    tabs->setCurrentIndex(0);
    tabs->setCurrentIndex(1);
    QCOMPARE(editor.markdown(), std::string("**edited**"));
    QVERIFY(editor.is_dirty());
}

void EditorAttachmentTest::sourceFormattingActionsInsertMarkdown() {
    MarkdownEditor editor;
    editor.set_markdown("");
    auto* tabs = editor.findChild<QTabWidget*>();
    auto* source = editor.findChild<QPlainTextEdit*>();
    QVERIFY(tabs != nullptr);
    QVERIFY(source != nullptr);
    tabs->setCurrentIndex(1);

    editor.insert_heading(2);
    editor.insert_bold();
    editor.insert_italic();
    editor.insert_strikethrough();
    editor.insert_bullets();
    editor.insert_numbered();
    editor.insert_checklist();
    editor.insert_quote();
    editor.insert_code();
    editor.insert_link();
    editor.insert_image();
    editor.insert_table();

    const auto markdown = source->toPlainText().toStdString();
    const auto has_formatting = markdown.find("## ") != std::string::npos && markdown.find("**") != std::string::npos
        && markdown.find("~~") != std::string::npos && markdown.find("1. ") != std::string::npos
        && markdown.find("- [ ] ") != std::string::npos && markdown.find("> ") != std::string::npos
        && markdown.find("```") != std::string::npos && markdown.find("](url)") != std::string::npos
        && markdown.find("![](assets/image.png)") != std::string::npos && markdown.find("| Column |") != std::string::npos;
    QVERIFY(has_formatting);
    QVERIFY(editor.is_dirty());
}

void EditorAttachmentTest::sourceFormattingWrapsSelection() {
    MarkdownEditor editor;
    editor.set_markdown("important");
    auto* tabs = editor.findChild<QTabWidget*>();
    auto* source = editor.findChild<QPlainTextEdit*>();
    QVERIFY(tabs != nullptr);
    QVERIFY(source != nullptr);
    tabs->setCurrentIndex(1);
    source->selectAll();
    editor.insert_bold();
    QCOMPARE(source->toPlainText(), QString("**important**"));
}

void EditorAttachmentTest::attachmentImportUsesSafeRelativeAssetLink() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdString());
    const auto source = root / "source image.png";
    {
        std::ofstream output(source, std::ios::binary);
        output << "attachment bytes";
    }

    const auto result = AttachmentStore::import_file(root / "task", source);
    QVERIFY(result.success);
    QVERIFY(result.relative_link.starts_with("assets/"));
    QVERIFY(result.relative_link.find('/') == result.relative_link.rfind('/'));
    QVERIFY(std::filesystem::is_regular_file(result.stored_path));
    QCOMPARE(std::filesystem::file_size(result.stored_path), static_cast<std::uintmax_t>(16));
}

void EditorAttachmentTest::attachmentRejectsUnsafeExtension() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdString());
    const auto source = root / "payload.bad extension";
    std::ofstream(source) << "data";

    const auto result = AttachmentStore::import_file(root / "task", source);
    QVERIFY(!result.success);
    QVERIFY(QString::fromStdString(result.error).contains("unsafe"));
}

void EditorAttachmentTest::sourceTypingPreservesInsertedText() {
    MarkdownEditor editor;
    editor.set_markdown("start");
    auto* tabs = editor.findChild<QTabWidget*>();
    auto* source = editor.findChild<QPlainTextEdit*>();
    QVERIFY(tabs != nullptr);
    QVERIFY(source != nullptr);
    tabs->setCurrentIndex(1);
    source->setFocus();
    source->moveCursor(QTextCursor::End);
    QTest::keyClicks(source, " typed text");
    QCOMPARE(source->toPlainText(), QString("start typed text"));
    QCOMPARE(editor.markdown(), std::string("start typed text"));
}

void EditorAttachmentTest::visualTypingPreservesInsertedText() {
    MarkdownEditor editor;
    editor.set_markdown("start");
    auto* tabs = editor.findChild<QTabWidget*>();
    auto* visual = editor.findChild<QTextEdit*>();
    QVERIFY(tabs != nullptr);
    QVERIFY(visual != nullptr);
    QVERIFY(editor.visual_supported());
    tabs->setCurrentIndex(0);
    visual->setFocus();
    visual->moveCursor(QTextCursor::End);
    QTest::keyClicks(visual, " typed text");
    QVERIFY(visual->toPlainText().contains("typed text"));
    QVERIFY(editor.markdown().find("typed text") != std::string::npos);
}

void EditorAttachmentTest::visualValidationFailsClosedWhenUnsupported() {
    const auto validation = validate_markdown_for_visual("<script>alert(1)</script>\n");
    QVERIFY(!validation.supported);
    QVERIFY(!validation.message.empty());
    MarkdownEditor editor;
    editor.set_markdown("<script>alert(1)</script>\n");
    QVERIFY(!editor.visual_supported());
}

namespace {
bool ordinary_markdown_enables_visual_editing() {
    if (!validate_markdown_for_visual("ordinary notes").supported) return false;
    MarkdownEditor editor;
    editor.set_markdown("ordinary notes");
    auto* visual = editor.findChild<QTextEdit*>();
    return editor.visual_supported() && visual != nullptr && !visual->isReadOnly();
}

bool mark_clean_clears_dirty_after_source_edit() {
    MarkdownEditor editor;
    editor.set_markdown("start");
    auto* tabs = editor.findChild<QTabWidget*>();
    auto* source = editor.findChild<QPlainTextEdit*>();
    if (tabs == nullptr || source == nullptr) return false;
    tabs->setCurrentIndex(1);
    source->setPlainText("start typed");
    if (!editor.is_dirty() || editor.markdown() != "start typed") return false;
    editor.mark_clean();
    return !editor.is_dirty() && editor.markdown() == "start typed";
}
}  // namespace

void EditorAttachmentTest::ordinaryMarkdownEnablesVisualEditing() {
    QVERIFY(ordinary_markdown_enables_visual_editing());
}

void EditorAttachmentTest::markCleanClearsDirtyAfterEdit() {
    QVERIFY(mark_clean_clears_dirty_after_source_edit());
}

namespace {
bool history_tab_shows_snapshot_markdown() {
    MarkdownEditor editor;
    editor.set_history({{"completion-1", "unused.md", "completion-1", "---\ntitle: Old\n---\n\nPrior notes\n"}});
    auto* tabs = editor.findChild<QTabWidget*>();
    if (tabs == nullptr || tabs->count() < 3) return false;
    tabs->setCurrentIndex(2);
    auto views = editor.findChildren<QPlainTextEdit*>();
    for (auto* view : views) {
        if (view->isReadOnly() && view->toPlainText().contains("Prior notes")) return true;
    }
    return false;
}

bool import_bytes_writes_png_attachment(const std::filesystem::path& root) {
    const auto result = AttachmentStore::import_bytes(root / "task", std::string(16, 'P'), ".png");
    return result.success && result.relative_link.find(".png") != std::string::npos
        && std::filesystem::is_regular_file(result.stored_path);
}
}  // namespace

void EditorAttachmentTest::historyTabShowsSnapshotMarkdown() {
    QVERIFY(history_tab_shows_snapshot_markdown());
}

void EditorAttachmentTest::importBytesWritesSafePngAttachment() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(import_bytes_writes_png_attachment(std::filesystem::path(temporary.path().toStdString())));
}

namespace {
MarkdownEditor* visual_ready(MarkdownEditor& editor, const char* markdown) {
    editor.set_markdown(markdown);
    auto* tabs = editor.findChild<QTabWidget*>();
    auto* visual = editor.findChild<QTextEdit*>();
    if (tabs == nullptr || visual == nullptr || !editor.visual_supported()) return nullptr;
    tabs->setCurrentIndex(0);
    visual->moveCursor(QTextCursor::End);
    return &editor;
}

bool markdown_has(const MarkdownEditor& editor, const char* needle) {
    return editor.markdown().find(needle) != std::string::npos;
}

bool survives_source_tab(MarkdownEditor& editor, const char* needle) {
    auto* tabs = editor.findChild<QTabWidget*>();
    if (tabs == nullptr || !markdown_has(editor, needle)) return false;
    tabs->setCurrentIndex(1);
    if (!markdown_has(editor, needle)) return false;
    tabs->setCurrentIndex(0);
    return markdown_has(editor, needle);
}
}

void EditorAttachmentTest::visualChecklistRoundTripsThroughSource() {
    MarkdownEditor editor;
    QVERIFY(visual_ready(editor, "notes") != nullptr);
    editor.insert_checklist();
    QVERIFY(survives_source_tab(editor, "- [ ]"));
}

void EditorAttachmentTest::visualCodeInsertsWithoutSelection() {
    MarkdownEditor editor;
    QVERIFY(visual_ready(editor, "notes") != nullptr);
    editor.insert_code();
    QVERIFY(survives_source_tab(editor, "```"));
}

void EditorAttachmentTest::visualCodeWrapsSelection() {
    MarkdownEditor editor;
    QVERIFY(visual_ready(editor, "fn") != nullptr);
    editor.findChild<QTextEdit*>()->selectAll();
    editor.insert_code();
    QVERIFY(markdown_has(editor, "`fn`"));
}

void EditorAttachmentTest::sourceChecklistSurvivesVisualTab() {
    MarkdownEditor editor;
    editor.set_markdown("notes");
    auto* tabs = editor.findChild<QTabWidget*>();
    QVERIFY(tabs != nullptr);
    tabs->setCurrentIndex(1);
    editor.insert_checklist();
    QVERIFY(markdown_has(editor, "- [ ]"));
    tabs->setCurrentIndex(0);
    QVERIFY(markdown_has(editor, "- [ ]"));
}

void EditorAttachmentTest::visualQuoteAndHeadingRoundTrip() {
    MarkdownEditor editor;
    QVERIFY(visual_ready(editor, "notes") != nullptr);
    editor.insert_quote();
    QVERIFY(survives_source_tab(editor, "> "));
    editor.set_markdown("Section");
    editor.findChild<QTabWidget*>()->setCurrentIndex(0);
    editor.insert_heading(2);
    QVERIFY(survives_source_tab(editor, "## "));
}

void EditorAttachmentTest::visualBoldInsertsWithoutSelection() {
    MarkdownEditor editor;
    QVERIFY(visual_ready(editor, "") != nullptr);
    editor.insert_bold();
    QVERIFY(markdown_has(editor, "**"));
}

QTEST_MAIN(EditorAttachmentTest)
#include "test_editor_attachment.moc"
