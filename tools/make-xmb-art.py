#!/usr/bin/env python3
"""Generate the Ghostcontrol Toolbox XMB home-screen art.

Produces the two PS5 sce_sys background images from the project's own key art:

  pic0  the home-screen background shown while the tile is selected: the
        ghost + DualSense hero on the right third, with a text-safe dark
        field on the left, spectral fog and a faint connection lattice.
  pic1  the launch image: the centred splash artwork, extended to 4K so it
        fills the screen instead of sitting in a square.

The artwork itself is the project's `core/assets/splash.jpg`; everything else
(gradients, fog, bloom, lattice, grain) is generated here, so the result is
reproducible from tracked sources.

Usage:
  python tools/make-xmb-art.py [--out DIR]

Writes pic0.png / pic1.png at 3840x2160 plus 1280x720 previews. Convert to the
shipped BC7 DDS form with tools/make-xmb-art.sh.
"""
from __future__ import annotations

import argparse
import pathlib
import sys

import numpy as np
from PIL import Image, ImageFilter

try:
    import cv2
except ImportError:  # pragma: no cover - generator convenience only
    cv2 = None

W, H = 3840, 2160
ROOT = pathlib.Path(__file__).resolve().parent.parent
SPLASH = ROOT / "core" / "assets" / "splash.jpg"
ICON = ROOT / "ui" / "sce_sys" / "icon0.png"

# Sampled from the splash artwork so the generated field matches the hero.
INK = np.array([0.008, 0.016, 0.031], dtype=np.float32)
DEEP = np.array([0.031, 0.094, 0.169], dtype=np.float32)
CYAN = np.array([0.208, 0.788, 1.000], dtype=np.float32)
AQUA = np.array([0.494, 0.914, 1.000], dtype=np.float32)
VIOLET = np.array([0.639, 0.361, 1.000], dtype=np.float32)
MAGENTA = np.array([1.000, 0.361, 0.784], dtype=np.float32)
BLUE = np.array([0.118, 0.357, 1.000], dtype=np.float32)


def _grid(w: int, h: int):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    return x, y


def radial(w: int, h: int, cx: float, cy: float, rx: float, ry: float, power: float = 2.0):
    x, y = _grid(w, h)
    d = ((x - cx * w) / rx) ** 2 + ((y - cy * h) / ry) ** 2
    return np.clip(1.0 - d, 0.0, 1.0) ** (power * 0.5)


def blur(a: np.ndarray, radius: float) -> np.ndarray:
    if radius <= 0:
        return a
    img = Image.fromarray(np.clip(a * 255.0, 0, 255).astype(np.uint8), mode="L")
    return np.asarray(img.filter(ImageFilter.GaussianBlur(radius)), dtype=np.float32) / 255.0


def blur_rgb(a: np.ndarray, radius: float) -> np.ndarray:
    out = np.empty_like(a)
    for c in range(3):
        out[..., c] = blur(a[..., c], radius)
    return out


def add(base: np.ndarray, color: np.ndarray, mask: np.ndarray, gain: float = 1.0):
    base += mask[..., None] * (color * gain)
    return base


def base_field(w: int = W, h: int = H) -> np.ndarray:
    x, y = _grid(w, h)
    v = (y / h)[..., None]
    hz = (x / w)[..., None]
    field = INK + (DEEP - INK) * (1.0 - v) * 0.9
    field = field + (BLUE * 0.05) * (1.0 - hz) * (1.0 - v)
    field = add(field, CYAN, radial(w, h, 0.74, 0.44, 0.62 * w, 0.70 * h, 2.2), 0.16)
    field = add(field, VIOLET, radial(w, h, 0.97, 0.74, 0.46 * w, 0.60 * h, 2.0), 0.13)
    field = add(field, MAGENTA, radial(w, h, 0.90, 0.20, 0.30 * w, 0.34 * h, 2.0), 0.05)
    field = add(field, BLUE, radial(w, h, 0.46, 0.06, 0.60 * w, 0.36 * h, 2.0), 0.06)
    return field


def fog(w: int, h: int, seed: int, bands: int = 9) -> np.ndarray:
    """Soft spectral ribbons: warped sine sheets, low frequency, additive."""
    rng = np.random.default_rng(seed)
    sw, sh = 480, 270
    x, y = _grid(sw, sh)
    nx, ny = x / sw, y / sh
    acc = np.zeros((sh, sw), dtype=np.float32)
    for i in range(bands):
        phase = rng.uniform(0, 2 * np.pi)
        freq = rng.uniform(1.1, 2.6)
        tilt = rng.uniform(-0.5, 0.5)
        warp = 0.16 * np.sin(ny * rng.uniform(2.0, 4.2) * np.pi + phase)
        centre = rng.uniform(0.18, 0.92)
        width = rng.uniform(0.035, 0.10)
        band = np.exp(-((ny - centre - tilt * nx * 0.35 - warp) / width) ** 2)
        band *= 0.55 + 0.45 * np.sin(nx * freq * np.pi + phase) ** 2
        acc += band * rng.uniform(0.25, 1.0)
    acc /= max(acc.max(), 1e-6)
    img = Image.fromarray((acc * 255).astype(np.uint8), mode="L").resize((w, h), Image.BICUBIC)
    return np.asarray(img, dtype=np.float32) / 255.0


def lattice(w: int, h: int) -> np.ndarray:
    """Faint connection mesh in the empty left field."""
    x, y = _grid(w, h)
    step = 96.0
    lx = np.abs(((x + step / 2) % step) - step / 2)
    ly = np.abs(((y + step / 2) % step) - step / 2)
    lines = np.maximum(np.exp(-(lx / 1.1) ** 2), np.exp(-(ly / 1.1) ** 2))
    dots = np.exp(-((lx ** 2 + ly ** 2) / (3.2 ** 2)))
    mesh = np.clip(lines * 0.55 + dots * 1.0, 0, 1)
    falloff = np.clip(1.0 - (x / w) / 0.62, 0, 1) ** 1.6
    return mesh * falloff


def _inpaint_text(src: Image.Image, rect: tuple[float, float, float, float]) -> Image.Image:
    """Erase baked lettering by swapping in a locally blurred plate.

    The band only contains soft smoke, so replacing it with a heavily blurred
    copy of itself is seamless while guaranteeing no glyph survives (masked
    inpainting left faint anti-aliased ghosts).
    """
    rgb = np.asarray(src, dtype=np.uint8)
    h, w = rgb.shape[:2]
    x0, y0, x1, y1 = (int(w * rect[0]), int(h * rect[1]), int(w * rect[2]), int(h * rect[3]))
    # A very wide blur removes the lettering completely; the patch only covers
    # flat smoke, so the lost detail is invisible.
    wide = cv2.GaussianBlur(rgb, (0, 0), 34.0)
    sub = rgb[y0:y1, x0:x1].astype(np.float32)
    patch = wide[y0:y1, x0:x1].astype(np.float32)
    mh, mw = sub.shape[:2]
    mask = np.zeros((mh, mw), dtype=np.float32)
    feather = max(6, int(min(mh, mw) * 0.30))
    mask[feather:mh - feather, feather:mw - feather] = 1.0
    mask = cv2.GaussianBlur(mask, (0, 0), feather / 2.0)
    blend = sub * (1 - mask[..., None]) + patch * mask[..., None]
    out = rgb.copy()
    out[y0:y1, x0:x1] = np.clip(blend, 0, 255).astype(np.uint8)
    return Image.fromarray(out)


def ghost_subject(scale_to: int, clean_bottom: bool = True):
    """Crop the hero (ghost + DualSense + cable) out of the splash art."""
    src = Image.open(SPLASH).convert("RGB")
    w, h = src.size
    if clean_bottom:
        src = _inpaint_text(src, (0.345, 0.772, 0.665, 0.845))
    # Drop the baked wordmark and strapline; keep the glowing subject.
    crop = src.crop((0, int(h * 0.208), w, h))
    cw, ch = crop.size  # PIL reports (width, height)
    scale = scale_to / ch
    crop = crop.resize((max(1, int(cw * scale)), scale_to), Image.LANCZOS)
    crop = crop.filter(ImageFilter.UnsharpMask(radius=2.0, percent=70, threshold=2))
    arr = np.asarray(crop, dtype=np.float32) / 255.0
    # a touch more body: lift contrast and saturation without clipping highlights
    arr = np.clip((arr - 0.5) * 1.10 + 0.5, 0, 1)
    grey = arr.mean(axis=2, keepdims=True)
    arr = np.clip(grey + (arr - grey) * 1.12, 0, 1)
    return arr


def icon_subject(scale_to: int):
    """The app icon's ghost + DualSense, with no baked lettering anywhere.

    The splash art carries a strapline that cannot be removed cleanly at 4K, so
    the home-screen hero is built from the icon plate instead: pure brand art,
    black background, which the emissive alpha drops out.
    """
    im = Image.open(ICON).convert("RGBA")
    plate = Image.new("RGBA", im.size, (0, 0, 0, 255))
    im = Image.alpha_composite(plate, im).convert("RGB")
    iw, ih = im.size
    scale = scale_to / ih
    im = im.resize((max(1, int(iw * scale)), scale_to), Image.LANCZOS)
    im = im.filter(ImageFilter.UnsharpMask(radius=1.6, percent=90, threshold=2))
    arr = np.asarray(im, dtype=np.float32) / 255.0
    arr = np.clip((arr - 0.5) * 1.14 + 0.5, 0, 1)
    grey = arr.mean(axis=2, keepdims=True)
    return np.clip(grey + (arr - grey) * 1.16, 0, 1)


def splash_plate(scale_to: int):
    """The whole splash plate, wordmark included, for the launch image."""
    src = Image.open(SPLASH).convert("RGB")
    iw, ih = src.size
    scale = scale_to / ih
    im = src.resize((max(1, int(iw * scale)), scale_to), Image.LANCZOS)
    im = im.filter(ImageFilter.UnsharpMask(radius=1.8, percent=60, threshold=2))
    arr = np.asarray(im, dtype=np.float32) / 255.0
    arr = np.clip((arr - 0.5) * 1.06 + 0.5, 0, 1)
    grey = arr.mean(axis=2, keepdims=True)
    return np.clip(grey + (arr - grey) * 1.08, 0, 1)


def soft_alpha(w: int, h: int, feather: float = 0.14) -> np.ndarray:
    """Radial-ish alpha so the square never shows an edge."""
    x, y = _grid(w, h)
    nx = (x / w - 0.5) * 2.0
    ny = (y / h - 0.5) * 2.0
    r = np.sqrt(nx ** 2 + ny ** 2) / 1.42
    a = np.clip((1.0 - r) / feather, 0.0, 1.0)
    return a ** 1.25


def emissive_alpha(sprite: np.ndarray, edge_feather: float = 0.20) -> np.ndarray:
    """Luminance-gated alpha: the dark backdrop disappears, the light stays.

    A rectangular ramp on top guarantees no visible crop edge: the art is a
    square plate and its smoke reaches the border.
    """
    lum = sprite.max(axis=2)
    a = np.clip((lum - 0.085) / 0.30, 0.0, 1.0)
    a = a * a * (3.0 - 2.0 * a)
    h, w = lum.shape
    x, y = _grid(w, h)
    nx = (x / w - 0.5) * 2.0
    ny = (y / h - 0.5) * 2.0
    r = np.sqrt(nx ** 2 + ny ** 2) / 1.42
    edge = np.clip((1.0 - r) / edge_feather, 0.0, 1.0) ** 1.2

    def ramp(t: np.ndarray, frac: float) -> np.ndarray:
        return np.clip(np.minimum(t, 1.0 - t) / frac, 0.0, 1.0) ** 1.4

    border = np.minimum(ramp(x / w, 0.085), ramp(y / h, 0.075))
    return np.clip(a * edge * border, 0.0, 1.0)


def place(canvas: np.ndarray, sprite: np.ndarray, cx: int, top: int, alpha: np.ndarray):
    h, w = sprite.shape[:2]
    x0 = int(cx - w / 2)
    y0 = int(top)
    ch, cw = canvas.shape[:2]
    xs0, ys0 = max(0, x0), max(0, y0)
    xs1, ys1 = min(cw, x0 + w), min(ch, y0 + h)
    if xs1 <= xs0 or ys1 <= ys0:
        return canvas
    sx0, sy0 = xs0 - x0, ys0 - y0
    sx1, sy1 = sx0 + (xs1 - xs0), sy0 + (ys1 - ys0)
    a = alpha[sy0:sy1, sx0:sx1][..., None]
    canvas[ys0:ys1, xs0:xs1] = canvas[ys0:ys1, xs0:xs1] * (1 - a) + sprite[sy0:sy1, sx0:sx1] * a
    return canvas


def emissive_pass(canvas: np.ndarray, sprite: np.ndarray, cx: int, top: int, alpha: np.ndarray):
    """Add bloom pulled from the sprite's brightest core, not its whole body."""
    lum = sprite.max(axis=2)
    core = np.clip((lum - 0.55) / 0.45, 0.0, 1.0) ** 1.6
    src = core * alpha
    h, w = src.shape
    canvas_h, canvas_w = canvas.shape[:2]
    glow = np.zeros((canvas_h, canvas_w), dtype=np.float32)
    x0 = int(cx - w / 2)
    y0 = int(top)
    xs0, ys0 = max(0, x0), max(0, y0)
    xs1, ys1 = min(canvas_w, x0 + w), min(canvas_h, y0 + h)
    if xs1 <= xs0 or ys1 <= ys0:
        return canvas
    glow[ys0:ys1, xs0:xs1] = src[ys0 - y0:ys1 - y0, xs0 - x0:xs1 - x0]
    for radius, gain in ((14, 0.42), (44, 0.30), (120, 0.24), (300, 0.16), (620, 0.10)):
        g = blur(glow, radius)
        canvas = add(canvas, CYAN * 0.60 + AQUA * 0.40, g, gain)
    return canvas


def finish(canvas: np.ndarray, seed: int = 7, text_safe: bool = True) -> Image.Image:
    # gentle filmic shoulder keeps highlights from clipping flat
    canvas = 1.0 - np.exp(-canvas * 1.35)
    # vignette
    v = radial(W, H, 0.52, 0.48, 0.88 * W, 0.94 * H, 2.2)
    canvas *= (0.62 + 0.38 * v)[..., None]
    if text_safe:
        # dim the left third so the system's title text stays legible
        x = np.linspace(0, 1, W, dtype=np.float32)[None, :, None]
        canvas *= (0.34 + 0.66 * np.clip((x - 0.06) / 0.44, 0, 1)) ** 0.75
    rng = np.random.default_rng(seed)
    grain = rng.normal(0.0, 0.0030, (H, W, 1)).astype(np.float32)
    canvas = canvas + grain
    dither = (rng.random((H, W, 1), dtype=np.float32) - 0.5) / 255.0
    canvas = np.clip(canvas + dither, 0, 1)
    return Image.fromarray((canvas ** (1 / 1.02) * 255.0 + 0.5).astype(np.uint8), mode="RGB")


def build_home() -> Image.Image:
    canvas = base_field()
    canvas = add(canvas, CYAN, fog(W, H, seed=11), 0.075)
    canvas = add(canvas, VIOLET, fog(W, H, seed=29), 0.050)
    canvas = add(canvas, AQUA, lattice(W, H), 0.045)

    sprite_h = 1560
    sprite = icon_subject(sprite_h)
    sh, sw = sprite.shape[:2]
    cx = int(W * 0.705)
    top = int(H * 0.175)
    alpha = emissive_alpha(sprite, edge_feather=0.18)

    canvas = add(canvas, CYAN, radial(W, H, 0.73, 0.50, 0.34 * W, 0.52 * H, 2.0), 0.075)
    canvas = place(canvas, sprite, cx, top, alpha)
    canvas = emissive_pass(canvas, sprite, cx, top, alpha)
    return finish(canvas, seed=7, text_safe=True)


def build_launch() -> Image.Image:
    canvas = base_field()
    canvas = add(canvas, CYAN, fog(W, H, seed=41), 0.070)
    canvas = add(canvas, MAGENTA, fog(W, H, seed=53), 0.030)

    sprite_h = int(H * 0.94)
    sprite = splash_plate(sprite_h)
    sh, sw = sprite.shape[:2]
    cx, top = W // 2, int(H * 0.030)
    alpha = emissive_alpha(sprite, edge_feather=0.18) * 0.95

    canvas = add(canvas, CYAN, radial(W, H, 0.50, 0.46, 0.32 * W, 0.54 * H, 2.0), 0.085)
    canvas = place(canvas, sprite, cx, top, alpha)
    canvas = emissive_pass(canvas, sprite, cx, top, alpha)
    return finish(canvas, seed=13, text_safe=False)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "ui" / "sce_sys"))
    ap.add_argument("--preview", default=str(ROOT / "ui" / "sce_sys" / "preview"))
    args = ap.parse_args()
    out = pathlib.Path(args.out)
    prev = pathlib.Path(args.preview)
    out.mkdir(parents=True, exist_ok=True)
    prev.mkdir(parents=True, exist_ok=True)

    for name, fn in (("pic0", build_home), ("pic1", build_launch)):
        img = fn()
        p = prev / f"{name}.png"
        img.save(p)
        img.resize((1280, 720), Image.LANCZOS).convert("RGB").save(
            prev / f"{name}-preview.png")
        print(f"wrote {p} {img.size[0]}x{img.size[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
