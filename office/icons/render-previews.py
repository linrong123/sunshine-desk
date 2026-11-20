#!/usr/bin/env python3
"""Render the hand-written SVGs with ImageMagick, then compose a proof sheet.

This Mac's ImageMagick 7.1.1-47 MSVG reader drops gradients and stroke opacity.
Translate this deliberately small SVG vocabulary to equivalent MVG primitives
and let ImageMagick rasterize those directly. SVG remains the only geometry
source; no browser, raster source, font, or third-party Python module is needed
to render an icon. Fonts are used only for the contact-sheet labels.
"""

from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET

OUT = Path(__file__).resolve().parent
MAGICK = "/opt/homebrew/bin/magick"
FONT = "/System/Library/Fonts/Supplemental/Arial.ttf"
NAMES = ["client-app", "host-app", "tray-normal", "tray-playing",
         "tray-pausing", "tray-locked"]
LABELS = ["Desk Client", "Desk Host", "Host / normal", "Host / playing",
          "Host / pausing", "Host / locked"]
NS = {"s": "http://www.w3.org/2000/svg"}


def magick(*args):
    subprocess.run([MAGICK, *map(str, args)], check=True)


def drawing(shape, scale, **overrides):
    """Translate one SVG shape to ImageMagick's equivalent vector primitive."""
    kind = shape.tag.split("}")[-1]
    a = dict(shape.attrib, **overrides)
    lines = ["push graphic-context", f"scale {scale},{scale}",
             "fill-opacity 1", "stroke-opacity 1", "fill black", "stroke none"]
    for key in ("fill", "stroke", "stroke-width", "fill-rule",
                "stroke-linejoin", "stroke-linecap"):
        if key in a:
            lines.append(f"{key} '{a[key]}'")
    if kind == "path":
        lines.append("path '" + " ".join(a["d"].split()) + "'")
    elif kind == "rect":
        x, y, w, h = [float(a[k]) for k in ("x", "y", "width", "height")]
        if "rx" in a:
            lines.append(f"roundrectangle {x},{y} {x+w},{y+h} "
                         f"{a['rx']},{a.get('ry', a['rx'])}")
        else:
            lines.append(f"rectangle {x},{y} {x+w},{y+h}")
    elif kind == "circle":
        x, y, r = [float(a[k]) for k in ("cx", "cy", "r")]
        lines.append(f"circle {x},{y} {x+r},{y}")
    else:
        raise ValueError(f"Unsupported SVG element: {kind}")
    lines.append("pop graphic-context")
    return "\n".join(lines)


def render_svg(source, target, scale, work):
    """Rasterize shape geometry and globally aligned gradients with magick.

    ImageMagick's gradient:vector preserves SVG's diagonal, user-space gradient.
    Temporary shape masks are render intermediates, never embedded in SVGs.
    """
    root = ET.parse(source).getroot()
    canvas = int(root.get("width")) * scale
    extent = f"{canvas}x{canvas}"
    gradients = {}
    for g in root.findall("s:defs/s:linearGradient", NS):
        assert g.get("gradientUnits") == "userSpaceOnUse"
        stops = list(g)
        assert len(stops) == 2 and stops[0].get("offset") in ("0", "0%")
        assert stops[1].get("offset") in ("1", "100%")
        vector = ",".join(str(float(g.get(k))*scale) for k in ("x1", "y1", "x2", "y2"))
        gradient = work / f"{source.stem}-{g.get('id')}.png"
        magick("-size", extent, "-define", f"gradient:vector={vector}",
               f"gradient:{stops[0].get('stop-color')}-{stops[1].get('stop-color')}", gradient)
        gradients[f"url(#{g.get('id')})"] = gradient
    magick("-size", extent, "xc:none", "-colorspace", "sRGB", "-type", "TrueColorAlpha", f"PNG32:{target}")
    mask, layer = work / "mask.png", work / "layer.png"
    for shape in root:
        if shape.tag.split("}")[-1] == "defs":
            continue
        gradient_fill = shape.get("fill") in gradients
        gradient_stroke = shape.get("stroke") in gradients
        if not (gradient_fill or gradient_stroke):
            magick(target, "-draw", drawing(shape, scale), target)
            continue
        # SVG's default paint order is fill followed by stroke.
        for prop in ("fill", "stroke"):
            value = shape.get(prop, "black" if prop == "fill" else "none")
            if value == "none":
                continue
            overrides = {"fill": "none", "stroke": "none", prop: value}
            if value in gradients:
                overrides[prop] = "white"
                magick("-size", extent, "xc:none", "-draw",
                       drawing(shape, scale, **overrides), mask)
                magick(gradients[value], mask, "-compose", "CopyOpacity", "-composite", layer)
                magick(target, "-colorspace", "sRGB", layer, "-compose", "Over", "-composite", target)
            else:
                magick(target, "-draw", drawing(shape, scale, **overrides), target)


def main():
    preview = OUT / "preview"
    preview.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="desk-icons-") as scratch:
        work = Path(scratch)
        for name in NAMES:
            tray = name.startswith("tray-")
            scale, canvas = (8, 64) if tray else (2, 1024)
            master = work / f"{name}-master.png"
            render_svg(OUT / f"{name}.svg", master, scale, work)
            sizes = (64, 32, 16) if tray else (1024, 256, 64, 32, 16)
            for size in sizes:
                magick(master, "-filter", "Lanczos", "-resize", f"{size}x{size}",
                       "-depth", "8", "-strip", f"PNG32:{preview / f'{name}-{size}.png'}")
            # Full vector render for the tray's 256 px inspection row.
            magick(master, "-resize", "256x256", "-depth", "8", "-strip",
                   f"PNG32:{work / f'{name}-256.png'}")

        panels = []
        for theme, bg, fg, secondary in (
            ("light", "#F3F5F7", "#202C36", "#65717D"),
            ("dark", "#18212B", "#F3F5F7", "#A9B6C3"),
        ):
            panel = work / f"{theme}.png"
            args = ["-size", "1792x560", f"xc:{bg}", "-font", FONT,
                    "-fill", fg, "-pointsize", "26", "-gravity", "NorthWest",
                    "-annotate", "+32+24", f"DESK  /  {theme.upper()} BACKGROUND"]
            for i, (name, label) in enumerate(zip(NAMES, LABELS)):
                center = 160 + i * 288
                args += ["-fill", fg, "-pointsize", "17", "-gravity", "NorthWest",
                         "-annotate", f"+{center-70}+76", label,
                         work / f"{name}-256.png", "-geometry", f"+{center-128}+104",
                         "-compose", "Over", "-composite",
                         preview / f"{name}-32.png", "-geometry", f"+{center-16}+404",
                         "-compose", "Over", "-composite"]
                if name.startswith("tray-"):
                    args += [preview / f"{name}-16.png", "-geometry", f"+{center-8}+488",
                             "-compose", "Over", "-composite"]
            args += ["-fill", secondary, "-pointsize", "13", "-gravity", "NorthWest",
                     "-annotate", "+32+370", "256 px  /  vector detail",
                     "-annotate", "+32+448", "32 px  /  actual pixels",
                     "-annotate", "+32+514", "16 px  /  tray icons, actual pixels",
                     "-depth", "8", "-strip", panel]
            magick(*args)
            panels.append(panel)
        magick(*panels, "-append", "-strip", OUT / "sheet.png")
    print("Rendered 22 transparent previews and out/sheet.png with ImageMagick.")


if __name__ == "__main__":
    main()
