"""Headless, muted driver for GTAAdvanceRecomp.exe over the debug TCP server.

    from drive import Game
    with Game() as g:
        g.run(600)              # advance frames with no keys
        g.tap("A")              # press + release
        g.shot("out.png")
"""
import json, os, socket, subprocess, time, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "GTAAdvanceRecomp.exe"
KEYS = {"A": 0, "B": 1, "SELECT": 2, "START": 3, "RIGHT": 4, "LEFT": 5,
        "UP": 6, "DOWN": 7, "R": 8, "L": 9}


def mask(*names):
    m = 0x3FF                      # KEYINPUT is active-low
    for n in names:
        m &= ~(1 << KEYS[n.upper()])
    return m


class Game:
    def __init__(self, port=19851, extra=(), env=None):
        e = dict(os.environ, GBARECOMP_DIAGNOSTICS="0")
        e.update(env or {})
        self.log = open(ROOT / "build" / ("drive.log" if port == 19851 else f"drive_{port}.log"), "w")
        # Never the player's own save: every scripted run gets a throwaway
        # copy (a script that walks into SAVE GAME would overwrite theirs).
        self.save_file = ROOT / "build" / f"drive_{port}.sav"
        if "--save" not in extra and "--save-path" not in extra:
            mine = ROOT.parent / "Grand Theft Auto Advance (USA).sav"
            self.save_file.write_bytes(mine.read_bytes() if mine.exists() else b"")
            extra = (*extra, "--save-path", str(self.save_file))
        self.p = subprocess.Popen(
            [str(EXE), "--no-window", "--volume", "0", "--uncapped",
             "--tcp", str(port), *extra],
            cwd=EXE.parent, env=e, stdout=self.log, stderr=subprocess.STDOUT)
        for _ in range(100):
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=120)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise RuntimeError("debug server did not come up")
        self.f = self.s.makefile("rwb")

    def cmd(self, **kw):
        self.f.write((json.dumps(kw) + "\n").encode())
        self.f.flush()
        line = self.f.readline()
        if not line:
            raise RuntimeError("game exited (see build/drive.log, build/crash.log)")
        return json.loads(line)

    def run(self, n, *keys):
        return self.cmd(cmd="run_frames", n=n, keyinput=mask(*keys))

    def tap(self, key, hold=6, after=30):
        self.run(hold, key)
        return self.run(after)

    def shot(self, path):
        from PIL import Image
        r = self.cmd(cmd="screenshot")
        Image.frombytes("RGB", (r["w"], r["h"]), bytes.fromhex(r["data"])).save(path)

    def mem(self, addr, length):
        """Read RAM: 0x02xxxxxx (EWRAM) or 0x03xxxxxx (IWRAM)."""
        cmd = "read_ewram" if (addr >> 24) == 2 else "read_iwram"
        return bytes.fromhex(self.cmd(cmd=cmd, addr=addr, len=length)["data"])

    def poke(self, addr, data):
        return self.cmd(cmd="write_mem", addr=addr, hex=bytes(data).hex())

    def save(self, path):
        return self.cmd(cmd="savestate_save", path=str(pathlib.Path(path).resolve()))

    def load(self, path):
        return self.cmd(cmd="savestate_load", path=str(pathlib.Path(path).resolve()))

    def __enter__(self):
        return self

    def __exit__(self, *a):
        try:
            self.cmd(cmd="quit")
        except Exception:
            pass
        try:
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()
        self.log.close()
