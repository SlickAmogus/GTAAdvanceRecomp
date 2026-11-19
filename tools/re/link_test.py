"""Two linked GBAs on this PC through the framework's link-cable netplay.

    python tools/re/link_test.py [frames]

Starts seat 0 and seat 1 of one session over 127.0.0.1 (SDL dummy video and
audio), each with a throwaway save and a scripted pad (both go START, three
times DOWN to MULTI PLAYER, A, A on the game's CONNECT CABLE screen, then A
through LIBERTY CITY SURVIVOR and START GAME), and writes each seat's picture every 60 frames to build/link/.
"""
import os, pathlib, shutil, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
EXE = ROOT / "build" / "GTAAdvanceRecomp.exe"
OUT = ROOT / "build" / "link"
A, START, DOWN = 0x1, 0x8, 0x80

UP, RIGHT, B = 0x40, 0x10, 0x2

def script(seat):
    rows, t = [], 1500
    def press(mask, hold=4, gap=60):
        nonlocal t
        rows.append((t, t + hold - 1, mask)); t += hold + gap
    press(START, gap=120)
    for _ in range(3): press(DOWN)
    press(A, gap=100)               # MULTI PLAYER: the game's link screen
    press(A, gap=400)               # A BUTTON TO CONNECT
    press(A, gap=100)               # LIBERTY CITY SURVIVOR
    press(A, gap=100)               # START GAME
    # Seat 0 stands still; seat 1 walks right for two seconds, then down.
    t += 700
    if seat == 1:
        rows.append((t, t + 120, RIGHT)); rows.append((t + 180, t + 300, 0x80))
    return rows

if __name__ == "__main__":
    frames = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
    shutil.rmtree(OUT, ignore_errors=True); OUT.mkdir(parents=True, exist_ok=True)
    mine = ROOT.parent / "Grand Theft Auto Advance (USA).sav"
    procs = []
    for seat in (0, 1):
        save = OUT / f"seat{seat}.sav"
        save.write_bytes(mine.read_bytes() if mine.exists() else b"")
        sfile = OUT / f"seat{seat}.script"
        sfile.write_text("".join(f"{a} {b} {m:#x}\n" for a, b, m in script(seat)))
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
                   GBARECOMP_DIAGNOSTICS="0", GBARECOMP_NETPLAY_SCRIPT=str(sfile),
                   GBARECOMP_NETPLAY_DUMP=str(OUT / f"seat{seat}"))
        bind, peer = (f"127.0.0.1:{5300 + seat}", f"127.0.0.1:{5301 - seat}")
        argv = [str(EXE), "--save-path", str(save), "--frames", str(frames),
                "--netplay-bind", bind, "--netplay-peer", peer,
                "--netplay-seat", str(seat), "--netplay-session", "4242",
                "--netplay-checkpoint", str(OUT / f"seat{seat}.paired")]
        log = open(OUT / f"seat{seat}.log", "w")
        procs.append((subprocess.Popen(argv, cwd=EXE.parent, env=env, stdout=log, stderr=subprocess.STDOUT), log))
    for p, log in procs:
        try: p.wait(timeout=600)
        except subprocess.TimeoutExpired: p.kill()
        log.close()
    for seat in (0, 1):
        dumps = sorted(OUT.glob(f"seat{seat}_*.ppm"), key=lambda q: int(q.stem.split("_")[1]))
        print(f"seat {seat}: exit {procs[seat][0].returncode}, {len(dumps)} pictures")
        print("   ", (OUT / f"seat{seat}.log").read_text(errors="replace").strip().splitlines()[-3:])
