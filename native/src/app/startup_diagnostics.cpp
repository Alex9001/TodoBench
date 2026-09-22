// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/startup_diagnostics.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>
#include <QVBoxLayout>
#include <cstdio>

namespace todobench {
namespace {
QString logs;
QString last_stage;
QMutex mutex;
constexpr qint64 log_limit = 1024 * 1024;
QString redact(QString value) { return value.replace(QDir::homePath(), "~"); }
void append_log(const QString& message) {
    QMutexLocker lock(&mutex);
    if (logs.isEmpty()) return;
    const auto path = logs + "/startup.log";
    auto bytes = (QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) + " " + redact(message) + "\n").toUtf8().left(8192);
    if (QFileInfo(path).size() + bytes.size() > log_limit) {
        QFile::remove(path + ".2");
        QFile::rename(path + ".1", path + ".2");
        QFile::rename(path, path + ".1");
    }
    QFile output(path);
    if (output.open(QIODevice::WriteOnly | QIODevice::Append)) { output.write(bytes); output.flush(); }
}
void qt_message(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    // Arbitrary Qt messages can contain document text. Retain their technical category
    // in logs and leave detailed output on stderr for an explicit terminal diagnostic.
    const auto category = QString("qt-message type=%1 category=%2").arg(type).arg(QString::fromLatin1(context.category));
    append_log(last_stage == "qapplication-started" ? category + " " + message : category);
    const auto stderr_message = redact(message).toUtf8();
    std::fprintf(stderr, "%s\n", stderr_message.constData());
}
} // namespace
bool initialize_diagnostics(const std::filesystem::path& state_directory) {
    if (!state_directory.empty()) logs = QString::fromStdString((state_directory / "logs").string());
#ifdef Q_OS_MACOS
    else logs = QDir::homePath() + "/Library/Logs/TodoBench";
#else
    else logs = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs";
#endif
    if (!QDir().mkpath(logs)) return false;
    QFile probe(logs + "/startup.log");
    if (!probe.open(QIODevice::WriteOnly | QIODevice::Append)) return false;
    probe.close();
    qInstallMessageHandler(qt_message);
    append_log(QString("startup version=%1 build=%2 os=%3 os-arch=%4 process-arch=%5 qt=%6")
        .arg(TODOBENCH_VERSION, QString::fromLatin1(__DATE__), QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture(), QSysInfo::buildCpuArchitecture(), qVersion()));
    return true;
}
void log_startup_stage(const QString& stage) { last_stage = stage; append_log("stage=" + stage); }
QString application_log_directory() { return logs; }
QString application_diagnostics() {
    return QString("TodoBench %1\nBuild: %2\nOS: %3\nOS architecture: %4\nProcess architecture: %5\nQt runtime: %6\nQt build: %7\nPlatform: %8\nStartup stage: %9\nLogs: %10\n")
        .arg(TODOBENCH_VERSION, QString::fromLatin1(__DATE__), QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture(),
             QSysInfo::buildCpuArchitecture(), qVersion(), QT_VERSION_STR,
             qobject_cast<QApplication*>(QCoreApplication::instance()) ? QApplication::platformName() : "GUI not initialized",
             last_stage, redact(logs));
}
void show_application_diagnostics(QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle("Application Diagnostics");
    dialog.resize(600, 350);
    auto* layout = new QVBoxLayout(&dialog);
    auto* report = new QPlainTextEdit(application_diagnostics(), &dialog);
    report->setReadOnly(true);
    layout->addWidget(report);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* copy = buttons->addButton("Copy Report", QDialogButtonBox::ActionRole);
    auto* open = buttons->addButton("Open Logs", QDialogButtonBox::ActionRole);
    QObject::connect(copy, &QPushButton::clicked, &dialog, [report] { QApplication::clipboard()->setText(report->toPlainText()); });
    QObject::connect(open, &QPushButton::clicked, &dialog, [] { QDesktopServices::openUrl(QUrl::fromLocalFile(logs)); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}
} // namespace todobench
