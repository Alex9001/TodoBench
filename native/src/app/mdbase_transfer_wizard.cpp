// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/mdbase_transfer_wizard.h"
#include "storage/mdbase_export.h"
#include "storage/mdbase_transfer.h"
#include "storage/mdbase_import.h"

#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QStringList>
#include <QTableWidget>
#include <QHeaderView>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QGroupBox>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <climits>
#include <filesystem>
#include <memory>

namespace todobench {

using namespace mdbase_transfer;

namespace {
constexpr uint kTransferWorkerStackSize = 8U * 1024U * 1024U;
}

// ---- helpers ----
static std::string qstr(const QString& s){ return s.toStdString(); }
static QString qstr(const std::string& s){ return QString::fromStdString(s); }
static QString qpath(const std::filesystem::path& path){
#if defined(Q_OS_WIN)
    return QString::fromStdWString(path.native());
#else
    const auto bytes = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<qsizetype>(bytes.size()));
#endif
}
static std::filesystem::path fspath(const QString& value){
#if defined(Q_OS_WIN)
    return std::filesystem::path(value.toStdWString());
#else
    const auto bytes = value.toUtf8();
    return std::filesystem::path(std::string(bytes.constData(),
                                             static_cast<size_t>(bytes.size())));
#endif
}

static bool paths_overlap(const std::filesystem::path& left,
                          const std::filesystem::path& right) {
    try {
        std::error_code error;
        const auto canonical_left = std::filesystem::weakly_canonical(
            std::filesystem::absolute(left), error);
        if (error) return false;
        const auto canonical_right = std::filesystem::weakly_canonical(
            std::filesystem::absolute(right), error);
        if (error) return false;
        const std::string left_text = canonical_left.generic_string();
        const std::string right_text = canonical_right.generic_string();
        return left_text == right_text || left_text.starts_with(right_text + "/") ||
               right_text.starts_with(left_text + "/");
    } catch (...) {
        return false;
    }
}

// ---- Export ----
struct ExportSummaryState {
    std::size_t attachment_count{0};
    std::size_t support_file_count{0};
    std::size_t warning_count{0};
    std::uint64_t source_bytes{0};
};

static bool path_has_assets_component(const std::filesystem::path& path) {
    return std::ranges::any_of(path, [](const std::filesystem::path& component) {
        return component == "assets";
    });
}

static bool is_native_record_file(const std::filesystem::path& path) {
    const auto filename = path.filename();
    return filename == "task.md" || filename == "project.md" || filename == "settings.json";
}

static void collect_export_summary(const std::filesystem::path& root,
                                   TransferCancellation* cancellation,
                                   ExportSummaryState& summary) {
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    if(error) {
        ++summary.warning_count;
        error.clear();
    }
    while(iterator != end && !cancellation->is_cancelled()) {
        const auto relative = iterator->path().lexically_relative(root);
        const auto status = iterator->symlink_status(error);
        if(error) {
            ++summary.warning_count;
            error.clear();
        } else if(std::filesystem::is_regular_file(status)) {
            const auto size = iterator->file_size(error);
            if(error) {
                ++summary.warning_count;
                error.clear();
            } else {
                summary.source_bytes += size;
                if(path_has_assets_component(relative)) ++summary.attachment_count;
                else if(!is_native_record_file(relative)) ++summary.support_file_count;
            }
        }
        iterator.increment(error);
        if(error) {
            ++summary.warning_count;
            error.clear();
        }
    }
}

MdbaseExportDialog::MdbaseExportDialog(const std::filesystem::path& workspace_root,
                                       std::size_t project_count,
                                       std::size_t task_count,
                                       std::size_t diagnostic_count,
                                       QWidget* parent)
    : QDialog(parent), workspace_root_(workspace_root), project_count_(project_count),
      task_count_(task_count), diagnostic_count_(diagnostic_count) {
    worker_pool_.setMaxThreadCount(1);
    worker_pool_.setStackSize(kTransferWorkerStackSize);
    setWindowTitle("Export as mdbase…");
    resize(640, 420);
    auto* layout = new QVBoxLayout(this);
    auto* info = new QLabel(this);
    info->setWordWrap(true);
    info->setText(QString("Workspace: %1").arg(qpath(workspace_root)));
    layout->addWidget(info);
    auto* form = new QFormLayout;
    dest_edit_ = new QLineEdit(this);
    dest_edit_->setPlaceholderText("Choose a new folder for the mdbase collection");
    auto* browse = new QPushButton("Browse…", this);
    connect(browse, &QPushButton::clicked, this, &MdbaseExportDialog::choose_destination);
    auto* row = new QHBoxLayout;
    row->addWidget(dest_edit_, 1);
    row->addWidget(browse);
    auto* rowWidget = new QWidget(this);
    rowWidget->setLayout(row);
    form->addRow("Destination:", rowWidget);
    layout->addLayout(form);
    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(summary_);
    error_ = new QLabel(this);
    error_->setStyleSheet("color: #c62828");
    error_->setWordWrap(true);
    layout->addWidget(error_);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* cancelBtn = new QPushButton("Cancel", this);
    export_button_ = new QPushButton("Export", this);
    export_button_->setDefault(true);
    buttons->addWidget(cancelBtn);
    buttons->addWidget(export_button_);
    layout->addLayout(buttons);
    connect(cancelBtn, &QPushButton::clicked, this, [this]{ reject(); });
    connect(export_button_, &QPushButton::clicked, this, &MdbaseExportDialog::run_export);
    connect(dest_edit_, &QLineEdit::textChanged, this, &MdbaseExportDialog::refresh_summary);
    refresh_summary();
    start_summary_scan();
}

void MdbaseExportDialog::choose_destination(){
    // Choose parent directory then new folder name. Never use source root as default.
    QString parent = QFileDialog::getExistingDirectory(this, "Choose parent directory for new collection",
        qpath(workspace_root_.parent_path()));
    if(parent.isEmpty()) return;
    bool ok=false;
    QString name = QInputDialog::getText(this, "New collection folder", "Folder name:", QLineEdit::Normal, "exported-mdbase", &ok);
    if(!ok || name.trimmed().isEmpty()) return;
    // Validate leaf name via archive_safety
    std::filesystem::path dest = fspath(parent) / fspath(name.trimmed());
    dest_edit_->setText(qpath(dest));
}

void MdbaseExportDialog::start_summary_scan(){
    summary_cancellation_.cancelled.store(false, std::memory_order_relaxed);
    auto state = std::make_shared<ExportSummaryState>();
    summary_watcher_ = new QFutureWatcher<void>(this);
    connect(summary_watcher_, &QFutureWatcher<void>::finished, this, [this, state]{
        auto* watcher = summary_watcher_;
        summary_watcher_ = nullptr;
        if(watcher) watcher->deleteLater();
        if(summary_cancellation_.is_cancelled()) return;
        attachment_count_ = state->attachment_count;
        support_file_count_ = state->support_file_count;
        inventory_warning_count_ = state->warning_count;
        source_bytes_ = state->source_bytes;
        summary_ready_ = true;
        refresh_summary();
    });
    summary_watcher_->setFuture(QtConcurrent::run(&worker_pool_,
        [root = workspace_root_, cancellation = &summary_cancellation_, state]{
            collect_export_summary(root, cancellation, *state);
        }));
}

void MdbaseExportDialog::refresh_summary(){
    QString dest = dest_edit_->text();
    QString text = QString("Workspace: %1 project(s), %2 task(s)\nSource: %3\nDestination: %4")
        .arg(project_count_).arg(task_count_)
        .arg(qpath(workspace_root_))
        .arg(dest.isEmpty()? QString("(choose a new folder)") : dest);
    if(summary_ready_) {
        text += QString("\nSource inventory estimate: %1 attachment(s), %2 other/support file(s), %3 total")
            .arg(attachment_count_).arg(support_file_count_)
            .arg(QLocale().formattedDataSize(static_cast<qint64>(source_bytes_)));
    } else {
        text += "\nCalculating attachment count and source size…";
    }
    const auto warning_count = diagnostic_count_ + inventory_warning_count_;
    if(warning_count > 0){
        text += QString("\nDiagnostics: %1 warning(s)/error(s) — export will block if errors remain.")
            .arg(warning_count);
    }
    summary_->setText(text);
    error_->clear();
}

MdbaseExportDialog::~MdbaseExportDialog(){
    summary_cancellation_.cancel();
    export_cancellation_.cancel();
    if(summary_watcher_ && summary_watcher_->isRunning()) summary_watcher_->waitForFinished();
    if(export_watcher_ && export_watcher_->isRunning()) export_watcher_->waitForFinished();
}

void MdbaseExportDialog::reject(){
    summary_cancellation_.cancel();
    if(export_watcher_ && export_watcher_->isRunning()){
        export_cancellation_.cancel();
        if(export_progress_) export_progress_->setLabelText("Cancelling export…");
        return;
    }
    QDialog::reject();
}

void MdbaseExportDialog::closeEvent(QCloseEvent* event){
    summary_cancellation_.cancel();
    if(export_watcher_ && export_watcher_->isRunning()){
        export_cancellation_.cancel();
        if(export_progress_) export_progress_->setLabelText("Cancelling export…");
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void MdbaseExportDialog::run_export(){
    if(export_watcher_ && export_watcher_->isRunning()) return;
    QString destQs = dest_edit_->text().trimmed();
    if(destQs.isEmpty()){
        error_->setText("Choose a destination folder. It must not exist yet.");
        return;
    }
    std::filesystem::path dest = fspath(destQs);
    // basic validation before worker: destination must not exist
    std::error_code ec;
    if(std::filesystem::exists(dest, ec)){
        error_->setText("Destination already exists — choose a new folder name.");
        return;
    }
    if (paths_overlap(dest, workspace_root_)) {
        error_->setText("Destination must not overlap the source workspace.");
        return;
    }

    error_->clear();
    export_button_->setEnabled(false);
    export_cancellation_.cancelled.store(false, std::memory_order_relaxed);

    export_progress_ = new QProgressDialog("Exporting workspace to mdbase…", "Cancel", 0, 0, this);
    export_progress_->setWindowModality(Qt::WindowModal);
    export_progress_->setMinimumDuration(0);
    export_progress_->show();
    connect(export_progress_, &QProgressDialog::canceled, this, [this]{
        export_cancellation_.cancel();
        if(export_progress_) export_progress_->setLabelText("Cancelling export…");
    });

    ExportRequest request;
    request.workspace_root = workspace_root_;
    request.destination = dest;
    request.cancellation = &export_cancellation_;
    QPointer<QProgressDialog> progress(export_progress_);
    QPointer<MdbaseExportDialog> dialog(this);
    request.progress = [progress, dialog, cancellation = &export_cancellation_](const std::string& phase, uint64_t processed, uint64_t total) {
        if(dialog && progress){
            QMetaObject::invokeMethod(dialog, [progress, phase, processed, total]{
                if(!progress) return;
                progress->setLabelText(QString("Exporting… %1").arg(qstr(phase)));
                if(total > 0){
                    progress->setRange(0, static_cast<int>(std::min<uint64_t>(total, INT_MAX)));
                    progress->setValue(static_cast<int>(std::min<uint64_t>(processed, INT_MAX)));
                }
            }, Qt::QueuedConnection);
        }
        return !cancellation->is_cancelled();
    };

    export_watcher_ = new QFutureWatcher<TransferResult>(this);
    connect(export_watcher_, &QFutureWatcher<TransferResult>::finished, this, [this]{
        auto* watcher = export_watcher_;
        const TransferResult result = watcher->result();
        export_watcher_ = nullptr;
        watcher->deleteLater();
        if(export_progress_){
            export_progress_->close();
            export_progress_->deleteLater();
            export_progress_ = nullptr;
        }
        export_button_->setEnabled(true);
        finish_export(result);
    });
    export_watcher_->setFuture(QtConcurrent::run(
        &worker_pool_, [request]{ return export_workspace(request); }));
}

void MdbaseExportDialog::finish_export(const TransferResult& result){
    if(export_cancellation_.is_cancelled() || result.cancelled){
        if(result.destination.empty() || !std::filesystem::exists(result.destination)){
            error_->setText("Export cancelled. No destination was published.");
            return;
        }
        // late cancel after publish -> success per spec
        if(result.outcome==TransferOutcome::Succeeded){
            // fall through to success
        } else {
            error_->setText("Export cancelled. No partial destination remains.");
            return;
        }
    }
    if(result.outcome != TransferOutcome::Succeeded){
        QString msg = QString::fromStdString(result.error);
        if(!result.diagnostics.empty()){
            msg += "\n\n" + qstr(result.diagnostics.front().message);
        }
        error_->setText(msg);
        QMessageBox::warning(this, "Export failed", msg);
        return;
    }
    // Success
    QString msg = QString("Exported %1 record(s) and %2 attachment(s) to %3\nReport: %4")
        .arg(result.record_count)
        .arg(result.asset_count)
        .arg(qpath(result.destination))
        .arg(qpath(result.report_path));
    QMessageBox box(this);
    box.setWindowTitle("Export succeeded");
    box.setText(msg);
    auto* openFolder = box.addButton("Open Folder", QMessageBox::ActionRole);
    box.addButton(QMessageBox::Close);
    box.exec();
    if(box.clickedButton()==openFolder){
        QDesktopServices::openUrl(QUrl::fromLocalFile(qpath(result.destination)));
    }
    accept();
}

// ---- Import wizard ----
// Page ids
enum PageId { Src=0, Records=1, Review=2, Progress=3, Result=4 };

struct SourceScanState {
    SnapshotResult snapshot_result;
    std::optional<CollectionInspection> inspection;
};

class MdbaseImportWizard::SourcePage final : public QWizardPage {
public:
    explicit SourcePage(MdbaseImportWizard* w): wizard_(w){
        setTitle("Source — choose mdbase collection");
        setSubTitle("Select the folder containing mdbase.yaml (spec 0.3.0). Diagnostics will appear below.");
        auto* layout = new QVBoxLayout(this);
        auto* row = new QHBoxLayout;
        edit_ = new QLineEdit(this);
        edit_->setObjectName("mdbaseSourcePath");
        edit_->setPlaceholderText("Path to mdbase collection root");
        browse_ = new QPushButton("Browse…", this);
        row->addWidget(edit_,1);
        row->addWidget(browse_);
        layout->addLayout(row);
        diag_ = new QLabel(this);
        diag_->setWordWrap(true);
        diag_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(diag_);
        progress_ = new QProgressBar(this);
        progress_->setRange(0, 0);
        progress_->hide();
        layout->addWidget(progress_);
        cancel_scan_ = new QPushButton("Cancel scan", this);
        cancel_scan_->setObjectName("mdbaseCancelSourceScan");
        cancel_scan_->hide();
        layout->addWidget(cancel_scan_);
        connect(browse_, &QPushButton::clicked, this, [this]{
            QString dir = QFileDialog::getExistingDirectory(this, "Choose mdbase collection root");
            if(!dir.isEmpty()) edit_->setText(dir);
        });
        connect(edit_, &QLineEdit::textChanged, this, [this]{
            scan_ready_ = false;
            diag_->clear();
            emit completeChanged();
        });
        connect(cancel_scan_, &QPushButton::clicked, this, [this]{
            wizard_->cancellation_.cancel();
            cancel_scan_->setEnabled(false);
            diag_->setText("Cancelling source scan…");
        });
        registerField("sourcePath*", edit_);
    }
    bool isComplete() const override {
        return !wizard_->source_scan_running_ && !edit_->text().trimmed().isEmpty();
    }
    bool validatePage() override {
        const QString path = edit_->text().trimmed();
        if(scan_ready_ && path == scanned_path_) return true;
        if(path.isEmpty()) return show_error("Choose a collection root.");
        const std::filesystem::path src = fspath(path);
        std::error_code ec;
        if(!std::filesystem::exists(src, ec) || !std::filesystem::is_directory(src, ec)){
            return show_error("Collection root not found or not a directory.");
        }
        begin_scan(src, path);
        return false;
    }
private:
    bool show_error(const QString& message) {
        diag_->setText(message);
        return false;
    }

    void begin_scan(const std::filesystem::path& source, const QString& requested_path) {
        if(wizard_->source_scan_running_) return;
        scan_ready_ = false;
        wizard_->snapshot_.reset();
        wizard_->inspection_.reset();
        wizard_->preview_.reset();
        wizard_->cancellation_.cancelled.store(false, std::memory_order_relaxed);
        wizard_->source_scan_running_ = true;
        edit_->setEnabled(false);
        browse_->setEnabled(false);
        progress_->setRange(0, 0);
        progress_->show();
        cancel_scan_->setEnabled(true);
        cancel_scan_->show();
        diag_->setText("Scanning and copying the source collection…");
        emit completeChanged();

        auto state = std::make_shared<SourceScanState>();
        SnapshotOptions options;
        options.cancellation = &wizard_->cancellation_;
        QPointer<SourcePage> page(this);
        options.progress = [page, cancellation = &wizard_->cancellation_](
                               const std::string& phase, uint64_t processed, uint64_t total) {
            if(page) {
                QMetaObject::invokeMethod(page, [page, phase, processed, total]{
                    if(page) page->show_progress(phase, processed, total);
                }, Qt::QueuedConnection);
            }
            return !cancellation->is_cancelled();
        };
        wizard_->source_watcher_ = new QFutureWatcher<void>(wizard_);
        connect(wizard_->source_watcher_, &QFutureWatcher<void>::finished, this,
                [this, state, requested_path]{ finish_scan(state, requested_path); });
        wizard_->source_watcher_->setFuture(QtConcurrent::run(&wizard_->worker_pool_,
            [state, source, options, cancellation = &wizard_->cancellation_, page]{
                state->snapshot_result = capture_snapshot(source, options);
                if(!state->snapshot_result.ok || !state->snapshot_result.snapshot) return;
                if(page) {
                    QMetaObject::invokeMethod(page, [page]{
                        if(page) {
                            page->diag_->setText("Inspecting collection configuration and records…");
                            page->progress_->setRange(0, 0);
                        }
                    }, Qt::QueuedConnection);
                }
                state->inspection = inspect_import_source(*state->snapshot_result.snapshot,
                                                          cancellation);
            }));
    }

    void show_progress(const std::string& phase, uint64_t processed, uint64_t total) {
        diag_->setText(QString("Source phase: %1 %2/%3")
                           .arg(qstr(phase)).arg(processed).arg(total));
        if(total == 0) {
            progress_->setRange(0, 0);
            return;
        }
        progress_->setRange(0, static_cast<int>(std::min<uint64_t>(total, INT_MAX)));
        progress_->setValue(static_cast<int>(std::min<uint64_t>(processed, INT_MAX)));
    }

    void finish_scan(const std::shared_ptr<SourceScanState>& state,
                     const QString& requested_path) {
        auto* watcher = wizard_->source_watcher_;
        wizard_->source_watcher_ = nullptr;
        if(watcher) watcher->deleteLater();
        wizard_->source_scan_running_ = false;
        edit_->setEnabled(true);
        browse_->setEnabled(true);
        progress_->hide();
        cancel_scan_->hide();
        if(wizard_->cancellation_.is_cancelled()) {
            show_error("Source scan cancelled. No destination was created.");
            emit completeChanged();
            return;
        }
        if(!state->snapshot_result.ok || !state->snapshot_result.snapshot) {
            show_snapshot_failure(state->snapshot_result);
            emit completeChanged();
            return;
        }
        if(!state->inspection) {
            show_error("Collection inspection did not complete.");
            emit completeChanged();
            return;
        }
        adopt_scan_result(state, requested_path);
    }

    void show_snapshot_failure(const SnapshotResult& captured) {
        QString message = qstr(captured.error);
        if(!captured.diagnostics.empty()) {
            message += "\n" + qstr(captured.diagnostics.front().message);
        }
        show_error("Snapshot failed: " + message);
    }

    void adopt_scan_result(const std::shared_ptr<SourceScanState>& state,
                           const QString& requested_path) {
        wizard_->snapshot_ = std::move(state->snapshot_result.snapshot);
        wizard_->inspection_ = std::move(state->inspection);
        const auto& inspection = *wizard_->inspection_;
        const QString text = inspection_text(inspection);
        diag_->setText(text);
        initialize_mapping(inspection);
        if(!inspection.valid || has_blocking_diagnostic(inspection)) {
            diag_->setText(text + "\nBlocking errors — correct the source before continuing.");
            emit completeChanged();
            return;
        }
        scan_ready_ = true;
        scanned_path_ = requested_path;
        emit completeChanged();
        QPointer<SourcePage> page(this);
        QTimer::singleShot(0, wizard_, [page]{
            if(page && page->wizard_->currentPage() == page && page->scan_ready_) {
                page->wizard_->next();
            }
        });
    }

    static QString inspection_text(const CollectionInspection& inspection) {
        QString text = inspection.valid
            ? QString("Inspection: %1 type(s), %2 record(s), timezone %3%4\n")
                .arg(inspection.types.size()).arg(inspection.all_record_paths.size())
                .arg(qstr(inspection.timezone))
                .arg(inspection.is_own_profile
                    ? " — own-profile (TodoBench export v1)" : " — foreign")
            : QString("Collection inspection: NOT valid (config/type errors).\n");
        for(const auto& diagnostic : inspection.diagnostics){
            text += QString("[%1] %2 %3\n").arg(qstr(diagnostic.code))
                .arg(qstr(diagnostic.message)).arg(qstr(diagnostic.path));
        }
        if(inspection.types.empty()){
            text += "No types discovered. You may still create an empty workspace on the Review page.";
        }
        return text;
    }

    void initialize_mapping(const CollectionInspection& inspection) {
        if(inspection.is_own_profile){
            wizard_->mapping_ = make_own_profile_mapping(inspection);
            return;
        }
        wizard_->mapping_ = ImportMapping();
        for(const auto& type : inspection.types){
            TypeMapping mapping;
            mapping.type_name = type.type_name;
            mapping.as_task = true;
            wizard_->mapping_.type_mappings[type.type_name] = std::move(mapping);
        }
        TypeMapping untyped;
        untyped.type_name = "__untyped__";
        untyped.is_untyped_bucket = true;
        untyped.as_task = true;
        wizard_->mapping_.type_mappings[untyped.type_name] = std::move(untyped);
    }

    static bool has_blocking_diagnostic(const CollectionInspection& inspection) {
        return std::ranges::any_of(inspection.diagnostics,
            [](const TransferDiagnostic& diagnostic) {
                return diagnostic.severity == TransferSeverity::Blocking ||
                       diagnostic.code == "unsupported_version";
            });
    }

    MdbaseImportWizard* wizard_;
    QLineEdit* edit_{nullptr};
    QPushButton* browse_{nullptr};
    QLabel* diag_{nullptr};
    QProgressBar* progress_{nullptr};
    QPushButton* cancel_scan_{nullptr};
    bool scan_ready_{false};
    QString scanned_path_;
};

class MdbaseImportWizard::RecordsPage final : public QWizardPage {
public:
    explicit RecordsPage(MdbaseImportWizard* w): wizard_(w){
        setTitle("Records and fields — select types and map fields");
        setSubTitle("Choose which record types become tasks/projects, map every source field, then assign each observed status and priority value.");
        outer_ = new QVBoxLayout(this);
        outer_->setContentsMargins(0, 0, 0, 0);
        scroll_ = new QScrollArea(this);
        scroll_->setWidgetResizable(true);
        outer_->addWidget(scroll_);
        recordTable_ = new QTableWidget(this);
        recordTable_->setObjectName("mdbaseRecordSelection");
        recordTable_->setColumnCount(3);
        recordTable_->setHorizontalHeaderLabels({"Import", "Source record", "Detected types"});
        recordTable_->horizontalHeader()->setStretchLastSection(true);
        recordTable_->setMinimumHeight(150);
        outer_->addWidget(new QLabel("Individual records:", this));
        outer_->addWidget(recordTable_);
        refreshValuesButton_ = new QPushButton("Refresh observed status and priority values", this);
        outer_->addWidget(refreshValuesButton_);
        valuesWidget_ = new QWidget(this);
        valuesLayout_ = new QVBoxLayout(valuesWidget_);
        valuesWidget_->setLayout(valuesLayout_);
        outer_->addWidget(valuesWidget_);
        diagLabel_ = new QLabel(this);
        diagLabel_->setWordWrap(true);
        diagLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        outer_->addWidget(diagLabel_);
        connect(refreshValuesButton_, &QPushButton::clicked, this, [this]{
            apply_rows_to_mapping();
            rebuild_value_mapping_controls();
            emit completeChanged();
        });
    }
    void initializePage() override {
        if(!wizard_->inspection_) return;
        rows_.clear();
        if(QWidget* previous = scroll_->takeWidget()) previous->deleteLater();
        auto* content = new QWidget;
        contentLayout_ = new QVBoxLayout(content);
        content->setLayout(contentLayout_);
        scroll_->setWidget(content);

        auto &insp = *wizard_->inspection_;
        for(auto &ti: insp.types){
            auto* box = new QGroupBox(QString("%1 — %2 record(s)").arg(qstr(ti.type_name)).arg(ti.record_count), this);
            auto* fl = new QFormLayout(box);
            auto* sel = new QCheckBox("Import this type", box);
            auto itMap = wizard_->mapping_.type_mappings.find(ti.type_name);
            bool selected = itMap!=wizard_->mapping_.type_mappings.end() ? itMap->second.selected : false;
            sel->setChecked(selected);
            fl->addRow(sel);
            auto* kind = new QComboBox(box);
            kind->addItem("As tasks", "task");
            kind->addItem("As projects", "project");
            bool asTask = itMap!=wizard_->mapping_.type_mappings.end() ? itMap->second.as_task : true;
            kind->setCurrentIndex(asTask?0:1);
            fl->addRow("Kind:", kind);
            TypeRow row;
            row.typeName = ti.type_name;
            row.sel = sel;
            row.kind = kind;
            const TypeMapping empty;
            const TypeMapping& mapping = itMap == wizard_->mapping_.type_mappings.end() ? empty : itMap->second;
            row.titleEdit = add_field(fl, box, "Title / display name:", mapping.title_field, "/summary");
            row.idEdit = add_field(fl, box, "Source ID:", mapping.id_field, "/id");
            row.statusEdit = add_field(fl, box, "Status:", mapping.status_field, "/state");
            row.prioEdit = add_field(fl, box, "Priority:", mapping.priority_field, "/priority");
            row.tagsEdit = add_field(fl, box, "Tags:", mapping.tags_field, "/tags");
            row.dueEdit = add_field(fl, box, "Due date:", mapping.due_field, "/due");
            row.createdEdit = add_field(fl, box, "Created time:", mapping.created_field, "/created_at");
            row.updatedEdit = add_field(fl, box, "Updated time:", mapping.updated_field, "/updated_at");
            row.completedEdit = add_field(fl, box, "Completed time:", mapping.completed_field, "/completed_at");
            row.recurrenceEdit = add_field(fl, box, "Recurrence:", mapping.recurrence_field, "/recurrence");
            row.remindersEdit = add_field(fl, box, "Reminders:", mapping.reminders_field, "/reminders");
            row.orderEdit = add_field(fl, box, "Order:", mapping.order_field, "/order");
            row.archivedEdit = add_field(fl, box, "Archived (projects):", mapping.archived_field, "/archived");
            row.projectMode = new QComboBox(box);
            row.projectMode->addItem("No project", "none");
            row.projectMode->addItem("Markdown link", "link");
            row.projectMode->addItem("Project ID reference", "id_ref");
            row.projectMode->addItem("Project name / label", "string_label");
            set_combo_value(row.projectMode, qstr(mapping.project_mode));
            fl->addRow("Project relationship:", row.projectMode);
            row.projectEdit = add_field(fl, box, "Project field:", mapping.project_field, "/project");
            row.parentMode = new QComboBox(box);
            row.parentMode->addItem("No parent", "none");
            row.parentMode->addItem("Markdown link", "link");
            row.parentMode->addItem("Parent ID reference", "id_ref");
            set_combo_value(row.parentMode, qstr(mapping.parent_mode));
            fl->addRow("Parent relationship:", row.parentMode);
            row.parentEdit = add_field(fl, box, "Parent field:", mapping.parent_field, "/parent");
            rows_.push_back(row);

            contentLayout_->addWidget(box);
        }
        // Untyped bucket
        {
            auto* box = new QGroupBox("Untyped records", this);
            auto* fl = new QFormLayout(box);
            auto* sel = new QCheckBox("Import untyped Markdown files as tasks", box);
            auto itMap = wizard_->mapping_.type_mappings.find("__untyped__");
            bool selected = itMap!=wizard_->mapping_.type_mappings.end() ? itMap->second.selected : false;
            sel->setChecked(selected);
            fl->addRow(sel);
            TypeRow row;
            row.typeName = "__untyped__";
            row.sel = sel;
            rows_.push_back(row);
            contentLayout_->addWidget(box);
        }
        for(auto &r: rows_){
            if(r.sel) connect(r.sel, &QCheckBox::toggled, this, [this]{ emit completeChanged(); });
        }
        rebuild_record_controls(insp);
        rebuild_value_mapping_controls();
    }
    bool validatePage() override {
        apply_rows_to_mapping();
        apply_record_selection();
        apply_value_mappings();
        wizard_->rebuild_preview();
        QString diag;
        if(wizard_->preview_){
            for(auto &b: wizard_->preview_->blocking_errors){
                diag += QString("[%1] %2 %3\n").arg(qstr(b.code)).arg(qstr(b.message)).arg(qstr(b.path));
            }
            for(auto &w: wizard_->preview_->warnings){
                diag += QString("warn [%1] %2\n").arg(qstr(w.code)).arg(qstr(w.message));
            }
            if(wizard_->preview_->has_blocking_errors()){
                diag += "\nBlocking errors remain — you can go to Review to see details and fix mappings.";
            }
        }
        diagLabel_->setText(diag);
        return true;
    }
private:
    static QLineEdit* add_field(QFormLayout* layout, QWidget* parent, const char* label,
                                const FieldSelector& selector, const char* placeholder){
        auto* edit = new QLineEdit(parent);
        edit->setPlaceholderText(QString("JSON Pointer, e.g. %1").arg(placeholder));
        edit->setText(qstr(selector.json_pointer));
        layout->addRow(label, edit);
        return edit;
    }
    static void set_combo_value(QComboBox* combo, const QString& value){
        const int index = combo->findData(value);
        combo->setCurrentIndex(index >= 0 ? index : 0);
    }
    static void set_selector(FieldSelector& selector, QLineEdit* edit){
        if(!edit) return;
        const QString text = edit->text().trimmed();
        selector = {text.toStdString(), !text.isEmpty()};
    }
    void rebuild_record_controls(const CollectionInspection& inspection) {
        recordRows_.clear();
        recordTable_->setRowCount(static_cast<int>(inspection.all_record_paths.size()));
        for(int row = 0; row < recordTable_->rowCount(); ++row) {
            const auto& path = inspection.all_record_paths[static_cast<std::size_t>(row)];
            auto* selected = new QCheckBox(recordTable_);
            selected->setChecked(!wizard_->mapping_.excluded_paths.contains(path));
            recordTable_->setCellWidget(row, 0, selected);
            recordTable_->setItem(row, 1, new QTableWidgetItem(qstr(path)));
            recordTable_->setItem(row, 2, new QTableWidgetItem(record_types(inspection, path)));
            connect(selected, &QCheckBox::toggled, this, [this]{ emit completeChanged(); });
            recordRows_.push_back({path, selected});
        }
        recordTable_->resizeColumnToContents(0);
        recordTable_->resizeColumnToContents(1);
    }
    static QString record_types(const CollectionInspection& inspection,
                                const std::string& path) {
        QStringList types;
        for(const auto& [type, paths] : inspection.type_to_paths) {
            if(std::ranges::find(paths, path) != paths.end()) types.push_back(qstr(type));
        }
        if(std::ranges::find(inspection.untyped_paths, path) != inspection.untyped_paths.end()) {
            types.push_back("Untyped");
        }
        types.sort();
        return types.join(", ");
    }
    void apply_record_selection() {
        for(const auto& [path, selected] : recordRows_) {
            if(selected->isChecked()) wizard_->mapping_.excluded_paths.erase(path);
            else wizard_->mapping_.excluded_paths.insert(path);
        }
    }
    void apply_rows_to_mapping(){
        for(auto& row: rows_){
            auto* tm = wizard_->mapping_.find_type(row.typeName);
            if(!tm){
                wizard_->mapping_.ensure_type(row.typeName);
                tm = wizard_->mapping_.find_type(row.typeName);
            }
            if(row.sel) tm->selected = row.sel->isChecked();
            if(row.kind) tm->as_task = row.kind->currentData().toString() == "task";
            set_selector(tm->title_field, row.titleEdit);
            set_selector(tm->id_field, row.idEdit);
            set_selector(tm->status_field, row.statusEdit);
            set_selector(tm->priority_field, row.prioEdit);
            set_selector(tm->tags_field, row.tagsEdit);
            set_selector(tm->due_field, row.dueEdit);
            set_selector(tm->created_field, row.createdEdit);
            set_selector(tm->updated_field, row.updatedEdit);
            set_selector(tm->completed_field, row.completedEdit);
            set_selector(tm->recurrence_field, row.recurrenceEdit);
            set_selector(tm->reminders_field, row.remindersEdit);
            set_selector(tm->order_field, row.orderEdit);
            set_selector(tm->archived_field, row.archivedEdit);
            if(row.projectMode) tm->project_mode = row.projectMode->currentData().toString().toStdString();
            set_selector(tm->project_field, row.projectEdit);
            if(row.parentMode) tm->parent_mode = row.parentMode->currentData().toString().toStdString();
            set_selector(tm->parent_field, row.parentEdit);
        }
    }
    static QString render_value(const QJsonValue& value){
        QJsonArray array;
        array.append(value);
        QString json = QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
        return json.mid(1, json.size() - 2);
    }
    struct TypeRow {
        std::string typeName;
        QCheckBox* sel{nullptr};
        QComboBox* kind{nullptr};
        QLineEdit* titleEdit{nullptr};
        QLineEdit* idEdit{nullptr};
        QLineEdit* statusEdit{nullptr};
        QLineEdit* prioEdit{nullptr};
        QLineEdit* tagsEdit{nullptr};
        QLineEdit* dueEdit{nullptr};
        QLineEdit* createdEdit{nullptr};
        QLineEdit* updatedEdit{nullptr};
        QLineEdit* completedEdit{nullptr};
        QLineEdit* recurrenceEdit{nullptr};
        QLineEdit* remindersEdit{nullptr};
        QLineEdit* orderEdit{nullptr};
        QLineEdit* archivedEdit{nullptr};
        QComboBox* projectMode{nullptr};
        QLineEdit* projectEdit{nullptr};
        QComboBox* parentMode{nullptr};
        QLineEdit* parentEdit{nullptr};
    };
    struct RecordRow {
        std::string path;
        QCheckBox* selected{nullptr};
    };
    struct ValueRow { QJsonValue value; QComboBox* combo; bool status; };

    static QComboBox* add_value_combo(QWidget* parent, const std::string& current, bool status){
        auto* combo = new QComboBox(parent);
        combo->addItem("Choose a native value", "");
        const std::initializer_list<std::pair<const char*, const char*>> entries = status
            ? std::initializer_list<std::pair<const char*, const char*>>{{"To do", "todo"}, {"In progress", "in_progress"}, {"Waiting", "waiting"}, {"Done", "done"}, {"Cancelled", "cancelled"}}
            : std::initializer_list<std::pair<const char*, const char*>>{{"None", "none"}, {"Low", "low"}, {"Normal", "normal"}, {"High", "high"}, {"Urgent", "urgent"}};
        for(const auto& entry: entries) combo->addItem(entry.first, entry.second);
        set_combo_value(combo, qstr(current));
        return combo;
    }
    void add_observed_values(const TypeRow& row, const FieldSelector& selector,
                             bool status, QFormLayout* form,
                             std::unordered_set<std::string>& seen) {
        if(!selector.is_set || selector.json_pointer.empty()) return;
        const auto observed = observed_field_values(
            *wizard_->inspection_, *wizard_->snapshot_, row.typeName,
            selector.json_pointer);
        for(const QJsonValue& value : observed){
            if(value.isUndefined() || value.isNull()) continue;
            const std::string key = render_value(value).toStdString();
            if(!seen.insert(key).second) continue;
            const auto current = status
                ? wizard_->mapping_.status_map.lookup(value)
                : wizard_->mapping_.priority_map.lookup(value);
            auto* combo = add_value_combo(valuesWidget_, current.value_or(""), status);
            form->addRow(render_value(value), combo);
            values_.push_back({value, combo, status});
        }
    }

    void add_value_groups(QGroupBox* status_box, QFormLayout* status_form,
                          QGroupBox* priority_box, QFormLayout* priority_form) {
        if(values_.empty()){
            auto* note = new QLabel(
                "Map a Status or Priority JSON Pointer above, then refresh the observed values.",
                valuesWidget_);
            note->setWordWrap(true);
            valuesLayout_->addWidget(note);
            status_box->deleteLater();
            priority_box->deleteLater();
            return;
        }
        if(status_form->rowCount() > 0) valuesLayout_->addWidget(status_box);
        else status_box->deleteLater();
        if(priority_form->rowCount() > 0) valuesLayout_->addWidget(priority_box);
        else priority_box->deleteLater();
    }

    void rebuild_value_mapping_controls(){
        values_.clear();
        QLayoutItem* item;
        while((item = valuesLayout_->takeAt(0)) != nullptr){
            if(item->widget()) item->widget()->deleteLater();
            delete item;
        }
        if(!wizard_->snapshot_ || !wizard_->inspection_) return;
        auto* statusBox = new QGroupBox("Observed status values", valuesWidget_);
        auto* statusForm = new QFormLayout(statusBox);
        auto* priorityBox = new QGroupBox("Observed priority values", valuesWidget_);
        auto* priorityForm = new QFormLayout(priorityBox);
        std::unordered_set<std::string> statusSeen;
        std::unordered_set<std::string> prioritySeen;
        for(const auto& row: rows_){
            const auto* tm = wizard_->mapping_.find_type(row.typeName);
            if(!tm || !tm->selected || tm->is_untyped_bucket) continue;
            add_observed_values(row, tm->status_field, true, statusForm, statusSeen);
            add_observed_values(row, tm->priority_field, false, priorityForm, prioritySeen);
        }
        add_value_groups(statusBox, statusForm, priorityBox, priorityForm);
    }
    void apply_value_mappings(){
        for(const auto& row: values_){
            const std::string native = row.combo->currentData().toString().toStdString();
            if(row.status) {
                if(native.empty()) wizard_->mapping_.status_map.scalar_to_native.erase(
                    render_value(row.value).toStdString());
                else wizard_->mapping_.status_map.put_for_json_value(row.value, native);
            } else {
                if(native.empty()) wizard_->mapping_.priority_map.scalar_to_native.erase(
                    render_value(row.value).toStdString());
                else wizard_->mapping_.priority_map.put_for_json_value(row.value, native);
            }
        }
    }
    MdbaseImportWizard* wizard_;
    QScrollArea* scroll_{nullptr};
    QTableWidget* recordTable_{nullptr};
    QVBoxLayout* outer_{nullptr};
    QVBoxLayout* contentLayout_{nullptr};
    QPushButton* refreshValuesButton_{nullptr};
    QWidget* valuesWidget_{nullptr};
    QVBoxLayout* valuesLayout_{nullptr};
    QLabel* diagLabel_{nullptr};
    std::vector<TypeRow> rows_;
    std::vector<RecordRow> recordRows_;
    std::vector<ValueRow> values_;
};

class MdbaseImportWizard::ReviewPage final : public QWizardPage {
public:
    explicit ReviewPage(MdbaseImportWizard* w): wizard_(w){
        setTitle("Review and destination — confirm conversion");
        setSubTitle("Check native projects/tasks, relationship changes, inactive fields, and choose the new workspace folder.");
        setCommitPage(true);
    }
    void initializePage() override {
        reset_layout();
        if(!wizard_->preview_) wizard_->rebuild_preview();
        const auto& preview = *wizard_->preview_;
        previewLabel_ = new QLabel(summary_text(preview), this);
        previewLabel_->setWordWrap(true);
        previewLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout_->addWidget(previewLabel_);
        add_record_review(preview);
        add_destination_chooser();
        add_acknowledgements();
        errorLabel_ = new QLabel(this);
        errorLabel_->setStyleSheet("color: #c62828");
        errorLabel_->setWordWrap(true);
        layout_->addWidget(errorLabel_);
    }
    bool isComplete() const override {
        if(!wizard_->preview_) return false;
        auto &p = *wizard_->preview_;
        if(p.has_blocking_errors()) {
            // Check if blocking is only empty_selection_requires_ack and ack checked? Our preview requires ack.
            // If empty and ack checked, preview would have no blocking. So any remaining blocking means not complete.
            return false;
        }
        QString dest = destEdit_ ? destEdit_->text().trimmed() : QString();
        if(dest.isEmpty()) return false;
        std::error_code ec;
        if(std::filesystem::exists(fspath(dest), ec)) return false;
        // Require warnings ack if there are warnings/fallbacks
        if((!p.warnings.empty() || !p.records.empty()) && ackWarnings_ && !ackWarnings_->isChecked()){
            // If there are warnings, require ack
            bool hasFallback=false;
            for(auto &r: p.records) if(!r.fallback_notes.empty()) hasFallback=true;
            if(!p.warnings.empty() || hasFallback) return false;
        }
        return true;
    }
    bool validatePage() override {
        // Ensure preview still current (source unchanged)
        if(wizard_->snapshot_ && !snapshot_unchanged(*wizard_->snapshot_)){
            errorLabel_->setText("Source changed since preview — please go Back and rescan (Source page Next).");
            return false;
        }
        // Destination already validated in isComplete
        return true;
    }
private:
    void reset_layout() {
        if(!layout_) {
            layout_ = new QVBoxLayout(this);
            setLayout(layout_);
            return;
        }
        QLayoutItem* item;
        while((item = layout_->takeAt(0)) != nullptr){
            if(item->widget()) item->widget()->deleteLater();
            delete item;
        }
    }

    uint64_t source_retention_bytes() const {
        uint64_t bytes = 0;
        if (!wizard_->snapshot_) return bytes;
        for (const auto& file : wizard_->snapshot_->inventory) bytes += file.size;
        return bytes;
    }

    static void append_diagnostics(QString& text, const QString& heading,
                                   const std::vector<TransferDiagnostic>& diagnostics,
                                   bool include_path) {
        if(diagnostics.empty()) return;
        text += QString("\n%1 (%2):\n").arg(heading).arg(diagnostics.size());
        for(const auto& diagnostic : diagnostics){
            text += include_path
                ? QString(" • [%1] %2 %3\n").arg(qstr(diagnostic.code))
                      .arg(qstr(diagnostic.message)).arg(qstr(diagnostic.path))
                : QString(" • [%1] %2\n").arg(qstr(diagnostic.code))
                      .arg(qstr(diagnostic.message));
        }
    }

    static void append_record_notes(QString& text, const TransferPreview& preview) {
        for(const auto& record : preview.records){
            if(!record.fallback_notes.empty()){
                text += QString("\nFallback for %1: %2\n").arg(qstr(record.source_path))
                    .arg(qstr(record.fallback_notes.front()));
            }
            if(!record.inactive_notes.empty()){
                text += QString("Inactive: %1\n").arg(qstr(record.inactive_notes.front()));
            }
        }
    }

    QString summary_text(const TransferPreview& preview) const {
        QString text = QString("Selected: %1 record(s) → %2 task(s), %3 project(s)\n")
            .arg(preview.selected_record_count).arg(preview.to_task_count)
            .arg(preview.to_project_count);
        text += QString("Estimated output: %1 bytes, source retention: %2 bytes\n")
            .arg(preview.estimated_output_bytes).arg(source_retention_bytes());
        append_diagnostics(text, "Warnings", preview.warnings, false);
        append_diagnostics(text, "Blocking errors — Finish disabled",
                           preview.blocking_errors, true);
        append_record_notes(text, preview);
        return text;
    }

    static QString relationship_label(const TransferPreview& preview,
                                      const std::string& choice,
                                      const QString& empty_label) {
        if(choice.empty()) return empty_label;
        if(choice.starts_with("label:")) return qstr(choice.substr(6));
        const auto found = std::ranges::find_if(preview.records,
            [&choice](const PreviewRecord& record){ return record.native_id == choice; });
        if(found == preview.records.end()) return qstr(choice);
        return qstr(found->native_title) + " — " + qstr(found->source_path);
    }

    static void add_record_candidates(QComboBox* combo, const TransferPreview& preview,
                                      const PreviewRecord& record, bool project) {
        for(const auto& candidate : preview.records) {
            if(candidate.native_id == record.native_id) continue;
            if(project && candidate.is_task) continue;
            if(!project && candidate.is_task != record.is_task) continue;
            combo->addItem(qstr(candidate.native_title) + " — " + qstr(candidate.source_path),
                           qstr(candidate.native_id));
        }
    }

    static void add_label_candidates(QComboBox* combo, const TransferPreview& preview) {
        std::unordered_set<std::string> labels;
        for(const auto& candidate : preview.records) {
            if(!candidate.native_project_choice.starts_with("label:")) continue;
            if(!labels.insert(candidate.native_project_choice).second) continue;
            combo->addItem(qstr(candidate.native_project_choice.substr(6)),
                           qstr(candidate.native_project_choice));
        }
    }

    void select_relationship_override(QComboBox* combo, const PreviewRecord& record,
                                      bool project) const {
        const auto& overrides = project ? wizard_->mapping_.record_project_overrides
                                        : wizard_->mapping_.record_parent_overrides;
        const auto override = overrides.find(record.source_path);
        if(override != overrides.end()) {
            int index = combo->findData(qstr(override->second));
            if(index < 0) {
                combo->addItem("Unavailable override: " + qstr(override->second),
                               qstr(override->second));
                index = combo->count() - 1;
            }
            combo->setCurrentIndex(index);
        }
    }

    QComboBox* relationship_combo(const TransferPreview& preview,
                                  const PreviewRecord& record, bool project) {
        auto* combo = new QComboBox(this);
        const auto current = project ? record.native_project_choice
                                     : record.native_parent_choice;
        const QString empty = project ? "Inbox" : "No parent";
        combo->addItem("Use mapped value: " + relationship_label(preview, current, empty),
                       "__mapped__");
        combo->addItem(empty, "");
        add_record_candidates(combo, preview, record, project);
        if(project) add_label_candidates(combo, preview);
        select_relationship_override(combo, record, project);
        return combo;
    }

    void add_record_review(const TransferPreview& preview) {
        relationshipRows_.clear();
        auto* table = new QTableWidget(static_cast<int>(preview.records.size()), 6, this);
        table->setObjectName("mdbaseRecordReview");
        table->setHorizontalHeaderLabels(
            {"Kind", "Source", "Native name", "Status / due", "Project", "Parent"});
        table->horizontalHeader()->setStretchLastSection(true);
        table->setMinimumHeight(220);
        for(int row = 0; row < table->rowCount(); ++row) {
            const auto& record = preview.records[static_cast<std::size_t>(row)];
            table->setItem(row, 0, new QTableWidgetItem(record.is_task ? "Task" : "Project"));
            table->setItem(row, 1, new QTableWidgetItem(qstr(record.source_path)));
            table->setItem(row, 2, new QTableWidgetItem(qstr(record.native_title)));
            const QString state = record.is_task
                ? qstr(record.native_status_native) + " / " +
                      (record.native_due_iso ? qstr(*record.native_due_iso) : "no due date")
                : (record.native_archived ? "Archived" : "Active");
            table->setItem(row, 3, new QTableWidgetItem(state));
            QComboBox* project = record.is_task
                ? relationship_combo(preview, record, true) : nullptr;
            auto* parent = relationship_combo(preview, record, false);
            if(project) table->setCellWidget(row, 4, project);
            else table->setItem(row, 4, new QTableWidgetItem("—"));
            table->setCellWidget(row, 5, parent);
            relationshipRows_.push_back({record.source_path, project, parent});
            if(project) connect(project, &QComboBox::currentIndexChanged, this,
                                [this]{ apply_relationship_overrides(); });
            connect(parent, &QComboBox::currentIndexChanged, this,
                    [this]{ apply_relationship_overrides(); });
        }
        table->resizeColumnsToContents();
        layout_->addWidget(new QLabel("Converted records and relationship overrides:", this));
        layout_->addWidget(table);
    }

    static void apply_override(std::unordered_map<std::string, std::string>& overrides,
                               const std::string& path, QComboBox* combo) {
        if(!combo) return;
        const std::string value = combo->currentData().toString().toStdString();
        if(value == "__mapped__") overrides.erase(path);
        else overrides[path] = value;
    }

    void apply_relationship_overrides() {
        for(const auto& row : relationshipRows_) {
            apply_override(wizard_->mapping_.record_project_overrides,
                           row.sourcePath, row.project);
            apply_override(wizard_->mapping_.record_parent_overrides,
                           row.sourcePath, row.parent);
        }
        wizard_->rebuild_preview();
        refresh_preview_label();
        emit completeChanged();
    }

    void refresh_preview_label() {
        if(previewLabel_ && wizard_->preview_) {
            previewLabel_->setText(summary_text(*wizard_->preview_));
        }
    }

    void add_destination_chooser() {
        auto* destRow = new QHBoxLayout;
        destEdit_ = new QLineEdit(this);
        destEdit_->setPlaceholderText("New workspace folder (must not exist)");
        if(!wizard_->destination_path_.empty()) destEdit_->setText(qpath(wizard_->destination_path_));
        auto* browse = new QPushButton("Browse…", this);
        connect(browse, &QPushButton::clicked, this, [this]{
            QString parent = QFileDialog::getExistingDirectory(this, "Choose parent for new workspace");
            if(parent.isEmpty()) return;
            bool ok=false;
            QString name = QInputDialog::getText(this, "New workspace folder", "Folder name:", QLineEdit::Normal, "imported-workspace", &ok);
            if(!ok || name.trimmed().isEmpty()) return;
            QString full = QDir(parent).filePath(name.trimmed());
            destEdit_->setText(full);
            wizard_->update_destination(fspath(full));
            emit completeChanged();
        });
        connect(destEdit_, &QLineEdit::textChanged, this, [this](const QString& t){
            wizard_->update_destination(fspath(t));
            emit completeChanged();
        });
        destRow->addWidget(destEdit_,1);
        destRow->addWidget(browse);
        auto* destWidget = new QWidget(this);
        destWidget->setLayout(destRow);
        layout_->addWidget(new QLabel("Destination (new workspace):", this));
        layout_->addWidget(destWidget);
    }

    void add_acknowledgements() {
        ackEmpty_ = new QCheckBox("Create an empty workspace (I acknowledge zero records)", this);
        ackEmpty_->setChecked(wizard_->mapping_.create_empty_workspace_ack);
        connect(ackEmpty_, &QCheckBox::toggled, this, [this](bool v){
            wizard_->mapping_.create_empty_workspace_ack = v;
            wizard_->rebuild_preview();
            refresh_preview_label();
            emit completeChanged();
        });
        layout_->addWidget(ackEmpty_);

        ackTitleFallback_ = new QCheckBox("Accept filename-stem fallback for missing titles", this);
        ackTitleFallback_->setChecked(wizard_->mapping_.accept_empty_title_as_filename_stem);
        connect(ackTitleFallback_, &QCheckBox::toggled, this, [this](bool v){
            wizard_->mapping_.accept_empty_title_as_filename_stem = v;
            wizard_->rebuild_preview();
            refresh_preview_label();
            emit completeChanged();
        });
        layout_->addWidget(ackTitleFallback_);

        ackWarnings_ = new QCheckBox("I have reviewed warnings and accept the listed fallbacks", this);
        layout_->addWidget(ackWarnings_);
        connect(ackWarnings_, &QCheckBox::toggled, this, [this]{ emit completeChanged(); });
    }
    MdbaseImportWizard* wizard_;
    struct RelationshipRow {
        std::string sourcePath;
        QComboBox* project{nullptr};
        QComboBox* parent{nullptr};
    };
    QVBoxLayout* layout_{nullptr};
    QLabel* previewLabel_{nullptr};
    QLineEdit* destEdit_{nullptr};
    QCheckBox* ackEmpty_{nullptr};
    QCheckBox* ackTitleFallback_{nullptr};
    QCheckBox* ackWarnings_{nullptr};
    QLabel* errorLabel_{nullptr};
    std::vector<RelationshipRow> relationshipRows_;
};

class MdbaseImportWizard::ProgressPage final : public QWizardPage {
public:
    explicit ProgressPage(MdbaseImportWizard* w): wizard_(w){
        setTitle("Progress — converting");
        setSubTitle("Conversion is running. You can cancel before publication; after publication success will be reported.");
    }
    void initializePage() override {
        setCommitPage(true);
        if(layout_) {
            QLayoutItem* it;
            while((it=layout_->takeAt(0))!=nullptr){ if(it->widget()) it->widget()->deleteLater(); delete it; }
        } else {
            layout_ = new QVBoxLayout(this);
            setLayout(layout_);
        }
        status_ = new QLabel("Preparing…", this);
        status_->setWordWrap(true);
        bar_ = new QProgressBar(this);
        bar_->setRange(0,0);
        bar_->setValue(0);
        cancelBtn_ = new QPushButton("Cancel", this);
        layout_->addWidget(status_);
        layout_->addWidget(bar_);
        layout_->addWidget(cancelBtn_);
        connect(cancelBtn_, &QPushButton::clicked, this, [this]{
            wizard_->cancellation_.cancel();
            status_->setText("Cancelling…");
            cancelBtn_->setEnabled(false);
        });
        // Start worker
        wizard_->worker_running_ = true;
        wizard_->cancellation_.cancelled.store(false, std::memory_order_relaxed);
        // Use QTimer singleShot to start after page shown
        QTimer::singleShot(50, this, [this]{ run_import(); });
    }
    bool isComplete() const override { return !wizard_->worker_running_; }
private:
    void run_import(){
        if(wizard_->worker_watcher_ && wizard_->worker_watcher_->isRunning()) return;
        if(!wizard_->snapshot_ || !wizard_->preview_ || wizard_->destination_path_.empty()){
            status_->setText("Missing preview or destination — go Back.");
            wizard_->worker_running_=false;
            emit completeChanged();
            wizard_->next();
            return;
        }
        // Check snapshot unchanged before publish
        if(!snapshot_unchanged(*wizard_->snapshot_)){
            mdbase_transfer::TransferResult r;
            r.outcome = TransferOutcome::IoFailed;
            r.error = "source_changed";
            r.diagnostics.push_back({TransferSeverity::Error, "source_changed", "source changed since snapshot; rescan required"});
            wizard_->set_last_result(r);
            wizard_->worker_running_=false;
            emit completeChanged();
            wizard_->next();
            return;
        }
        // Prevent concurrent transfer from same window? Our flag worker_running_ does.
        bar_->setRange(0, 0);
        status_->setText("Copying and converting records…");
        QPointer<ProgressPage> page(this);
        TransferProgress progress = [page, cancellation = &wizard_->cancellation_](const std::string& phase, uint64_t processed, uint64_t total){
            if(page){
                QMetaObject::invokeMethod(page, [page, phase, processed, total]{
                    if(!page) return;
                    page->status_->setText(QString("Phase: %1 %2/%3").arg(qstr(phase)).arg(processed).arg(total));
                    if(total > 0){
                        page->bar_->setRange(0, static_cast<int>(std::min<uint64_t>(total, INT_MAX)));
                        page->bar_->setValue(static_cast<int>(std::min<uint64_t>(processed, INT_MAX)));
                    }
                }, Qt::QueuedConnection);
            }
            return !cancellation->is_cancelled();
        };
        const TransferSnapshot* snapshot = &*wizard_->snapshot_;
        const TransferPreview* preview = &*wizard_->preview_;
        const std::filesystem::path destination = wizard_->destination_path_;
        wizard_->worker_watcher_ = new QFutureWatcher<TransferResult>(wizard_);
        connect(wizard_->worker_watcher_, &QFutureWatcher<TransferResult>::finished, this, [this]{
            auto* watcher = wizard_->worker_watcher_;
            const TransferResult result = watcher->result();
            wizard_->worker_watcher_ = nullptr;
            watcher->deleteLater();
            wizard_->set_last_result(result);
            wizard_->worker_running_ = false;
            emit completeChanged();
            wizard_->next();
        });
        wizard_->worker_watcher_->setFuture(QtConcurrent::run(&wizard_->worker_pool_, [snapshot, preview, destination, cancellation = &wizard_->cancellation_, progress]{
            return execute_import(*snapshot, *preview, destination, TransferLimits{}, cancellation, progress);
        }));
    }
    MdbaseImportWizard* wizard_;
    QVBoxLayout* layout_{nullptr};
    QLabel* status_{nullptr};
    QProgressBar* bar_{nullptr};
    QPushButton* cancelBtn_{nullptr};
};

class MdbaseImportWizard::ResultPage final : public QWizardPage {
public:
    explicit ResultPage(MdbaseImportWizard* w): wizard_(w){
        setTitle("Result — import finished");
        setFinalPage(true);
    }
    void initializePage() override {
        if(layout_){
            QLayoutItem* it;
            while((it=layout_->takeAt(0))!=nullptr){ if(it->widget()) it->widget()->deleteLater(); delete it; }
        } else {
            layout_ = new QVBoxLayout(this);
            setLayout(layout_);
        }
        auto &r = wizard_->last_result_;
        QString text;
        if(r.outcome==TransferOutcome::Succeeded){
            text = QString("Import succeeded to %1\n%2 record(s), %3 attachment(s)\nReport: %4")
                .arg(qpath(r.destination))
                .arg(r.record_count).arg(r.asset_count)
                .arg(qpath(r.report_path));
            wizard_->imported_path_ = r.destination;
            wizard_->import_succeeded_ = true;
        } else if(r.outcome==TransferOutcome::Cancelled){
            text = "Import cancelled — no destination was published.";
            wizard_->import_succeeded_ = false;
        } else {
            text = QString("Import failed: %1\n").arg(qstr(r.error));
            for(auto &d: r.diagnostics) text += QString("[%1] %2 %3\n").arg(qstr(d.code)).arg(qstr(d.message)).arg(qstr(d.path));
            wizard_->import_succeeded_ = false;
        }
        auto* label = new QLabel(text, this);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout_->addWidget(label);
        if(r.outcome==TransferOutcome::Succeeded){
            auto* openFolder = new QPushButton("Open Folder", this);
            auto* openWs = new QPushButton("Open Imported Workspace", this);
            layout_->addWidget(openFolder);
            layout_->addWidget(openWs);
            connect(openFolder, &QPushButton::clicked, this, [this]{
                QDesktopServices::openUrl(QUrl::fromLocalFile(qpath(wizard_->imported_path_)));
            });
            connect(openWs, &QPushButton::clicked, this, [this]{
                wizard_->open_imported_workspace_requested_ = true;
                wizard_->accept();
            });
        } else {
            auto* closeBtn = new QPushButton("Close", this);
            layout_->addWidget(closeBtn);
            connect(closeBtn, &QPushButton::clicked, this, [this]{ wizard_->reject(); });
        }
    }
private:
    MdbaseImportWizard* wizard_;
    QVBoxLayout* layout_{nullptr};
};

MdbaseImportWizard::MdbaseImportWizard(QWidget* parent): QWizard(parent) {
    worker_pool_.setMaxThreadCount(1);
    worker_pool_.setStackSize(kTransferWorkerStackSize);
    setWindowTitle("Import from mdbase…");
    resize(800, 600);
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::NoBackButtonOnStartPage, false);
    addPage(new SourcePage(this));
    addPage(new RecordsPage(this));
    addPage(new ReviewPage(this));
    addPage(new ProgressPage(this));
    addPage(new ResultPage(this));
    setStartId(Src);
}

MdbaseImportWizard::~MdbaseImportWizard(){
    cancellation_.cancel();
    if(source_watcher_ && source_watcher_->isRunning()) source_watcher_->waitForFinished();
    if(worker_watcher_ && worker_watcher_->isRunning()) worker_watcher_->waitForFinished();
}

void MdbaseImportWizard::reject(){
    if((source_watcher_ && source_watcher_->isRunning()) ||
       (worker_watcher_ && worker_watcher_->isRunning())){
        cancellation_.cancel();
        return;
    }
    QWizard::reject();
}

void MdbaseImportWizard::closeEvent(QCloseEvent* event){
    if((source_watcher_ && source_watcher_->isRunning()) ||
       (worker_watcher_ && worker_watcher_->isRunning())){
        cancellation_.cancel();
        event->ignore();
        return;
    }
    QWizard::closeEvent(event);
}

void MdbaseImportWizard::rebuild_preview(){
    if(!snapshot_ || !inspection_) return;
    preview_ = build_transfer_preview(*snapshot_, *inspection_, mapping_, &cancellation_);
}

void MdbaseImportWizard::update_destination(const std::filesystem::path& p){
    destination_path_ = p;
}

void MdbaseImportWizard::set_last_result(const TransferResult& r){
    last_result_ = r;
}

} // namespace todobench
