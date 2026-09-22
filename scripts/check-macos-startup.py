#!/usr/bin/env python3
"""Launch an installed package with Cocoa/LaunchServices and require observable readiness."""
from pathlib import Path
import argparse
import json
import os
import shutil
import signal
import subprocess
import time
import tempfile


def stop(pid):
    if not pid:
        return
    try:
        os.kill(pid, signal.SIGTERM)
        for _ in range(30):
            time.sleep(0.1)
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                return
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def capture_failure(pid, directory, started):
    if pid:
        subprocess.run(['/usr/bin/sample', str(pid), '2', '-file', str(directory / 'sample.txt')],
                       capture_output=True, timeout=10, check=False)
    crashes = directory / 'crashes'
    crashes.mkdir(exist_ok=True)
    for root in [Path.home() / 'Library/Logs/DiagnosticReports', Path('/Library/Logs/DiagnosticReports')]:
        for report in root.glob('TodoBench*'):
            if report.is_file() and report.stat().st_mtime >= started:
                shutil.copy2(report, crashes / report.name)


def launch(helper, bundle, state, directory, arguments, expected):
    directory.mkdir(parents=True, exist_ok=True)
    report = directory / 'ready.json'
    pid_file = directory / 'pid'
    started = time.time()
    pid = None
    try:
        with (directory / 'launcher-stderr.txt').open('w') as stderr:
            subprocess.run([str(helper), str(bundle), str(pid_file), '--state-dir', str(state),
                            '--startup-check', str(report), *arguments], stderr=stderr, stdout=stderr,
                           timeout=20, check=True)
        pid = int(pid_file.read_text())
        deadline = started + 30
        while time.time() < deadline:
            if report.exists():
                result = json.loads(report.read_text())
                assert result['ready'] and result['state'] == expected, result
                assert result['platform'] == 'cocoa', result
                return pid, result
            time.sleep(0.1)
        raise TimeoutError('No visible, responsive native application after 30 seconds')
    except BaseException:
        if pid is None and pid_file.exists():
            pid = int(pid_file.read_text())
        capture_failure(pid, directory, started)
        stop(pid)
        raise


def verify(bundle, artifacts):
    artifacts.mkdir(parents=True, exist_ok=True)
    artifacts = Path(tempfile.mkdtemp(prefix="run-", dir=artifacts))
    helper = artifacts / 'macos-launch'
    subprocess.run(['clang', '-fobjc-arc', '-framework', 'Cocoa',
                    str(Path(__file__).with_name('macos-launch.m')), '-o', str(helper)], check=True)
    fixture = Path(__file__).resolve().parents[1] / 'docs/fixtures/workspace-v1'
    workspace = artifacts / 'workspace'
    shutil.copytree(fixture, workspace, dirs_exist_ok=True)
    state = artifacts / 'state'
    primary = None
    try:
        primary, _ = launch(helper, bundle, state, artifacts / 'first-launch', [], 'onboarding')
        secondary, result = launch(helper, bundle, state, artifacts / 'activation', [], 'activated')
        assert result['pid'] == primary, 'activation acknowledged by the wrong process'
        stop(secondary)
        stop(primary)
        primary, _ = launch(helper, bundle, state, artifacts / 'explicit-workspace', [str(workspace)], 'workspace')
        other = artifacts / 'second-workspace'
        shutil.copytree(fixture, other)
        secondary, result = launch(helper, bundle, state, artifacts / 'open-another-workspace', [str(other)], 'activated')
        assert result['pid'] == primary
        stop(secondary)
        deadline = time.time() + 30
        while time.time() < deadline and not (other / '.todobench/workspace.lock').exists():
            time.sleep(0.1)
        assert (other / '.todobench/workspace.lock').exists(), 'activation discarded the requested workspace'
        stop(primary)
        primary, _ = launch(helper, bundle, state, artifacts / 'restore-test-workspace', [str(workspace)], 'workspace')
        stop(primary)
        # A successful explicit launch persists the workspace in the isolated QSettings.
        workspace.rename(artifacts / 'unavailable-workspace')
        primary, _ = launch(helper, bundle, state, artifacts / 'missing-workspace', [], 'onboarding')
        stop(primary)
        (artifacts / 'unavailable-workspace').rename(workspace)
        preferences = {p: p.read_bytes() for p in (state / 'settings').rglob('*') if p.is_file()}
        primary, _ = launch(helper, bundle, state, artifacts / 'safe-start', ['--safe-start'], 'onboarding')
        stop(primary)
        assert all(p.read_bytes() == value for p, value in preferences.items()), 'safe start changed saved preferences'
    finally:
        stop(primary)
    print(f'Native Cocoa startup passed for {bundle}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('artifacts', type=Path)
    args = parser.parse_args()
    verify(args.bundle.resolve(), args.artifacts.resolve())


if __name__ == '__main__':
    main()
