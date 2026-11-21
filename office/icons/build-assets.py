#!/usr/bin/env python3
"""Build every raster/container icon file used by the Moonlight Desk client and
Sunshine Desk host from the six SVG sources next to this script.

Usage: python3 build-assets.py <output dir>
Writes <out>/client/... and <out>/server/... with the repository-relative paths.
Needs macOS (iconutil), ImageMagick, and Pillow. Rendering reuses render-previews.py.
"""
import importlib.util, io, shutil, struct, subprocess, sys, tempfile
from pathlib import Path
from PIL import Image

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("rp", HERE / "render-previews.py")
rp = importlib.util.module_from_spec(spec); spec.loader.exec_module(rp)

OUT = Path(sys.argv[1]).resolve()
WORK = Path(tempfile.mkdtemp(prefix="icons-"))
_masters = {}

def master(name):
    if name not in _masters:
        scale = 8 if name.startswith("tray-") else 2
        p = WORK / f"{name}-master.png"
        rp.render_svg(HERE / f"{name}.svg", p, scale, WORK)
        _masters[name] = Image.open(p).convert("RGBA")
    return _masters[name]

def sized(name, size):
    im = master(name)
    return im if im.width == size else im.resize((size, size), Image.LANCZOS)

def png_bytes(im):
    b = io.BytesIO(); im.save(b, "PNG"); return b.getvalue()

def save_png(name, size, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    sized(name, size).save(path, "PNG")

def bmp_entry(im):
    w, h = im.size
    px = im.tobytes("raw", "BGRA")
    rows = [px[y*w*4:(y+1)*w*4] for y in range(h)][::-1]
    andmask = b"\0" * (((w + 31)//32*4) * h)
    return struct.pack("<IiiHHIIiiII", 40, w, h*2, 1, 32, 0, len(px)+len(andmask), 0, 0, 0, 0) + b"".join(rows) + andmask

def save_ico(name, sizes, path, bmp_small):
    path.parent.mkdir(parents=True, exist_ok=True)
    blobs = []
    for s in sizes:
        im = sized(name, s)
        blobs.append((s, png_bytes(im) if (s >= 256 or not bmp_small) else bmp_entry(im)))
    hdr = struct.pack("<HHH", 0, 1, len(blobs)); off = 6 + 16*len(blobs); ent = b""; body = b""
    for s, b in blobs:
        ent += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(b), off + len(body)); body += b
    path.write_bytes(hdr + ent + body)

def save_icns(name, path):
    iconset = WORK / "x.iconset"; shutil.rmtree(iconset, ignore_errors=True); iconset.mkdir()
    for base in (16, 32, 128, 256, 512):
        sized(name, base).save(iconset / f"icon_{base}x{base}.png")
        sized(name, base*2).save(iconset / f"icon_{base}x{base}@2x.png")
    path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(path)], check=True)

# ---- client ----
c = OUT / "client"
save_ico("client-app", [256,192,128,96,64,48,40,32,24,16], c/"app/moonlight.ico", True)
save_icns("client-app", c/"app/moonlight.icns")
save_png("client-app", 64, c/"app/moonlight_wix.png")
save_png("client-app", 116, c/"app/deploy/steamlink/moonlight.png")

# ---- server ----
s = OUT / "server"; img = s/"src_assets/common/assets/web/public/images"
save_ico("host-app", [256,128,64,48,32,24,16], s/"sunshine.ico", True)
save_png("host-app", 256, s/"sunshine.png")
save_icns("host-app", s/"src_assets/macos/build/sunshine.icns")
web_sizes = [16,20,24,28,32,36,40,44,48,52,56,60,64,80,96,112,128,144,160,176,192,208,224,240,256]
save_ico("host-app", web_sizes, img/"sunshine.ico", False)
save_png("host-app", 16, img/"logo-sunshine-16.png")
save_png("host-app", 45, img/"logo-sunshine-45.png")
for st in ("locked", "pausing", "playing"):
    t = f"tray-{st}"
    save_ico(t, web_sizes, img/f"sunshine-{st}.ico", False)
    save_png(t, 256, img/f"sunshine-{st}.png")
    save_png(t, 16, img/f"sunshine-{st}-16.png")
    save_png(t, 45, img/f"sunshine-{st}-45.png")
print("done", OUT)
