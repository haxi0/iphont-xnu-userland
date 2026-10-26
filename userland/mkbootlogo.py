#!/usr/bin/env python3
"""Make the boot logo that backboardd shows while iOS starts -- ProgressUI's apple-logo*.png, which a
PUIProgressWindow draws for BKDisplayRenderOverlayAppleLogo -- out of a picture of one's own.

  mkbootlogo.py PICTURE.png ../ios165/rootfs/System/Library/PrivateFrameworks/ProgressUI.framework bootlogo [SCALE]

Every apple-logo*.png the framework has is made again at its own size in pixels, so whichever one
ProgressUI picks for the device takes the room Apple's took, times SCALE (default 1; the canvas keeps
Apple's aspect ratio, so a fixed frame would shrink it back, not stretch it): the picture cropped to
what of it is not transparent, scaled to fit with its aspect ratio kept, centred on a transparent canvas. build.sh puts
the directory in place of Apple's files (src/init.c, flash_root; the system image's ProgressUI.framework).

Pillow is not in shell.nix; once, by hand:
  nix-shell -p 'python3.withPackages (p: [ p.pillow ])' --run 'python3 mkbootlogo.py ...'
"""
import os
import struct
import sys

from PIL import Image


def png_size(path):
    """Width and height from IHDR -- Apple's files are CgBI PNGs, which Pillow does not open."""
    data = open(path, 'rb').read()
    pos = 8
    while pos < len(data):
        length, kind = struct.unpack_from('>I4s', data, pos)
        if kind == b'IHDR':
            return struct.unpack_from('>II', data, pos + 8)
        pos += 12 + length
    raise SystemExit(f'mkbootlogo.py: {path}: no IHDR')


def fit(picture, width, height):
    scale = min(width / picture.width, height / picture.height)
    size = (max(1, round(picture.width * scale)), max(1, round(picture.height * scale)))
    canvas = Image.new('RGBA', (width, height))
    canvas.paste(picture.resize(size, Image.LANCZOS), ((width - size[0]) // 2, (height - size[1]) // 2))
    return canvas


def main():
    if len(sys.argv) not in (4, 5):
        raise SystemExit(__doc__)
    picture, framework, out = sys.argv[1:4]
    scale = float(sys.argv[4]) if len(sys.argv) == 5 else 1
    picture = Image.open(picture).convert('RGBA')
    bbox = picture.getchannel('A').getbbox()
    if bbox is None:
        raise SystemExit('mkbootlogo.py: the picture is transparent all over')
    picture = picture.crop(bbox)
    os.makedirs(out, exist_ok=True)
    names = sorted(n for n in os.listdir(framework) if n.startswith('apple-logo') and n.endswith('.png'))
    if not names:
        raise SystemExit(f'mkbootlogo.py: no apple-logo*.png in {framework}')
    for name in names:
        width, height = (round(n * scale) for n in png_size(os.path.join(framework, name)))
        logo = fit(picture, width, height)
        assert logo.size == (width, height)
        logo.save(os.path.join(out, name), optimize=True)
        print(f'{name}: {width} x {height}')


main()
