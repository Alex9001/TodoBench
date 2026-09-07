#!/usr/bin/env python3
"""Read the release version from CMake, the single authoritative source."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
match = re.search(r'project\(TodoBench VERSION ([0-9]+\.[0-9]+\.[0-9]+)', (root / 'CMakeLists.txt').read_text())
if not match:
    raise SystemExit('CMake project version not found')
print(match[1])
