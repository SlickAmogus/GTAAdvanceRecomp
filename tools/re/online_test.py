"""Two copies on this PC playing through an online room.

    python tools/re/online_test.py SERVER [frames]

SERVER is a room server ("127.0.0.1:27999" for a local sh_master). Seat 0
opens a room (--online-host); once its log names the room code, seat 1 joins
it (--online-join). Both then run the same scripted pad as link_test.py and
write pictures and memory to build/online/.
"""
import os, pathlib, re, shutil, subprocess, sys, time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from link_test import EXE, ROOT, script

OUT = ROOT / "build" / "online"

if __name__ == "__main__":
    server = sys.argv[1]
    frames = int(sys.argv[2]) if len(sys.argv) > 2 else 3800
    shutil.rmtree(OUT, ignore_errors=True); OUT.mkdir(parents=True, exist_ok=True)
    mine = ROOT.parent / "Grand Theft Auto Advance (USA).sav"
    procs, code = [], None
    for seat in (0, 1):
        save = OUT / f"seat{seat}.sav"
        save.write_bytes(mine.read_bytes() if mine.exists() else b"")
        sfile = OUT / f"seat{seat}.script"
        sfile.write_text("".join(f"{a} {b} {m:#x}\n" for a, b, m in script(seat)))
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
                   GBARECOMP_DIAGNOSTICS="0", GBARECOMP_NETPLAY_SCRIPT=str(sfile),
                   GBARECOMP_NETPLAY_DUMP=str(OUT / f"seat{seat}"))
        room = ["--online-host"] if seat == 0 else ["--online-join", code]
        argv = [str(EXE), "--save-path", str(save), "--frames", str(frames),
                "--online-server", server, "--online-name", f"Tester{seat}", *room,
                "--netplay-checkpoint", str(OUT / f"seat{seat}.paired")]
        logpath = OUT / f"seat{seat}.log"
        log = open(logpath, "w")
        procs.append((subprocess.Popen(argv, cwd=EXE.parent, env=env, stdout=log, stderr=subprocess.STDOUT), log))
        if seat == 0:
            for _ in range(200):
                m = re.search(r"in room (\d+)", logpath.read_text(errors="replace"))
                if m: code = m.group(1); break
                time.sleep(0.1)
            else:
                print("host never got a room:", logpath.read_text(errors="replace")[-500:]); sys.exit(1)
            print("room", code)
    for p, log in procs:
        try: p.wait(timeout=900)
        except subprocess.TimeoutExpired: p.kill()
        log.close()
    for seat in (0, 1):
        dumps = list(OUT.glob(f"seat{seat}_*.ppm"))
        print(f"seat {seat}: exit {procs[seat][0].returncode}, {len(dumps)} pictures")
        lines = (OUT / f"seat{seat}.log").read_text(errors="replace").strip().splitlines()
        print("   ", [l for l in lines if l.startswith("online:")], lines[-2:])
