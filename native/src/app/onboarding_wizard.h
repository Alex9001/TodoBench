// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "storage/sample_workspaces.h"
#include <QWizard>
#include <functional>
class QLineEdit;
class QRadioButton;
class QListWidget;
class QLabel;
class QComboBox;
namespace todobench {
struct WorkspaceSetup {
    std::filesystem::path directory;
    WorkspaceRecipe recipe;
    bool open_existing{false};
};
class OnboardingWizard final : public QWizard {
public:
    using Commit = std::function<bool(const WorkspaceSetup&, QString&)>;
    explicit OnboardingWizard(QWidget* parent, Commit commit, bool new_only = false, const QString& notice = {});
    WorkspaceSetup selection() const;
protected:
    void initializePage(int id) override;
    void accept() override;
private:
    void create_welcome(const QString& notice);
    void create_workflows();
    void create_location();
    void create_preferences();
    void create_review();
    void show_workflow();
    bool validate_location();
    void update_review();
    void update_preview();
    Commit commit_;
    QRadioButton* existing_{nullptr};
    QListWidget* workflows_{nullptr};
    QLabel* description_{nullptr};
    QListWidget* task_preview_{nullptr};
    QLineEdit* directory_{nullptr};
    QLineEdit* name_{nullptr};
    QLabel* location_error_{nullptr};
    QComboBox* theme_{nullptr};
    QComboBox* keyboard_{nullptr};
    QLabel* appearance_preview_{nullptr};
    QLabel* summary_{nullptr};
    QLabel* finish_error_{nullptr};
};
}
