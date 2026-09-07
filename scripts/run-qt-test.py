#!/usr/bin/env python3
"""Keep Qt test diagnostics visible even with Windows GUI stream handling."""
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory() as temporary:
    log = Path(temporary) / 'test.txt'
    try:
        result = subprocess.run([sys.argv[1], '-o', str(log) + ',txt', '-v1', *sys.argv[2:]], timeout=85)
        status = result.returncode
    except subprocess.TimeoutExpired:
        print('Qt test exceeded 85 seconds', flush=True)
        status = 1
    finally:
        if log.exists():
            print(log.read_text(encoding='utf-8', errors='replace'), flush=True)
    sys.exit(status)
