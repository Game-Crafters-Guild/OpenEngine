"""Generates the Blank template's picker thumbnail: an illustrated card of what the template holds.

The sky gradient is the Blank scene's own Game View sky (zenith to horizon), with a thin horizon
line and a camera and a sun drawn as line glyphs. The card carries no text: the picker prints the
template's description directly below the image. The picker shows the 680x410 image covering a
280x131 preview, so it is scaled by 680/280 and loses about 46 rows top and bottom: everything
drawn stays inside that window. The glyph pair is centred vertically on the image, which is the
centre of that window, and the horizon sits below it.

Run from anywhere: python generate_blank_thumbnail.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "Apps" / "Editor" / "ProjectTemplates" / "Blank" / "thumbnail.png"

WIDTH, HEIGHT = 680, 410   # the Basic 3D thumbnail's size
SUPERSAMPLE = 4
HORIZON_Y = 280
GLYPHS_CENTRE_Y = HEIGHT // 2  # the camera and sun together span 104 rows, the sun 28 rows higher

ZENITH = (15, 74, 140)     # the Blank scene's sky at the top of its Game View
MID_SKY = (49, 105, 157)
HORIZON = (82, 118, 143)
BELOW = (66, 96, 118)      # below the horizon, a step darker so the line reads
GLYPH = (236, 242, 248)
SUN = (255, 236, 196)


def lerp(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def sky_colour(y):
    t = y / HORIZON_Y
    return lerp(ZENITH, MID_SKY, t / 0.6) if t < 0.6 else lerp(MID_SKY, HORIZON, (t - 0.6) / 0.4)


def draw_background(draw, s):
    for y in range(HEIGHT * s):
        row = y / s
        colour = sky_colour(row) if row < HORIZON_Y else BELOW
        draw.line([(0, y), (WIDTH * s, y)], fill=colour)
    draw.rectangle([0, HORIZON_Y * s - s, WIDTH * s, HORIZON_Y * s + s], fill=(176, 198, 216))


def draw_camera(draw, s, cx, cy):
    stroke = 4 * s
    body = [cx - 52, cy - 30, cx + 30, cy + 30]
    draw.rounded_rectangle([v * s for v in body], radius=8 * s, outline=GLYPH, width=stroke)
    draw.ellipse([(cx - 11 - 20) * s, (cy - 20) * s, (cx - 11 + 20) * s, (cy + 20) * s],
                 outline=GLYPH, width=stroke)
    lens = [(cx + 30, cy - 10), (cx + 62, cy - 28), (cx + 62, cy + 28), (cx + 30, cy + 10)]
    draw.polygon([(x * s, y * s) for x, y in lens], outline=GLYPH, width=stroke)


def draw_sun(draw, s, cx, cy):
    radius = 22
    draw.ellipse([(cx - radius) * s, (cy - radius) * s, (cx + radius) * s, (cy + radius) * s],
                 fill=SUN)
    for i in range(8):
        dx, dy = [(1, 0), (0.7071, 0.7071), (0, 1), (-0.7071, 0.7071),
                  (-1, 0), (-0.7071, -0.7071), (0, -1), (0.7071, -0.7071)][i]
        inner, outer = radius + 10, radius + 24
        draw.line([((cx + dx * inner) * s, (cy + dy * inner) * s),
                   ((cx + dx * outer) * s, (cy + dy * outer) * s)], fill=SUN, width=4 * s)


def main():
    s = SUPERSAMPLE
    image = Image.new("RGB", (WIDTH * s, HEIGHT * s))
    draw = ImageDraw.Draw(image)
    draw_background(draw, s)
    draw_camera(draw, s, 250, GLYPHS_CENTRE_Y + 22)
    draw_sun(draw, s, 446, GLYPHS_CENTRE_Y - 6)
    image = image.resize((WIDTH, HEIGHT), Image.LANCZOS)
    image.save(OUT, optimize=True)
    print(f"{OUT} {image.size} {OUT.stat().st_size} bytes")


if __name__ == "__main__":
    main()
