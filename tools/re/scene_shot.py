"""Native composite beside the game's own picture, from a savestate.

    python tools/re/scene_shot.py [state] [w h] [out.png]
"""
import pathlib, sys, time
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from drive import Game, ROOT
from PIL import Image

def shot(g, w, h):
    t = time.time()
    r = g.cmd(cmd="gta_scene_shot", w=w, h=h)
    if not r.get("ok"): raise RuntimeError(r)
    return Image.open(ROOT / "build" / "scene_shot.ppm").copy(), r["ms"] / 1000

if __name__ == "__main__":
    state = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build" / "gameplay.state")
    w, h = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (1280, 540)
    out = sys.argv[4] if len(sys.argv) > 4 else str(ROOT / "build" / "scene_cmp.png")
    with Game() as g:
        g.run(5); g.load(state); g.run(10)
        native, secs = shot(g, w, h)
        g.shot(str(ROOT / "build" / "t.png"))
    game = Image.open(ROOT / "build" / "t.png").resize((h * 3 // 2, h), Image.NEAREST)
    sheet = Image.new("RGB", (native.width + game.width + 8, h))
    sheet.paste(native, (0, 0)); sheet.paste(game, (native.width + 8, 0)); sheet.save(out)
    print(f"{w}x{h} composite in {secs*1000:.0f} ms -> {out}")
