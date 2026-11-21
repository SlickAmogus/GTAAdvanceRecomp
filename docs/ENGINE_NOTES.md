# Grand Theft Auto Advance (BGTE, USA): engine notes

What is known about the game's code and data, worked out from the cartridge.
Addresses are for the USA ROM (SHA-1 `742fc0062ad930253ffea449953061381577649e`).
The native renderer in `src/scene/` is built on these.

## Code layout

| Range | What |
|---|---|
| `0x080000C0` | crt0 (ARM); IRQ dispatcher at `0x08000104`, handler table at `0x02000170` |
| `0x08000220-0x08067000` | Thumb code (`main` at `0x08000430`) |
| `0x08067000-0x0806B500` | ARM routines, most of them run from IWRAM |
| `0x0806B500-0x08071C00` | Thumb (libc) |
| after `0x08071C00` | data |

The loader near `0x08011C00` walks a table at `0x08CA4164` of twelve records
`{EWRAM pointer variable, ROM start, ROM end}` and copies each ARM routine into
IWRAM, storing where it landed in the variable (`0x0201CE80..0x0201CEB4`).
In the city they land from `0x03000100` up. Two sound mixer routines
(`0x0806B090`, `0x0806B298`) are copied the same way, to `0x03005120` /
`0x03005324` in the city and to `0x03000320` / `0x03000524` on the title
screen. `recomp.toml` declares these placements.

## The city picture (video mode 1)

| Layer | Priority | Content |
|---|---|---|
| BG2 (affine, 8bpp) | 3 | ground, streamed from the block map |
| OBJ | 3 | people, cars, markers |
| BG0 (8bpp, 240x160 worth of tiles) | 2 | buildings, software-rasterised |
| BG1 (4bpp) | 0 | HUD digits, radar disc, district name, messages |
| OBJ | 0 | HUD sprites (weapon icon, radar blips) |

## Camera and projection

`IWRAM 0x03000014`: camera `x, y, z` as 16.16 world units (`z` is the height,
164 on foot; it rises when driving fast). World `y` grows down the screen.

The view is a 90-degree pyramid looking straight down, projected onto a
240x240 square whose centre is the camera; the screen shows rows 0..159 of it:

```
screen_x = 120 + 118 * X / Z
screen_y = 120 - 118 * Y / Z
```

with `X, Y` relative to the camera and `Z` the distance below it. The table at
`0x08C9A528` is `65536 / Z` (u16 per integer `Z`).

`IWRAM 0x03000068 / 6C`: world units per screen pixel on the ground (16.16);
`0x03000060 / 64`: its inverse. The ground plane is about 3 units above
`z = 0` as far as the scale is concerned (1.366 units per pixel at `z = 164`).

### The visible rectangle

Set from the camera by `0x0800A130` (per-frame camera routines `0x08008F7C`,
`0x08009848`) and by `0x08008BC4` (direct placement):

| IWRAM | Meaning |
|---|---|
| `0x50, 0x58` | left, right in whole world units: `x -/+ z` |
| `0x54, 0x5C` | top, bottom: `y - z`, `y + z * 85 / 256` |
| `0x84, 0x88, 0x8C, 0x90` | `x -/+ z`, `y -/+ z` in 16.16, used to pick buildings |

Sprites are limited to this rectangle and their screen column is
`(world_x - left) * scale`. `0x08014500` (update one sprite object) also
releases a sprite whose left edge passes column 240 (`cmp r0, #0xF0` at
`0x08014650`). The widescreen hook widens `0x50 / 0x58` and that limit.

## Timing: 15 world ticks a second

The city's main loop (`0x0805DDB8`) runs one world tick (`0x08051030`:
camera, entities, building pass `0x0800D2B0`, sprites) and then waits for the
picture flip **and** until at least four display frames have passed since the
last tick:

```
0805DE76  ldr r0, [r6]      ; EWRAM 0x02035CA8, display frames so far
0805DE7A  cmp r0, #3
0805DE7C  ble wait
```

Everything moves a fixed amount per tick, so this is the game's speed, not a
performance cap: with the limit lowered the game simply runs up to four times
too fast (tried). `IWRAM 0x03000000` holds the display frames the last tick
took (capped at 5) but movement does not scale with it.

The vertical-blank handler (`0x08000220`) flips the picture when a tick is
ready (state byte `EWRAM 0x02000130`): it streams the ground (`0x08010544`)
and copies the tick's sprite list to hardware OAM (`0x080127EC`, first call of
the flip). Flips land 3 to 6 display frames apart.

The camera variable (`IWRAM 0x14`) changes at the start of the *next* tick's
computation, a few display frames before that tick is flipped. The camera
that belongs to the sprites on screen is therefore the one current at the
flip; the native renderer latches it there.

The building instance list is re-sorted and relinked inside `0x0800D2B0`
(`0x0800D180`), and instances stream in and out across display frames, so
between flips the list is often incomplete. `0x0800E3E4` is the first call
after the sort; the renderer copies the list there.

### Pacing on the PC

A tick costs the console's CPU three to five display frames, so on hardware
flips land 2 to 6 frames apart and busy streets run slow. The port charges
the city's CPU time at one eighth (the runtime's cycle discount): every tick
fits, and flips land exactly four frames apart.

### Pad

The pad is read once per tick. `IWRAM 0x03000098` = buttons newly pressed on
that read, `0x0300009C` = buttons held, both in KEYINPUT bit order with 1 =
down. A press shorter than a tick can be missed entirely, and the parts of a
chord must all be down on the same read. `EWRAM 0x02010C7A` selects the
control scheme (0 = the default described under Controls).

## Static objects

The city is divided into 16x16 sectors of 1024 world units. The sector table
is at `EWRAM 0x02015650`: from `+0x24`, twelve bytes per sector (`count`,
pointer to records, runtime list), sixteen sectors per row; columns and rows
at `+0xC28` / `+0xC24`. Each record is sixteen bytes:

| Offset | Meaning |
|---|---|
| `+0` | index within the sector |
| `+4, +6` | u16 x, y of the object's corner (the instance sits at its middle) |
| `+8` | u16 type: `>= 0x80` is building model `type - 0x80`, `0x40..0x7F` is prop model `type + 266` |
| `+10` | low two bits = quarter turns |

`0x0800C8B4` turns a sector's records into instances as the camera nears it.
The model table is at `0x08EBF7F4`, sixteen bytes per model (id, geometry
pointer, two animation lists).

## Menus and the cut multiplayer

Menu text comes from a string table at `0x08EC4324` (u32 pointers, 618 per
language; `0x0805E330(id)` looks one up). A menu is a u32 title string id and
a u32 entry count, then up to seven entries of 0x90 bytes: u32 parent (first
entry) or index, u32 label string id, u32 submenu, u8 action, u8 number of
values, then up to sixteen u32 value string ids, and a u32 pointer to the
setting's byte at the end. Actions seen: 0 open submenu or pick a value, 2
start the selected multiplayer mode, 3 new game, 4 load, 5 link handshake
(`0x08065E5C`), 7 mode, 10 credits, 11 erase.

The title opens `0x08332D4C` (NEW GAME, LOAD GAME, ERASE GAME; a hidden fourth
entry is a developer mission select, PICK LEVEL (CHEAT), `0x08335920`). The
multiplayer menu `0x08334138` (Liberty City Survivor `0x08334534`, City
Circuit `0x08334930`, Car Jack Crazy `0x08334D2C`, Special Delivery
`0x08335128`) still names the old main menu `0x08332554` as its parent but
nothing leads to it, and each mode menu counts only START GAME although its
settings entries follow it. The serial code is `0x08065428..0x080666A4`
(multi-player mode, ten-word packets chained from the serial interrupt
`0x08066554`); the vertical-blank handler runs `0x080664BC` while the link
flag (`EWRAM 0x02000D08`) is set, and the player code picks the local player
through a flag at `+0xC` of `EWRAM 0x02010C60`.

The crt0 interrupt dispatcher (ARM `0x08000104`, copied to EWRAM `0x020004D0`
and installed from the literal at `0x08000410`) tests VBlank, VCount, HBlank,
timers 0-2, DMA, keypad and cartridge, but not timer 3 or serial, although its
nesting mask (`0x20C0`) is the multi-player SIO one. The link's handler
`0x08066554` is never referenced. The link runs on the serial interrupt for the
handshake and then on timer 3 (`0x08066220`: IE serial off, timer 3 on,
reload `0xABFB`), with that same handler serving each tick.

### Rampages and the link

A skull pickup (`0x0804C374`) calls `0x0806219C` to start a rampage; it
stores the rampage's parameters at `EWRAM 0x02010C7E..84`, posts event `0x5E`
through `0x080591F8` and answers 1, and only then is the pickup used up. The
city's event dispatcher `0x0805E634` runs the pending event (u16 at
`0x02010C7C`, jump table at `0x0805E680` indexed by event - 0x11); `0x5E`
lands at `0x0805E85C`, which calls the rampage setup `0x08060C78` (text id
557, "USE %A TO KILL %9 GANG MEMBERS."). In a link match the console taking a
skull stops serving the cable: the other freezes, the taker goes black, and
both end on LINK ERROR (`0x08065F9C`, text id 455), which waits for B and goes
to the main menu. Dropping only the event was not enough. The port makes
`0x0806219C` answer 0 while the link flag is set.

## Ground

World units to ground texels: multiply by `0x28 / 64 = 0.625`. One tile is 8
texels, one block is 5x5 tiles (64 world units).

| EWRAM | Meaning |
|---|---|
| `0x0201CE58, 5C` | ground width, height in texels |
| `0x0201CE60` | pointer: u16 block id per block, row length `1 << shift` |
| `0x0201CE64` | pointer: block definitions, 25 u16 tile ids each (row-major) |
| `0x0201CE68` | pointer: 8x8 tiles, 8bpp, 64 bytes each |
| `0x0201CE74` | shift (8) |

`0x08010110` restreams the whole visible part into the BG2 map and tile
cache (256 tiles), `0x08010544` does it incrementally.

## Buildings

`EWRAM 0x02016280`: head of a linked list of instances near the camera (about
60, covering roughly 1600 units around it). Each instance, 0x20 bytes:

| Offset | Meaning |
|---|---|
| `+0x00` | pointer to the model |
| `+0x04, 08, 0C` | position `x, y, z` (16.16) |
| `+0x10` | pointer to bounding box: six s32, min xyz then max xyz, world-aligned |
| `+0x14, +0x18` | next, previous |
| `+0x1C` | u16 distance key (sorting) |
| `+0x1E` | bit 7 = visible this frame, low 7 bits = animation frame |
| `+0x1F` | bits 0-1 = quarter turns, bit 7 = roof texture laid out along the world axes |

Model: `+0` id, `+4` pointer to geometry, `+8` optional list of wall texture
pointers per animation frame, `+0xC` optional list of roof records (0x18 bytes
each) per frame.

Geometry: `+0x30` u16 roof texture index, `+0x32` u16 wall texture index,
`+0x34` u16 face count, `+0x38` four pointers to face lists, one per quarter
turn (the vertices are stored already turned).

Face record (variable size):

| Offset | Meaning |
|---|---|
| `+0` | record size in bytes |
| `+1` | texture index |
| `+2` | palette index for untextured faces |
| `+3` | flags: low 3 bits = vertex count (3 or 4), `0x10` = textured, `0x08` = wall quad |
| `+4` | vertices, three s16 each (`x, y, z`; `y` up the screen, `z` height) |
| then | for textured faces, one `(u, v)` byte pair per vertex |

Seen: `0x03` flat triangle, `0x13` textured triangle, `0x1C` textured wall
quad, `0x14` roof quad.

Camera space: `X = vx - (cam_x - pos_x)`, `Y = vy + (cam_y - pos_y)`,
`Z = (cam_z - pos_z) - vz`. A face is drawn when the determinant of its first
three vertices is not positive. The game clips to `|X| <= Z`, `|Y| <= Z`,
`Z >= 32`.

Textures are 8bpp, index 0 transparent, in the BG palette:

- walls and triangles: 64x64, pointer table at `0x08EBF5F4` (u32 per index);
  `v` is stored bottom-up (`row = 63 - v`).
- roofs: 128x128, table at `0x08EBEB2C`, 0x18 bytes per index: three mip
  levels, each a pair of pointers (the second is the picture turned a
  quarter). Roof `v` is `128 - v`. With placement bit 7 set (nearly always)
  the model's own coordinates are ignored and the texture runs left to right,
  top to bottom along the world axes.

The game's own pipeline: `0x0800D2B0` sorts the list, marks visible instances
and calls `0x0800DB64` per instance, which walks a display list at
`IWRAM 0x03000028` (count at `0x24`) of projected primitives (`0x03` flat
triangle, `0x13` textured triangle, `0x14` roof rectangle, `0x1C` wall quad)
and hands them to the IWRAM rasterisers. The whole layer is redrawn only when
the camera moved or every fourth frame.

## Controls (default scheme, from the game's tutorial text)

On foot: A attack, B sprint, A+B jump, R strafe, L enter car, Select+L / R
previous / next weapon. Driving: A accelerate, B reverse, Down brake, R
handbrake, L exit, Up+R horn or siren, A+B vehicle weapon, Select starts a
side mission.
