// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/workspace_archive.h"
#include "storage/workspace_scanner.h"
#include "storage/workspace_store.h"
#include "storage/settings_codec.h"
#include <QCoreApplication>
#include <iostream>
int edit_workspace(const std::filesystem::path& root) {
    const auto snapshot = todobench::WorkspaceScanner{}.scan(root);
    if (!snapshot.diagnostics.empty()) { std::cerr << snapshot.diagnostics[0].message; return 1; }
    const todobench::WorkspaceStore store(root);
    for (auto [id, task] : snapshot.tasks) {
        task.priority = todobench::Priority::High;
        if (store.save_task(task).status != todobench::SaveStatus::Saved) return 1;
    }
    for (auto [id, project] : snapshot.projects) {
        project.display_name = "TodoBench edit";
        if (store.save_project(project).status != todobench::SaveStatus::Saved) return 1;
    }
    auto loaded = todobench::load_settings(root / "settings.json");
    if (!std::holds_alternative<todobench::Settings>(loaded)) return 1;
    auto settings = std::get<todobench::Settings>(loaded);
    settings.saved_views.at(0).sort = todobench::TaskSort::Due;
    std::string error;
    return todobench::save_settings(root / "settings.json", settings, error) ? 0 : 1;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 3) return 2;
    const std::string mode = argv[1];
    if (argc == 4 && mode == "export") return todobench::WorkspaceArchive::export_workspace(argv[2], argv[3]).success ? 0 : 1;
    if (argc == 4 && mode == "import") return todobench::WorkspaceArchive::import_workspace(argv[2], argv[3]).success ? 0 : 1;
    if (mode == "edit") return edit_workspace(argv[2]);
    return 2;
}
