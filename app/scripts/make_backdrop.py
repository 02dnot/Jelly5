#!/usr/bin/env python3
# Jelly5 — Jellyfin for PS5
# SPDX-License-Identifier: GPL-3.0-or-later
"""
The launch backdrop: near black with two broad glows in the brand's blue and
violet. The app draws the same picture itself at the panel's resolution
(ui::launch_backdrop, src/ui/screen.cpp: keep the numbers in step); this script
makes it as a 3840x2160 PNG for the PS5's own pictures (pic0/pic1.dds), so the
system's launch screen hands over to the app without a jump.

    python3 -m venv /tmp/dds && /tmp/dds/bin/pip install numpy pillow etcpak
    /tmp/dds/bin/python app/scripts/make_backdrop.py /tmp/backdrop.png
    /tmp/dds/bin/python app/scripts/make_dds.py /tmp/backdrop.png
"""
import sys

import numpy as np
from PIL import Image

BASE = (7, 7, 12)
# (r, g, b), centre (x, y as fractions of the screen), radius (in screen heights), strength
GLOWS = [
    ((38, 92, 255), (0.04, 0.06), 0.62, 0.34),
    ((128, 58, 236), (0.98, 1.02), 0.58, 0.30),
]


def render(w, h, seed=7):
    x = (np.arange(w) + 0.5) / w
    y = (np.arange(h) + 0.5) / h
    aspect = w / h
    img = np.zeros((h, w, 3), np.float64)
    img[:] = BASE
    for color, (cx, cy), r, k in GLOWS:
        gx = np.exp(-(((x - cx) * aspect) ** 2) / r**2)   # separable: exp(-(dx²+dy²)/r²)
        gy = np.exp(-((y - cy) ** 2) / r**2)
        img += k * np.outer(gy, gx)[:, :, None] * np.array(color, np.float64)
    # Dither (±0.5 of a step) so the dark gradients never band.
    img += np.random.default_rng(seed).random((h, w, 1)) - 0.5
    return Image.fromarray(np.clip(np.rint(img), 0, 255).astype(np.uint8))


if __name__ == "__main__":
    render(3840, 2160).save(sys.argv[1] if len(sys.argv) > 1 else "backdrop.png")
