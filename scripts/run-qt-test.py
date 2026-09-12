#!/usr/bin/env python3
"""Keep Qt test diagnostics visible even with Windows GUI stream handling."""
from pathlib import Path
import subprocess
import sys
import tempfile

timeout_seconds = 120

with tempfile.TemporaryDirectory() as temporary:
    log = Path(temporary) / 'test.txt'
    try:
        result = subprocess.run([sys.argv[1], '-o', str(log) + ',txt', '-v1', *sys.argv[2:]],
                                timeout=timeout_seconds)
        status = result.returncode
    except subprocess.TimeoutExpired:
        print(f'Qt test exceeded {timeout_seconds} seconds', flush=True)
        status = 1
    finally:
        if log.exists():
            print(log.read_text(encoding='utf-8', errors='replace'), flush=True)
    sys.exit(status)
