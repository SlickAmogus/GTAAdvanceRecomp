"""Scripted check of mouse pointing in the game's menus.

    python tools/re/menu_mouse.py

From the main menu: reads the detected list, points at each entry and checks
the game's highlight follows, clicks "new game", then right-clicks back.
"""
import pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from drive import Game, ROOT

def menu(g): return g.cmd(cmd="gta_menu")
def point(g, x, y, l=0, r=0, frames=70):
    g.cmd(cmd="gta_pointer", x=x, y=y, l=l, r=r); g.run(frames); return menu(g)

if __name__ == "__main__":
    ok = True
    with Game() as g:
        g.run(1500); g.tap("START", after=120)
        m = menu(g); print("main menu:", m)
        lines = m["lines"]
        for want in (2, 0, 1, 2, 0):
            l = lines[want]
            got = point(g, (l[0] + l[2]) // 2, (l[1] + l[3]) // 2)["selected"]
            print(f"  point at entry {want}: game selected {got}"); ok &= got == want
        l = lines[0]
        m2 = point(g, (l[0] + l[2]) // 2, (l[1] + l[3]) // 2, l=1, frames=120)
        g.shot(str(ROOT / "build" / "menu_click.png"))
        print("after clicking entry 0:", m2)
        m3 = point(g, 5, 5, r=1, frames=120)
        g.shot(str(ROOT / "build" / "menu_back.png"))
        print("after right click:", m3)
    print("PASS" if ok else "FAIL")
