#!/usr/bin/env python3
"""Consolidate a verified draft into eight downloads, update metadata, and two supporting assets."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import zipfile

root, version = Path(sys.argv[1]), sys.argv[2]
tag = 'v' + version
prefix = 'TodoBench-' + version
packages = {
    prefix + '-x86_64.AppImage.zsync': 'Linux x86_64 — AppImage update metadata',
    prefix + '-x86_64.AppImage': 'Linux x86_64 — AppImage',
    prefix + '-linux-x86_64.tar.gz': 'Linux x86_64 — Portable archive',
    prefix + '-windows-x64-setup.exe': 'Windows x64 — Installer',
    prefix + '-windows-x64.zip': 'Windows x64 — Portable ZIP',
    prefix + '-macos-arm64.dmg': 'macOS Apple Silicon — DMG',
    prefix + '-macos-arm64.zip': 'macOS Apple Silicon — Application ZIP',
    prefix + '-macos-x86_64.dmg': 'macOS Intel — DMG',
    prefix + '-macos-x86_64.zip': 'macOS Intel — Application ZIP',
}
release = json.loads(subprocess.check_output(['gh', 'release', 'view', tag, '--json', 'isDraft,assets'], text=True))
assert release['isDraft'], 'Only drafts can be consolidated'
support = root / ('zz-' + prefix + '-source-and-build-info.zip')
with zipfile.ZipFile(support, 'w', compression=zipfile.ZIP_STORED) as archive:
    for file in sorted(root.iterdir()):
        if file.is_file() and file.name not in packages and not file.name.startswith('zz-') and file.name != 'SHA256SUMS':
            archive.write(file, file.name)
checksums = root / ('zz-' + prefix + '-SHA256SUMS.txt')
checksums.write_text(''.join(hashlib.sha256((root / name).read_bytes()).hexdigest() + '  ' + name + '\n' for name in [*packages, support.name]))
subprocess.run(['gh', 'release', 'upload', tag, str(support) + '#Corresponding source and build information', str(checksums) + '#SHA-256 checksums', '--clobber'], check=True)
for asset in release['assets']:
    if asset['name'] not in packages:
        subprocess.run(['gh', 'release', 'delete-asset', tag, asset['name'], '--yes'], check=True)
    else:
        subprocess.run(['gh', 'api', '--method', 'PATCH', 'repos/{owner}/{repo}/releases/assets/' + str(asset['apiUrl'].rsplit('/', 1)[1]), '-f', 'label=' + packages[asset['name']]], check=True)
print('Draft consolidated into eleven labeled assets')
