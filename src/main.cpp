// Grand Theft Auto Advance (BGTE, USA) - static recompilation host.
//
// The generated cartridge code lives in ../generated; everything else
// (window, audio, input, saves, PPU) comes from the shared gbarecomp runtime.
#include "runtime.h"
#if defined(GTA_NETPLAY)
#include "gba_netplay_build_identity.h"
#include "multiplayer_launch.h"
#endif
#include "pc_controls.h"
#include "menu_mouse.h"
#include "scene_host.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <cstring>
#include <string>
#include <vector>

// The ground is the mode-1 affine layer (BG2), drawn zoomed. Letting the
// runtime sample it at the window's resolution sharpens it when the player
// raises Display > Resolution scale; buildings (BG0), HUD and sprites keep
// their original pixels.
static bool ground_layer_is_affine(int bg, int) { return bg == 2; }

// The cartridge still carries its link-cable multiplayer menu (MULTI PLAYER:
// Liberty City Survivor, City Circuit, Car Jack Crazy, Special Delivery, at
// ROM 0x08334138), but no menu leads to it. The title screen opens menu
// 0x08332D4C (NEW GAME, LOAD GAME, ERASE GAME); its hidden fourth slot holds a
// developer mission select (PICK LEVEL (CHEAT)). Menus are a u32 title string
// id and a u32 entry count, then up to seven entries of 0x90 bytes: u32
// parent (first entry) or index, u32 label string id, u32 submenu, u8 action.
// This puts MULTI PLAYER in the fourth slot and makes the title menu its
// parent, so B leads back. The entry takes action 5, the menu's link
// handshake (0x08065E5C, "A BUTTON TO CONNECT"), which on success sets the
// link flag the player code reads (which console am I) before the mode list
// opens.
//
// Each mode's menu also keeps its settings rows (AREA, TIME LIMIT, FRAG LIMIT
// / LAPS / CARS / PACKAGES, AI OPPONENTS, WEAPONS) with only START GAME
// counted; the full counts are restored.
//
// Data only: no code or literal changes, so the generated code still matches
// the image.
static gbarecomp::ModRomDataResult restore_multiplayer_menu(std::uint8_t* rom, std::size_t size, const char** error) {
    constexpr std::size_t kTitleMenu = 0x00332D4C, kEntry = 0x90, kMultiMenu = 0x00334138;
    if (size < kMultiMenu + 0x100) { *error = "ROM too small"; return gbarecomp::ModRomDataResult::Failed; }
    auto u32 = [&](std::size_t at) {
        std::uint32_t v; std::memcpy(&v, rom + at, 4); return v;
    };
    auto put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(rom + at, &v, 4); };
    const std::size_t entries = kTitleMenu + 8, slot = entries + 3 * kEntry;
    // Expected shipped layout; anything else is left alone.
    if (u32(kTitleMenu) != 58 || u32(kTitleMenu + 4) != 3 || u32(entries + 4) != 62 ||
        u32(entries + kEntry + 4) != 63 || u32(slot + 4) != 82 || u32(kMultiMenu) != 60 ||
        u32(kMultiMenu + 4) != 4 || u32(kMultiMenu + 8) != 0x08332554u)
        return gbarecomp::ModRomDataResult::Unchanged;
    std::memset(rom + slot, 0, kEntry);
    put32(slot + 4, 60);                                  // "MULTI PLAYER"
    put32(slot + 8, 0x08000000u + kMultiMenu);
    rom[slot + 12] = 5;                                   // link handshake first
    // Mode menus: {menu, entries with their settings}.
    static const struct { std::size_t menu; std::uint32_t title, count; } kModes[] = {
        {0x00334534, 69, 5},    // LIBERTY CITY SURVIVOR: area, time, frags, AI
        {0x00334930, 68, 7},    // CITY CIRCUIT: area, circuit, time, laps, AI, weapons
        {0x00334D2C, 67, 5},    // CAR JACK CRAZY: area, time, cars, AI
        {0x00335128, 66, 5},    // SPECIAL DELIVERY: area, time, packages, AI
    };
    for (const auto& m : kModes) {
        if (u32(m.menu + 4) != 1) continue;
        bool rows_present = true;
        for (std::uint32_t k = 1; k < m.count; ++k)
            rows_present &= u32(m.menu + 8 + k * kEntry + 4) != 0;
        if (rows_present) put32(m.menu + 4, m.count);
    }
    put32(kTitleMenu + 4, 4);
    put32(kMultiMenu + 8, 0x08000000u + kTitleMenu);      // parent: back to the title menu
    return gbarecomp::ModRomDataResult::Prepared;
}

// Game-owned debug server commands (headless testing).
static int debug_tcp_command(const char* request,
                             void (*write)(void* ctx, const char* data, std::size_t len),
                             void* write_ctx) {
    // {"cmd":"gta_keys","buttons":4,"frames":1} / {"cmd":"gta_cycle","dir":1}
    auto number = [&](const char* key, long fallback) {
        const char* at = std::strstr(request, key);
        at = at ? std::strchr(at + std::strlen(key), ':') : nullptr;
        return at ? std::strtol(at + 1, nullptr, 0) : fallback;
    };
    static const char kOk[] = "{\"ok\":true}";
    if (std::strstr(request, "\"gta_keys\"")) {
        gta_pc_controls_debug_buttons(static_cast<int>(number("\"buttons\"", 0)),
                                      static_cast<int>(number("\"frames\"", 1)));
        write(write_ctx, kOk, sizeof(kOk) - 1);
        return 1;
    }
    if (std::strstr(request, "\"gta_cycle\"")) {
        gta_pc_controls_debug_cycle(static_cast<int>(number("\"dir\"", 1)));
        write(write_ctx, kOk, sizeof(kOk) - 1);
        return 1;
    }
    return gta_menu_tcp_command(request, write, write_ctx) ||
           gta_debug_tcp_command(request, write, write_ctx);
}

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
#if defined(GTA_NETPLAY)
    // The cartridge's own link-cable multiplayer (MULTI PLAYER on the title
    // menu), over the network: every peer runs both GBAs and only controller
    // input travels (framework MULTIPLAYER.md). Content hash = SHA-256 of the
    // USA cartridge.
    opts.netplay = gbarecomp::make_gba_netplay_launch(
        "gtaadvance-usa", GBARECOMP_NETPLAY_BUILD_ID,
        "215c574d5afbda0aadbf89dc76b765cfaaeb35953eab63f7687d8bfe8a8e0f4d");
    opts.netplay->supported_media = 1u << static_cast<unsigned>(gbarecomp::GbaLinkMedium::Cable);
    // The netplay options (--netplay-*) are taken out of the argument list.
    static std::vector<std::string> arg_strings;
    arg_strings.assign(args.begin(), args.end());
    try {
        gbarecomp::parse_gba_netplay_arguments(arg_strings, *opts.netplay);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "netplay: %s\n", e.what());
        return 1;
    }
    args.clear();
    for (std::string& a : arg_strings) args.push_back(a.data());
#endif
    opts.builtin_game_name = "Grand Theft Auto Advance";
    opts.builtin_rom_sha1  = "742fc0062ad930253ffea449953061381577649e";
    opts.freely_resizable_window = true;
    opts.show_fps_by_default = true;
    opts.high_resolution_affine_provider = ground_layer_is_affine;
    opts.prepare_mod_rom_data = restore_multiplayer_menu;
    opts.tcp_command = debug_tcp_command;
    gta_scene_install(&opts);
    gta_pc_controls_install(&opts, argv[0]);

    std::vector<std::string> remembered;
    if (!scripted && !display_flag) gta_pc_controls_startup_args(&remembered);
    for (std::string& s : remembered) args.push_back(s.data());

    return gbarecomp::run_game(static_cast<int>(args.size()), args.data(), opts);
}
