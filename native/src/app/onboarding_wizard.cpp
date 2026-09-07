// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/onboarding_wizard.h"
#include "app/theme.h"
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>
#include <QDir>

namespace todobench {
namespace {
enum Page { Welcome, Workflow, Location, Preferences, Review };
class SetupPage final : public QWizardPage {
public:
    std::function<int()> next;
    std::function<bool()> validate;
    int nextId() const override { return next ? next() : QWizardPage::nextId(); }
    bool validatePage() override { return validate ? validate() : true; }
    void require(const QString& name, QWidget* widget) { registerField(name + "*", widget); }
};
SetupPage* make_page(QWizard& wizard, int id, const QString& title, const QString& subtitle) {
    auto* page = new SetupPage;
    page->setTitle(title);
    page->setSubTitle(subtitle);
    wizard.setPage(id, page);
    return page;
}
QLabel* paragraph(const QString& text, QVBoxLayout* layout) {
    auto* label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    layout->addWidget(label);
    return label;
}
QString location_problem(const WorkspaceSetup& setup) {
    if (setup.directory.empty()) return "Choose a workspace folder.";
    if (!setup.directory.is_absolute()) return "Use a full folder path, or choose Browse.";
    std::error_code error;
    const bool exists = std::filesystem::exists(setup.directory, error);
    if (error) return QString::fromStdString(error.message());
    if (setup.open_existing) {
        if (std::filesystem::is_directory(setup.directory / "projects", error)) return {};
        return "This folder does not contain a workspace. Select the folder containing settings.json and projects.";
    }
    if (setup.recipe.name.empty()) return "Give your workspace a name.";
    if (std::filesystem::is_symlink(setup.directory, error)) return "Choose the actual folder rather than a symbolic link.";
    if (!exists) return {};
    if (!std::filesystem::is_directory(setup.directory, error)) return "Choose a folder, not a file.";
    if (!std::filesystem::is_empty(setup.directory, error)) return "This folder is not empty. Choose an empty folder, or go Back and open the existing workspace.";
    return error ? QString::fromStdString(error.message()) : QString{};
}
}  // namespace

OnboardingWizard::OnboardingWizard(QWidget* parent, Commit commit, bool new_only, const QString& notice)
    : QWizard(parent), commit_(std::move(commit)) {
    setObjectName("onboardingWizard");
    setWindowTitle("Welcome to TodoBench");
    setWizardStyle(QWizard::ModernStyle);
    resize(780, 620);
    setMinimumSize(640, 520);
    setOption(QWizard::NoBackButtonOnStartPage);
    setButtonText(QWizard::CancelButton, "Not now");
    create_welcome(notice);
    create_workflows();
    create_location();
    create_preferences();
    create_review();
    if (new_only) setStartId(Workflow);
}

void OnboardingWizard::create_welcome(const QString& notice) {
    auto* page = make_page(*this, Welcome, "A place for what you need to do", "Choose how you want to get started.");
    auto* layout = new QVBoxLayout(page);
    paragraph("Keep a simple personal list or organise a whole project. TodoBench saves tasks and notes in a folder you control, and reopens your last workspace when you return.", layout);
    if (!notice.isEmpty()) paragraph(notice, layout)->setObjectName("startupNotice");
    auto* create = new QRadioButton("Create a workspace — empty or with a sample", page);
    create->setObjectName("createWorkspaceChoice");
    create->setChecked(true);
    existing_ = new QRadioButton("Open a workspace I already have", page);
    existing_->setObjectName("openExistingChoice");
    layout->addWidget(create);
    layout->addWidget(existing_);
    paragraph("Next, choose a workflow and see the tasks it includes. Nothing is written until you choose Create workspace on the final page.", layout);
    layout->addStretch();
    page->next = [this] { return existing_->isChecked() ? Location : Workflow; };
}

void OnboardingWizard::create_workflows() {
    auto* page = make_page(*this, Workflow, "Choose your starting point", "Editable examples for everyday life and work. Choose one, or start empty.");
    auto* layout = new QHBoxLayout(page);
    workflows_ = new QListWidget(page);
    workflows_->setObjectName("sampleWorkflows");
    workflows_->setMinimumWidth(220);
    workflows_->setWordWrap(true);
    for (const auto& value : sample_workflows()) {
        const auto workflow = value.toObject();
        auto* item = new QListWidgetItem(workflow.value("name").toString(), workflows_);
        item->setData(Qt::UserRole, workflow.value("id").toString());
        item->setSizeHint(QSize(200, 54));
    }
    layout->addWidget(workflows_, 2);
    auto* preview = new QVBoxLayout;
    description_ = paragraph({}, preview);
    description_->setObjectName("sampleDescription");
    paragraph("A few tasks in this starting point:", preview);
    task_preview_ = new QListWidget(page);
    task_preview_->setObjectName("sampleTaskPreview");
    task_preview_->setSelectionMode(QAbstractItemView::NoSelection);
    task_preview_->setWordWrap(true);
    preview->addWidget(task_preview_, 1);
    layout->addLayout(preview, 3);
    connect(workflows_, &QListWidget::currentRowChanged, this, [this] { show_workflow(); });
    workflows_->setCurrentRow(1);
}

void OnboardingWizard::show_workflow() {
    const auto workflow = sample_workflow(workflows_->currentItem()->data(Qt::UserRole).toString().toStdString());
    description_->setText(workflow.value("description").toString() + "\n\n" + workflow.value("features").toString());
    task_preview_->clear();
    for (const auto& task : workflow.value("tasks").toArray()) {
        if (task_preview_->count() == 5) break;
        task_preview_->addItem(task.toObject().value("title").toString());
    }
    if (task_preview_->count() != 0) return;
    const auto tutorial = workflow.value("id").toString() == "tutorial";
    task_preview_->addItems(tutorial ? QStringList{"01 Start here", "02 Edit Markdown notes", "03 Filter tasks", "04 Try subtasks", "07 Attach files and back up"}
                                    : QStringList{"Your empty Inbox — ready for your first task"});
}

void OnboardingWizard::create_location() {
    auto* page = make_page(*this, Location, "Choose your workspace folder", "Files go directly into this folder. The workspace name is a label, not an extra subfolder.");
    auto* layout = new QVBoxLayout(page);
    auto* form = new QFormLayout;
    name_ = new QLineEdit("My Tasks", page);
    name_->setObjectName("workspaceName");
    form->addRow("Workspace &name", name_);
    auto* row = new QHBoxLayout;
    directory_ = new QLineEdit(page);
    directory_->setObjectName("workspaceDirectory");
    directory_->setPlaceholderText("Choose a folder, or enter its full path");
    auto* browse = new QPushButton("Browse…", page);
    browse->setObjectName("browseWorkspace");
    row->addWidget(directory_, 1);
    row->addWidget(browse);
    form->addRow("Workspace &folder", row);
    layout->addLayout(form);
    paragraph("For a new workspace, use an empty folder or type the path of a new folder. To open existing work, select its folder containing settings.json and projects. Your files stay where you choose.", layout);
    location_error_ = paragraph({}, layout);
    location_error_->setObjectName("workspaceLocationError");
    layout->addStretch();
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getExistingDirectory(this, "Select the workspace folder", directory_->text());
        if (!path.isEmpty()) directory_->setText(path);
    });
    page->require("workspaceDirectory", directory_);
    page->validate = [this] { return validate_location(); };
    page->next = [this] { return existing_->isChecked() ? Review : Preferences; };
}

void OnboardingWizard::create_preferences() {
    auto* page = make_page(*this, Preferences, "Make it comfortable", "These choices belong to this workspace. You can change them later in Settings.");
    auto* layout = new QVBoxLayout(page);
    auto* form = new QFormLayout;
    theme_ = new QComboBox(page);
    theme_->setObjectName("setupTheme");
    for (const auto& preset : theme_presets()) theme_->addItem(preset.label, QString::fromStdString(preset.id));
    keyboard_ = new QComboBox(page);
    keyboard_->setObjectName("setupKeyboard");
    keyboard_->addItem("Browser — familiar Ctrl+T, Ctrl+Tab and Ctrl+W", "browser");
    keyboard_->addItem("Total Commander — function keys for task actions", "total_commander");
    form->addRow("&Theme", theme_);
    form->addRow("&Keyboard preset", keyboard_);
    layout->addLayout(form);
    appearance_preview_ = paragraph("A calm place to plan your next step.\n\nNew Task adds an item. Complete / Reopen ticks it off.", layout);
    appearance_preview_->setAutoFillBackground(true);
    appearance_preview_->setMargin(24);
    appearance_preview_->setMinimumHeight(150);
    connect(theme_, &QComboBox::currentIndexChanged, this, [this] { update_preview(); });
    update_preview();
    paragraph("Every action also has a visible menu or button. You don't need to learn shortcuts. Sample reminders start off; notifications are yours to enable.", layout);
    layout->addStretch();
}

void OnboardingWizard::update_preview() {
    appearance_preview_->setPalette(theme_palette(theme_->currentData().toString().toStdString(), {}, system_theme_palette()));
}

void OnboardingWizard::create_review() {
    auto* page = make_page(*this, Review, "Ready when you are", "Review the destination, then take your first step.");
    auto* layout = new QVBoxLayout(page);
    summary_ = paragraph({}, layout);
    summary_->setObjectName("setupSummary");
    paragraph("Your first steps\n\n1. Select a task to read or edit its notes. Changes save automatically.\n2. Use New Task to capture your own work, and Complete / Reopen to finish it.\n3. Use Open tab… beside the tabs for projects and saved views.\n4. Type words in the filter to find tasks instantly.\n\nSamples are ordinary, editable tasks. Move examples to Trash when you're ready; you can restore them later. File → Export Workspace creates a backup of the whole folder.", layout);
    finish_error_ = paragraph({}, layout);
    finish_error_->setObjectName("setupError");
    layout->addStretch();
    page->setFinalPage(true);
}

WorkspaceSetup OnboardingWizard::selection() const {
    WorkspaceSetup result;
    result.directory = directory_->text().trimmed().toStdString();
    result.open_existing = existing_->isChecked();
    result.recipe.name = name_->text().trimmed().toStdString();
    result.recipe.workflow = workflows_->currentItem()->data(Qt::UserRole).toString().toStdString();
    result.recipe.theme = theme_->currentData().toString().toStdString();
    result.recipe.keyboard = keyboard_->currentData().toString().toStdString();
    return result;
}

bool OnboardingWizard::validate_location() {
    const auto problem = location_problem(selection());
    location_error_->setText(problem);
    return problem.isEmpty();
}

void OnboardingWizard::initializePage(int id) {
    QWizard::initializePage(id);
    if (id == Location) name_->setEnabled(!existing_->isChecked());
    if (id == Review) update_review();
}

void OnboardingWizard::update_review() {
    const auto setup = selection();
    const auto action = setup.open_existing ? "Open existing workspace" : "Create workspace: " + QString::fromStdString(setup.recipe.name);
    auto summary = action + "\n\nFolder: " + QString::fromStdString(setup.directory.string());
    if (!setup.open_existing) summary += "\n\nStarting point: " + sample_workflow(setup.recipe.workflow).value("name").toString()
        + "\nAppearance: " + theme_->currentText() + "\nControls: " + keyboard_->currentText();
    summary_->setText(summary + "\n\nTodoBench will reopen this workspace on your next launch.");
    setButtonText(QWizard::FinishButton, setup.open_existing ? "Open workspace" : "Create workspace");
    finish_error_->clear();
}

void OnboardingWizard::accept() {
    if (!validate_location()) { finish_error_->setText(location_error_->text()); return; }
    QString error;
    if (!commit_(selection(), error)) { finish_error_->setText(error); return; }
    QWizard::accept();
}
}  // namespace todobench
