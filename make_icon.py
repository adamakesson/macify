# Generates Macify's icons:
#   app.ico         exe / Start menu: white keyboard and mouse on a blue tile
#   tray-dark.ico   tray, for a dark taskbar (white glyph)
#   tray-light.ico  tray, for a light taskbar (black glyph)
from PIL import Image, ImageDraw

S = 1024  # draw large, downsample for anti-aliasing


def glyph(color, scale=1.0, cx=0.5, cy=0.5, bold=False):
    """Keyboard with a mouse beside it, on a transparent canvas."""
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def box(x0, y0, x1, y1):
        # unit coords (0..1 around the glyph centre) -> pixels
        f = lambda v, c: (c + (v - 0.5) * scale) * S
        return [f(x0, cx), f(y0, cy), f(x1, cx), f(y1, cy)]

    # keyboard body
    d.rounded_rectangle(box(0.02, 0.30, 0.70, 0.78), radius=S * 0.06 * scale, fill=color)
    # keys: cut out (transparent)
    gap = 0.035 if bold else 0.028
    rows = [(0.36, 0.47), (0.505, 0.615)]
    cols = 4
    x0, x1 = 0.08, 0.64
    w = (x1 - x0 - gap * (cols - 1)) / cols
    for (ry0, ry1) in rows:
        for c in range(cols):
            kx = x0 + c * (w + gap)
            d.rounded_rectangle(box(kx, ry0, kx + w, ry1), radius=S * 0.015 * scale, fill=(0, 0, 0, 0))
    # space bar
    d.rounded_rectangle(box(0.20, 0.65, 0.52, 0.72), radius=S * 0.015 * scale, fill=(0, 0, 0, 0))

    # mouse: clear a ring around it so it reads as separate from the keyboard
    ring = 0.035
    d.rounded_rectangle(box(0.64 - ring, 0.40 - ring, 0.98 + ring, 0.86 + ring), radius=S * 0.17 * scale, fill=(0, 0, 0, 0))
    d.rounded_rectangle(box(0.64, 0.40, 0.98, 0.86), radius=S * 0.17 * scale, fill=color)
    # button split and scroll wheel
    lw = 0.03 if bold else 0.022
    d.rectangle(box(0.81 - lw / 2, 0.40, 0.81 + lw / 2, 0.58), fill=(0, 0, 0, 0))
    d.rectangle(box(0.64, 0.58 - lw / 2, 0.98, 0.58 + lw / 2), fill=(0, 0, 0, 0))
    return img


# app icon: blue tile + white glyph
tile = Image.new('RGBA', (S, S), (0, 0, 0, 0))
grad = Image.new('RGBA', (S, S))
top, bottom = (78, 140, 255), (36, 92, 230)
gd = ImageDraw.Draw(grad)
for y in range(S):
    t = y / (S - 1)
    gd.line([(0, y), (S, y)], fill=tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
mask = Image.new('L', (S, S), 0)
pad = S * 0.06
ImageDraw.Draw(mask).rounded_rectangle([pad, pad, S - pad, S - pad], radius=S * 0.22, fill=255)
tile.paste(grad, (0, 0), mask)
g = glyph((255, 255, 255, 255), scale=0.74, cx=0.5, cy=0.47)
tile.alpha_composite(g)
# the glyph's cut-outs must show the tile, not transparency: composite onto the tile only
app = Image.new('RGBA', (S, S), (0, 0, 0, 0))
app.paste(tile, (0, 0), mask)
sizes = [16, 20, 24, 32, 40, 48, 64, 256]
app.resize((256, 256), Image.LANCZOS).save('app.ico', sizes=[(s, s) for s in sizes])

# tray icons: glyph only, filling the square
tray_sizes = [16, 20, 24, 32, 40, 48, 64]
for name, color in [('tray-dark.ico', (255, 255, 255, 255)), ('tray-light.ico', (26, 26, 26, 255))]:
    t = glyph(color, scale=1.0, bold=True)
    t.resize((64, 64), Image.LANCZOS).save(name, sizes=[(s, s) for s in tray_sizes])

# previews for checking by eye
prev = Image.new('RGBA', (560, 140), (40, 40, 40, 255))
prev.alpha_composite(app.resize((128, 128), Image.LANCZOS), (6, 6))
for i, s in enumerate([16, 24, 32, 48]):
    prev.alpha_composite(app.resize((s, s), Image.LANCZOS), (150 + i * 60, 10))
    prev.alpha_composite(glyph((255, 255, 255, 255), bold=True).resize((s, s), Image.LANCZOS), (150 + i * 60, 80))
light = Image.new('RGBA', (140, 140), (238, 238, 238, 255))
for i, s in enumerate([16, 24, 32]):
    light.alpha_composite(glyph((26, 26, 26, 255), bold=True).resize((s, s), Image.LANCZOS), (10 + i * 40, 50))
prev.alpha_composite(light, (410, 0))
prev.save('icon-preview.png')
