#pragma once
#include <cstddef>

#include "runtime.h"

// Player-facing native renderer settings (owned by scene_host.cpp, saved by
// pc_controls.cpp).
struct GtaSceneSettings {
    int native = 1;          // 0 = the game's own 240x160 picture
    int resolution = 0;      // 0 = window resolution, then 75 / 50 / 33 / 25 %
    int smooth = 1;          // blend between the game's 15 world ticks a second
    int frame_rate = 1;      // presents per game frame: 0 = match the display, 1..4 = 60..240 fps (above 60 is experimental)
};
GtaSceneSettings* gta_scene_settings();

// Installs the present-time compositor. Call before gbarecomp::run_game().
void gta_scene_install(gbarecomp::RunOptions* opts);

// Debug TCP only: write the current composite as a PPM. False when the game
// is not showing the city.
bool gta_scene_debug_shot(const char* path, int width, int height);

// Game-owned debug TCP commands (RunOptions::tcp_command).
int gta_debug_tcp_command(const char* request,
                          void (*write)(void* ctx, const char* data, std::size_t len),
                          void* write_ctx);
