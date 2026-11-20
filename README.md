# GTAAdvanceRecomp

A native PC build of **Grand Theft Auto Advance** (GBA, USA, `BGTE`) made by
statically recompiling the cartridge with
[GBARecomp](https://github.com/mstan/gbarecomp), plus a native renderer that
draws the city at the window's own resolution and shape (16:9, 21:9, ...), a PC
keyboard/mouse layout and an in-game options menu.

No game data is in this folder. You need your own cartridge dump and your own
GBA BIOS dump.

| Input | SHA-1 |
|---|---|
| Grand Theft Auto Advance (USA) ROM, 16 MiB | `742fc0062ad930253ffea449953061381577649e` |
| GBA BIOS, 16 KiB | `300c20df6731a33952ded8c436f7f186d25d3492` |

## Layout

The build expects the ROM and two checkouts beside this folder:

```
GTAAdvance/
  Grand Theft Auto Advance (USA).gba   your ROM
  gbarecomp/                           github.com/mstan/gbarecomp  @ 780ff64
    bios/gba_bios.bin                  your BIOS
  recomp-ui/                           github.com/mstan/recomp-ui  @ ab72f8d
  GTAAdvanceRecomp/                    this repository
```

The simplest way to get a framework checkout with everything applied is the
fork that carries these changes:

```
git clone --recurse-submodules -b pc-ports https://github.com/SlickAmogus/gbarecomp
```

The fork is required for the multiplayer work below (sub-frame presents, the
game's ROM-data preparation in single player and netplay, and a test hook in
the netplay runner are only there). To start from upstream `mstan/gbarecomp`
for the single-player game, apply the patches (the same ones the Duke Nukem
Advance recomp uses):

```
cd gbarecomp
git apply ../GTAAdvanceRecomp/patches/0001-*.patch ../GTAAdvanceRecomp/patches/0002-*.patch ../GTAAdvanceRecomp/patches/0003-*.patch
git -C external/arm-recomp-core apply ../../../GTAAdvanceRecomp/patches/0004-*.patch
git apply ../GTAAdvanceRecomp/patches/0005-*.patch
```

`0005` makes exclusive fullscreen use the desktop's own resolution (instead of
switching the monitor to a tiny mode) and fixes switching between borderless
and exclusive from the menu.

## Build (Windows)

Needs Visual Studio 2022 (to build the recompiler), MSYS2 MinGW64 with GCC,
CMake, Ninja and SDL2, and Python 3.

```powershell
.\build.ps1 -RealBios
```

This scans the ROM for function seeds, regenerates `generated/`, recompiles
the BIOS and builds `build\GTAAdvanceRecomp.exe`. Close the game first: a
running exe cannot be replaced. `game.toml` holds the ROM and BIOS paths.

Every build ends by writing `build\GTAAdvance.zip` (`tools\package.ps1`), a
self-contained copy for your other PCs. It contains your ROM and BIOS, so
keep it to your own machines.

## Playing

The game starts in borderless fullscreen; **Esc** opens the options menu
(display mode, window scale, filters, volume, save states, Graphics and
Controls) and Alt+Enter toggles fullscreen. The display mode is remembered.
The shell's turbo hotkey is unbound and its performance overlay is on F3,
because their defaults (Tab, F) are used by the layout below; rebind them in
`config.ini` under `[KeyMap]`.

### Native renderer

On by default (Esc > Graphics > Native renderer). The ground and the buildings
are drawn on the PC straight from the game's world data:

- at the window's resolution, with clean building edges and perspective;
- at the window's aspect ratio: a 16:9 or 21:9 window sees more of the streets
  to the sides instead of bars, and the game is told about the extra width so
  people and traffic exist there too. The radar keeps to the left edge, the
  status block and district name to the right;
- **Smooth motion** (on by default): the game moves its world only 15 times a
  second. The renderer shows the steps in between, so scrolling, people and
  traffic move continuously; the picture is then about 1/12 s behind the
  game. Turn it off for the original stepping.
- **Show player under cover** (on by default): the original hides you
  completely under bridges, gates and awnings. Here you, and the car you are
  in, are drawn see-through over whatever covers you.
- **Frame rate**: 60 by default. 120 / 180 / 240 and "Match display" draw the
  in-between states more often, but this is experimental: in a first test at
  3440x1440 it reached 90 to 104 pictures a second with hitches.
- **Render resolution** trades sharpness for speed or a chunkier look.

People, cars and the HUD are the game's own sprites, scaled up. The game's own
building rasteriser is skipped while the option is on (its pixels would not be
shown). Turning the option
off shows the untouched 240x160 picture. Menus and cutscenes are always the
original picture.

| Key | On foot | Driving |
|---|---|---|
| W A S D | move | steer, S brakes |
| Left Ctrl, J, left click | attack | accelerate |
| Left Shift, K, right click | sprint | reverse |
| Space | strafe | handbrake |
| F | enter car | exit car |
| C | jump | vehicle weapon |
| Q / E | previous / next weapon | same |
| H | | horn / siren |
| Tab | Select | Select (start side missions) |
| Enter | pause | pause |

The original bindings (arrows, X = A, Z = B, ...) keep working.

The game reads the pad only 15 times a second, so every key of this layout
is held until the game has seen it: a quick tap is never lost, and Q / E play
the whole "hold Select, press L or R" weapon chord in the right order.

**Mouse in the game's menus** (main menu, pause menu, save slots): point at
an entry to select it, left click to choose it, right click to go back. On
screens without a list (dialogue, title) a left click continues. The pointer
is hidden while you are in the city.

## Multiplayer (work in progress)

The cartridge still contains the link-cable multiplayer the game was built
with: Liberty City Survivor, City Circuit, Car Jack Crazy and Special
Delivery, each with its settings (area, time limit, frag limit / laps / cars /
packages, AI opponents, weapons) and a "CONNECT CABLE" handshake. None of it
is reachable in the shipped game. This build puts **MULTI PLAYER** back on the
title menu (a data-only change to the menu tables, see `src/main.cpp`).

Without a second console the modes run on their own (with AI opponents). The
build includes the framework's link-cable netplay, where every player runs
both GBAs and only controller input travels, so two copies can be linked over
the network by command line:

```
GTAAdvanceRecomp --netplay-bind 0.0.0.0:5300 --netplay-peer FRIEND_IP:5301 --netplay-seat 0 --netplay-session 4242
GTAAdvanceRecomp --netplay-bind 0.0.0.0:5301 --netplay-peer YOUR_IP:5300 --netplay-seat 1 --netplay-session 4242
```

The shipped interrupt dispatcher has no case for the link's serial and timer 3
interrupts (likely why the mode was cut: on hardware it locks up the same way).
Two function hooks route them to the game's own handler (`src/main.cpp`), and
with that two linked consoles connect and play the same Liberty City Survivor
round. Not finished: an in-game way to host or join through the online server.
The title menu also offers the developer mission select, PICK LEVEL (CHEAT). `tools/re/link_test.py`
runs two linked copies on one PC with scripted input.

## Repository map

| Path | What |
|---|---|
| `src/main.cpp` | Host entry point |
| `src/pc_controls.cpp` | Keyboard/mouse layout, its menu rows and settings |
| `src/menu_mouse.cpp` | Mouse pointing in the game's own menus |
| `src/scene/` | Native renderer (ground and buildings from the game's world data) |
| `src/scene_host.cpp` | Composites the native scene with the game's sprites and HUD; widescreen hooks |
| `docs/ENGINE_NOTES.md` | What is known about the game's engine and data formats |
| `recomp.toml` | Hand-written recompiler config (entry points, RAM-copied routines) |
| `tools/scan_seeds.py` | Finds pointer-table functions and computed-jump landings |
| `tools/drive.py` | Headless, muted test driver over the debug server |
| `tools/re/` | Reverse-engineering and test tools (Ghidra scripts, native-vs-game shots, smoothness, menu and input checks) |
| `patches/` | Fixes to apply to the gbarecomp checkout |

## Licensing

GBARecomp is under the PolyForm Noncommercial License 1.0.0, so builds of
this project are for noncommercial use. recomp-ui is MIT. Grand Theft Auto
Advance is the property of its rights holders; nothing of it is included.
