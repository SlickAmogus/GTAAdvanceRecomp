"""Checks that short key taps reach the game in the city, and that the weapon
switch chord arrives as Select, Select+shoulder, Select, nothing.

    python tools/re/input_check.py [state]

The game reads the pad once per world tick (15 a second); IWRAM 0x0300009C
is what it saw last.
"""
import pathlib, struct, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from drive import Game, ROOT

SELECT, R, L = 4, 0x100, 0x200

def seen(g): return struct.unpack("<I", g.mem(0x0300009C, 4))[0] & 0x3FF

if __name__ == "__main__":
    state = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build" / "gameplay.state")
    ok = True
    with Game() as g:
        g.run(5); g.load(state); g.run(30)
        # One-frame Select taps at every phase of the tick.
        hits = 0
        for phase in range(8):
            g.run(20 + phase)
            g.cmd(cmd="gta_keys", buttons=SELECT, frames=1)
            got = False
            for _ in range(16):
                g.run(1); got |= bool(seen(g) & SELECT)
            hits += got
        print(f"one-frame taps seen by the game: {hits} of 8"); ok &= hits == 8
        # The chord, as the sequence of distinct pad states the game saw.
        for direction, shoulder in ((1, R), (-1, L)):
            g.run(30)
            g.cmd(cmd="gta_cycle", dir=direction)
            states = []
            for _ in range(60):
                g.run(1); s = seen(g) & (SELECT | R | L)
                if not states or states[-1] != s: states.append(s)
            want = [0, SELECT, SELECT | shoulder, SELECT, 0]
            if states and states[0] != 0: states.insert(0, 0)
            print(f"chord dir {direction:+d}: game saw {[hex(s) for s in states]}"); ok &= states == want
    print("PASS" if ok else "FAIL")
