#!/usr/bin/env python3
"""Dev-time asset baker for AMUIE.

Rasterizes Open Sans + Lucide icons into one 8-bit alpha atlas and emits a
committed C++ source/header pair. The runtime never loads files or fonts.

Requires: Pillow and rsvg-convert (development only).
"""
from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageFont
from scipy import ndimage

ROOT = Path(__file__).resolve().parents[1]
ICONS_DIR = ROOT / "tools" / "icons"
OUT_DIR = ROOT / "src" / "assets"

FONT_CANDIDATES = [
    Path("/usr/share/fonts/cantarell/Cantarell-VF.otf"),
    Path("/usr/share/fonts/TTF/OpenSans-Regular.ttf"),
    Path("/usr/share/fonts/noto/NotoSans-Regular.ttf"),
    Path("/usr/share/fonts/TTF/DejaVuSans.ttf"),
]
SEMIBOLD_CANDIDATES = [
    Path("/usr/share/fonts/cantarell/Cantarell-VF.otf"),
    Path("/usr/share/fonts/TTF/OpenSans-SemiBold.ttf"),
    Path("/usr/share/fonts/noto/NotoSans-SemiBold.ttf"),
    Path("/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"),
]
# Text weights baked into the atlas: 0 = regular, 1 = bold, 2 = medium.
# Weight axis values apply to variable fonts; other fonts fall back to the
# nearest named instance.
FONT_ROLES = [
    (0, FONT_CANDIDATES, "Regular", 400),
    (1, SEMIBOLD_CANDIDATES, "Bold", 700),
    (2, FONT_CANDIDATES, "Regular", 500),
]
BAKE = 28
PAD = 2
# Text glyphs are stored as an 8-bit signed distance field so they stay crisp and
# legible when scaled down (the UI uses 11-15 px from a 28 px bake). Icons keep
# their pre-antialiased alpha. SDF text is padded by SDF_SPREAD texels per side so
# the ramp is never clamped at the quad edge.
SDF_SPREAD = 3
# Fixed line box so swapping fonts never shifts widget vertical rhythm.
LINE_ASCENT = 30
LINE_DESCENT = 9

# Extra text codepoints beyond ASCII: Latin-1 (Spanish), typography, symbols.
EXTRA_CODEPOINTS = [
    0x00A0, 0x00A1, 0x00B7, 0x00BF, 0x00D7,
    *range(0x00C0, 0x0100),
    0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026,
    0x2190, 0x2191, 0x2192, 0x2193, 0x2212,
]

# (file stem, private-use codepoint, baked pixel size)
ICONS = [
    ("user", 0xE001, 28),
    ("sparkles", 0xE002, 28),
    ("clock", 0xE003, 28),
    ("panels-top-left", 0xE004, 28),
    ("chart-column", 0xE005, 28),
    ("save", 0xE006, 28),
    ("copy", 0xE007, 28),
    ("play", 0xE008, 28),
    ("trash", 0xE009, 28),
    ("chevron-down", 0xE00A, 28),
    ("image-plus", 0xE00B, 28),
    ("settings", 0xE00C, 28),
    ("info", 0xE00D, 28),
    ("plus", 0xE00E, 28),
    ("x", 0xE00F, 28),
    ("minus", 0xE010, 28),
    ("square", 0xE011, 28),
    ("external-link", 0xE012, 28),
    ("undo-2", 0xE013, 28),
    ("redo-2", 0xE014, 28),
    ("folder-open", 0xE015, 28),
    ("scan-face", 0xE016, 28),
    ("check", 0xE017, 28),
    ("image", 0xE018, 28),
    ("clipboard", 0xE019, 28),
    ("mouse-pointer", 0xE01A, 28),
    ("rotate-ccw", 0xE01B, 28),
    ("download", 0xE01C, 28),
    ("upload", 0xE01D, 28),
    ("monitor", 0xE01E, 28),
    ("logo", 0xE01F, 48),
    ("sliders-horizontal", 0xE020, 28),
    ("history", 0xE021, 28),
    ("box", 0xE022, 28),
    ("folder", 0xE023, 28),
    ("photo", 0xE024, 28),
    ("user-filled", 0xE030, 28),
    ("sliders-filled", 0xE031, 28),
    ("history-filled", 0xE032, 28),
    ("box-filled", 0xE033, 28),
    ("folder-filled", 0xE034, 28),
    ("chart-bars-filled", 0xE035, 28),
    ("settings-filled", 0xE036, 28),
    ("scan-face-filled", 0xE037, 28),
    ("image-plus", 0xE100, 96),
    ("scan-face", 0xE101, 96),
    ("image", 0xE102, 96),
    ("scan", 0xE103, 96),
    ("user-round", 0xE104, 96),
    ("play-filled", 0xE105, 28),
    ("image-hero", 0xE106, 96),
    ("scan-person", 0xE107, 96),
]

# Large hero icons use a thinner stroke so they read like the reference when
# drawn at ~90 px instead of the 24 px design grid.
THIN_STROKE_CODEPOINTS = {0xE100, 0xE101, 0xE102, 0xE103, 0xE104, 0xE106, 0xE107}


def to_sdf(img: Image.Image, spread: int) -> Image.Image:
    """Encode an anti-aliased alpha glyph as a padded 8-bit signed distance field.

    128 (0.5) lies on the glyph contour; larger values are inside the glyph and
    smaller values outside. ``spread`` texels of padding are added on every side so
    the distance ramp is never clamped at the quad edge. Rendering then reconstructs
    smooth edges at any output size instead of point-sampling the baked bitmap.
    """
    src = np.asarray(img, dtype=np.float32) / 255.0
    h, w = src.shape
    pad = spread
    canvas = np.zeros((h + pad * 2, w + pad * 2), dtype=np.float32)
    canvas[pad:pad + h, pad:pad + w] = src
    inside = canvas >= 0.5
    dist_out = ndimage.distance_transform_edt(~inside)
    dist_in = ndimage.distance_transform_edt(inside)
    signed = dist_in - dist_out  # > 0 inside the glyph, < 0 outside
    encoded = np.clip(128.0 + signed * (127.0 / spread), 0.0, 255.0).astype(np.uint8)
    return Image.fromarray(encoded, mode="L")


def render_svg(path: Path, size: int, thin: bool = False) -> Image.Image:
    svg = path.read_text(encoding="utf-8").replace("currentColor", "black")
    if thin:
        svg = svg.replace('stroke-width="2"', 'stroke-width="1.4"')
    else:
        # The reference icon set reads noticeably bolder than Lucide's default
        # 2px stroke at UI sizes; thicken outline icons to match.
        svg = svg.replace('stroke-width="2"', 'stroke-width="2.7"')
    with tempfile.TemporaryDirectory() as td:
        sp = Path(td) / "icon.svg"
        pp = Path(td) / "icon.png"
        sp.write_text(svg, encoding="utf-8")
        subprocess.run(
            ["rsvg-convert", "-w", str(size * 3), "-h", str(size * 3), "-o", str(pp), str(sp)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        img = Image.open(pp).convert("RGBA")
    alpha = img.getchannel("A")
    return alpha.resize((size, size), Image.LANCZOS)


def build_glyphs() -> tuple[list[dict], int, int]:
    glyphs: list[dict] = []
    ascent = descent = 0

    codepoints = list(range(0x20, 0x7F)) + EXTRA_CODEPOINTS
    fonts = [
        (weight, next(p for p in candidates if p.exists()), variation, axis)
        for weight, candidates, variation, axis in FONT_ROLES
    ]
    for weight, font_path, variation, axis in fonts:
        font = ImageFont.truetype(str(font_path), BAKE)
        applied = False
        try:
            font.set_variation_by_axes([axis])
            applied = True
        except (OSError, ValueError):
            pass
        if not applied:
            try:
                font.set_variation_by_name(variation)
            except (OSError, ValueError):
                pass
        ascent, descent = font.getmetrics()
        for cp in codepoints:
            ch = chr(cp)
            bbox = font.getbbox(ch, anchor="ls")
            advance = float(font.getlength(ch))
            mask = font.getmask(ch, mode="L")
            w, h = mask.size
            img = None
            ox = oy = 0
            if bbox and w > 0 and h > 0:
                img = Image.frombytes("L", (w, h), bytes(mask))
                ox, oy = int(bbox[0]), int(bbox[1])
                # Text is stored as a padded signed distance field (see to_sdf).
                img = to_sdf(img, SDF_SPREAD)
                ox -= SDF_SPREAD
                oy -= SDF_SPREAD
                w, h = img.size
            glyphs.append(
                dict(codepoint=cp, weight=weight, img=img, x=0, y=0, w=w, h=h,
                     off_x=ox, off_y=oy, bake=BAKE, advance=advance)
            )

    for stem, cp, size in ICONS:
        path = ICONS_DIR / f"{stem}.svg"
        if not path.exists():
            print(f"warning: missing {path}, skipping")
            continue
        img = render_svg(path, size, thin=cp in THIN_STROKE_CODEPOINTS)
        glyphs.append(
            dict(codepoint=cp, weight=0, img=img, x=0, y=0, w=size, h=size,
                 off_x=0, off_y=0, bake=size, advance=0.0)
        )

    return glyphs, LINE_ASCENT, LINE_DESCENT


def pack(glyphs: list[dict], width: int) -> tuple[dict[int, tuple[int, int]], int]:
    todo = [g for g in glyphs if g["img"] is not None]
    todo.sort(key=lambda g: (g["h"], g["w"]), reverse=True)
    x = y = row_h = 0
    pos: dict[int, tuple[int, int]] = {}
    for g in todo:
        if x + g["w"] + PAD > width:
            x = 0
            y += row_h + PAD
            row_h = 0
        pos[id(g)] = (x, y)
        x += g["w"] + PAD
        row_h = max(row_h, g["h"])
    height = y + row_h + PAD
    return pos, height


def main() -> None:
    glyphs, ascent, descent = build_glyphs()

    width = 512
    pos, height = pack(glyphs, width)
    if height > 1024:
        width = 1024
        pos, height = pack(glyphs, width)
    height = 1 << (height - 1).bit_length()  # power of two
    height = max(height, 256)

    atlas = Image.new("L", (width, height), 0)
    for g in glyphs:
        if g["img"] is None:
            continue
        gx, gy = pos[id(g)]
        g["x"], g["y"] = gx, gy
        atlas.paste(g["img"], (gx, gy))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    data = atlas.tobytes()
    glyphs.sort(key=lambda g: (g["codepoint"], g["weight"]))

    hpp = [
        "#pragma once",
        '#include "core/font.hpp"',
        "",
        "// Generated by tools/gen_assets.py. Do not edit by hand.",
        "",
    ]
    (OUT_DIR / "font_atlas.hpp").write_text("\n".join(hpp) + "\n", encoding="utf-8")

    cpp = [
        '#include "assets/font_atlas.hpp"',
        "",
        "// Generated by tools/gen_assets.py. Do not edit by hand.",
        "",
        f"const int kAtlasWidth = {width};",
        f"const int kAtlasHeight = {height};",
        f"const int kFontAscent = {ascent};",
        f"const int kFontDescent = {descent};",
        f"const int kFontBakeSize = {BAKE};",
        "",
        f"const uint8_t kAtlasPixels[{len(data)}] = {{",
    ]
    line: list[str] = []
    for i, b in enumerate(data):
        line.append(f"{b},")
        if len(line) == 32:
            cpp.append("    " + " ".join(line))
            line.clear()
    if line:
        cpp.append("    " + " ".join(line))
    cpp.append("};")
    cpp.append("")
    cpp.append(f"const Glyph kGlyphs[{len(glyphs)}] = {{")
    for g in glyphs:
        cpp.append(
            f"    {{0x{g['codepoint']:04X}u, {g['x']}, {g['y']}, {g['w']}, {g['h']}, "
            f"{g['off_x']}, {g['off_y']}, {g['bake']}, {g['weight']}, {g['advance']:.4f}f}},"
        )
    cpp.append("};")
    cpp.append(f"const int kGlyphCount = {len(glyphs)};")
    cpp.append("")

    (OUT_DIR / "font_atlas.cpp").write_text("\n".join(cpp), encoding="utf-8")
    print(f"atlas {width}x{height}, {len(glyphs)} glyphs, ascent={ascent} descent={descent}")


if __name__ == "__main__":
    main()
