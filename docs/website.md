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

The site source is prepared for GitHub Pages. Automatic Actions triggers are
currently disabled to avoid unrequested compute. Screenshot and documentation
updates do not deploy the site. Run any publication workflow only after explicit
approval. Local builds and validation do not require GitHub Actions.

Update the README's versioned download table when publishing a new application
release. The site itself resolves released assets at build time. Keep all local
URLs relative so the `/TodoBench/` project prefix works correctly.

## Screenshots and branding

The site and README share original screenshots in `site/assets/screenshots/`.
`team.png`, `home.png`, and `everyday.png` were refreshed from the native
application at commit `63328cabf1e98e3ed9cfbdb37ec67f14d781052a` on 2026-10-06.
They show the built-in fictional sample workspaces with the current List view.
The four appearance images show that same Team handbook sample in Light, Dark,
Brown, and Paper. All are native Qt widget captures at 1280 × 820 logical pixels
with 2× display scaling, saved at 2560 × 1640 pixels without upscaling.
They contain no personal workspace data. The first README image is the same
current Team screenshot. Native fonts can vary by platform.

Regenerate sample captures from a current native build:

```bash
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=2 \
TODOBENCH_SAMPLE_SCREENSHOTS="$PWD/build/site-screenshots" \
build/dev/test_main_window sampleWorkflowRenders
```

Copy only the chosen screenshots into the site assets. Inspect new screenshots
before publishing; do not include personal workspaces. The social preview uses
the real Team project screenshot. The site logo is a copy of the app's existing
`packaging/icons/todobench.svg` master; keep it in sync if that master changes.
