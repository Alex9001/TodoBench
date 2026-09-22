# TodoBench macOS beta tester instructions

Use the version, exact source commit, and workflow results in `build-info.txt`
when reporting feedback. Automated checks do not replace testing on your Mac;
the original reported startup failure has not yet been diagnosed.

## Choose your installer

Open Apple menu → About This Mac. If it shows an Apple M-series **Chip**, use
`TodoBench-<version>-macos-arm64.dmg`. If it shows an Intel **Processor**, use
`TodoBench-<version>-macos-x86_64.dmg`. Both are included; install only one.

## Verify and install

1. Extract the tester ZIP. In Terminal, type `cd `, drag the extracted folder into
   Terminal, and press Return. Run `shasum -a 256 -c SHA256SUMS`; all entries must
   say `OK`. Download only from the TodoBench GitHub release.
2. Quit any existing TodoBench instance. Open your matching DMG and drag
   `TodoBench.app` to Applications. Eject the disk image and open the installed app
   from Applications (do not run it from inside the disk image).
3. This beta is ad-hoc signed, **without an Apple publisher certificate or
   notarization**. If macOS blocks it, attempt to open it once, then use
   System Settings → Privacy & Security → Open Anyway if you choose to proceed.
   Record the exact warning. Do not disable Gatekeeper globally.
4. Use a new sample workspace or a copy of your real workspace. Keep a backup.
   Folder names can change when titles change; undo history lasts for this session.

## Functional checklist

Record Pass / Fail / Not tested for each item, with steps and screenshots for failures.

- Launch from Applications: a responsive window appears; note time to first window.
- Create or open a test workspace, quit, and reopen: the workspace returns.
- With the app quit, move the test workspace aside. Reopen: onboarding appears
  instead of hanging. Restore the folder afterward.
- Quit, run the safe-start command below: onboarding opens. Quit and reopen
  normally to check that saved preferences remain intact.
- While running, open the app again: the existing window activates. Use the
  explicit-workspace command below with a second test workspace and check it opens.
- Create tasks/subtasks; edit title and Markdown notes; change status and priority;
  try bulk changes, List/Table, filters (including notes), and sorting.
- Add an attachment; rename/move its task or project; confirm local links work.
- Try Undo and Redo; trash and restore a task. Confirm content and attachments survive.
- Try recurring completion and reminder dismiss/snooze. Check after reopening.
- Edit a task externally and confirm refresh/conflict handling preserves your edits.
- Quit and reopen; confirm tasks, notes, attachments, and view settings persist.

## Diagnostic commands

Quit any running instance before a terminal or safe-start launch. Replace the
workspace example with the absolute path to your test workspace.

```sh
sw_vers
uname -m
"/Applications/TodoBench.app/Contents/MacOS/TodoBench" --version
"/Applications/TodoBench.app/Contents/MacOS/TodoBench" --diagnostics
"/Applications/TodoBench.app/Contents/MacOS/TodoBench" 2>&1 | tee "$HOME/Desktop/TodoBench-launch.txt"
```

Run these separately as needed (the launch command above stays running until quit):

```sh
"/Applications/TodoBench.app/Contents/MacOS/TodoBench" --safe-start
open -n "/Applications/TodoBench.app" --args "/absolute/path/to/test-workspace"
open "$HOME/Library/Logs/TodoBench"
```

Help → Application Diagnostics also offers Copy Report and Open Logs. Attach
`startup.log` and rotated `.1` / `.2` logs if present. If hung, use Activity Monitor
→ TodoBench → Sample Process. If it exits, save the relevant TodoBench crash report
from Console. Review diagnostics for private paths or content before sharing.

## Feedback form

Submit through https://github.com/Alex9001/TodoBench/issues/new?template=bug-report.yml
or return this completed form to the person coordinating the beta.

- Version and source commit (build-info.txt):
- Exact DMG filename and checksum verification result:
- Mac model / Intel or Apple Silicon / macOS version:
- Clean install or upgrade; previous version:
- Launch result: window / bouncing Dock icon / exits / warning (exact text):
- Approximate failure time and time zone:
- Steps to reproduce:
- Expected result:
- Actual result:
- Safe-start result:
- Checklist results, including items not tested:
- Diagnostic report, terminal output, startup logs, sample/crash report:
- Screenshots or a minimal non-private sample workspace:

No tester feedback is implied by the automated verification recorded in this packet.
