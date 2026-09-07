// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/tutorial_workspace.h"
#include "storage/workspace_creation.h"

#include "storage/attachment_store.h"
#include "storage/settings_codec.h"
#include "storage/workspace_scanner.h"

#include <QDateTime>
#include <QTimeZone>
#include <QUuid>

#include <stdexcept>

namespace todobench {
namespace {

std::string new_id() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}

void require_saved(const SaveResult& result) {
    if (result.status != SaveStatus::Saved) throw std::runtime_error(result.message);
}

ProjectRecord add_project(const WorkspaceStore& store, const std::filesystem::path& parent,
                          const std::string& name, const std::string& slug) {
    ProjectRecord project;
    project.id = new_id();
    project.display_name = name;
    project.source_path = (parent / "projects" / (slug + "--" + project.id) / "project.md").string();
    require_saved(store.save_project(project));
    return project;
}

TaskRecord lesson(const ProjectRecord& project, int number, const std::string& title,
                  const std::string& body, const std::string& topic) {
    TaskRecord task;
    task.id = new_id();
    task.project_id = project.id;
    task.title = title;
    task.body = body;
    task.tags = {topic, "tutorial"};
    task.order = number * 1024;
    task.due_yaml = "null";
    task.recurrence_yaml = "null";
    task.created_at = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString();
    task.updated_at = task.created_at;
    task.revision = new_id();
    task.source_path = (std::filesystem::path(project.source_path).parent_path() / "tasks"
        / ("lesson-" + std::to_string(number) + "--" + task.id) / "task.md").string();
    return task;
}

std::string add_lessons(const WorkspaceStore& store, const ProjectRecord& project) {
    auto welcome = lesson(project, 1, "01 Start here", R"md(# Welcome to TodoBench

These are real tasks, so you can edit, complete, move, or delete them as you learn.
Everything is saved as Markdown inside this workspace folder.

## A five-minute tour

1. Select the numbered tasks on the left and try each exercise.
2. Expand **04 Try subtasks** to see a real task branch.
3. Open the **Launch example** tab to explore a nested project.
4. Use **Inbox** for your own tasks; it starts empty.

Edits save automatically after a short pause. **Task > Save Task** saves immediately.
Use **Complete / Reopen** to tick off a lesson, or press Space with the task list focused.

## When you are finished

Use **Move to Trash** on lessons you no longer need. Trashing a parent also removes
its subtasks; **Task > Restore from Trash** brings them back. Repeat for the four
sample tasks in Launch example. Your Inbox and your own projects are separate.
Example saved views can be removed from **View > Saved Views > Delete Saved View**.

No demo reminders are enabled. You decide whether to try notifications.
)md", "learn");
    welcome.status = TaskStatus::InProgress;
    require_saved(store.create_task(welcome));

    auto notes = lesson(project, 2, "02 Edit Markdown notes", R"md(# A note you can play with

Select **these words** and use Bold, Italic, or Strikethrough above the editor.
Switch between **Visual** and **Source** to see the same note in two forms.

## Try a checklist

- [ ] Replace this line with something you want to do
- [ ] Add another checklist item using the toolbar
- [x] This item is already checked

| Feature | Try it |
| --- | --- |
| Headings | Select a paragraph, then choose H2 |
| Tags | Edit the comma-separated Tags field above |
| Source | Change this Markdown directly |

```text
Your notes live in task.md, next to an optional assets folder.
```

Metadata and notes are separate: changing a priority does not rewrite untouched notes.
)md", "notes");
    require_saved(store.create_task(notes));

    auto filters = lesson(project, 3, "03 Filter tasks", R"md(# Narrow the task list

Try these in the search field above the list, then use **Clear**:

- `tag:example` shows the sample launch tasks.
- `status:waiting` finds work waiting on someone else.
- `priority:urgent` finds the example needing attention.
- `launch tag:example` combines a title word with a tag.

A project tab already limits the search to that project; choose **All Tasks**
to search across the entire workspace. Project tabs include nested projects.

Open **View > Saved Views > Open Saved View** for the supplied examples.
Save your own combination with **Save Current View**. **Open tab…** beside the tabs opens projects and saved views.
Tags such as learn, notes, and example have colors you can change in Settings.
)md", "learn");
    require_saved(store.create_task(filters));

    auto parent = lesson(project, 4, "04 Try subtasks", R"md(# A task with independent children

Expand this row using its disclosure arrow. Each child is its own task with its
own notes, status, and priority. A Markdown checklist stays inside one task.

Try completing this parent before both children are done. TodoBench offers
**Complete the whole branch** or **Complete only this task**.

Use **Task > New Subtask** to add a third child. In **View > Sort > Manual**,
drag tasks to reorder or reparent them. Moving a parent moves its branch too.
)md", "learn");
    require_saved(store.create_task(parent));
    auto child = lesson(project, 5, "Select me: I am a separate subtask", "Change my title or priority, then complete me from the toolbar.\n", "learn");
    child.parent_id = parent.id;
    require_saved(store.create_task(child));
    child = lesson(project, 6, "Then complete the parent task", "Leave me unfinished to try the parent-completion choices.\n", "learn");
    child.parent_id = parent.id;
    require_saved(store.create_task(child));

    auto controls = lesson(project, 7, "05 Choose your controls", R"md(# Make the workspace comfortable

- Drag the divider to give more room to the list or notes.
- Try **View > Appearance** for theme and row density.
- Open **Edit > Settings** for tag colors, project icons, formatting rules,
  layout settings, and keyboard overrides.
- Choose **View > Keyboard preset > Total Commander** to try the function-key strip.
  Switch back to **Browser** whenever you like.

Hover over toolbar icons to see their purpose and assigned shortcut.
The workspace settings travel with the folder, including saved views and colors.
)md", "learn");
    require_saved(store.create_task(controls));
    return welcome.id;
}

void add_schedule_lesson(const WorkspaceStore& store, const ProjectRecord& project) {
    auto task = lesson(project, 8, "06 Try a weekly review", R"md(# Repeat without making a backlog

This task is due a week after workspace creation and repeats every week.
Use **Recurrence** above to inspect its schedule, then **Complete / Reopen**
to try a completion. The same task advances to its next date; **History** keeps
the completed notes. **Edit > Undo Completion** reverses the last completion.

- [ ] Review what went well
- [ ] Choose the next small step

## Optional reminder experiment

No reminders are configured yet. To try one, keep a due date and click **Reminders**.
Due-date reminders use 09:00 in the workspace timezone. They work while TodoBench
is running, including in the tray where supported. Explicit **File > Quit** stops them.

Use **Task > Complete and stop repeating** when you no longer want this example to repeat.
)md", "schedule");
    task.due_yaml = QDate::currentDate().addDays(7).toString(Qt::ISODate).toStdString();
    task.recurrence_yaml = "enabled: true\nmode: fixed_calendar\ninterval: 1\nunit: weeks\nreset_checklist: true\n";
    require_saved(store.create_task(task));
}

void add_attachment_lesson(const WorkspaceStore& store, const ProjectRecord& project) {
    auto task = lesson(project, 9, "07 Attach files and back up", R"md(# Files belong to their task

This lesson includes a tiny local text attachment. Inspect its relative link in Source.
Use **Task > Add Attachment** to copy one of your files into the task's assets folder;
pasted images are stored there too. Moving the task keeps those files together.

## Try a backup

1. Choose **File > Export Workspace (.7z)**.
2. Choose **File > Import Workspace (.7z)** and a new destination.
3. Open that restored workspace and find these same tasks, settings, and attachment.

You can also archive the workspace directory with 7-Zip yourself.
**File > Open Workspace Folder** opens the folder containing your Markdown files.

)md", "files");
    const auto asset = AttachmentStore::import_bytes(std::filesystem::path(task.source_path).parent_path(),
        "TodoBench tutorial attachment\n\nThis file lives with its task. It travels with moves and .7z backups.\n", ".txt");
    if (!asset.success) throw std::runtime_error(asset.error);
    task.body += "[Example attachment](" + asset.relative_link + ")\n";
    require_saved(store.create_task(task));
}

void add_examples(const WorkspaceStore& store, const ProjectRecord& project) {
    auto task = lesson(project, 20, "Launch: resolve the overdue blocker", "An intentionally overdue, urgent example. Notice its automatic formatting.\nChange the due date or priority to see the appearance change.\n", "example");
    task.priority = Priority::Urgent;
    task.due_yaml = QDate::currentDate().addDays(-1).toString(Qt::ISODate).toStdString();
    require_saved(store.create_task(task));
    task = lesson(project, 21, "Launch: waiting for feedback", "Waiting is a separate state. Try `status:waiting` in All Tasks, or open the Tutorial: waiting saved view.\n", "example");
    task.status = TaskStatus::Waiting;
    task.previous_open_status = TaskStatus::Waiting;
    require_saved(store.create_task(task));
    task = lesson(project, 22, "Launch: first draft approved", "This completed example demonstrates the Done style. Select it and use Complete / Reopen to bring it back.\n", "example");
    task.status = TaskStatus::Done;
    task.completed_at = task.created_at;
    require_saved(store.create_task(task));
    task = lesson(project, 23, "Launch: retired idea", "Cancelled tasks remain available for reference. Use the State field to change this example.\n", "example");
    task.status = TaskStatus::Cancelled;
    task.priority = Priority::Low;
    require_saved(store.create_task(task));
}

void populate(const std::filesystem::path& root, const std::string& name) {
    require_saved(WorkspaceStore::create_workspace(root, name));
    const WorkspaceStore store(root);
    const auto inbox = WorkspaceScanner{}.scan(root).projects.begin()->second;
    const auto tutorial = add_project(store, root, "Tutorial", "tutorial");
    const auto example = add_project(store, std::filesystem::path(tutorial.source_path).parent_path(),
                                     "Launch example", "launch-example");
    const auto welcome = add_lessons(store, tutorial);
    add_schedule_lesson(store, tutorial);
    add_attachment_lesson(store, tutorial);
    add_examples(store, example);
    auto settings = std::get<Settings>(load_settings(root / "settings.json"));
    settings.workspace_name = name;
    settings.timezone = QTimeZone::systemTimeZoneId().toStdString();
    settings.formatting_rules = default_formatting_rules();
    settings.tag_colors = {{"learn", "#bbdefb"}, {"notes", "#d1c4e9"}, {"example", "#ffe0b2"},
                           {"schedule", "#c8e6c9"}, {"files", "#b2ebf2"}};
    settings.saved_views = {{"Tutorial: examples", "tag:example", TaskSort::Manual},
                            {"Tutorial: waiting", "tag:tutorial status:waiting", TaskSort::Manual}};
    settings.open_view_tabs = {{"All Tasks", {}, TaskSort::Manual, welcome, 0, true},
        {"Tutorial", "project:" + tutorial.id, TaskSort::Manual, welcome, 0, false},
        {"Launch example", "project:" + example.id, TaskSort::Manual, {}, 0, false},
        {"Inbox", "project:" + inbox.id, TaskSort::Manual, {}, 0, false}};
    settings.active_view_tab = 1;
    std::string error;
    if (!save_settings(root / "settings.json", settings, error)) throw std::runtime_error(error);
}

}  // namespace

SaveResult create_tutorial_workspace(const std::filesystem::path& root, const std::string& name) {
    return create_staged_workspace(root, [&name](const std::filesystem::path& staging) { populate(staging, name); });
}

}  // namespace todobench
