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
from urllib.parse import urlencode


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


def qt_icu_source(stage):
    # Qt's official 6.8.3 Linux SDK includes ICU 73.2, separate from Ubuntu ICU.
    # The pinned Qt provisioning recipe records the binary's provenance.
    recipe = 'https://raw.githubusercontent.com/qt/qt5/v6.8.3/coin/provisioning/qtci-linux-RHEL-8.10-x86_64/30-install_icu.sh'
    records = [download(recipe, stage / 'qt-sdk-icu-provisioning.sh')]
    source = 'https://github.com/unicode-org/icu/releases/download/release-73-2/icu4c-73_2-src.tgz'
    records.append(download(source, stage / 'icu4c-73_2-src.tgz'))
    checksums = urlopen(source.rsplit('/', 1)[0] + '/SHASUM512.txt', timeout=120).read().decode()
    expected = next(line.split()[0] for line in checksums.splitlines() if line.endswith('icu4c-73_2-src.tgz'))
    assert hashlib.sha512((stage / 'icu4c-73_2-src.tgz').read_bytes()).hexdigest() == expected
    return records


def archived_ubuntu_source(stage, source):
    name, version = source.split('=', 1)
    query = urlencode({'ws.op': 'getPublishedSources', 'source_name': name,
                       'version': version, 'exact_match': 'true'})
    publications = json.load(urlopen('https://api.launchpad.net/1.0/ubuntu/+archive/primary?' + query, timeout=120))
    assert publications['entries'], 'Exact Ubuntu source unavailable: ' + source
    publication = publications['entries'][0]['self_link']
    urls = json.load(urlopen(publication + '?ws.op=sourceFileUrls', timeout=120))
    records = [download(url, stage / url.rsplit('/', 1)[1]) for url in urls]
    descriptor = next(stage / record['file'] for record in records if record['file'].endswith('.dsc'))
    checksums = descriptor.read_text().split('Checksums-Sha256:\n', 1)[1]
    for line in checksums.splitlines():
        if not line.startswith(' '):
            break
        expected, size, filename = line.split()
        content = (stage / filename).read_bytes()
        assert len(content) == int(size) and hashlib.sha256(content).hexdigest() == expected, filename
    (stage / (name + '-launchpad.json')).write_text(json.dumps({'publication': publication, 'downloads': records}, indent=2) + '\n')


def linux_sources(stage):
    # Ubuntu's deb822 sources must offer the exact installed source versions.
    subprocess.run(['sudo', 'sed', '-i', 's/^Types: deb$/Types: deb deb-src/', '/etc/apt/sources.list.d/ubuntu.sources'], check=True)
    subprocess.run(['sudo', 'apt-get', 'update'], check=True, stdout=subprocess.DEVNULL)
    sources = set()
    qt_plugins = {p.name for p in (Path(os.environ['QT_ROOT_DIR']) / 'plugins').rglob('*.so')}
    for file in Path('build/release/AppDir').rglob('*.so*'):
        if not file.is_file() or (file.name.startswith('libQt6') or file.name in qt_plugins) or (file.name.startswith('libicu') and '.so.73' in file.name):
            continue
        matches = output('dpkg-query', '-S', '*/' + file.name).splitlines()
        package = matches[0].split(': ')[0]
        sources.add(output('dpkg-query', '-W', '-f=${source:Package}=${source:Version}', package))
    (stage / 'ubuntu-sources.txt').write_text('\n'.join(sorted(sources)) + '\n')
    for source in sorted(sources):
        result = subprocess.run(['apt-get', 'source', '--download-only', source], cwd=stage)
        if result.returncode:
            archived_ubuntu_source(stage, source)


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
    dependencies = set(output('brew', 'deps', '--installed', '--union', 'yaml-cpp', 'libarchive').splitlines())
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


def rust_sources(stage):
    """Capture the exact Rust inputs used by the mdbase bridge.

    The bridge is statically linked, so its Rust crates are not discoverable
    from the platform package's shared-library scan.  Keep a locked Cargo
    manifest, Cargo's complete vendored registry, and the local mdbase-rs
    source in the dependency archive.  The generated metadata makes the
    license and source identity of every locked crate auditable without
    requiring Cargo to be installed.
    """
    bridge = Path('native/mdbase_bridge').resolve()
    rust_stage = stage / 'rust'
    rust_stage.mkdir(parents=True, exist_ok=True)
    for name in ('Cargo.toml', 'Cargo.lock', 'rust-toolchain.toml'):
        shutil.copy2(bridge / name, rust_stage / name)
    (rust_stage / 'mdbase-rs-revision.txt').write_text(
        (bridge / 'vendor' / 'MDBASE_RS_REVISION').read_text())
    (rust_stage / 'mdbase-spec-revision.txt').write_text(
        (bridge / 'vendor' / 'MDBASE_SPEC_REVISION').read_text())
    shutil.copytree(bridge / 'vendor' / 'mdbase-rs', rust_stage / 'mdbase-rs')

    vendor = rust_stage / 'registry'
    cargo = os.environ.get('CARGO', 'cargo')
    cargo_prefix = [cargo]
    # The release build pins the toolchain in rust-toolchain.toml.  rustup's
    # explicit selector is portable across the CI hosts; installations that
    # use a direct cargo binary simply omit the selector.
    if shutil.which('rustup'):
        cargo_prefix = [cargo, '+1.94.0']
    subprocess.run(cargo_prefix + ['vendor', '--manifest-path', str(bridge / 'Cargo.toml'),
                                   '--locked', '--versioned-dirs', '--quiet', str(vendor)], check=True)
    (rust_stage / 'cargo-config.toml').write_text(
        '[source.crates-io]\n'
        'replace-with = "vendored-sources"\n\n'
        '[source.vendored-sources]\n'
        'directory = "registry"\n')
    metadata = json.loads(output(*(cargo_prefix + ['metadata', '--manifest-path',
                                                    str(bridge / 'Cargo.toml'),
                                                    '--locked', '--format-version', '1'])))
    packages = []
    for package in metadata['packages']:
        packages.append({key: package.get(key) for key in
                         ('name', 'version', 'id', 'license', 'license_file',
                          'source', 'repository', 'homepage')})
    (rust_stage / 'dependencies.json').write_text(json.dumps({
        'toolchain': '1.94.0',
        'manifest': 'Cargo.toml',
        'lockfile': 'Cargo.lock',
        'packages': packages,
    }, indent=2) + '\n')


def main():
    destination = Path(sys.argv[1]).resolve()
    name = 'TodoBench-' + output(sys.executable, 'scripts/version.py') + '-' + os.environ['RELEASE_PLATFORM'] + '-dependency-source'
    stage = Path('build') / name
    stage.mkdir(parents=True, exist_ok=True)
    stage = stage.resolve()
    records = qt_sources(stage)
    system = platform.system()
    if system == 'Linux':
        records.extend(qt_icu_source(stage))
        linux_sources(stage)
    elif system == 'Windows':
        windows_sources(stage)
    else:
        records.extend(macos_sources(stage))
    rust_sources(stage)
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
