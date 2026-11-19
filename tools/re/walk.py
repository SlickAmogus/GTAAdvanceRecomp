"""Walk around from a savestate and stack native-vs-game picture pairs.

    python tools/re/walk.py [state] [out.png]
"""
import pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from drive import Game, ROOT
from PIL import Image

STEPS = [("UP",), ("LEFT",), ("LEFT", "UP"), ("RIGHT",), ("RIGHT",), ("DOWN", "RIGHT"), ("DOWN",), ("DOWN",)]

if __name__ == "__main__":
    state = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build" / "gameplay.state")
    out = sys.argv[2] if len(sys.argv) > 2 else str(ROOT / "build" / "walk.png")
    rows = []
    with Game() as g:
        g.run(5); g.load(state); g.run(10)
        for keys in STEPS:
            g.run(110, *keys, "B"); g.run(4)
            r = g.cmd(cmd="gta_scene_shot", w=960, h=540)
            g.shot(str(ROOT / "build" / "t.png"))
            row = Image.new("RGB", (960 + 810 + 8, 540), (60, 0, 60))
            if r.get("ok"): row.paste(Image.open(ROOT / "build" / "scene_shot.ppm"), (0, 0))
            row.paste(Image.open(ROOT / "build" / "t.png").resize((810, 540), Image.NEAREST), (968, 0))
            rows.append(row)
    sheet = Image.new("RGB", (rows[0].width, 548 * len(rows)))
    for i, row in enumerate(rows): sheet.paste(row, (0, i * 548))
    sheet.save(out); print(out, sheet.size)
