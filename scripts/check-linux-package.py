#!/usr/bin/env python3
"""Check every packaged ELF against Ubuntu 22.04 and exercise visible X11 startup."""
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def output(*args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs)


def inspect(root):
    count = 0
    env = {**os.environ, 'LD_LIBRARY_PATH': str(root / 'usr/lib')}
    for file in root.rglob('*'):
        if not file.is_file():
            continue
        with file.open('rb') as stream:
            if stream.read(4) != b'\x7fELF':
                continue
        count += 1
        symbols = output('objdump', '-T', str(file))
        for line in symbols.splitlines():
            if '*UND*' not in line:
                continue
            for family, value in re.findall(r'\b(GLIBCXX|GLIBC|CXXABI)_([0-9.]+)', line):
                limit = {'GLIBC': (2, 35), 'GLIBCXX': (3, 4, 30), 'CXXABI': (1, 3, 13)}[family]
                assert tuple(map(int, value.split('.'))) <= limit, (file, line)
        dynamic = output('readelf', '-d', str(file))
        for line in dynamic.splitlines():
            if '(RPATH)' in line or '(RUNPATH)' in line:
                for path in re.search(r'\[(.*)\]', line)[1].split(':'):
                    assert not path or path.startswith('$ORIGIN'), (file, line)
        if '(NEEDED)' in dynamic:
            libraries = output('ldd', str(file), env=env)
            assert 'not found' not in libraries, (file, libraries)
            for path in re.findall(r'=> (/\S+)', libraries):
                resolved = Path(path).resolve()
                assert resolved.is_relative_to(root.resolve()) or str(resolved).startswith(('/usr/lib/', '/lib/')), (file, libraries)
    assert count > 10, count
    print(f'Checked {count} ELF files against Ubuntu 22.04 symbol limits')


def launch(binary, state, report, expected, *args):
    env = {**os.environ, 'QT_QPA_PLATFORM': 'xcb', 'APPIMAGE_EXTRACT_AND_RUN': '1',
           'HOME': str(state), 'XDG_CONFIG_HOME': str(state / 'config'),
           'XDG_DATA_HOME': str(state / 'data'), 'XDG_CACHE_HOME': str(state / 'cache')}
    state.mkdir(exist_ok=True)
    version = output(str(binary), '--version', env=env)
    assert output(sys.executable, 'scripts/version.py').strip() in version, version
    with report.with_suffix('.log').open('w') as log:
        process = subprocess.Popen([str(binary), '--state-dir', str(state / 'app'), '--startup-check', str(report), *args], env=env, stdout=log, stderr=log, start_new_session=True)
        try:
            for _ in range(300):
                assert process.poll() is None, report.with_suffix('.log').read_text()
                if report.exists():
                    result = json.loads(report.read_text())
                    assert result['ready'] and result['platform'] == 'xcb' and result['state'] == expected, result
                    subprocess.run(['import', '-window', 'root', str(report.with_suffix('.png'))], check=True)
                    return
                time.sleep(.1)
            raise AssertionError('No visible responsive window: ' + str(report))
        finally:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            process.wait(timeout=20)


def main():
    packages = Path(sys.argv[1]).resolve()
    evidence = Path('build/linux-evidence').resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        for package in sorted(packages.iterdir()):
            if not (package.suffix == '.AppImage' or package.name.endswith('-linux-x86_64.tar.gz')):
                continue
            stage = root / package.name
            stage.mkdir()
            if package.suffix == '.AppImage':
                package.chmod(0o755)
                subprocess.run([str(package), '--appimage-extract'], cwd=stage, check=True, stdout=subprocess.DEVNULL)
                appdir = stage / 'squashfs-root'
                binary = package
            else:
                subprocess.run(['tar', '-xzf', str(package), '-C', str(stage)], check=True)
                appdir = next(stage.iterdir())
                binary = appdir / 'TodoBench'
            inspect(appdir)
            workspace = stage / 'workspace'
            shutil.copytree('docs/fixtures/workspace-v1', workspace)
            for name, expected, args in [('first', 'onboarding', []), ('workspace', 'workspace', [str(workspace)]), ('safe', 'onboarding', ['--safe-start'])]:
                launch(binary, stage / 'state', evidence / (package.name + '-' + name + '.json'), expected, *args)
    print('Linux package symbol, visible startup, workspace, and safe-start checks passed')


if __name__ == '__main__':
    main()
