#!/usr/bin/env python3
"""Exercise extracted native packages and installers without touching user state."""
from pathlib import Path
import os
import platform
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, **kwargs).stdout


def smoke(binary, state):
    env = os.environ.copy()
    for variable, name in [('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'), ('XDG_CACHE_HOME', 'cache'), ('TMPDIR', 'tmp')]:
        (state / name).mkdir(exist_ok=True)
        env[variable] = str(state / name)
    env.update(QT_QPA_PLATFORM='offscreen', APPIMAGE_EXTRACT_AND_RUN='1')
    if os.name == 'nt':
        env['QT_QPA_FONTDIR'] = str(Path(os.environ['WINDIR']) / 'Fonts')
    assert 'TodoBench' in run(str(binary), '--version', env=env)
    fixture = Path(__file__).resolve().parents[1] / 'docs/fixtures/workspace-v1'
    workspace = state / 'workspace'
    shutil.copytree(fixture, workspace, dirs_exist_ok=True)
    process = subprocess.Popen([str(binary), str(workspace)], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=(os.name != "nt"))
    try:
        time.sleep(3)
        assert process.poll() is None, process.communicate()
    finally:
        if os.name != "nt":
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        elif process.poll() is None:
            process.terminate()
        process.communicate(timeout=15)


def check_macos(bundle):
    run('codesign', '--verify', '--deep', '--strict', str(bundle))
    assert (bundle / 'Contents/Resources/todobench.icns').is_file()
    for file in (bundle / 'Contents').rglob('*'):
        if not file.is_file() or file.is_symlink():
            continue
        result = subprocess.run(['otool', '-L', str(file)], capture_output=True, text=True)
        assert '/opt/homebrew/' not in result.stdout and '/usr/local/opt/' not in result.stdout, result.stdout


def verify_archive(package, stage):
    extracted = stage / 'extracted'
    extracted.mkdir()
    if package.name.endswith('.tar.gz'):
        with tarfile.open(package) as archive:
            archive.extractall(extracted, filter='data')
        binary = next(extracted.glob('*/TodoBench'))
        assert list(extracted.rglob('libQt6Core.so*')), 'Qt runtime missing from portable archive'
    else:
        run('ditto', '-x', '-k', str(package), str(extracted)) if platform.system() == 'Darwin' else zipfile.ZipFile(package).extractall(extracted)
        if platform.system() == 'Darwin':
            bundle = next(extracted.glob('*.app'))
            check_macos(bundle)
            binary = bundle / 'Contents/MacOS/TodoBench'
        else:
            binary = next(extracted.rglob('TodoBench.exe'))
            assert (binary.parent / 'Qt6Core.dll').is_file()
    smoke(binary, stage)


def verify_installer(package, stage):
    if package.suffix == '.dmg':
        mount = stage / 'mounted'
        run('hdiutil', 'attach', '-nobrowse', '-mountpoint', str(mount), str(package))
        try:
            bundle = mount / 'TodoBench.app'
            check_macos(bundle)
            smoke(bundle / 'Contents/MacOS/TodoBench', stage)
        finally:
            run('hdiutil', 'detach', str(mount))
    else:
        install = stage / 'installed'
        run(str(package), '/S', '/D=' + str(install))
        smoke(install / 'TodoBench.exe', stage)
        run(str(install / 'Uninstall.exe'), '/S', '_?=' + str(install))


def main():
    count = 0
    for package in sorted(Path(sys.argv[1]).resolve().iterdir()):
        if not package.is_file() or package.suffix == '.sha256':
            continue
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            if package.name.endswith(('.tar.gz', '.zip')):
                verify_archive(package, stage)
            elif package.suffix in ('.dmg', '.exe'):
                verify_installer(package, stage)
            elif package.suffix == '.AppImage':
                smoke(package, stage)
            else:
                continue
            count += 1
            print('Verified', package.name)
    assert count == 2, f'Expected two native packages, verified {count}'


if __name__ == '__main__':
    main()
