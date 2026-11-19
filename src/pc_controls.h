#pragma once
#include <string>
#include <vector>

#include "runtime.h"

// Installs the PC keyboard/mouse layout and its settings-menu rows into the
// run options. Call once before gbarecomp::run_game().
void gta_pc_controls_install(gbarecomp::RunOptions* opts, const char* argv0);

// Remembered display choices (fullscreen mode, scaling filter) as runtime CLI
// flags. Call after gta_pc_controls_install(); skip for scripted runs.
void gta_pc_controls_startup_args(std::vector<std::string>* extra);

// Debug TCP only: hold pad buttons (KEYINPUT bit mask, 1 = down) for a number
// of frames as if typed, and queue a weapon switch (-1 previous, +1 next).
void gta_pc_controls_debug_buttons(int buttons, int frames);
void gta_pc_controls_debug_cycle(int direction);
