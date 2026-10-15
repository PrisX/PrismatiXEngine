"""Generates the original starter artwork in packages/cli/templates/minimal/Assets.

The pictures are procedural (gradients, shapes and blurs), so the starter
carries no third-party art. Run: python scripts/generate-starter-art.py
"""
import math
import random
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1] / "packages" / "cli" / "templates" / "minimal" / "Assets"
W, H = 1280, 720
rng = random.Random(20261016)


def lerp(a, b, t):
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


def vertical_gradient(width, height, stops):
    """stops: [(position 0..1, (r, g, b))]"""
    out = np.zeros((height, width, 3), np.float32)
    ys = np.linspace(0, 1, height)
    for channel in range(3):
        out[:, :, channel] = np.interp(ys, [s[0] for s in stops], [s[1][channel] for s in stops])[:, None]
    return Image.fromarray(out.clip(0, 255).astype(np.uint8), "RGB").convert("RGBA")


def glow(size, radius, color, blur):
    layer = Image.new("RGBA", size, color[:3] + (0,))
    d = ImageDraw.Draw(layer)
    cx, cy = size[0] // 2, size[1] // 2
    d.ellipse((cx - radius, cy - radius, cx + radius, cy + radius), fill=color)
    alpha = layer.getchannel("A").filter(ImageFilter.GaussianBlur(blur))
    layer.putalpha(alpha)
    return layer


def skyline(width, base, heights, color, window_color, seed, lit=0.18):
    r = random.Random(seed)
    layer = Image.new("RGBA", (width, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    x = -20
    while x < width:
        w = r.randint(*heights[1])
        h = r.randint(*heights[0])
        d.rectangle((x, base - h, x + w, H), fill=color)
        if r.random() < 0.25:  # a roof spire or water tank
            d.rectangle((x + w // 3, base - h - r.randint(8, 22), x + w // 3 + 4, base - h), fill=color)
        if window_color:
            for wy in range(base - h + 8, base - 6, 9):
                for wx in range(x + 5, x + w - 5, 8):
                    if r.random() < lit:
                        d.rectangle((wx, wy, wx + 3, wy + 4), fill=window_color)
        x += w + r.randint(-4, 6)
    return layer


def backdrop():
    # Dusk sky: indigo night overhead warming to a peach horizon.
    sky = vertical_gradient(W, H, [(0.0, (22, 24, 64)), (0.38, (66, 52, 118)),
                                   (0.62, (196, 118, 132)), (0.74, (246, 176, 128)), (1.0, (252, 214, 160))])
    canvas = sky.copy()
    stars = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(stars)
    for _ in range(260):
        x, y = rng.uniform(0, W), rng.uniform(0, H * 0.5)
        a = int(255 * (1 - y / (H * 0.5)) * rng.uniform(0.3, 1.0))
        r = rng.choice([0.6, 0.8, 1.0, 1.4])
        d.ellipse((x - r, y - r, x + r, y + r), fill=(255, 248, 236, a))
    canvas.alpha_composite(stars)
    # Crescent moon with a soft halo.
    canvas.alpha_composite(glow((W, H), 70, (255, 236, 200, 90), 40), (330, -230))
    moon = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    md = ImageDraw.Draw(moon)
    md.ellipse((945, 92, 1025, 172), fill=(255, 244, 222, 255))
    md.ellipse((965, 80, 1045, 160), fill=(0, 0, 0, 0))
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).ellipse((945, 92, 1025, 172), fill=255)
    ImageDraw.Draw(mask).ellipse((965, 80, 1045, 160), fill=0)
    moon.putalpha(mask)
    canvas.alpha_composite(moon)
    # Thin clouds catching the last light.
    clouds = Image.new("RGBA", (W, H), (255, 196, 176, 0))
    cd = ImageDraw.Draw(clouds)
    for _ in range(14):
        x, y = rng.uniform(-100, W), rng.uniform(260, 420)
        w, h = rng.uniform(180, 420), rng.uniform(10, 24)
        cd.ellipse((x, y, x + w, y + h), fill=(255, 196, 176, rng.randint(40, 90)))
    clouds.putalpha(clouds.getchannel("A").filter(ImageFilter.GaussianBlur(6)))
    canvas.alpha_composite(clouds)
    # City: far haze, middle and near skylines with lit windows.
    canvas.alpha_composite(skyline(W, 500, ((40, 120), (40, 90)), (112, 78, 128, 255), None, 1))
    canvas.alpha_composite(skyline(W, 520, ((60, 170), (36, 80)), (70, 50, 98, 255), (255, 214, 160, 150), 2, 0.12))
    canvas.alpha_composite(skyline(W, 560, ((70, 210), (50, 110)), (38, 30, 64, 255), (255, 206, 140, 220), 3, 0.2))
    # Interior: walls framing a tall window, a sill and drapes.
    interior = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    idr = ImageDraw.Draw(interior)
    wall = (24, 20, 44, 255)
    idr.rectangle((0, 0, 150, H), fill=wall)
    idr.rectangle((W - 150, 0, W, H), fill=wall)
    idr.rectangle((0, 0, W, 46), fill=wall)
    idr.rectangle((0, 590, W, H), fill=(30, 25, 52, 255))
    frame = (54, 42, 80, 255)
    idr.rectangle((150, 46, W - 150, 62), fill=frame)
    idr.rectangle((150, 46, 166, 590), fill=frame)
    idr.rectangle((W - 166, 46, W - 150, 590), fill=frame)
    idr.rectangle((632, 46, 648, 590), fill=frame)
    idr.rectangle((150, 300, W - 150, 310), fill=frame)
    idr.rectangle((120, 578, W - 120, 602), fill=(70, 56, 98, 255))  # sill
    idr.rectangle((120, 602, W - 120, 610), fill=(20, 16, 36, 255))
    canvas.alpha_composite(interior)
    # Drapes with soft folds.
    for side in (0, 1):
        drape = np.zeros((H, 210, 4), np.float32)
        xs = np.arange(210)
        fold = 0.5 + 0.5 * np.sin(xs / 210 * math.pi * 5 + side)
        shade = 30 + 26 * fold
        drape[:, :, 0] = (shade * 1.25)[None, :]
        drape[:, :, 1] = (shade * 0.85)[None, :]
        drape[:, :, 2] = (shade * 1.7)[None, :]
        edge = np.clip(xs / 210, 0, 1)
        drape[:, :, 3] = 255 * np.clip(1.25 - edge * 1.1, 0, 1)[None, :]
        im = Image.fromarray(drape.clip(0, 255).astype(np.uint8), "RGBA")
        if side == 1:
            im = im.transpose(Image.FLIP_LEFT_RIGHT)
        canvas.alpha_composite(im, (0 if side == 0 else W - 210, 0))
    # Glass sheen and floating motes.
    sheen = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sheen)
    sd.polygon([(260, 62), (360, 62), (210, 300), (166, 300)], fill=(255, 255, 255, 18))
    sd.polygon([(760, 310), (820, 310), (700, 578), (660, 578)], fill=(255, 255, 255, 12))
    canvas.alpha_composite(sheen)
    for _ in range(26):
        x, y, r = rng.uniform(160, W - 160), rng.uniform(80, 560), rng.uniform(3, 9)
        canvas.alpha_composite(glow((64, 64), r, (255, 222, 170, rng.randint(40, 110)), r * 0.8),
                               (int(x) - 32, int(y) - 32))
    # Vignette.
    yy, xx = np.mgrid[0:H, 0:W]
    v = ((xx - W / 2) / (W / 2)) ** 2 + ((yy - H / 2) / (H / 2)) ** 2
    vig = np.clip((v - 0.55) * 0.55, 0, 0.6)
    arr = np.array(canvas).astype(np.float32)
    arr[:, :, :3] *= (1 - vig)[:, :, None]
    return Image.fromarray(arr.clip(0, 255).astype(np.uint8), "RGBA")


def spirit():
    """小光: a small light spirit with a gentle face and a trailing wisp."""
    w, h = 400, 600
    canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    cx, cy = 200, 250
    # Outer halo and body glow.
    canvas.alpha_composite(glow((w, h), 150, (255, 210, 140, 70), 60))
    canvas.alpha_composite(glow((w, h), 105, (255, 226, 170, 140), 30), (0, cy - h // 2))
    # Trailing wisp: tapering circles curving down and to the left.
    wisp = Image.new("RGBA", (w, h), (255, 214, 150, 0))
    wd = ImageDraw.Draw(wisp)
    for i in range(60):
        t = i / 59
        x = cx - 20 * math.sin(t * math.pi * 1.4) - t * 30
        y = cy + 60 + t * 250
        r = 62 * (1 - t) ** 1.3 + 3
        wd.ellipse((x - r, y - r, x + r, y + r), fill=(255, 214, 150, int(150 * (1 - t))))
    wisp.putalpha(wisp.getchannel("A").filter(ImageFilter.GaussianBlur(10)))
    canvas.alpha_composite(wisp)
    # Body: a radial gradient orb.
    size = 200
    yy, xx = np.mgrid[0:size, 0:size]
    d = np.sqrt((xx - size / 2 + 18) ** 2 + (yy - size / 2 + 22) ** 2) / (size / 2)
    body = np.zeros((size, size, 4), np.float32)
    inner, outer = np.array([255, 252, 240]), np.array([255, 196, 120])
    t = np.clip(d / 1.25, 0, 1)[:, :, None]
    body[:, :, :3] = inner * (1 - t) + outer * t
    edge = np.sqrt((xx - size / 2) ** 2 + (yy - size / 2) ** 2) / (size / 2)
    body[:, :, 3] = 255 * np.clip((1 - edge) * 14, 0, 1)
    orb = Image.fromarray(body.clip(0, 255).astype(np.uint8), "RGBA")
    canvas.alpha_composite(orb, (cx - size // 2, cy - size // 2))
    # Face: closed smiling eyes, a small mouth and warm cheeks.
    face = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    fd = ImageDraw.Draw(face)
    ink = (92, 58, 54, 255)
    fd.arc((cx - 52, cy - 8, cx - 18, cy + 22), 200, 340, fill=ink, width=5)
    fd.arc((cx + 18, cy - 8, cx + 52, cy + 22), 200, 340, fill=ink, width=5)
    fd.arc((cx - 14, cy + 14, cx + 14, cy + 36), 20, 160, fill=ink, width=4)
    blush = Image.new("RGBA", (w, h), (255, 140, 130, 0))
    bd = ImageDraw.Draw(blush)
    bd.ellipse((cx - 70, cy + 18, cx - 38, cy + 36), fill=(255, 140, 130, 120))
    bd.ellipse((cx + 38, cy + 18, cx + 70, cy + 36), fill=(255, 140, 130, 120))
    blush.putalpha(blush.getchannel("A").filter(ImageFilter.GaussianBlur(4)))
    canvas.alpha_composite(blush)
    canvas.alpha_composite(face)
    # A little star, worn like a hair clip, and sparkles around.
    def star(draw, x, y, r, color):
        points = []
        for k in range(10):
            a = -math.pi / 2 + k * math.pi / 5
            rr = r if k % 2 == 0 else r * 0.45
            points.append((x + rr * math.cos(a), y + rr * math.sin(a)))
        draw.polygon(points, fill=color)
    sd = ImageDraw.Draw(canvas)
    star(sd, cx + 62, cy - 66, 20, (255, 238, 150, 255))
    for x, y, r in [(70, 120, 7), (330, 170, 9), (300, 400, 6), (96, 360, 5), (350, 300, 4)]:
        star(sd, x, y, r, (255, 246, 214, 230))
    return canvas


ROOT.mkdir(parents=True, exist_ok=True)
backdrop().save(ROOT / "window.png", optimize=True)
spirit().save(ROOT / "guide.png", optimize=True)
print("wrote", ROOT / "window.png", ROOT / "guide.png")
