#!/usr/bin/env python3
"""Headless regression checks for startup states and diagnostics (not a native Mac launch check)."""
from pathlib import Path
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def ready(binary, state, output, *arguments):
    process = subprocess.Popen([str(binary), '--state-dir', str(state), '--startup-check', str(output), *arguments],
                               env={**os.environ, 'QT_QPA_PLATFORM': 'offscreen'})
    try:
        for _ in range(100):
            if output.exists():
                data = json.loads(output.read_text())
                assert data['ready'], data
                return process, data
            if process.poll() is not None:
                raise AssertionError(f'Startup exited {process.returncode}; see {state}')
            time.sleep(0.1)
        raise AssertionError('Readiness report missing')
    except BaseException:
        process.kill()
        process.wait()
        raise


def stop(process):
    if process.poll() is None:
        process.terminate()
    process.wait(timeout=10)


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        state = root / 'state'
        diagnostic = subprocess.run([str(binary), '--state-dir', str(state), '--diagnostics'],
                                    env={**os.environ, 'QT_QPA_PLATFORM': 'unavailable'},
                                    capture_output=True, text=True, check=True)
        assert 'GUI not initialized' in diagnostic.stdout
        assert 'Process architecture:' in diagnostic.stdout
        primary, report = ready(binary, state, root / 'first.json')
        try:
            assert report['state'] == 'onboarding'
            secondary, report = ready(binary, state, root / 'activate.json')
            assert secondary.wait(timeout=10) == 0
            assert report['state'] == 'activated' and report['pid'] == primary.pid
            if hasattr(signal, 'SIGSTOP'):
                os.kill(primary.pid, signal.SIGSTOP)
                try:
                    failure = subprocess.run([str(binary), '--state-dir', str(state), '--startup-check', str(root / 'failed.json')],
                                             env={**os.environ, 'QT_QPA_PLATFORM': 'offscreen'}, timeout=10)
                    assert failure.returncode != 0
                    assert json.loads((root / 'failed.json').read_text())['state'] == 'ipc-failed'
                finally:
                    os.kill(primary.pid, signal.SIGCONT)
        finally:
            stop(primary)
        fixture = Path(__file__).resolve().parents[1] / 'docs/fixtures/workspace-v1'
        workspace = root / 'workspace'
        shutil.copytree(fixture, workspace)
        process, report = ready(binary, state, root / 'workspace.json', str(workspace))
        stop(process)
        assert report['state'] == 'workspace'
        other = root / 'second-workspace'
        shutil.copytree(fixture, other)
        primary, report = ready(binary, state, root / 'switch-first.json', str(workspace))
        try:
            secondary, report = ready(binary, state, root / 'switch-second.json', str(other))
            assert secondary.wait(timeout=10) == 0
            for _ in range(50):
                settings = '\n'.join(p.read_text() for p in (state / 'settings').rglob('*.ini'))
                if 'second-workspace' in settings:
                    break
                time.sleep(0.1)
            assert 'second-workspace' in settings, 'activation lost the requested workspace'
        finally:
            stop(primary)
        process, report = ready(binary, state, root / 'workspace-again.json', str(workspace))
        stop(process)
        workspace.rename(root / 'away')
        process, report = ready(binary, state, root / 'missing.json')
        stop(process)
        assert report['state'] == 'onboarding'
        (root / 'away').rename(workspace)
        preferences = {p: p.read_bytes() for p in (state / 'settings').rglob('*') if p.is_file()}
        process, report = ready(binary, state, root / 'safe.json', '--safe-start')
        stop(process)
        assert report['state'] == 'onboarding'
        assert all(p.read_bytes() == data for p, data in preferences.items())
        for _ in range(4):
            (state / 'logs/startup.log').write_bytes(b'x' * (1024 * 1024))
            subprocess.run([str(binary), '--state-dir', str(state), '--diagnostics'],
                           capture_output=True, check=True)
        logs = list((state / 'logs').glob('startup.log*'))
        assert 1 <= len(logs) <= 3
        assert all(p.stat().st_size <= 1024 * 1024 for p in logs)
        assert all('Reference task' not in p.read_text() for p in logs)
    print('Startup diagnostics, readiness, isolated state, safe start, and IPC checks passed')


if __name__ == '__main__':
    main()
