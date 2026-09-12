// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/mdbase_transfer_wizard.h"
#include "tb_test_assertions.h"

#include <QElapsedTimer>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTest>

#include <filesystem>

using namespace todobench;

class MdbaseWizardTest final : public QObject {
    Q_OBJECT
private slots:
    void sourceSnapshotDoesNotBlockPageValidation();
};

void MdbaseWizardTest::sourceSnapshotDoesNotBlockPageValidation() {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                         "fixtures/mdbase-v03-minimal";
    MdbaseImportWizard wizard;
    wizard.show();
    auto* source = wizard.findChild<QLineEdit*>("mdbaseSourcePath");
    auto* cancel = wizard.findChild<QPushButton*>("mdbaseCancelSourceScan");
    TB_VERIFY(source != nullptr);
    TB_VERIFY(cancel != nullptr);
    source->setText(QString::fromStdString(fixture.string()));

    wizard.next();
    TB_COMPARE(wizard.currentId(), 0);
    TB_VERIFY(cancel->isVisible());

    QElapsedTimer timeout;
    timeout.start();
    while(wizard.currentId() == 0 && timeout.elapsed() < 10000) {
        QTest::qWait(10);
    }
    TB_COMPARE(wizard.currentId(), 1);
    auto* records = wizard.findChild<QTableWidget*>("mdbaseRecordSelection");
    TB_VERIFY(records != nullptr);
    TB_VERIFY(records->rowCount() > 0);
}

QTEST_MAIN(MdbaseWizardTest)
#include "test_mdbase_wizard.moc"
