"""Widescreen check: walk around with the picture widened and stack the
native composites, to see people and cars in the side margins.

    python tools/re/wide_walk.py [state] [out.png]
"""
import pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from drive import Game, ROOT
from PIL import Image, ImageDraw

W, H = 1720, 720
STEPS = [("RIGHT",), ("RIGHT",), ("DOWN",), ("DOWN",), ("LEFT",), ("DOWN",)]

if __name__ == "__main__":
    state = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build" / "gameplay.state")
    out = sys.argv[2] if len(sys.argv) > 2 else str(ROOT / "build" / "wide_walk.png")
    rows = []
    with Game() as g:
        g.run(5); g.load(state); g.run(10)
        g.cmd(cmd="gta_scene_shot", w=W, h=H)          # sets the widening
        for keys in STEPS:
            g.run(120, *keys, "B")
            r = g.cmd(cmd="gta_scene_shot", w=W, h=H)
            if not r.get("ok"): continue
            im = Image.open(ROOT / "build" / "scene_shot.ppm").copy()
            d = ImageDraw.Draw(im)                     # the game's own 240 columns
            box = (W - H * 3 // 2) // 2
            d.line([(box, 0), (box, H)], fill=(255, 0, 255)); d.line([(W - box, 0), (W - box, H)], fill=(255, 0, 255))
            rows.append(im)
        rect = [int.from_bytes(g.mem(0x03000050 + k * 4, 4), "little", signed=True) for k in range(4)]
        print("visible rectangle", rect)
    sheet = Image.new("RGB", (W, (H + 4) * len(rows)))
    for i, row in enumerate(rows): sheet.paste(row, (0, i * (H + 4)))
    sheet.save(out); print(out, sheet.size)
