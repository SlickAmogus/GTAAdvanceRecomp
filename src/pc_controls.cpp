// PC controls for Grand Theft Auto Advance: a keyboard/mouse layout on top of
// the runtime's own bindings (arrows, X = A, Z = B, ...), which keep working.
//
// The game's default control scheme, from its own tutorial text:
//   on foot   D-pad move, A attack, B sprint, A+B jump, R strafe,
//             L enter car, Select+L / Select+R previous / next weapon
//   driving   D-pad left/right steer, A accelerate, B reverse, D-pad down
//             brake, R handbrake, L exit car, Up+R horn / siren,
//             A+B vehicle weapon
//
// Everything here only presses pad buttons; no guest memory is written.
#include "pc_controls.h"
#include "menu_mouse.h"
#include "scene_host.h"

#include <SDL.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "gba_bus.h"
#include "gba_io.h"
#include "recomp_runtime_ui.h"
#include "runtime_bus_bridge.h"
#include "touch_input.h"

namespace {

constexpr char kKeyLayout[] = "gta.pc_layout";
constexpr char kKeyMouse[]  = "gta.mouse_buttons";
constexpr char kKeyFilter[] = "gta.filter";
constexpr char kKeyNative[] = "gta.native_renderer";
constexpr char kKeyResolution[] = "gta.resolution";
constexpr char kKeySmooth[] = "gta.smooth";
constexpr char kKeyFrameRate[] = "gta.frame_rate";

struct Settings {
    int layout = 1;         // WASD layout on
    int mouse = 1;          // mouse buttons attack / sprint
    int fullscreen = 1;     // 0 windowed, 1 borderless, 2 exclusive (remembered)
    int filter = 0;         // 0 sharp pixels, 1 smooth, 2 sharp-bilinear
} g_settings;

std::string g_settings_path;

int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void ensure_hotkeys(const std::filesystem::path& dir);

void load_settings(const char* argv0) {
    // Beside the executable. (No SDL here: this runs before SDL_Init, and a
    // headless run never initialises SDL at all.)
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::absolute(
        std::filesystem::path(argv0 ? argv0 : "."), ec).parent_path();
    g_settings_path = (dir / "gta_pc.ini").string();
    ensure_hotkeys(dir);
    std::FILE* f = std::fopen(g_settings_path.c_str(), "r");
    if (!f) return;
    char key[64];
    int value = 0;
    while (std::fscanf(f, " %63[^= \n] = %d", key, &value) == 2) {
        if (!std::strcmp(key, "pc_layout")) g_settings.layout = value != 0;
        else if (!std::strcmp(key, "mouse_buttons")) g_settings.mouse = value != 0;
        else if (!std::strcmp(key, "fullscreen")) g_settings.fullscreen = clamp(value, 0, 2);
        else if (!std::strcmp(key, "filter")) g_settings.filter = clamp(value, 0, 2);
        else if (!std::strcmp(key, "native_renderer")) gta_scene_settings()->native = value != 0;
        else if (!std::strcmp(key, "resolution")) gta_scene_settings()->resolution = clamp(value, 0, 4);
        else if (!std::strcmp(key, "smooth_motion")) gta_scene_settings()->smooth = value != 0;
        else if (!std::strcmp(key, "frame_rate")) gta_scene_settings()->frame_rate = clamp(value, 0, 4);
    }
    std::fclose(f);
}

// The runtime's own hotkeys (config.ini [KeyMap]) default to Tab = turbo and
// F = performance overlay, both of which this layout uses for the game. Give
// them other defaults unless the player has already chosen bindings.
void ensure_hotkeys(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / "config.ini";
    std::string text;
    if (std::FILE* f = std::fopen(path.string().c_str(), "rb")) {
        char buf[4096];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
        std::fclose(f);
    }
    if (text.find("[KeyMap]") != std::string::npos) return;
    if (!text.empty() && text.back() != '\n') text += '\n';
    text += "\n[KeyMap]\n"
            "# Hotkeys of the PC shell. Empty = unbound.\n"
            "Fullscreen=Alt+Return\n"
            "Pause=Shift+P\n"
            "Turbo=\n"
            "DisplayPerf=F3\n";
    if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
}

void save_settings() {
    std::FILE* f = std::fopen(g_settings_path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "pc_layout = %d\nmouse_buttons = %d\nfullscreen = %d\nfilter = %d\n"
                    "native_renderer = %d\nresolution = %d\nsmooth_motion = %d\nframe_rate = %d\n",
                 g_settings.layout, g_settings.mouse, g_settings.fullscreen, g_settings.filter,
                 gta_scene_settings()->native, gta_scene_settings()->resolution,
                 gta_scene_settings()->smooth, gta_scene_settings()->frame_rate);
    std::fclose(f);
}

// The runtime's own Display > Fullscreen row applies the mode but does not
// remember it (its launcher normally would). Watch the window and save the
// mode whenever it changes so the next launch starts the same way.
void remember_display_mode() {
    static unsigned tick = 0;
    if (++tick % 30u) return;
    SDL_Window* w = SDL_GetKeyboardFocus();
    if (!w) return;
    const Uint32 flags = SDL_GetWindowFlags(w);
    const int mode =
        (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP ? 1
        : (flags & SDL_WINDOW_FULLSCREEN) ? 2 : 0;
    if (mode == g_settings.fullscreen) return;
    g_settings.fullscreen = mode;
    save_settings();
}

// GBA KEYINPUT bits (active low).
enum : int { kA = 0, kB = 1, kSelect = 2, kStart = 3, kRight = 4, kLeft = 5,
             kUp = 6, kDown = 7, kR = 8, kL = 9 };

// In the city the game reads the pad once per world tick, 15 times a second,
// so a key tapped for less than four display frames can go unnoticed, and a
// chord whose parts are not all down on the same read arrives as something
// else. The game's own record of the buttons it saw last (IWRAM 0x0300009C,
// same bit order as KEYINPUT, 1 = down) is used as the acknowledgement:
// every press is kept down until the game has seen it.
struct PadView {
    bool city = false;
    std::uint16_t seen = 0;     // buttons the game saw on its last read
};

PadView pad_view() {
    PadView v;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return v;
    const std::uint16_t dispcnt = bus->io().read16(0x000);
    v.city = (dispcnt & 7u) == 1u && (dispcnt & 0x0500) == 0x0500;
    std::uint32_t held = 0;
    std::memcpy(&held, bus->iwram_ptr() + 0x9C, 4);
    v.seen = static_cast<std::uint16_t>(held & 0x3FF);
    return v;
}

// Keeps each newly pressed button down until the game has seen it (in the
// city) or for two frames (elsewhere). `want` and the result are active high.
std::uint16_t stretch_presses(std::uint16_t want, const PadView& pad) {
    static std::uint16_t latched = 0;
    static int age[10] = {};
    for (int bit = 0; bit < 10; ++bit) {
        const std::uint16_t mask = static_cast<std::uint16_t>(1u << bit);
        if ((want & mask) && !(latched & mask)) { latched |= mask; age[bit] = 0; }
        if (!(latched & mask)) continue;
        ++age[bit];
        const bool seen = pad.city ? (pad.seen & mask) != 0 : age[bit] >= 2;
        if (!(want & mask) && (seen || age[bit] > 12)) latched &= static_cast<std::uint16_t>(~mask);
    }
    return static_cast<std::uint16_t>(want | latched);
}

// Weapon switching is "hold Select, press L or R". One key press plays the
// whole chord, each step waiting for the game to have seen the one before:
// Select, then Select with the shoulder button, then the shoulder button is
// let go first (alone it means "enter car" or "strafe"), then Select.
int g_cycle_step = 0;       // 0 idle, 1 Select, 2 Select+shoulder, 3 Select, 4 released
int g_cycle_dir = 0;        // -1 previous (L), +1 next (R)
int g_cycle_wait = 0;       // frames spent in the current step
int g_cycle_queued = 0;     // one more press made while a chord was playing
bool g_cycle_key_was_down = false;

std::uint16_t weapon_chord(const PadView& pad) {
    if (g_cycle_step == 0) {
        if (!g_cycle_queued) return 0;
        g_cycle_dir = g_cycle_queued; g_cycle_queued = 0;
        g_cycle_step = 1; g_cycle_wait = 0;
    }
    const std::uint16_t select = 1u << kSelect;
    const std::uint16_t shoulder = static_cast<std::uint16_t>(1u << (g_cycle_dir < 0 ? kL : kR));
    auto advance = [&](bool done, int limit) {
        if (done || ++g_cycle_wait > limit) { ++g_cycle_step; g_cycle_wait = 0; }
    };
    switch (g_cycle_step) {
        case 1: advance(pad.seen & select, 12); return select;
        case 2: advance((pad.seen & shoulder) && (pad.seen & select), 12);
                return static_cast<std::uint16_t>(select | shoulder);
        case 3: advance(!(pad.seen & shoulder), 10); return select;
        default: advance(!(pad.seen & select), 10);
                 if (g_cycle_step > 4) g_cycle_step = 0;
                 return 0;
    }
}

// Debug TCP (headless testing): buttons to hold for a number of frames, and
// weapon-switch presses to queue, as if they came from the keyboard.
std::atomic<int> g_test_buttons{0}, g_test_frames{0}, g_test_cycle{0};

std::uint16_t input_frame(const gbarecomp::TouchFrameInfo*) {
    std::uint16_t keys = 0x03FF;
    // In the game's menus the mouse points and clicks; in the city its
    // buttons are attack and sprint.
    const bool in_menu = g_settings.mouse && gta_menu_mouse(&keys);
    const PadView pad = pad_view();
    std::uint16_t want = 0;                 // active high
    auto press = [&](int bit) { want |= static_cast<std::uint16_t>(1u << bit); };
    if (g_test_frames.load() > 0) { --g_test_frames; want |= static_cast<std::uint16_t>(g_test_buttons.load()); }
    if (const int c = g_test_cycle.exchange(0)) g_cycle_queued = c;

    // Headless runs never open a window, and no SDL call is made then.
    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        remember_display_mode();
        // The pointer is for the game's menus; in the city it would only sit
        // on top of the picture. (The Esc menu shows it again by itself.)
        static int cursor_shown = -1;
        if (cursor_shown != (in_menu ? 1 : 0)) {
            cursor_shown = in_menu ? 1 : 0;
            SDL_ShowCursor(in_menu ? SDL_ENABLE : SDL_DISABLE);
        }
        if (SDL_GetKeyboardFocus()) {
            if (g_settings.layout) {
                const Uint8* k = SDL_GetKeyboardState(nullptr);
                if (k[SDL_SCANCODE_W]) press(kUp);
                if (k[SDL_SCANCODE_S]) press(kDown);
                if (k[SDL_SCANCODE_A]) press(kLeft);
                if (k[SDL_SCANCODE_D]) press(kRight);
                if (k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_J]) press(kA);   // attack / accelerate
                if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_K]) press(kB);  // sprint / reverse
                if (k[SDL_SCANCODE_SPACE]) press(kR);                        // strafe / handbrake
                if (k[SDL_SCANCODE_F]) press(kL);                            // enter / exit car
                if (k[SDL_SCANCODE_C]) { press(kA); press(kB); }             // jump / vehicle weapon
                if (k[SDL_SCANCODE_H]) { press(kUp); press(kR); }            // horn / siren
                if (k[SDL_SCANCODE_TAB]) press(kSelect);

                const bool q = k[SDL_SCANCODE_Q], e = k[SDL_SCANCODE_E];
                if ((q || e) && !g_cycle_key_was_down) g_cycle_queued = q ? -1 : 1;
                g_cycle_key_was_down = q || e;
            }
            if (g_settings.mouse && !in_menu) {
                const Uint32 buttons = SDL_GetMouseState(nullptr, nullptr);
                if (buttons & SDL_BUTTON_LMASK) press(kA);
                if (buttons & SDL_BUTTON_RMASK) press(kB);
            }
        }
    }
    want = stretch_presses(want, pad);
    if (pad.city) {
        // While a chord plays, the shoulder buttons belong to it alone.
        const std::uint16_t chord = weapon_chord(pad);
        if (g_cycle_step) want &= static_cast<std::uint16_t>(~((1u << kL) | (1u << kR) | (1u << kSelect)));
        want |= chord;
    } else {
        g_cycle_step = 0; g_cycle_queued = 0;
    }
    return static_cast<std::uint16_t>(keys & ~want);
}

// ── settings-menu rows ──────────────────────────────────────────────────────
const char* const kFilterChoices[] = {"Sharp pixels", "Smooth", "Sharp-bilinear"};

const char* const kFrameRateChoices[] = {"Match display", "60", "120", "180", "240"};
const char* const kResolutionChoices[] = {"Window (sharpest)", "75%", "50%", "33%", "25% (chunky)"};

const RecompRuntimeUiItem kItems[] = {
    {kKeyNative, "Graphics", "Native renderer",
     "Draw the city on the PC at the window's own resolution and shape (16:9, 21:9, "
     "anything): wider windows see more of the streets. Turn off for the untouched "
     "240x160 picture.",
     RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr},
    {kKeyResolution, "Graphics", "Render resolution",
     "Resolution of the native city view relative to the window. Lower values are "
     "faster and give a chunkier, more retro look.",
     RECOMP_RUNTIME_UI_CHOICE, 0, 4, 1, kResolutionChoices, 5, nullptr},
    {kKeySmooth, "Graphics", "Smooth motion",
     "The game moves the world 15 times a second. With this on, the native "
     "renderer shows the steps in between, so scrolling, people and traffic "
     "move smoothly; the picture is then one step (1/15 s) behind the game. "
     "Turn off for the original stepping.",
     RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr},
    {kKeyFrameRate, "Graphics", "Frame rate",
     "How often the city is drawn, as a multiple of the game's 60 frames a second. "
     "Match display picks the multiple nearest your monitor (120 on a 144 Hz screen). "
     "Experimental above 60: it does not hold its rate yet and can hitch. Needs Smooth motion.",
     RECOMP_RUNTIME_UI_CHOICE, 0, 4, 1, kFrameRateChoices, 5, nullptr},
    {kKeyLayout, "Controls", "WASD layout",
     "W/A/S/D move and steer. Left Ctrl or J: attack / accelerate. Left Shift or K: "
     "sprint / reverse. Space: strafe / handbrake. F: enter or exit a car. C: jump / "
     "vehicle weapon. Q and E: previous / next weapon. H: horn or siren. Tab: Select. "
     "The original bindings (arrows, X, Z, ...) keep working.",
     RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr},
    {kKeyMouse, "Controls", "Mouse buttons",
     "In the city: left click attacks / accelerates, right click sprints / reverses. "
     "In the game's menus: point at an entry to select it, left click to choose it, "
     "right click to go back; elsewhere a click continues.",
     RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr},
    {kKeyFilter, "Graphics", "Scaling filter (next launch)",
     "How the picture is scaled up. Sharp pixels is the raw look, Smooth blurs, "
     "Sharp-bilinear keeps pixels crisp without shimmer at odd sizes. Takes effect "
     "the next time the game starts.",
     RECOMP_RUNTIME_UI_CHOICE, 0, 2, 1, kFilterChoices, 3, nullptr},
};

int ui_get(const char* key, int* out) {
    if (!std::strcmp(key, kKeyLayout)) *out = g_settings.layout;
    else if (!std::strcmp(key, kKeyMouse)) *out = g_settings.mouse;
    else if (!std::strcmp(key, kKeyFilter)) *out = g_settings.filter;
    else if (!std::strcmp(key, kKeyNative)) *out = gta_scene_settings()->native;
    else if (!std::strcmp(key, kKeyResolution)) *out = gta_scene_settings()->resolution;
    else if (!std::strcmp(key, kKeySmooth)) *out = gta_scene_settings()->smooth;
    else if (!std::strcmp(key, kKeyFrameRate)) *out = gta_scene_settings()->frame_rate;
    else return 0;
    return 1;
}

int ui_set(const char* key, int value) {
    if (!std::strcmp(key, kKeyLayout)) g_settings.layout = value != 0;
    else if (!std::strcmp(key, kKeyMouse)) g_settings.mouse = value != 0;
    else if (!std::strcmp(key, kKeyFilter)) g_settings.filter = clamp(value, 0, 2);
    else if (!std::strcmp(key, kKeyNative)) gta_scene_settings()->native = value != 0;
    else if (!std::strcmp(key, kKeyResolution)) gta_scene_settings()->resolution = clamp(value, 0, 4);
    else if (!std::strcmp(key, kKeySmooth)) gta_scene_settings()->smooth = value != 0;
    else if (!std::strcmp(key, kKeyFrameRate)) gta_scene_settings()->frame_rate = clamp(value, 0, 4);
    else return 0;
    save_settings();
    return 1;
}

}  // namespace

void gta_pc_controls_startup_args(std::vector<std::string>* extra) {
    if (g_settings.fullscreen)
        extra->push_back("--fullscreen=" + std::to_string(g_settings.fullscreen));
    if (g_settings.filter == 1) { extra->push_back("--linear-filter"); extra->push_back("1"); }
    if (g_settings.filter == 2) { extra->push_back("--sharp-filter"); extra->push_back("1"); }
}

void gta_pc_controls_install(gbarecomp::RunOptions* opts, const char* argv0) {
    load_settings(argv0);
    opts->input_frame = input_frame;
    opts->touch_pad_default = 0;          // no on-screen pad on desktop
    opts->ui_extra_items = kItems;
    opts->ui_extra_item_count = sizeof(kItems) / sizeof(kItems[0]);
    opts->ui_get = ui_get;
    opts->ui_set = ui_set;
}

void gta_pc_controls_debug_buttons(int buttons, int frames) {
    g_test_buttons = buttons; g_test_frames = frames;
}

void gta_pc_controls_debug_cycle(int direction) { g_test_cycle = direction; }
