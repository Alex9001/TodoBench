# Contributing to TodoBench

Bug reports, documentation improvements, and focused code changes are welcome.

## Report a problem

[Open an issue](https://github.com/Alex9001/TodoBench/issues) with your TodoBench
version, operating system, installation format, steps to reproduce, and expected
and actual behavior. For appearance issues, include the starting theme, target
theme, display scaling, and a screenshot if useful. Use a small fictional
workspace when sharing reproduction files; remove personal notes and attachments.

For a feature request, describe the task you are trying to accomplish and what
currently gets in the way. Discuss changes to the workspace format before
implementing them so other clients can remain compatible.

## Make a change

1. Fork the repository and create a focused branch from `main`.
2. Follow the [build instructions](docs/packaging.md) and existing code style.
3. Add meaningful regression coverage for behavior changes. Run the applicable
   checks in [Development](docs/development.md); native changes require the full
   suite and quality gate. Website-only changes use [the site build](docs/website.md).
4. Open a pull request describing the problem, resulting behavior, and validation.
   Include screenshots for visible interface changes.

Preserve unknown metadata and settings, stable identities, and untouched Markdown
bodies. The [workspace specification](docs/workspace-format-v1.md) and independent
fixtures are public interfaces. Keep general-purpose samples fictional.

Contributions are distributed under the project's GPL-3.0-or-later license.
Retain applicable third-party license notices. Do not commit build outputs,
credentials, personal workspaces, or downloaded release packages.
