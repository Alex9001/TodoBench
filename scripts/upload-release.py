#!/usr/bin/env python3
import hashlib
import os
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1])
version = subprocess.check_output([sys.executable, 'scripts/version.py'], text=True).strip()
files = [p for p in sorted(root.iterdir()) if p.is_file() and p.name.startswith('TodoBench-') and p.suffix != '.sha256']
manifest = root / ('checksums-' + os.environ['RELEASE_PLATFORM'] + '.txt')
manifest.write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.name + '\n' for p in files))
subprocess.run(['gh', 'release', 'upload', 'v' + version, *map(str, files), str(manifest), '--clobber'], check=True)
