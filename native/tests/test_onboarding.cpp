// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/onboarding_wizard.h"
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QTest>
#include <QTemporaryDir>
#include <QDir>
#include <fstream>
using namespace todobench;
class OnboardingTest final : public QObject {
    Q_OBJECT
private slots:
    void samplePreviewMatchesSelection();
    void finishCommitsChosenRecipe();
    void cancellationWritesNothing();
    void rejectsNonemptyDestination();
    void existingWorkspaceSkipsSampleAndPreferences();
    void failedCreationKeepsWizardOpen();
    void rendersPages();
};
namespace {
void choose_destination(OnboardingWizard& wizard, const QString& root) {
    wizard.next(); // Workflow -> Location
    wizard.findChild<QLineEdit*>("workspaceDirectory")->setText(root);
    wizard.findChild<QLineEdit*>("workspaceName")->setText("My practice workspace");
}
}
void OnboardingTest::samplePreviewMatchesSelection() {
    OnboardingWizard wizard(nullptr, [](const auto&, auto&) { return true; }, true);
    auto* choices = wizard.findChild<QListWidget*>("sampleWorkflows");
    QCOMPARE(choices->count(), 8);
    choices->setCurrentRow(5);
    QCOMPARE(wizard.selection().recipe.workflow, "client");
    QVERIFY(wizard.findChild<QListWidget*>("sampleTaskPreview")->item(0)->text().contains("Northline Plastering"));
    QVERIFY(wizard.findChild<QLabel*>("sampleDescription")->text().contains("20 tasks"));
}
void OnboardingTest::finishCommitsChosenRecipe() {
    QTemporaryDir temporary;
    bool committed = false;
    WorkspaceSetup actual;
    OnboardingWizard wizard(nullptr, [&](const auto& setup, auto&) { committed = true; actual = setup; return true; }, true);
    wizard.show();
    wizard.findChild<QListWidget*>("sampleWorkflows")->setCurrentRow(6);
    choose_destination(wizard, temporary.path());
    wizard.next(); // Preferences
    wizard.findChild<QComboBox*>("setupTheme")->setCurrentIndex(1);
    wizard.findChild<QComboBox*>("setupKeyboard")->setCurrentIndex(1);
    wizard.next(); // Review
    QVERIFY(!committed);
    wizard.button(QWizard::FinishButton)->click();
    QVERIFY(committed);
    QCOMPARE(actual.recipe.workflow, "launch");
    QCOMPARE(actual.recipe.theme, "light");
    QCOMPARE(actual.recipe.keyboard, "total_commander");
    QCOMPARE(actual.directory, std::filesystem::path(temporary.path().toStdString()));
}
void OnboardingTest::cancellationWritesNothing() {
    QTemporaryDir temporary;
    bool committed = false;
    OnboardingWizard wizard(nullptr, [&](const auto&, auto&) { committed = true; return true; }, true);
    wizard.show();
    choose_destination(wizard, temporary.path());
    wizard.reject();
    QVERIFY(!committed);
    QVERIFY(std::filesystem::is_empty(temporary.path().toStdString()));
}
void OnboardingTest::rejectsNonemptyDestination() {
    QTemporaryDir temporary;
    std::ofstream(std::filesystem::path(temporary.path().toStdString()) / "keep.txt") << "keep";
    OnboardingWizard wizard(nullptr, [](const auto&, auto&) { return true; }, true);
    wizard.show();
    choose_destination(wizard, temporary.path());
    wizard.next();
    QCOMPARE(wizard.currentId(), 2);
    QVERIFY(wizard.findChild<QLabel*>("workspaceLocationError")->text().contains("not empty"));
}
void OnboardingTest::existingWorkspaceSkipsSampleAndPreferences() {
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "existing";
    QCOMPARE(WorkspaceStore::create_workspace(root, "Existing").status, SaveStatus::Saved);
    bool existing = false;
    OnboardingWizard wizard(nullptr, [&](const auto& setup, auto&) { existing = setup.open_existing; return true; });
    wizard.show();
    wizard.findChild<QRadioButton*>("openExistingChoice")->setChecked(true);
    wizard.next();
    QCOMPARE(wizard.currentId(), 2);
    wizard.findChild<QLineEdit*>("workspaceDirectory")->setText(QString::fromStdString(root.string()));
    wizard.next();
    QCOMPARE(wizard.currentId(), 4);
    wizard.button(QWizard::FinishButton)->click();
    QVERIFY(existing);
}
void OnboardingTest::failedCreationKeepsWizardOpen() {
    QTemporaryDir temporary;
    OnboardingWizard wizard(nullptr, [](const auto&, QString& error) { error = "Disk is full"; return false; }, true);
    wizard.show();
    choose_destination(wizard, temporary.path());
    wizard.next();
    wizard.next();
    wizard.button(QWizard::FinishButton)->click();
    QVERIFY(wizard.isVisible());
    QCOMPARE(wizard.findChild<QLabel*>("setupError")->text(), QString("Disk is full"));
}
void OnboardingTest::rendersPages() {
    QTemporaryDir temporary;
    OnboardingWizard wizard(nullptr, [](const auto&, auto&) { return true; });
    wizard.show();
    wizard.findChild<QLineEdit*>("workspaceDirectory")->setText(temporary.path());
    const auto screenshots = qEnvironmentVariable("TODOBENCH_ONBOARDING_SCREENSHOTS");
    for (int page = 0; page < 5; ++page) {
        QApplication::processEvents();
        QCOMPARE(wizard.currentId(), page);
        if (!screenshots.isEmpty()) {
            QDir().mkpath(screenshots);
            QVERIFY(wizard.grab().save(screenshots + "/page-" + QString::number(page) + ".png"));
            if (page == 1) {
                auto* choices = wizard.findChild<QListWidget*>("sampleWorkflows");
                const auto selected = choices->currentRow();
                for (int row = 0; row < choices->count(); ++row) {
                    choices->setCurrentRow(row);
                    QApplication::processEvents();
                    QVERIFY(wizard.grab().save(screenshots + "/workflow-" + QString::number(row) + ".png"));
                }
                choices->setCurrentRow(selected);
            }
        }
        if (page < 4) wizard.next();
    }
}
QTEST_MAIN(OnboardingTest)
#include "test_onboarding.moc"
