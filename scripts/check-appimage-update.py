#!/usr/bin/env python3
"""Require stable-channel update information and a matching zsync control file."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile


def verify(appimage):
    architecture = appimage.stem.rsplit('-', 1)[1]
    expected = f'gh-releases-zsync|Alex9001|TodoBench|latest|TodoBench-*-{architecture}.AppImage.zsync'
    actual = subprocess.check_output([str(appimage.resolve()), '--appimage-updateinformation'], text=True).strip()
    assert actual == expected, (actual, expected)
    control = appimage.with_name(appimage.name + '.zsync').read_bytes()
    header, blocks = control.split(b'\n\n', 1)
    fields = dict(line.split(': ', 1) for line in header.decode().splitlines())
    assert fields['Filename'] == appimage.name, fields
    assert fields['URL'] == appimage.name, fields
    assert int(fields['Length']) == appimage.stat().st_size, fields
    assert fields['SHA-1'] == hashlib.sha1(appimage.read_bytes()).hexdigest(), fields
    assert blocks, 'Missing zsync block checksums'
    with tempfile.TemporaryDirectory() as temporary:
        rebuilt = Path(temporary) / appimage.name
        subprocess.run(['zsync', '-i', str(appimage.resolve()), '-o', str(rebuilt),
                        str(appimage.with_name(appimage.name + '.zsync').resolve())], check=True)
        assert rebuilt.read_bytes() == appimage.read_bytes(), 'zsync reconstruction differs'
    print('Verified update channel and zsync payload:', appimage.name)


if __name__ == '__main__':
    verify(Path(sys.argv[1]))
