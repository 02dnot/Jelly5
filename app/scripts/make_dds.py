#!/usr/bin/env python3
# Jelly5 — Jellyfin for PS5
# SPDX-License-Identifier: GPL-3.0-or-later
"""
The home screen's backgrounds: the PS5 shows sce_sys/pic0.dds behind the tile
when Jelly5 is selected and pic1.dds while it launches. Both must be a single
3840x2160 BC7_UNORM DX10 DDS without mipmaps (as ProsperoTV's prepare-assets
checks). PNG is ignored there.

    python3 -m venv /tmp/dds && /tmp/dds/bin/pip install numpy etcpak pillow
    /tmp/dds/bin/python app/scripts/make_backdrop.py /tmp/backdrop.png   (the launch backdrop)
    /tmp/dds/bin/python app/scripts/make_dds.py /tmp/backdrop.png
"""
import struct
import sys
from pathlib import Path

import etcpak
from PIL import Image

W, H = 3840, 2160
DXGI_FORMAT_BC7_UNORM = 98


def dds_header(width, height, size):
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000          # caps, height, width, pixelformat, linearsize
    pf = struct.pack("<II4sIIIII", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    head = struct.pack("<4sIIIIII", b"DDS ", 124, flags, height, width, size, 0) + struct.pack("<I", 1)
    head += b"\0" * 44 + pf + struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)
    dx10 = struct.pack("<IIIII", DXGI_FORMAT_BC7_UNORM, 3, 0, 1, 0)   # format, TEXTURE2D, flags, array 1
    return head + dx10


def main():
    src = Path(sys.argv[1])
    out = Path(__file__).resolve().parent.parent / "sce_sys"
    img = Image.open(src).convert("RGBA")
    # Fill 3840x2160 (cover, centred), then encode.
    scale = max(W / img.width, H / img.height)
    img = img.resize((round(img.width * scale), round(img.height * scale)), Image.LANCZOS)
    left, top = (img.width - W) // 2, (img.height - H) // 2
    img = img.crop((left, top, left + W, top + H))
    data = etcpak.compress_bc7(img.tobytes(), W, H)
    assert len(data) == (W // 4) * (H // 4) * 16, len(data)
    blob = dds_header(W, H, len(data)) + data
    assert len(blob) == 148 + len(data)
    for name in ("pic0.dds", "pic1.dds"):
        (out / name).write_bytes(blob)
        print(f"{out / name}: {len(blob)} bytes")


if __name__ == "__main__":
    main()
