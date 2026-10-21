"""Generates the engine's grid ground texture: a faint prototype grid, one 1 m cell per repeat.

The image is one tileable repeat: a flat 0.22 linear grey (the ground's albedo, that of dry soil or
short grass; the auto exposure meters the frame against it, so a lighter ground darkens the whole frame)
with a light line through its middle row and column. The texture is sRGB-encoded and the Ground Grid
material's base color is white, so the gray is carried by the texture alone. The material is the ground of
the New Scene and of the Basic 3D template. The plane primitive spans 1 m and UV 0..1, so a material tiling
equal to the plane scale (6000 on both grounds) puts one repeat on each meter
with the repeat's edges on whole metres; the line in the middle of the repeat therefore falls on the
half metres, and a 1 m cube centred on a whole metre fills exactly one cell.

Run from anywhere: python generate_ground_grid.py
"""

from pathlib import Path

import numpy as np
from PIL import Image

SIZE = 128                 # texels per repeat, i.e. per meter at the material's tiling
LINE_WIDTH_TEXELS = 4.0    # about 3 cm on the ground
GROUND_LINEAR = 0.22       # dry soil or short grass; a grey card is 0.18
LINE_LINEAR = 0.35         # light enough that the 1 m cells read near the camera
OUT = Path(__file__).resolve().parents[2] / "Engine" / "Assets" / "Textures" / "GroundGrid.png"


def linear_to_srgb(value):
    return np.where(value <= 0.0031308, value * 12.92, 1.055 * np.power(value, 1.0 / 2.4) - 0.055)


def line_coverage():
    """Per-texel coverage of a line of LINE_WIDTH_TEXELS centred on the repeat's middle, box-filtered."""
    centres = np.arange(SIZE) + 0.5
    distance = np.abs(centres - SIZE * 0.5)
    half = LINE_WIDTH_TEXELS * 0.5
    return np.clip(half - distance + 0.5, 0.0, 1.0)


def main():
    edge = line_coverage()
    coverage = np.maximum(edge[None, :], edge[:, None])
    linear = GROUND_LINEAR + (LINE_LINEAR - GROUND_LINEAR) * coverage
    grey = np.round(linear_to_srgb(linear) * 255.0).astype(np.uint8)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(grey, mode="L").convert("RGB").save(OUT, optimize=True)
    print(f"{OUT} {OUT.stat().st_size} bytes")


if __name__ == "__main__":
    main()
