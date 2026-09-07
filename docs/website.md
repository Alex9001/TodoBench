# Website and repository presentation

The public product site is <https://alex9001.github.io/TodoBench/>. It is a static
GitHub Pages project site, built from `site/` using Python's standard library.
There is no framework, npm install, account service, analytics, external font,
or browser-side API dependency. Downloads and screenshot links work without
JavaScript; JavaScript adds theme previews and an accessible screenshot dialog.

## Build and preview

Install Python 3.10+ and an authenticated GitHub CLI, then run from the repository:

```bash
python3 scripts/build-site.py
python3 -m http.server 4173 --directory build/pages
```

Open <http://localhost:4173/>. The build resolves the latest **published stable**
release through GitHub, verifies every platform download against its actual asset
inventory, checks local links and documentation targets, and validates image
attributes, section anchors, and structured data. It writes only to `build/pages`.
Release version strings come from the published tag, which the native release
workflow derives from CMake. An unreleased CMake version never creates download
links to nonexistent binaries.

For a reproducible offline build, save an inventory once:

```bash
gh release view --repo Alex9001/TodoBench --json tagName,isDraft,isPrerelease,url,assets > build/site-release.json
python3 scripts/build-site.py --release-data build/site-release.json
```

The deployed `release.json` records the exact inventory used. Check JavaScript
with `node --check site/app.js`; lint the workflow with `actionlint`.

## Publish

GitHub Pages is configured to use **GitHub Actions**. `.github/workflows/pages.yml`
validates pull requests, and deploys changes on `main` or a manual run. Publishing
an application release also refreshes the site from `main` using the new release's
asset inventory. When a release is published using the repository's automatic
`GITHUB_TOKEN`, GitHub may suppress a downstream release event; the release
workflow explicitly dispatches Pages after publication to cover that path.
A missing required asset fails the build and leaves the existing site in place.
Only `build/pages` is uploaded, never the whole repository or a workspace.

Update the README's versioned download table when publishing a new application
release. The site itself resolves released assets at build time. Keep all local
URLs relative so the `/TodoBench/` project prefix works correctly.

## Screenshots and branding

The site and README share original screenshots in `site/assets/screenshots/`.
`team.png`, `home.png`, and `everyday.png` were captured from the built-in sample
workspaces at 1280 × 820 using the native `sampleWorkflowRenders` test. They use
fictional sample data and the Light theme. The four theme images are Linux native
release-verification captures, also at 1280 × 820. They show the same task so the
palette differences are directly comparable. They are app screenshots, not HTML
recreations. Layout and native fonts can vary by platform.

Regenerate sample captures from a current native build:

```bash
QT_QPA_PLATFORM=offscreen \
TODOBENCH_SAMPLE_SCREENSHOTS="$PWD/build/site-screenshots" \
build/dev/test_main_window sampleWorkflowRenders
```

Copy only the chosen screenshots into the site assets. Inspect new screenshots
before publishing; do not include personal workspaces. The social preview uses
the real Team project screenshot. The site logo is a copy of the app's existing
`packaging/icons/todobench.svg` master; keep it in sync if that master changes.
