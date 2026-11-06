#!/usr/bin/env python3
"""
Generates VaCuus's logo SVGs from the site's own Archivo font (VaCuus-gmv.1).

    pip install fonttools brotli uharfbuzz      # once, in any venv
    python3 Tools/gen_brand_svg.py              # rewrites docs/marketing/brand/*.svg

WHY OUTLINED AND NOT <text>. The wordmark has always been live text -- the site hero
(.vc-wordmark, site/.vitepress/theme/style.css), og.jpg and the Fab art all set "VaCuus" in
Archivo 800 at font-stretch 116% with the C in amber. An SVG that says font-family="Archivo"
renders in whatever sans the viewer happens to have: an <img>-loaded SVG cannot fetch a web
font, and GitHub's README is exactly that. So the glyphs are shaped once here, with the same
settings the CSS uses, and written out as paths. The file then looks the same everywhere and
the font is no longer needed to draw it.

SHAPED, NOT JUST LOOKED UP. HarfBuzz applies the font's own kerning, which the browser does
too (font-kerning: auto), so the outlines match the live wordmark rather than a naive glyph
row. letter-spacing -0.035em is added after every glyph but the last, as CSS does.

WHAT IT EMITS, all under docs/marketing/brand/ (README.md links them by relative path --
docs/marketing/README.md says why, and what that costs the packaged copy):

    vacuus-wordmark-dark.svg    for dark backgrounds   (ink #e7edf5, C #ffb43c)
    vacuus-wordmark-light.svg   for light backgrounds  (ink #14181d, C #d98c14)
    vacuus-mark.svg             the square app mark: "Vc" on the dark tile the favicons use

The colours are the site's --vc-* tokens; the light C is --vc-accent-bright, not the darker
--vc-accent the site uses for body text on paper -- a logo is large type, and the darker
amber reads as brown at that size.

The font is OFL (site/public/fonts/OFL-NOTICE.txt); outlines drawn with it are artwork, not
a redistribution of the font.
"""

import io
import pathlib

import uharfbuzz as hb
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
FONT = ROOT / "site/public/fonts/archivo-latin.woff2"
OUT = ROOT / "docs/marketing/brand"

# The CSS the wordmark is set with: .vc-wordmark in site/.vitepress/theme/style.css.
WGHT = 800
WDTH = 116
LETTER_SPACING_EM = -0.035

# The square mark is set narrower and heavier than the wordmark, to match the "Vc" of the
# favicons it inherits from: at wdth 116 the pair is ~2:1 and sits in the tile like a bar;
# wght 900 / wdth 75 lands the favicon's 1.39:1 (291x209 px of ink in 512, measured).
MARK_WGHT = 900
MARK_WDTH = 75

TOKENS = {
    "dark": {"ink": "#e7edf5", "accent": "#ffb43c"},
    "light": {"ink": "#14181d", "accent": "#d98c14"},
}


def load_font(wght=WGHT, wdth=WDTH):
    # HarfBuzz reads TrueType, not WOFF2; fontTools unwraps the container in memory.
    tt = TTFont(FONT)
    tt.flavor = None
    buf = io.BytesIO()
    tt.save(buf)
    face = hb.Face(buf.getvalue())
    font = hb.Font(face)
    font.set_variations({"wght": wght, "wdth": wdth})
    return font, face.upem


def shape_runs(font, upem, runs, size):
    """runs: [(text, colour_key)]. Returns [(colour_key, svg_path_d)] plus the ink bounds,
    in pixels at `size`, y down, baseline at y=0."""
    text = "".join(t for t, _ in runs)
    buf = hb.Buffer()
    buf.add_str(text)
    buf.guess_segment_properties()
    hb.shape(font, buf, {"kern": True, "liga": True})

    # Map each cluster (a character index) back to the run it came from.
    owner = []
    for t, key in runs:
        owner += [key] * len(t)

    scale = size / upem
    spacing = LETTER_SPACING_EM * upem
    pens = {}
    bounds = BoundsPen(None)
    x = 0.0
    infos, positions = buf.glyph_infos, buf.glyph_positions
    for i, (info, pos) in enumerate(zip(infos, positions)):
        key = owner[info.cluster]
        pen = pens.setdefault(key, SVGPathPen(None, ntos=lambda v: f"{v:.2f}".rstrip("0").rstrip(".")))
        # Font units -> pixels, flipping y so the SVG baseline sits at 0 with y growing down.
        xform = (scale, 0, 0, -scale, (x + pos.x_offset) * scale, -pos.y_offset * scale)
        font.draw_glyph_with_pen(info.codepoint, TransformPen(pen, xform))
        font.draw_glyph_with_pen(info.codepoint, TransformPen(bounds, xform))
        x += pos.x_advance + (spacing if i < len(infos) - 1 else 0)
    return [(k, p.getCommands()) for k, p in pens.items()], bounds.bounds


def wordmark(font, upem, theme):
    colours = TOKENS[theme]
    paths, (x0, y0, x1, y1) = shape_runs(
        font, upem, [("Va", "ink"), ("C", "accent"), ("uus", "ink")], size=200
    )
    pad = 4
    vb = f"{x0 - pad:.1f} {y0 - pad:.1f} {x1 - x0 + 2 * pad:.1f} {y1 - y0 + 2 * pad:.1f}"
    body = "\n".join(f'  <path fill="{colours[k]}" d="{d}"/>' for k, d in paths)
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{vb}" '
        f'width="{x1 - x0 + 2 * pad:.0f}" height="{y1 - y0 + 2 * pad:.0f}" role="img" aria-label="VaCuus">\n'
        f"  <!-- Generated by Tools/gen_brand_svg.py from Archivo wght {WGHT} wdth {WDTH}. Do not hand-edit. -->\n"
        f"{body}\n</svg>\n"
    )


def mark():
    # Geometry measured off site/public/favicon-512.png, which this mark replaces as the
    # source: corner radius ~17% of the side, "Vc" ink spanning x 113..404 of 512 and
    # centred vertically, amber glow in the top-left corner on a near-black tile.
    side = 512
    font, upem = load_font(MARK_WGHT, MARK_WDTH)
    paths, (x0, y0, x1, y1) = shape_runs(font, upem, [("V", "ink"), ("c", "accent")], size=300)
    target_w = 291
    s = target_w / (x1 - x0)
    tx = (side - target_w) / 2 - x0 * s
    ty = side / 2 - (y0 + y1) / 2 * s
    colours = TOKENS["dark"]
    body = "\n".join(
        f'    <path fill="{colours[k]}" d="{d}"/>' for k, d in paths
    )
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {side} {side}" width="{side}" height="{side}" role="img" aria-label="VaCuus">
  <!-- Generated by Tools/gen_brand_svg.py from Archivo wght {MARK_WGHT} wdth {MARK_WDTH}. Do not hand-edit. -->
  <defs>
    <linearGradient id="tile" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#14120f"/>
      <stop offset="1" stop-color="#080a0d"/>
    </linearGradient>
    <radialGradient id="glow" cx="0.18" cy="0.12" r="0.75">
      <stop offset="0" stop-color="#ffb43c" stop-opacity="0.16"/>
      <stop offset="1" stop-color="#ffb43c" stop-opacity="0"/>
    </radialGradient>
    <clipPath id="clip"><rect width="{side}" height="{side}" rx="88"/></clipPath>
  </defs>
  <g clip-path="url(#clip)">
    <rect width="{side}" height="{side}" fill="url(#tile)"/>
    <rect width="{side}" height="{side}" fill="url(#glow)"/>
  </g>
  <g transform="translate({tx:.2f} {ty:.2f}) scale({s:.5f})">
{body}
  </g>
</svg>
"""


def main():
    font, upem = load_font()
    OUT.mkdir(parents=True, exist_ok=True)
    for theme in ("dark", "light"):
        (OUT / f"vacuus-wordmark-{theme}.svg").write_text(wordmark(font, upem, theme))
    (OUT / "vacuus-mark.svg").write_text(mark())
    for p in sorted(OUT.glob("*.svg")):
        print(f"{p.relative_to(ROOT)}  {p.stat().st_size} bytes")


if __name__ == "__main__":
    main()
