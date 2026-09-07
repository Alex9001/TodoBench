#!/usr/bin/env python3
import json
from pathlib import Path
import subprocess
import sys

version, commit = sys.argv[1:]
tag = 'v' + version
existing = subprocess.run(['gh', 'release', 'view', tag, '--json', 'isDraft'], capture_output=True, text=True)
if existing.returncode == 0:
    if not json.loads(existing.stdout)['isDraft']:
        raise SystemExit('Refusing to alter an already published release')
    subprocess.run(['gh', 'release', 'edit', tag, '--target', commit], check=True)
else:
    subprocess.run(['gh', 'release', 'create', tag, '--draft', '--target', commit,
                    '--title', 'TodoBench ' + version, '--notes-file', 'packaging/RELEASE_NOTES.md'], check=True)
Path('dist').mkdir(exist_ok=True)
source = Path('dist') / ('TodoBench-' + version + '-source.tar.gz')
subprocess.run(['git', 'archive', '--format=tar.gz', '--prefix=TodoBench-' + version + '/', '-o', str(source), commit], check=True)
Path('dist/SOURCE_COMMIT').write_text(commit + '\n')
subprocess.run(['gh', 'release', 'upload', tag, str(source), 'dist/SOURCE_COMMIT', 'LICENSE',
                'packaging/THIRD_PARTY_NOTICES.md', 'docs/packaging.md', '--clobber'], check=True)
