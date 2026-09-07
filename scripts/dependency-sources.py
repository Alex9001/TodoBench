#!/usr/bin/env python3
"""Retain the dependency inputs and build recipes used by each native runner."""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
from urllib.request import urlopen


def output(*args):
    return subprocess.check_output(args, text=True).strip()


def download(url, destination, expected=None):
    with urlopen(url, timeout=120) as source, destination.open('wb') as target:
        shutil.copyfileobj(source, target)
    digest = hashlib.sha256(destination.read_bytes()).hexdigest()
    if expected and digest != expected:
        raise RuntimeError('Dependency checksum mismatch: ' + url)
    return {'url': url, 'file': destination.name, 'sha256': digest}


def qt_sources(stage):
    records = []
    for module in ('qtbase', 'qtsvg'):
        url = f'https://download.qt.io/archive/qt/6.8/6.8.3/submodules/{module}-everywhere-src-6.8.3.tar.xz'
        checksum = urlopen(url + '.sha256', timeout=120).read().decode().split()[0]
        records.append(download(url, stage / url.rsplit('/', 1)[1], checksum))
    return records


def linux_sources(stage):
    # Ubuntu's deb822 sources must offer the exact installed source versions.
    subprocess.run(['sudo', 'sed', '-i', 's/^Types: deb$/Types: deb deb-src/', '/etc/apt/sources.list.d/ubuntu.sources'], check=True)
    subprocess.run(['sudo', 'apt-get', 'update'], check=True, stdout=subprocess.DEVNULL)
    sources = set()
    for file in Path('build/release/AppDir').rglob('*.so*'):
        if not file.is_file() or file.name.startswith(('libQt6', 'libq')):
            continue
        matches = output('dpkg-query', '-S', '*/' + file.name).splitlines()
        package = matches[0].split(': ')[0]
        sources.add(output('dpkg-query', '-W', '-f=${source:Package}=${source:Version}', package))
    (stage / 'ubuntu-sources.txt').write_text('\n'.join(sorted(sources)) + '\n')
    for source in sorted(sources):
        subprocess.run(['apt-get', 'source', '--download-only', source], cwd=stage, check=True)


def windows_sources(stage):
    vcpkg = Path(os.environ['VCPKG_ROOT'])
    for source in (vcpkg / 'downloads').iterdir():
        if source.is_file() and source.name.endswith(('.gz', '.xz', '.bz2', '.zip', '.tgz')):
            shutil.copy2(source, stage / source.name)
    subprocess.run(['git', '-C', str(vcpkg), 'archive', '--format=tar.gz', '-o', str(stage / 'vcpkg-build-recipes.tar.gz'), 'HEAD'], check=True)
    shutil.copytree('build/release/vcpkg_installed/vcpkg', stage / 'vcpkg-status', dirs_exist_ok=True)
    (stage / 'vcpkg-commit.txt').write_text(output('git', '-C', str(vcpkg), 'rev-parse', 'HEAD') + '\n')
    for copyright_file in Path('build/release/vcpkg_installed').rglob('copyright'):
        shutil.copy2(copyright_file, stage / (copyright_file.parent.name + '-copyright'))


def macos_sources(stage):
    dependencies = set(output('brew', 'deps', '--installed', '--recursive', 'yaml-cpp', 'libarchive').splitlines())
    dependencies.update(('yaml-cpp', 'libarchive'))
    records = []
    for formula in sorted(dependencies):
        info = json.loads(output('brew', 'info', '--json=v2', formula))['formulae'][0]
        url = info['urls']['stable']
        destination = stage / (formula.replace('/', '-') + '-' + url['url'].rsplit('/', 1)[1])
        records.append(download(url['url'], destination, url['checksum']))
        (stage / (formula.replace('/', '-') + '.rb')).write_text(output('brew', 'cat', formula) + '\n')
        (stage / (formula.replace('/', '-') + '.json')).write_text(json.dumps(info, indent=2) + '\n')
    return records


def main():
    destination = Path(sys.argv[1]).resolve()
    name = 'TodoBench-' + output(sys.executable, 'scripts/version.py') + '-' + os.environ['RELEASE_PLATFORM'] + '-dependency-source'
    stage = Path('build') / name
    stage.mkdir(parents=True, exist_ok=True)
    stage = stage.resolve()
    records = qt_sources(stage)
    system = platform.system()
    if system == 'Linux':
        linux_sources(stage)
    elif system == 'Windows':
        windows_sources(stage)
    else:
        records.extend(macos_sources(stage))
    provenance = {'commit': output('git', 'rev-parse', 'HEAD'), 'platform': platform.platform(),
                  'cmake': output('cmake', '--version'), 'qt': '6.8.3', 'downloads': records}
    (stage / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    shutil.copy('docs/packaging.md', stage / 'BUILD.md')
    shutil.copytree('packaging/licenses', stage / 'licenses', dirs_exist_ok=True)
    shutil.copy('packaging/THIRD_PARTY_NOTICES.md', stage)
    with tarfile.open(destination / (name + '.tar.gz'), 'w:gz') as archive:
        archive.add(stage, arcname=name)


if __name__ == '__main__':
    main()
