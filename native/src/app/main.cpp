// SPDX-License-Identifier: GPL-3.0-or-later
#include <QApplication>
#include "app/icons.h"
#include <QTimer>
#include "app/instance_guard.h"
#include "app/main_window.h"

#include <iostream>
#include <filesystem>
#include <string>

int main(int argc, char* argv[]) {
    std::filesystem::path initial_workspace;
    if (argc > 1) {
        const std::string argument = argv[1];
        if (argument == "--version" || argument == "-v") {
            std::cout << "TodoBench " TODOBENCH_VERSION "\n";
            return 0;
        }
        if (argument == "--help" || argument == "-h") {
            std::cout << "TodoBench " TODOBENCH_VERSION "\nNative Markdown workspace task manager.\n\nUsage: TodoBench [workspace-directory]\n";
            return 0;
        }
        initial_workspace = std::filesystem::path(argument);
    }
    QApplication application(argc, argv);
    application.setOrganizationName("TodoBench");
    application.setApplicationName("TodoBench");
    application.setApplicationVersion(TODOBENCH_VERSION);
    application.setWindowIcon(todobench::launcher_icon());
    application.setQuitOnLastWindowClosed(false);
    todobench::InstanceGuard guard;
    if (!guard.become_primary()) return 0;
    todobench::MainWindow window;

    guard.on_activation([&window] {
        window.setWindowState(window.windowState() & ~Qt::WindowMinimized);
        window.show();
        window.showNormal();
        window.raise();
        window.activateWindow();
    });
    window.show();
    QTimer::singleShot(0, &window, [&window, initial_workspace] { window.start_session(initial_workspace); });
    return application.exec();
}
