#!/usr/bin/env python3
"""Build the static GitHub Pages site against a published release inventory."""
import argparse
from html.parser import HTMLParser
import json
from pathlib import Path
import re
import shutil
import subprocess
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
REPO = 'Alex9001/TodoBench'
SITE_URL = 'https://alex9001.github.io/TodoBench/'


class Page(HTMLParser):
    def __init__(self, source):
        super().__init__(convert_charrefs=True)
        self.links = []
        self.ids = set()
        self.images = 0
        self.headings = 0
        self.feed(source)

    def handle_starttag(self, tag, pairs):
        attrs = dict(pairs)
        if 'id' in attrs:
            if attrs['id'] in self.ids:
                raise ValueError(f'Duplicate HTML id: {attrs["id"]}')
            self.ids.add(attrs['id'])
        self.headings += tag == 'h1'
        for key in ('href', 'src'):
            if key in attrs:
                self.links.append(attrs[key])
        if tag == 'img':
            self.images += 1
            if 'alt' not in attrs or not {'width', 'height'} <= attrs.keys():
                raise ValueError('Images require alt text and dimensions')


def release_inventory(path):
    if path:
        release = json.loads(path.read_text())
    else:
        result = subprocess.run(
            ['gh', 'release', 'view', '--repo', REPO, '--json',
             'tagName,isDraft,isPrerelease,url,assets'],
            check=True, capture_output=True, text=True,
        )
        release = json.loads(result.stdout)
    tag = release['tagName']
    if release['isDraft'] or release['isPrerelease'] or not re.fullmatch(r'v\d+\.\d+\.\d+', tag):
        raise ValueError('The site must use a published stable semantic-version release')
    if release['url'] != f'https://github.com/{REPO}/releases/tag/{tag}':
        raise ValueError('Unexpected release repository or URL')
    return release


def validate_local_link(link, output, page):
    parsed = urlsplit(link)
    if parsed.scheme or parsed.netloc:
        # GitHub documentation URLs must point to real repository files.
        prefix = f'https://github.com/{REPO}/blob/main/'
        if link.startswith(prefix):
            target = ROOT / unquote(urlsplit(link.removeprefix(prefix)).path)
            if not target.is_file():
                raise ValueError(f'Missing documentation: {link}')
        return
    if parsed.path.startswith('/'):
        raise ValueError(f'Use relative paths for GitHub project Pages: {link}')
    target = (output / unquote(parsed.path or 'index.html')).resolve()
    if not target.is_relative_to(output) or not target.is_file():
        raise ValueError(f'Missing or nonportable local link: {link}')
    if target.name == 'index.html' and parsed.fragment and unquote(parsed.fragment) not in page.ids:
        raise ValueError(f'Missing section: {link}')


def validate_links(page, output, release):
    assets = {asset['name'] for asset in release['assets']}
    download_base = f'https://github.com/{REPO}/releases/download/{release["tagName"]}/'
    downloads = set()
    for link in page.links:
        if link.startswith(download_base):
            name = unquote(link.removeprefix(download_base))
            if name not in assets:
                raise ValueError(f'Download absent from published release: {name}')
            downloads.add(name)
        validate_local_link(link, output, page)
    if len(downloads) != 10:
        raise ValueError('Expected 8 platform packages, checksums, and exact source')
    if page.headings != 1:
        raise ValueError('Expected one primary heading')
    return len(downloads)


def build():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--release-data', type=Path, help='Previously saved gh release view JSON for offline builds')
    args = parser.parse_args()
    release = release_inventory(args.release_data)
    output = ROOT / 'build' / 'pages'
    if output.exists():
        shutil.rmtree(output)
    shutil.copytree(ROOT / 'site', output)
    replacements = {
        '@@VERSION@@': release['tagName'][1:],
        '@@RELEASE_URL@@': release['url'],
        '@@DOWNLOAD_BASE@@': f'https://github.com/{REPO}/releases/download/{release["tagName"]}',
    }
    path = output / 'index.html'
    html = path.read_text()
    for token, value in replacements.items():
        html = html.replace(token, value)
    if re.search(r'@@[A-Z_]+@@', html):
        raise ValueError('Unresolved template token')
    path.write_text(html)
    page = Page(html)
    downloads = validate_links(page, output, release)
    structured = re.search(r'<script type="application/ld\+json">(.*?)</script>', html, re.S)
    data = json.loads(structured[1])
    if data['softwareVersion'] != release['tagName'][1:]:
        raise ValueError('Structured data version differs from the download version')
    for theme in ('light', 'dark', 'brown', 'paper'):
        if not (output / 'assets' / 'screenshots' / f'{theme}.png').is_file():
            raise ValueError(f'Missing interactive theme: {theme}')
    (output / '.nojekyll').touch()
    (output / 'sitemap.xml').write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">'
        f'<url><loc>{SITE_URL}</loc></url></urlset>\n'
    )
    # Preserve the exact release inventory used to resolve the download buttons.
    (output / 'release.json').write_text(json.dumps(release, indent=2) + '\n')
    print(f'Built {output}: {release["tagName"]}, {downloads} verified downloads, '
          f'{len(page.links)} checked links, {page.images} accessible image elements')


if __name__ == '__main__':
    build()
