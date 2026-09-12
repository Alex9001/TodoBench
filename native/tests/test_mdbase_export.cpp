// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/mdbase_export.h"
#include "storage/mdbase_transfer.h"
#include "storage/workspace_store.h"
#include "storage/workspace_scanner.h"
#include "storage/mdbase_bridge_client.h"
#include "app/workspace_controller.h"
#include "tb_test_assertions.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QTest>
#include <fstream>
#include <filesystem>
#include <stdexcept>

using namespace todobench;
using namespace todobench::mdbase_transfer;

class MdbaseExportTest : public QObject {
  Q_OBJECT
private slots:
    void emptyWorkspaceExportsAndValidates();
    void nestedProjectsPreserveLayout();
    void existingDestinationIsError();
    void throwingProgressCleansStaging();
};

namespace {
std::filesystem::path create_sample_workspace(const std::filesystem::path& root, bool withNested) {
    std::string err;
    // NOLINTNEXTLINE - ensure workspace creation checks return status
    (void)WorkspaceStore::create_workspace(root, "ExportTest");
    WorkspaceScanner scanner;
    auto snap = scanner.scan(root);
    auto inbox = snap.projects.begin()->first;
    // Add a task with subtask
    WorkspaceController ctrl;
    ctrl.open_workspace(root, err);
    std::string taskId, subId, proj2;
    ctrl.create_task(inbox, "Root task", taskId, err);
    ctrl.create_subtask(taskId, "Subtask", subId, err);
    if (withNested) {
        ctrl.create_project("ChildProj", inbox, proj2, err);
        std::string t2;
        ctrl.create_task(proj2, "Child project task", t2, err);
        // Write an asset
        auto taskRec = ctrl.snapshot().tasks.at(taskId);
        auto assets = std::filesystem::path(taskRec.source_path).parent_path() / "assets";
        std::filesystem::create_directories(assets);
        std::ofstream(assets / "image.png", std::ios::binary) << "PNGFAKE";
    }
    // Ensure due/recurrence etc present
    auto task = ctrl.snapshot().tasks.at(taskId);
    task.due_yaml = "2026-09-04";
    task.recurrence_yaml = "enabled: true\nmode: fixed_calendar\ninterval: 1\nunit: weeks\n";
    ctrl.save_task(task);
    return root;
}
}

void MdbaseExportTest::emptyWorkspaceExportsAndValidates() {
    QTemporaryDir tmp; TB_VERIFY(tmp.isValid());
    auto ws = std::filesystem::path(tmp.path().toStdString()) / "ws";
    auto out = std::filesystem::path(tmp.path().toStdString()) / "out_coll";
    std::string err;
    TB_VERIFY(WorkspaceStore::create_workspace(ws, "Empty").status == SaveStatus::Saved);
    ExportRequest req; req.workspace_root = ws; req.destination = out;
    auto res = export_workspace(req);
    TB_VERIFY2(res.outcome == TransferOutcome::Succeeded, qPrintable(QString::fromStdString(res.error)));
    TB_VERIFY(std::filesystem::exists(out / "mdbase.yaml"));
    TB_VERIFY(std::filesystem::exists(out / "_types/task.md"));
    TB_VERIFY(std::filesystem::exists(out / "_types/project.md"));
    TB_VERIFY(std::filesystem::exists(out / ".todobench/mdbase_export_report.json"));
    // Validate via bridge
    todobench::mdbase::CollectionHandle h; QString oerr; auto open = todobench::mdbase::open_collection(out, h, &oerr); TB_VERIFY2(open.valid, qPrintable(oerr));
    auto insp = h.inspect(); TB_VERIFY(insp.valid);
    // Oracle check would be via python; here ensure no crash and types present
    TB_VERIFY2(insp.result.value("types").toArray().size()==2, qPrintable(QString::fromStdString(res.error)));
    // Source unchanged
    TB_VERIFY(std::filesystem::exists(ws / "settings.json"));
}

void MdbaseExportTest::nestedProjectsPreserveLayout() {
    QTemporaryDir tmp; TB_VERIFY(tmp.isValid());
    auto ws = std::filesystem::path(tmp.path().toStdString()) / "ws2";
    auto out = std::filesystem::path(tmp.path().toStdString()) / "out2";
    create_sample_workspace(ws, true);
    ExportRequest req; req.workspace_root = ws; req.destination = out;
    auto res = export_workspace(req);
    std::string diagMsg = res.diagnostics.empty() ? std::string{} : res.diagnostics[0].message;
    TB_VERIFY2(res.outcome == TransferOutcome::Succeeded, qPrintable(QString::fromStdString(res.error + " " + diagMsg)));
    // Check preserved asset bytes exactly
    WorkspaceScanner sc; auto snap = sc.scan(ws);
    // Find a task with assets
    bool foundAsset=false;
    for (auto& [id, task] : snap.tasks) {
        auto srcAsset = std::filesystem::path(task.source_path).parent_path() / "assets/image.png";
        if (std::filesystem::exists(srcAsset)) {
            std::string rel = std::filesystem::relative(std::filesystem::path(task.source_path).parent_path(), ws).generic_string();
            auto outAsset = out / rel / "assets/image.png";
            TB_VERIFY2(std::filesystem::exists(outAsset), qPrintable(QString::fromStdString(outAsset.string())));
            std::ifstream a(srcAsset, std::ios::binary), b(outAsset, std::ios::binary);
            std::string sa((std::istreambuf_iterator<char>(a)), std::istreambuf_iterator<char>());
            std::string sb((std::istreambuf_iterator<char>(b)), std::istreambuf_iterator<char>());
            TB_COMPARE(sa, sb);
            foundAsset=true;
        }
    }
    TB_VERIFY(foundAsset);
    // Check links resolve
    todobench::mdbase::CollectionHandle h; QString oerr; TB_VERIFY(todobench::mdbase::open_collection(out, h, &oerr).valid);
    for (auto& [id, task] : snap.tasks) {
        std::string rel = std::filesystem::relative(std::filesystem::path(task.source_path), ws).generic_string();
        QJsonObject in; in["path"]=QString::fromStdString(rel);
        auto r = h.read(in); TB_VERIFY(r.valid);
        TB_VERIFY(r.result.value("frontmatter").toObject().contains("todobench_project_link"));
    }
}

void MdbaseExportTest::existingDestinationIsError() {
    QTemporaryDir tmp; TB_VERIFY(tmp.isValid());
    auto ws = std::filesystem::path(tmp.path().toStdString()) / "ws3";
    auto out = std::filesystem::path(tmp.path().toStdString()) / "out3";
    std::string err;
    TB_VERIFY(WorkspaceStore::create_workspace(ws, "Empty3").status == SaveStatus::Saved);
    std::filesystem::create_directories(out);
    ExportRequest req; req.workspace_root = ws; req.destination = out;
    auto res = export_workspace(req);
    TB_VERIFY(res.outcome != TransferOutcome::Succeeded);
    bool found=false; for (auto& d: res.diagnostics) if (d.code=="destination_exists") found=true;
    TB_VERIFY(found);
}

void MdbaseExportTest::throwingProgressCleansStaging() {
    QTemporaryDir tmp; TB_VERIFY(tmp.isValid());
    const auto root = std::filesystem::path(tmp.path().toStdString());
    const auto workspace = root / "ws-throw";
    const auto destination = root / "out-throw";
    create_sample_workspace(workspace, false);

    ExportRequest request;
    request.workspace_root = workspace;
    request.destination = destination;
    request.progress = [](const std::string& phase, uint64_t, uint64_t) {
        if (phase == "copy") throw std::runtime_error("injected progress failure");
        return true;
    };
    const auto result = export_workspace(request);
    TB_VERIFY(result.outcome != TransferOutcome::Succeeded);
    TB_VERIFY(!std::filesystem::exists(destination));
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const std::string name = entry.path().filename().string();
        TB_VERIFY2(!name.starts_with(".todobench-stage-"), name.c_str());
    }
}

QTEST_MAIN(MdbaseExportTest)
#include "test_mdbase_export.moc"
