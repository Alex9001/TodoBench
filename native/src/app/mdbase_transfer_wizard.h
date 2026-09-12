// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "storage/mdbase_transfer.h"
#include "storage/mdbase_import.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QWizard>
#include <QWizardPage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <QProgressBar>
#include <QCheckBox>
#include <QComboBox>
#include <QTableWidget>

#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <optional>

class QCloseEvent;
class QProgressDialog;

namespace todobench {

class WorkspaceController;

// ---- Export dialog (7.1) ----
class MdbaseExportDialog final : public QDialog {
    Q_OBJECT
public:
    explicit MdbaseExportDialog(const std::filesystem::path& workspace_root,
                                std::size_t project_count,
                                std::size_t task_count,
                                std::size_t diagnostic_count,
                                QWidget* parent = nullptr);
    ~MdbaseExportDialog() override;

    void reject() override;

private slots:
    void choose_destination();
    void run_export();

private:
    void start_summary_scan();
    void refresh_summary();
    void finish_export(const mdbase_transfer::TransferResult& result);
    void closeEvent(QCloseEvent* event) override;
    std::filesystem::path workspace_root_;
    std::size_t project_count_{0};
    std::size_t task_count_{0};
    std::size_t diagnostic_count_{0};
    std::size_t attachment_count_{0};
    std::size_t support_file_count_{0};
    std::size_t inventory_warning_count_{0};
    std::uint64_t source_bytes_{0};
    bool summary_ready_{false};
    QLineEdit* dest_edit_{nullptr};
    QLabel* summary_{nullptr};
    QPushButton* export_button_{nullptr};
    QLabel* error_{nullptr};
    QProgressDialog* export_progress_{nullptr};
    QFutureWatcher<void>* summary_watcher_{nullptr};
    QFutureWatcher<mdbase_transfer::TransferResult>* export_watcher_{nullptr};
    mdbase_transfer::TransferCancellation summary_cancellation_;
    mdbase_transfer::TransferCancellation export_cancellation_;
};

// ---- Import wizard (7.2) 5 pages: Source -> Records -> Review -> Progress -> Result ----
class MdbaseImportWizard final : public QWizard {
    Q_OBJECT
public:
    explicit MdbaseImportWizard(QWidget* parent = nullptr);
    ~MdbaseImportWizard() override;

    void reject() override;

    // Result after accepted wizard
    std::filesystem::path imported_workspace_path() const { return imported_path_; }
    bool import_succeeded() const { return import_succeeded_; }
    bool open_imported_workspace_requested() const { return open_imported_workspace_requested_; }

private:
    // pages
    class SourcePage;
    class RecordsPage;
    class ReviewPage;
    class ProgressPage;
    class ResultPage;

    friend class SourcePage;
    friend class RecordsPage;
    friend class ReviewPage;
    friend class ProgressPage;
    friend class ResultPage;

    // State held across pages
    std::optional<mdbase_transfer::TransferSnapshot> snapshot_;
    std::optional<mdbase_transfer::CollectionInspection> inspection_;
    mdbase_transfer::ImportMapping mapping_;
    std::optional<mdbase_transfer::TransferPreview> preview_;
    std::filesystem::path destination_path_;
    std::filesystem::path imported_path_;
    bool import_succeeded_{false};
    bool open_imported_workspace_requested_{false};
    mdbase_transfer::TransferResult last_result_;
    mdbase_transfer::TransferCancellation cancellation_;

    QFutureWatcher<void>* source_watcher_{nullptr};
    bool source_scan_running_{false};
    QFutureWatcher<mdbase_transfer::TransferResult>* worker_watcher_{nullptr};
    bool worker_running_{false};

    void closeEvent(QCloseEvent* event) override;
    void rebuild_preview();
    void update_destination(const std::filesystem::path& p);
    void set_last_result(const mdbase_transfer::TransferResult& r);
};

} // namespace todobench
