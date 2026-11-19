"""Camera shown on each display frame while running, with smooth motion on
and off: the step between consecutive frames should be even when it is on.

    python tools/re/smoothness.py [state]
"""
import pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from drive import Game, ROOT

if __name__ == "__main__":
    state = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build" / "gameplay.state")
    for smooth in (1, 0):
        with Game() as g:
            g.run(5); g.load(state); g.run(10)
            g.cmd(cmd="gta_scene_shot", w=480, h=320, smooth=smooth)
            g.run(40, "DOWN", "B")
            ys = []
            for _ in range(48):
                g.run(1, "DOWN", "B")
                r = g.cmd(cmd="gta_scene_shot", w=480, h=320)
                ys.append(r["cam_y"])
            steps = [b - a for a, b in zip(ys, ys[1:])]
            print(f"smooth={smooth}: steps per display frame:",
                  " ".join(f"{s:.1f}" for s in steps))
