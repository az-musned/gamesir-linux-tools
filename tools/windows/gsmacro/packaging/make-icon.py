# Draws gsmacro's icon (app/gsmacro.ico) from scratch, in the app's own
# colours: a controller (blue = player input) with a lightning bolt (orange =
# macro) on a dark tile. Run:  python packaging/make-icon.py   (needs Pillow)
import os
from PIL import Image, ImageDraw, ImageFilter

S = 1024  # drawn large, then scaled down per icon size
TILE = (32, 37, 46, 255)       # kPadBody
EDGE = (64, 72, 88, 255)       # kPadEdge
PAD = (76, 154, 255, 255)      # kPhys
BOLT = (255, 159, 67, 255)     # kMacro
HOLE = (24, 28, 35, 255)


def draw(simple):
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = 24
    d.rounded_rectangle([m, m, S - m, S - m], radius=200, fill=TILE, outline=EDGE, width=0 if simple else 18)

    # Controller silhouette: body plus two grips.
    body = [150, 300, 874, 640]
    d.rounded_rectangle(body, radius=170, fill=PAD)
    for cx in (290, 734):
        d.ellipse([cx - 150, 430, cx + 150, 790], fill=PAD)
    if not simple:
        # D-pad and face buttons cut into the body.
        d.rounded_rectangle([250, 440, 360, 480], radius=12, fill=HOLE)
        d.rounded_rectangle([285, 405, 325, 515], radius=12, fill=HOLE)
        for x, y in ((720, 410), (720, 510), (670, 460), (770, 460)):
            d.ellipse([x - 26, y - 26, x + 26, y + 26], fill=HOLE)

    # Lightning bolt (the macro), with a dark outline so it reads on the pad.
    bolt = [(560, 150), (380, 560), (505, 560), (440, 880), (660, 430), (530, 430), (610, 150)]
    d.polygon(bolt, fill=HOLE)
    outline = im.copy()
    grown = Image.new("L", (S, S), 0)
    ImageDraw.Draw(grown).polygon(bolt, fill=255)
    grown = grown.filter(ImageFilter.MaxFilter(41 if simple else 33))
    im.paste(Image.new("RGBA", (S, S), TILE), (0, 0), grown)
    d = ImageDraw.Draw(im)
    d.polygon(bolt, fill=BOLT)
    return im


here = os.path.dirname(os.path.abspath(__file__))
app = os.path.join(here, "..", "app")
full, simple = draw(False), draw(True)
sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
frames = [(simple if s <= 32 else full).resize((s, s), Image.LANCZOS) for s in sizes]
frames[-1].save(os.path.join(app, "gsmacro.ico"), format="ICO", sizes=[(s, s) for s in sizes], append_images=frames[:-1])
full.resize((256, 256), Image.LANCZOS).save(os.path.join(here, "..", "docs", "screenshots", "icon.png"))
print("wrote app/gsmacro.ico and docs/screenshots/icon.png")
