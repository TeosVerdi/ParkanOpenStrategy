#!/usr/bin/env python3
"""Builds the app icon (res/parkan.icns, committed) from an installed game.

Usage: make_icon.py <game dir> [out.icns]

Source: the GOG release's goggame-*.ico (256 px), else the largest icon in iron_3d.exe's resources
(RT_GROUP_ICON / RT_ICON; retail disc installs). macOS only: sizes through sips, packed by iconutil.
"""
import os
import struct
import subprocess
import sys
import tempfile


def ico_images(data):
    """(width, image bytes) for each entry of an .ico file; image bytes are PNG or a DIB."""
    _, kind, n = struct.unpack_from('<3H', data, 0)
    if kind != 1:
        raise ValueError('not an icon file')
    out = []
    for i in range(n):
        w, _, _, _, _, bpp, size, off = struct.unpack_from('<4B2H2I', data, 6 + 16 * i)
        out.append((w or 256, bpp, data[off:off + size]))
    return out


def pe_icons(data):
    """The images of the first RT_GROUP_ICON in a PE file, as (width, bpp, bytes)."""
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('not a PE file')
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    opt = pe + 24
    rsrc_rva = struct.unpack_from('<I', data, opt + 96 + 2 * 8)[0]  # data directory 2: resources
    sections = []
    for i in range(nsec):
        s = pe + 24 + struct.unpack_from('<H', data, pe + 20)[0] + 40 * i
        vsize, va, rsize, raw = struct.unpack_from('<4I', data, s + 8)
        sections.append((va, max(vsize, rsize), raw))

    def at(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return raw + rva - va
        raise ValueError('rva outside the sections')

    base = at(rsrc_rva)

    def entries(off):
        named, ids = struct.unpack_from('<2H', data, base + off + 12)
        for i in range(named + ids):
            name, target = struct.unpack_from('<2I', data, base + off + 16 + 8 * i)
            yield name, target

    def leaf(target):  # first language
        while target & 0x80000000:
            target = next(entries(target & 0x7fffffff))[1]
        rva, size = struct.unpack_from('<2I', data, base + target)
        return data[at(rva):at(rva) + size]

    by_type = {name: target for name, target in entries(0)}
    icons = {name: leaf(t) for name, t in entries(by_type[3] & 0x7fffffff)}  # RT_ICON
    group = leaf(next(entries(by_type[14] & 0x7fffffff))[1])                 # RT_GROUP_ICON
    n = struct.unpack_from('<H', group, 4)[0]
    out = []
    for i in range(n):
        w, _, _, _, _, bpp, _, ident = struct.unpack_from('<4B2HIH', group, 6 + 14 * i)
        out.append((w or 256, bpp, icons[ident]))
    return out


def dib_to_ico(width, bpp, image):
    """A one-image .ico around a DIB, so sips can read it."""
    header = struct.pack('<3H', 0, 1, 1) + struct.pack('<4B2H2I', width % 256, width % 256, 0, 0, 1, bpp, len(image), 22)
    return header + image


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    game = sys.argv[1]
    out = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..', 'res', 'parkan.icns'))
    names = os.listdir(game)
    goggame = [n for n in names if n.lower().startswith('goggame-') and n.lower().endswith('.ico')]
    exe = [n for n in names if n.lower() == 'iron_3d.exe']
    if goggame:
        images = ico_images(open(os.path.join(game, goggame[0]), 'rb').read())
    elif exe:
        images = pe_icons(open(os.path.join(game, exe[0]), 'rb').read())
    else:
        sys.exit('no goggame-*.ico or iron_3d.exe in ' + game)
    width, bpp, image = max(images, key=lambda e: (e[0], e[1]))
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'source.png' if image[:4] == b'\x89PNG' else 'source.ico')
        open(src, 'wb').write(image if src.endswith('.png') else dib_to_ico(width, bpp, image))
        iconset = os.path.join(tmp, 'parkan.iconset')
        os.mkdir(iconset)
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = size * scale
                name = f'icon_{size}x{size}{"@2x" if scale == 2 else ""}.png'
                subprocess.run(['sips', '-s', 'format', 'png', '-z', str(px), str(px), src, '--out', os.path.join(iconset, name)],
                               check=True, capture_output=True)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        subprocess.run(['iconutil', '-c', 'icns', iconset, '-o', out], check=True)
    print(f'{out}: from {goggame[0] if goggame else exe[0]} ({width} px source)')


if __name__ == '__main__':
    main()
