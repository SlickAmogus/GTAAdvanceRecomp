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
#include "scene_host.h"

#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "recomp_runtime_ui.h"
#include "touch_input.h"

namespace {

constexpr char kKeyLayout[] = "gta.pc_layout";
constexpr char kKeyMouse[]  = "gta.mouse_buttons";
constexpr char kKeyFilter[] = "gta.filter";
constexpr char kKeyNative[] = "gta.native_renderer";
constexpr char kKeyResolution[] = "gta.resolution";

struct Settings {
    int layout = 1;         // WASD layout on
    int mouse = 1;          // mouse buttons attack / sprint
    int fullscreen = 0;     // 0 windowed, 1 borderless, 2 exclusive (remembered)
    int filter = 0;         // 0 sharp pixels, 1 smooth, 2 sharp-bilinear
} g_settings;

std::string g_settings_path;

int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void load_settings(const char* argv0) {
    // Beside the executable. (No SDL here: this runs before SDL_Init, and a
    // headless run never initialises SDL at all.)
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::absolute(
        std::filesystem::path(argv0 ? argv0 : "."), ec).parent_path();
    g_settings_path = (dir / "gta_pc.ini").string();
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
    }
    std::fclose(f);
}

void save_settings() {
    std::FILE* f = std::fopen(g_settings_path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "pc_layout = %d\nmouse_buttons = %d\nfullscreen = %d\nfilter = %d\n",
                 g_settings.layout, g_settings.mouse, g_settings.fullscreen, g_settings.filter);
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

// Weapon switching is "hold Select, press L or R". One key press plays the
// whole chord: Select alone for a few frames, then Select with the shoulder
// button, then a gap so repeated presses register separately.
int g_cycle_dir = 0;        // -1 previous (L), +1 next (R)
int g_cycle_timer = 0;
bool g_cycle_key_was_down = false;
constexpr int kCycleLead = 3, kCyclePress = 5, kCycleGap = 4;

std::uint16_t input_frame(const gbarecomp::TouchFrameInfo*) {
    std::uint16_t keys = 0x03FF;
    // Headless runs never open a window, and no SDL call is made then.
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return keys;
    remember_display_mode();
    if (!SDL_GetKeyboardFocus()) return keys;
    auto press = [&](int bit) { keys &= static_cast<std::uint16_t>(~(1u << bit)); };

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
        if ((q || e) && !g_cycle_key_was_down && g_cycle_timer == 0) {
            g_cycle_dir = q ? -1 : 1;
            g_cycle_timer = kCycleLead + kCyclePress + kCycleGap;
        }
        g_cycle_key_was_down = q || e;
    }
    if (g_cycle_timer > 0) {
        --g_cycle_timer;
        if (g_cycle_timer >= kCycleGap) {
            press(kSelect);
            if (g_cycle_timer < kCycleGap + kCyclePress) press(g_cycle_dir < 0 ? kL : kR);
        }
    }
    if (g_settings.mouse) {
        const Uint32 buttons = SDL_GetMouseState(nullptr, nullptr);
        if (buttons & SDL_BUTTON_LMASK) press(kA);
        if (buttons & SDL_BUTTON_RMASK) press(kB);
    }
    return keys;
}

// ── settings-menu rows ──────────────────────────────────────────────────────
const char* const kFilterChoices[] = {"Sharp pixels", "Smooth", "Sharp-bilinear"};

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
    {kKeyLayout, "Controls", "WASD layout",
     "W/A/S/D move and steer. Left Ctrl or J: attack / accelerate. Left Shift or K: "
     "sprint / reverse. Space: strafe / handbrake. F: enter or exit a car. C: jump / "
     "vehicle weapon. Q and E: previous / next weapon. H: horn or siren. Tab: Select. "
     "The original bindings (arrows, X, Z, ...) keep working.",
     RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr},
    {kKeyMouse, "Controls", "Mouse buttons",
     "Left click: attack / accelerate. Right click: sprint / reverse.",
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
    else return 0;
    return 1;
}

int ui_set(const char* key, int value) {
    if (!std::strcmp(key, kKeyLayout)) g_settings.layout = value != 0;
    else if (!std::strcmp(key, kKeyMouse)) g_settings.mouse = value != 0;
    else if (!std::strcmp(key, kKeyFilter)) g_settings.filter = clamp(value, 0, 2);
    else if (!std::strcmp(key, kKeyNative)) gta_scene_settings()->native = value != 0;
    else if (!std::strcmp(key, kKeyResolution)) gta_scene_settings()->resolution = clamp(value, 0, 4);
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
