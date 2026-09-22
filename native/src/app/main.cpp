// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/icons.h"
#include "app/instance_guard.h"
#include "app/main_window.h"
#include "app/startup_diagnostics.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>
#include <filesystem>
#include <iostream>
#include <cstdio>
#include <stdexcept>

namespace {
struct Options {
    std::filesystem::path workspace, state, report;
    bool diagnostics{false}, safe_start{false}, version{false}, help{false};
};
Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string value = argv[i];
        if (value == "--diagnostics") options.diagnostics = true;
        else if (value == "--safe-start") options.safe_start = true;
        else if (value == "--version" || value == "-v") options.version = true;
        else if (value == "--help" || value == "-h") options.help = true;
        else if (value == "--state-dir" || value == "--startup-check") {
            if (++i == argc) throw std::runtime_error("Missing value for " + value);
            (value == "--state-dir" ? options.state : options.report) = std::filesystem::absolute(argv[i]);
        } else if (value.starts_with('-') || !options.workspace.empty()) throw std::runtime_error("Unknown argument: " + value);
        else options.workspace = std::filesystem::absolute(value);
    }
    return options;
}
bool write_report(const Options& options, const QString& state, qint64 pid, bool ready) {
    if (options.report.empty()) return true;
    QSaveFile output(QString::fromStdString(options.report.string()));
    const auto bytes = QJsonDocument(QJsonObject{{"state", state}, {"ready", ready}, {"pid", pid},
        {"platform", QApplication::platformName()}, {"version", TODOBENCH_VERSION},
        {"logs", todobench::application_log_directory()}}).toJson();
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
}
void schedule_startup_check(QApplication& application, todobench::MainWindow& window, const Options& options) {
    if (options.report.empty()) return;
    auto* timer = new QTimer(&application);
    timer->setInterval(100);
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();
    auto ticks = std::make_shared<int>(0);
    QObject::connect(timer, &QTimer::timeout, &window, [&, timer, elapsed, ticks, options] {
        const auto state = window.startup_state();
        const bool visible = window.isVisible() && window.windowHandle() && window.windowHandle()->isExposed();
        if (visible && !state.isEmpty()) ++*ticks;
        else *ticks = 0;
        if (*ticks >= 2) {
            todobench::log_startup_stage("ready-" + state);
            if (!write_report(options, state, QCoreApplication::applicationPid(), true)) application.exit(3);
            timer->stop();
        } else if (elapsed->elapsed() >= 30000) {
            todobench::log_startup_stage("startup-timeout");
            write_report(options, "timeout", QCoreApplication::applicationPid(), false);
            application.exit(4);
        }
    });
    timer->start();
}
int launch_gui(int argc, char** argv, const Options& options) {
    todobench::log_startup_stage("qapplication-started");
    QApplication application(argc, argv);
    todobench::log_startup_stage("qapplication-created");
    application.setWindowIcon(todobench::launcher_icon());
    application.setQuitOnLastWindowClosed(false);
    const auto state = options.state.empty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                                            : QString::fromStdString(options.state.string());
    todobench::InstanceGuard guard;
    const auto outcome = guard.start(state, QString::fromStdString(options.workspace.string()));
    if (outcome == todobench::InstanceOutcome::Failed) {
        todobench::log_startup_stage("ipc-failed");
        std::cerr << guard.error().toStdString() << '\n';
        write_report(options, "ipc-failed", 0, false);
        return 2;
    }
    if (outcome == todobench::InstanceOutcome::Activated) {
        todobench::log_startup_stage("existing-instance-activated");
        return write_report(options, "activated", guard.primary_pid(), true) ? 0 : 3;
    }
    todobench::log_startup_stage("primary-instance");
    todobench::MainWindow window;
    todobench::log_startup_stage("main-window-created");
    guard.on_activation([&window](const QString& workspace) {
        window.setWindowState(window.windowState() & ~Qt::WindowMinimized);
        window.showNormal();
        window.raise();
        window.activateWindow();
        if (!workspace.isEmpty()) QTimer::singleShot(0, &window, [&window, workspace] { window.open_workspace(workspace.toStdString()); });
        todobench::log_startup_stage("activation-handled");
    });
    window.show();
    schedule_startup_check(application, window, options);
    QTimer::singleShot(0, &window, [&window, options] { window.start_session(options.workspace, options.safe_start); });
    return application.exec();
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        const auto options = parse_options(argc, argv);
        if (options.help || options.version) {
            std::cout << "TodoBench " TODOBENCH_VERSION "\n";
            if (options.help) std::cout << "Native Markdown workspace task manager.\nUsage: TodoBench [--diagnostics] [--safe-start] [workspace-directory]\nInternal checks: --startup-check <report.json> --state-dir <directory>\n";
            return 0;
        }
        QCoreApplication::setOrganizationName("TodoBench");
        QCoreApplication::setApplicationName("TodoBench");
        QCoreApplication::setApplicationVersion(TODOBENCH_VERSION);
        if (!options.state.empty()) {
            std::filesystem::create_directories(options.state);
            QSettings::setDefaultFormat(QSettings::IniFormat);
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromStdString((options.state / "settings").string()));
        }
        if (!options.report.empty() && !options.state.empty()) {
            const auto stderr_path = (options.state / "stderr.log").string();
            if (!std::freopen(stderr_path.c_str(), "a", stderr)) return 2;
        }
        if (!todobench::initialize_diagnostics(options.state)) { std::cerr << "Cannot initialize startup logs\n"; return 2; }
        if (options.diagnostics) { QCoreApplication application(argc, argv); todobench::log_startup_stage("diagnostics"); std::cout << todobench::application_diagnostics().toStdString(); return 0; }
        return launch_gui(argc, argv, options);
    } catch (const std::exception& error) {
        todobench::log_startup_stage("startup-exception");
        std::cerr << error.what() << '\n';
        return 2;
    }
}
