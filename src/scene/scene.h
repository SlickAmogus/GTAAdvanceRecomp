// Native scene renderer for Grand Theft Auto Advance: draws the ground and the
// buildings straight from the game's world data at any resolution and aspect.
// Formats and addresses are documented in docs/ENGINE_NOTES.md.
#pragma once
#include <cstddef>
#include <cstdint>

namespace gtascene {

struct Memory {
    const std::uint8_t* ewram = nullptr;    // 0x02000000, 256 KiB
    const std::uint8_t* iwram = nullptr;    // 0x03000000, 32 KiB
    const std::uint8_t* rom = nullptr;      // 0x08000000
    std::size_t rom_size = 0;
    const std::uint8_t* palette = nullptr;  // 0x05000000 (BG palette first)
};

// The picture is `height` pixels tall for the game's 160 lines; the width is
// free, and anything beyond 3:2 shows more of the world to the sides.
struct View {
    int width = 0, height = 0;
    float cam_x = 0, cam_y = 0, cam_z = 0;  // world units
    float ground_z = 0;                     // camera height above the ground plane
    // Rows to draw, [row_begin, row_end): lets several threads share a frame.
    int row_begin = 0, row_end = 0;
};

// One building instance, copied out of the game's list at a moment when the
// list is whole (the game relinks it over several display frames).
struct Instance {
    std::uint32_t model = 0;
    std::int32_t x = 0, y = 0, z = 0;       // 16.16 world position
    std::int32_t box[6] = {};               // min xyz, max xyz around the position
    std::uint8_t state = 0, placement = 0;
};

// Walks the game's instance list. Returns the number copied (at most `max`),
// or -1 if the list is not walkable right now.
int read_instances(const Memory& mem, Instance* out, int max);

// True while a city is loaded and the camera is valid; fills the camera.
bool read_view(const Memory& mem, View* view);

// Ground layer: every pixel is written (opaque).
void draw_ground(const Memory& mem, const View& view, std::uint32_t* argb);

// Buildings, drawn over whatever is already in `argb`. `depth` is scratch
// space of width*height floats.
void draw_buildings(const Memory& mem, const View& view, const Instance* instances, int count,
                    std::uint32_t* argb, float* depth);

}  // namespace gtascene
