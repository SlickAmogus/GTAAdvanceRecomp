"""Function seeds for the Ghidra decompile (tools/re/GtaDecompile.java).

Real entry points only: Thumb BL targets in the code region, the seeds from
recomp.toml / recomp_scan.toml, and ARM BL targets inside the routines the
game copies to IWRAM. Also writes the code part of the ROM on its own, so
Ghidra's analysis never walks the 15 MiB of assets.

    python tools/re/make_seeds.py <out seeds.txt> <out code.bin>
"""
import pathlib, re, struct, sys
root = pathlib.Path(__file__).resolve().parents[2]
rom = (root.parent / "Grand Theft Auto Advance (USA).gba").read_bytes()
LO, HI = 0xC0, 0x71C00
h = lambda o: struct.unpack_from("<H", rom, o)[0]
seeds = {}
for o in range(0x220, 0x67000 - 2, 2):
    a, b = h(o), h(o + 2)
    if (a & 0xF800) == 0xF000 and (b & 0xF800) == 0xF800:
        off = ((a & 0x7FF) << 12) | ((b & 0x7FF) << 1)
        if off & 0x400000: off -= 0x800000
        t = o + 4 + off
        if LO <= t < HI: seeds[0x08000000 + t] = 1
copies = []
for name in ("recomp.toml", "recomp_scan.toml"):
    text = (root / name).read_text(encoding="utf-8")
    for a, m in re.findall(r'addr = 0x([0-9A-Fa-f]{8})\nmode = "(\w+)"', text):
        seeds.setdefault(int(a, 16), 1 if m == "thumb" else 0)
    for m in re.finditer(r"runtime_start = (0x\w+)\s+source_start\s+= (0x\w+)\s+size\s+= (0x\w+)\s+name\s+= \"(\w+)\"", text):
        copies.append((int(m[1], 16), int(m[2], 16), int(m[3], 16), m[4]))
for rt, src, size, name in copies:
    for o in range(0, size & ~3, 4):
        w = struct.unpack_from("<I", rom, src - 0x08000000 + o)[0]
        if (w & 0x0F000000) == 0x0B000000 and (w >> 28) == 0xE:
            off = w & 0xFFFFFF
            if off & 0x800000: off -= 0x1000000
            t = rt + o + 8 + off * 4
            if rt <= t < rt + size: seeds.setdefault(t, 0)
# The title-screen mixer placement overlaps the gameplay renderer; keep gameplay.
lines = ["%08X %d" % s for s in sorted(seeds.items())]
lines += ["COPY %08X %08X %X" % c[:3] for c in copies if not c[3].endswith("_title")]
pathlib.Path(sys.argv[1]).write_text("\n".join(lines))
pathlib.Path(sys.argv[2]).write_bytes(rom[:0x72000])
print(len(seeds), "seeds")
