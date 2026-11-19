// Grand Theft Auto Advance (BGTE, USA) - static recompilation host.
//
// The generated cartridge code lives in ../generated; everything else
// (window, audio, input, saves, PPU) comes from the shared gbarecomp runtime.
#include "runtime.h"
#include "pc_controls.h"
#include "scene_host.h"

#include <cstring>
#include <string>
#include <vector>

// The ground is the mode-1 affine layer (BG2), drawn zoomed. Letting the
// runtime sample it at the window's resolution sharpens it when the player
// raises Display > Resolution scale; buildings (BG0), HUD and sprites keep
// their original pixels.
static bool ground_layer_is_affine(int bg, int) { return bg == 2; }

int main(int argc, char** argv) {
    // Launcher-seam flags are sent by framework tooling (replay_diagnostic.py)
    // to every title; this host has no launcher, so drop them.
    std::vector<char*> args;
    bool scripted = false;      // headless / tooling runs keep explicit flags only
    bool display_flag = false;  // the player chose a mode on the command line
    for (int i = 0; i < argc; ++i) {
        if (i > 0 && std::strcmp(argv[i], "--no-launcher") == 0) continue;
        if (!std::strcmp(argv[i], "--no-window") || !std::strcmp(argv[i], "--frames") ||
            !std::strcmp(argv[i], "--tcp") || !std::strcmp(argv[i], "--replay-diagnostic"))
            scripted = true;
        if (!std::strncmp(argv[i], "--fullscreen", 12) ||
            !std::strcmp(argv[i], "--linear-filter") || !std::strcmp(argv[i], "--sharp-filter"))
            display_flag = true;
        args.push_back(argv[i]);
    }

    gbarecomp::RunOptions opts;
    opts.builtin_game_name = "Grand Theft Auto Advance";
    opts.builtin_rom_sha1  = "742fc0062ad930253ffea449953061381577649e";
    opts.freely_resizable_window = true;
    opts.show_fps_by_default = true;
    opts.high_resolution_affine_provider = ground_layer_is_affine;
    opts.tcp_command = gta_debug_tcp_command;
    gta_scene_install(&opts);
    gta_pc_controls_install(&opts, argv[0]);

    std::vector<std::string> remembered;
    if (!scripted && !display_flag) gta_pc_controls_startup_args(&remembered);
    for (std::string& s : remembered) args.push_back(s.data());

    return gbarecomp::run_game(static_cast<int>(args.size()), args.data(), opts);
}
