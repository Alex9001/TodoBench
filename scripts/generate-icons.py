#!/usr/bin/env python3
"""Regenerate launcher formats from the SVG master (CairoSVG and Pillow)."""
from pathlib import Path
import io
import cairosvg
from PIL import Image

root = Path(__file__).resolve().parents[1]
assets = root / 'packaging/icons'
png = cairosvg.svg2png(url=str(assets / 'todobench.svg'), output_width=1024, output_height=1024)
(assets / 'todobench.png').write_bytes(png)
image = Image.open(io.BytesIO(png))
image.save(assets / 'todobench.ico', sizes=[(n, n) for n in (16, 24, 32, 48, 64, 128, 256)])
image.save(assets / 'todobench.icns')
(root / 'packaging/linux/todobench.svg').write_bytes((assets / 'todobench.svg').read_bytes())
