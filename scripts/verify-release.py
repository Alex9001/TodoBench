#!/usr/bin/env python3
"""Verify downloaded release assets before changing the draft's visibility."""
import hashlib
import json
from pathlib import Path
import sys
import tarfile


def main():
    root, version, commit = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
    prefix = 'TodoBench-' + version
    required = [prefix + suffix for suffix in ('-linux-x86_64.AppImage', '-linux-x86_64.tar.gz',
                '-windows-x64.zip', '-windows-x64-setup.exe', '-macos-x86_64.zip', '-macos-x86_64.dmg',
                '-macos-arm64.zip', '-macos-arm64.dmg', '-source.tar.gz')]
    for name in required:
        assert (root / name).is_file() and (root / name).stat().st_size > 0, name
    assert (root / 'SOURCE_COMMIT').read_text().strip() == commit
    for target in ('linux-x86_64', 'windows-x64', 'macos-x86_64', 'macos-arm64'):
        manifest = root / ('checksums-' + target + '.txt')
        entries = manifest.read_text().splitlines()
        assert len(entries) == 3, manifest
        for line in entries:
            expected, name = line.split('  ', 1)
            assert Path(name).name == name
            assert hashlib.sha256((root / name).read_bytes()).hexdigest() == expected, name
        source = root / (prefix + '-' + target + '-dependency-source.tar.gz')
        with tarfile.open(source) as archive:
            provenance = next(m for m in archive.getmembers() if m.name.endswith('/provenance.json'))
            assert json.load(archive.extractfile(provenance))['commit'] == commit
    with tarfile.open(root / (prefix + '-source.tar.gz')) as archive:
        names = set(archive.getnames())
        assert prefix + '/CMakeLists.txt' in names and prefix + '/LICENSE' in names
    assets = [p for p in sorted(root.iterdir()) if p.is_file() and p.name != 'SHA256SUMS']
    (root / 'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.name + '\n' for p in assets))
    print('All eight native packages, exact source, dependency sources, and checksums verified.')


if __name__ == '__main__':
    main()
