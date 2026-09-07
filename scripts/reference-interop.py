#!/usr/bin/env python3
"""Independent workspace v1 consumer: Python/PyYAML + the 7-Zip CLI."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unicodedata
import yaml


def document(path):
    raw = path.read_bytes().removeprefix(b'\xef\xbb\xbf')
    lines = raw.splitlines(keepends=True)
    assert lines[0].rstrip(b'\r\n') == b'---'
    end = next(i for i in range(1, len(lines)) if lines[i].rstrip(b'\r\n') == b'---')
    return yaml.safe_load(b''.join(lines[1:end])), b''.join(lines[end + 1:])


def independent_edit(root):
    for path in root.rglob('*.md'):
        metadata, body = document(path)
        metadata['x-reference'] = {'nested': [True, 17, {'owner': 'independent'}]}
        if metadata['kind'] == 'task':
            metadata['title'] = 'Independent edit'
        path.write_bytes(b'---\n' + yaml.safe_dump(metadata, allow_unicode=True).encode() + b'---\n' + body)
    path = root / 'settings.json'
    settings = json.loads(path.read_text())
    settings['x-reference'] = {'nested': [1, True]}
    path.write_text(json.dumps(settings, indent=2) + '\n')


def verify(before, after):
    # macOS tools/filesystems can use canonically equivalent decomposed names.
    names = lambda root: {unicodedata.normalize('NFC', p.relative_to(root).as_posix()): p
                          for p in root.rglob('*') if p.is_file()}
    originals, imported = names(before), names(after)
    assert originals.keys() == imported.keys(), (originals.keys() - imported.keys(), imported.keys() - originals.keys())
    for path in before.rglob('*'):
        if not path.is_file():
            continue
        other = imported[unicodedata.normalize('NFC', path.relative_to(before).as_posix())]
        if path.suffix == '.md':
            old, old_body = document(path)
            new, new_body = document(other)
            assert old_body == new_body
            assert old['id'] == new['id']
            assert old['x-mobile'] == new['x-mobile']
            assert new['x-reference']['nested'][2]['owner'] == 'independent'
            if old['kind'] == 'task':
                assert new['title'] == 'Independent edit' and new['priority'] == 'high'
                assert old['recurrence'] == new['recurrence']
                assert old['reminders'] == new['reminders']
        elif path.name == 'settings.json':
            old, new = json.loads(path.read_text()), json.loads(other.read_text())
            assert old['x-mobile'] == new['x-mobile']
            assert new['x-reference']['nested'] == [1, True]
            assert old['saved_views'][0]['x-mobile'] == new['saved_views'][0]['x-mobile']
            assert new['saved_views'][0]['sort'] == 'due'
        else:
            assert path.read_bytes() == other.read_bytes()


def main():
    driver = str(Path(sys.argv[1]).resolve())
    seven = shutil.which('7z') or shutil.which('7zz')
    assert seven, '7-Zip is required; interoperability tests must not be skipped'
    fixture = Path(__file__).resolve().parents[1] / 'docs/fixtures/workspace-v1'
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        run = lambda *args: subprocess.run(args, check=True, stderr=subprocess.STDOUT)
        run(driver, 'export', str(fixture), str(root / 'snapshot.7z'))
        run(seven, 'x', str(root / 'snapshot.7z'), '-o' + str(root / 'external'))
        independent_edit(root / 'external')
        subprocess.run([seven, 'a', '-t7z', str(root / 'edited.7z'), '.'], cwd=root / 'external', check=True, stdout=subprocess.PIPE)
        run(driver, 'import', str(root / 'edited.7z'), str(root / 'imported'))
        run(driver, 'edit', str(root / 'imported'))
        verify(fixture, root / 'imported')
    print('Independent extraction/edit/repack/import/save/read passed.')


if __name__ == '__main__':
    main()
