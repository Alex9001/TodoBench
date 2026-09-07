// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/sample_workspaces.h"
#include "storage/workspace_creation.h"
#include "storage/workspace_scanner.h"
#include "storage/settings_codec.h"
#include "storage/attachment_store.h"
#include <QFile>
#include <QJsonDocument>
#include <QTimeZone>
#include <QDateTime>
#include <QUuid>
#include <stdexcept>

static void initialize_workflow_resources() { Q_INIT_RESOURCE(workflows); }

namespace todobench {
namespace {
std::string text(const QJsonObject& value, const char* key) { return value.value(key).toString().toStdString(); }
std::string new_id() { return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(); }
void require_saved(const SaveResult& result) {
    if (result.status != SaveStatus::Saved) throw std::runtime_error(result.message);
}

QJsonArray read_catalog() {
    initialize_workflow_resources();
    QFile file(":/todobench/workflows.json");
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read sample workflows");
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isArray()) throw std::runtime_error("Invalid sample workflow catalog");
    return document.array();
}

using ProjectMap = std::unordered_map<std::string, ProjectRecord>;
ProjectMap create_projects(const std::filesystem::path& root, const QJsonObject& workflow) {
    const auto snapshot = WorkspaceScanner{}.scan(root);
    ProjectMap projects{{"inbox", snapshot.projects.begin()->second}};
    WorkspaceStore store(root);
    for (const auto& value : workflow.value("projects").toArray()) {
        const auto source = value.toObject();
        const auto parent = text(source, "parent");
        ProjectRecord project;
        project.id = new_id();
        project.display_name = text(source, "name");
        auto directory = root;
        if (!parent.empty()) {
            project.parent_id = projects.at(parent).id;
            directory = std::filesystem::path(projects.at(parent).source_path).parent_path();
        }
        project.source_path = (directory / "projects" / (text(source, "key") + "--" + project.id) / "project.md").string();
        require_saved(store.save_project(project));
        projects.emplace(text(source, "key"), std::move(project));
    }
    return projects;
}

void apply_sample_metadata(TaskRecord& task, const QJsonObject& source) {
    for (const auto& tag : source.value("tags").toArray()) task.tags.push_back(tag.toString().toStdString());
    if (source.contains("status") && !parse_task_status(text(source, "status"), task.status))
        throw std::runtime_error("Invalid sample status");
    if (source.contains("priority") && !parse_priority(text(source, "priority"), task.priority))
        throw std::runtime_error("Invalid sample priority");
    if (source.contains("due")) task.due_yaml = QDate::currentDate().addDays(source.value("due").toInt()).toString(Qt::ISODate).toStdString();
    if (source.value("weekly").toBool()) task.recurrence_yaml =
        "enabled: true\nmode: fixed_calendar\ninterval: 1\nunit: weeks\nreset_checklist: true\n";
    if (task.status == TaskStatus::Done) task.completed_at = task.updated_at;
    if (task.status == TaskStatus::Waiting) task.previous_open_status = task.status;
}

void attach_brief(TaskRecord& task, const QJsonObject& source) {
    if (!source.contains("attachment")) return;
    const auto attachment = AttachmentStore::import_bytes(std::filesystem::path(task.source_path).parent_path(),
                                                          text(source, "attachment"), ".txt");
    if (!attachment.success) throw std::runtime_error(attachment.error);
    task.body += "\n\n[Open the project brief](" + attachment.relative_link + ")\n";
}

std::string create_tasks(const std::filesystem::path& root, const QJsonObject& workflow, const ProjectMap& projects) {
    WorkspaceStore store(root);
    std::unordered_map<std::string, std::string> ids;
    std::string first;
    int order = 0;
    for (const auto& value : workflow.value("tasks").toArray()) {
        const auto source = value.toObject();
        const auto& project = projects.at(text(source, "project"));
        TaskRecord task;
        task.id = new_id();
        task.project_id = project.id;
        task.title = text(source, "title");
        task.body = text(source, "body");
        task.order = ++order * 1024;
        task.created_at = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString();
        task.updated_at = task.created_at;
        task.revision = new_id();
        if (source.contains("parent")) task.parent_id = ids.at(text(source, "parent"));
        task.source_path = (std::filesystem::path(project.source_path).parent_path() / "tasks"
                            / (text(source, "key") + "--" + task.id) / "task.md").string();
        apply_sample_metadata(task, source);
        attach_brief(task, source);
        require_saved(store.create_task(task));
        ids.emplace(text(source, "key"), task.id);
        if (first.empty()) first = task.id;
    }
    return first;
}

void configure_views(Settings& settings, const QJsonObject& workflow, const ProjectMap& projects, const std::string& first) {
    settings.open_view_tabs = {{"All Tasks", {}, TaskSort::Manual, first, 0, true}};
    const auto active = text(workflow, "active");
    for (const auto& [key, project] : projects) {
        if (key != active && key != "inbox") continue;
        settings.open_view_tabs.push_back({project.display_name, "project:" + project.id, TaskSort::Manual,
                                          key == active ? first : "", 0, false});
        if (key == active) settings.active_view_tab = static_cast<int>(settings.open_view_tabs.size()) - 1;
    }
    for (const auto& value : workflow.value("views").toArray()) {
        const auto view = value.toObject();
        settings.saved_views.push_back({text(view, "name"), text(view, "expression"), TaskSort::Manual});
    }
    settings.tag_colors = {{"client", "#bbdefb"}, {"qa", "#c8e6c9"}, {"blocker", "#ffcdd2"},
        {"assets", "#b2dfdb"}, {"review", "#d1c4e9"}, {"search", "#ffe0b2"}, {"waiting", "#fff9c4"}};
}

void populate(const std::filesystem::path& root, const WorkspaceRecipe& recipe) {
    const auto workflow = sample_workflow(recipe.workflow);
    require_saved(WorkspaceStore::create_workspace(root, recipe.name, recipe.workflow == "tutorial"));
    const auto loaded = load_settings(root / "settings.json");
    if (!std::holds_alternative<Settings>(loaded)) throw std::runtime_error("Cannot load new workspace settings");
    auto settings = std::get<Settings>(loaded);
    if (recipe.workflow != "tutorial" && recipe.workflow != "blank") {
        const auto projects = create_projects(root, workflow);
        const auto first = create_tasks(root, workflow, projects);
        configure_views(settings, workflow, projects, first);
    }
    settings.theme = recipe.theme;
    settings.keyboard_preset = recipe.keyboard;
    settings.timezone = QTimeZone::systemTimeZoneId().toStdString();
    settings.formatting_rules = default_formatting_rules();
    std::string error;
    if (!save_settings(root / "settings.json", settings, error)) throw std::runtime_error(error);
}
}  // namespace

const QJsonArray& sample_workflows() { static const auto catalog = read_catalog(); return catalog; }
QJsonObject sample_workflow(const std::string& id) {
    for (const auto& value : sample_workflows()) {
        if (text(value.toObject(), "id") == id) return value.toObject();
    }
    throw std::runtime_error("Unknown sample workflow: " + id);
}
SaveResult create_sample_workspace(const std::filesystem::path& root, const WorkspaceRecipe& recipe) {
    return create_staged_workspace(root, [&recipe](const std::filesystem::path& staging) { populate(staging, recipe); });
}
}  // namespace todobench
