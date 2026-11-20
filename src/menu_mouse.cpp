// Mouse pointing in the game's own menus.
//
// The menus draw their text as pixels into a regular background layer (the
// front-most one that is switched on; the pause menu shares it with its
// backdrop), one entry per line, in yellow, with the selected entry in a
// brighter yellow. Which entry is selected is a local variable of
// each menu routine, so instead of reading it, the lines and the highlighted
// one are found in the layer itself. Pointing at a line then presses Up or
// Down until that line is the highlighted one; a left click presses A once
// it is, a right click presses B.
//
// Screens without a recognisable list (dialogue, title, name entry) still get
// left click = A and right click = B.
#include "menu_mouse.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gba_bus.h"
#include "gba_io.h"
#include "runtime_bus_bridge.h"

namespace {

std::uint16_t rd16(const std::uint8_t* p) {
    std::uint16_t v; std::memcpy(&v, p, 2); return v;
}

struct Line { int top, bottom, left, right, bright; };

struct Menu {
    bool screen = false;        // a menu-like screen (not the city view)
    std::vector<Line> lines;    // the list's entries, top to bottom
    int selected = -1;          // index into lines, -1 = not known
};

// How yellow-and-bright (0..255) each pixel of the text layer is; 0 for
// anything that is not menu text (clear pixels, outlines, backdrop art).
bool read_text_layer(gba::GbaBus* bus, std::uint8_t* out /* 240*160 */) {
    const std::uint16_t dispcnt = bus->io().read16(0x000);
    const int mode = dispcnt & 7u;
    if (mode > 1) return false;
    int bg = -1, best_prio = 4;
    for (int i = 0; i < (mode == 0 ? 4 : 2); ++i) {     // front-most regular layer that is on
        if (!(dispcnt & (0x100 << i))) continue;
        const int prio = bus->io().read16(0x008 + i * 2) & 3;
        if (prio < best_prio) { best_prio = prio; bg = i; }
    }
    if (bg < 0) return false;
    const std::uint16_t cnt = bus->io().read16(0x008 + bg * 2);
    const int hofs = bus->io().read16(0x010 + bg * 4) & 0x1FF, vofs = bus->io().read16(0x012 + bg * 4) & 0x1FF;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* pal = bus->pal_ptr();
    const std::uint32_t char_base = ((cnt >> 2) & 3) * 0x4000u, screen_base = ((cnt >> 8) & 31) * 0x800u;
    const bool bpp8 = cnt & 0x80;
    const int map_w = (cnt & 0x4000) ? 512 : 256, map_h = (cnt & 0x8000) ? 512 : 256;
    std::uint8_t level[256];
    for (int i = 0; i < 256; ++i) {
        const std::uint16_t c = rd16(pal + i * 2);
        const int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        const bool yellow = r >= 18 && g >= 12 && b <= 12 && r >= g;
        level[i] = yellow ? static_cast<std::uint8_t>((r + g) * 4) : 0;
    }
    for (int y = 0; y < 160; ++y) {
        for (int x = 0; x < 240; ++x) {
            const int mx = (x + hofs) & (map_w - 1), my = (y + vofs) & (map_h - 1);
            std::uint32_t block = screen_base;
            if (mx >= 256) block += 0x800;
            if (my >= 256) block += (map_w == 512) ? 0x1000 : 0x800;
            const std::uint16_t e = rd16(vram + block + (((my & 255) >> 3) * 32 + ((mx & 255) >> 3)) * 2);
            int tx = mx & 7, ty = my & 7;
            if (e & 0x400) tx = 7 - tx;
            if (e & 0x800) ty = 7 - ty;
            const std::uint32_t t = e & 0x3FF;
            int index = 0;
            if (bpp8) {
                const std::uint32_t at = char_base + t * 64 + ty * 8 + tx;
                if (at < 0x10000) index = vram[at];
            } else {
                const std::uint32_t at = char_base + t * 32 + ty * 4 + (tx >> 1);
                if (at < 0x10000) {
                    index = (tx & 1) ? vram[at] >> 4 : vram[at] & 15;
                    if (index) index += (e >> 12) * 16;
                }
            }
            out[y * 240 + x] = level[index];
        }
    }
    return true;
}

Menu detect(gba::GbaBus* bus) {
    Menu m;
    const std::uint16_t dispcnt = bus->io().read16(0x000);
    const bool city = (dispcnt & 7u) == 1u && (dispcnt & 0x0500) == 0x0500;
    m.screen = !city && !(dispcnt & 0x80);
    static std::uint8_t px[240 * 160];
    if (!m.screen || !read_text_layer(bus, px)) return m;

    // Horizontal bands of text. Only the yellow fill of the letters counts:
    // their dark outline and drop shadow run into the line below.
    constexpr int kLit = 1;
    std::vector<Line> bands;
    int start = -1;
    for (int y = 0; y <= 160; ++y) {
        int count = 0;
        if (y < 160) for (int x = 0; x < 240; ++x) count += px[y * 240 + x] >= kLit;
        if (count >= 4 && start < 0) start = y;
        if (count < 4 && start >= 0) {
            if (y - start >= 7 && y - start <= 24) {
                Line b{start, y, 240, 0, 0};
                for (int yy = start; yy < y; ++yy)
                    for (int x = 0; x < 240; ++x)
                        if (const int v = px[yy * 240 + x]; v >= kLit) {
                            b.left = std::min(b.left, x); b.right = std::max(b.right, x + 1);
                        }
                // Mean brightness of the fill.
                long sum = 0, n = 0;
                for (int yy = start; yy < y; ++yy)
                    for (int x = b.left; x < b.right; ++x)
                        if (const int v = px[yy * 240 + x]; v >= kLit) { sum += v; ++n; }
                b.bright = n ? static_cast<int>(sum / n) : 0;
                bands.push_back(b);
            }
            start = -1;
        }
    }
    // The list: the longest run of bands at an even pitch. Titles and
    // prompts sit apart from it.
    int best_from = 0, best_len = 0;
    for (std::size_t i = 0; i < bands.size(); ++i) {
        std::size_t j = i;
        int pitch = 0;
        while (j + 1 < bands.size()) {
            // Measured between bottom edges: tall letters move the top edge.
            const int step = bands[j + 1].bottom - bands[j].bottom;
            if (step > 30 || (pitch && std::abs(step - pitch) > 2)) break;
            pitch = pitch ? pitch : step;
            ++j;
        }
        const int len = static_cast<int>(j - i) + 1;
        if (len > best_len) { best_len = len; best_from = static_cast<int>(i); }
    }
    if (best_len < 2) return m;
    m.lines.assign(bands.begin() + best_from, bands.begin() + best_from + best_len);
    // The selected entry is the one clearly brighter than the rest.
    int top = 0, second = -1;
    for (int i = 1; i < best_len; ++i) {
        if (m.lines[i].bright > m.lines[top].bright) { second = top; top = i; }
        else if (second < 0 || m.lines[i].bright > m.lines[second].bright) second = i;
    }
    if (second >= 0 && m.lines[top].bright - m.lines[second].bright >= 5) m.selected = top;
    return m;
}

// ---- pointer --------------------------------------------------------------------
std::atomic<int> g_test_x{-1}, g_test_y{-1}, g_test_left{0}, g_test_right{0};
std::atomic<bool> g_test_pointer{false};

struct Pointer { bool valid = false; int x = 0, y = 0; bool moved = false; int left = 0, right = 0; };

Pointer read_pointer() {
    static int last_x = -1000, last_y = -1000;
    static bool was_left = false, was_right = false;
    Pointer p;
    if (g_test_pointer.load(std::memory_order_relaxed)) {
        p.valid = true;
        p.x = g_test_x.load(); p.y = g_test_y.load();
        p.left = g_test_left.exchange(0); p.right = g_test_right.exchange(0);
    } else {
        if (!SDL_WasInit(SDL_INIT_VIDEO)) return p;
        SDL_Window* w = SDL_GetMouseFocus();
        if (!w) { was_left = was_right = false; return p; }
        int mx = 0, my = 0, ww = 0, wh = 0;
        const Uint32 buttons = SDL_GetMouseState(&mx, &my);
        SDL_GetWindowSize(w, &ww, &wh);
        if (ww <= 0 || wh <= 0) return p;
        // The menus are shown as the 240x160 picture, centred and as large as fits.
        const float scale = std::min(ww / 240.0f, wh / 160.0f);
        p.valid = true;
        p.x = static_cast<int>((mx - (ww - 240.0f * scale) * 0.5f) / scale);
        p.y = static_cast<int>((my - (wh - 160.0f * scale) * 0.5f) / scale);
        const bool left = buttons & SDL_BUTTON_LMASK, right = buttons & SDL_BUTTON_RMASK;
        p.left = left && !was_left; p.right = right && !was_right;
        was_left = left; was_right = right;
    }
    p.moved = p.x != last_x || p.y != last_y;
    last_x = p.x; last_y = p.y;
    return p;
}

int line_under(const Menu& m, const Pointer& p) {
    for (std::size_t i = 0; i < m.lines.size(); ++i) {
        const Line& l = m.lines[i];
        if (p.y >= l.top - 2 && p.y < l.bottom + 2 && p.x >= l.left - 8 && p.x < l.right + 8)
            return static_cast<int>(i);
    }
    return -1;
}

// One button press at a time: held a few frames, then a pause while the menu
// reacts (its highlight moves a few frames later).
enum : int { kA = 0, kB = 1, kUp = 6, kDown = 7 };
constexpr int kHold = 3, kSettle = 9;
int g_tap_bit = -1, g_tap_timer = 0;
int g_target = -1;          // entry the pointer asked for
bool g_click = false;       // press A once the target is selected
int g_budget = 0;           // presses left before giving up on a target
int g_was_screen = 0;       // frames the menu screen has been up

void tap(int bit) { g_tap_bit = bit; g_tap_timer = kHold + kSettle; }

// The wheel only arrives as events; a watch counts the notches.
std::atomic<int> g_wheel{0};
int SDLCALL wheel_watch(void*, SDL_Event* e) {
    if (e->type == SDL_MOUSEWHEEL) {
        const int y = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
        g_wheel += y;
    }
    return 1;
}

}  // namespace

int gta_mouse_wheel_take() {
    static bool watching = false;
    if (!watching && SDL_WasInit(SDL_INIT_VIDEO)) { SDL_AddEventWatch(wheel_watch, nullptr); watching = true; }
    return g_wheel.exchange(0);
}

void gta_mouse_wheel_inject(int notches) { g_wheel += notches; }

bool gta_menu_mouse(std::uint16_t* keys) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return false;
    const Menu m = detect(bus);
    Pointer p = read_pointer();
    static int wheel = 0;                   // notches not yet turned into presses
    wheel += gta_mouse_wheel_take();
    if (!m.screen) { g_target = -1; g_click = false; g_was_screen = 0; g_tap_timer = 0; wheel = 0; return false; }
    // Clicks made while the screen was still appearing are not for it.
    if (++g_was_screen < 12) { p.left = p.right = 0; wheel = 0; }
    wheel = std::clamp(wheel, -4, 4);

    if (g_tap_timer > 0) {
        if (g_tap_timer > kSettle) *keys &= static_cast<std::uint16_t>(~(1u << g_tap_bit));
        --g_tap_timer;
        return true;
    }
    // The wheel steps through the list: one Up or Down per notch.
    if (wheel != 0) {
        g_target = -1; g_click = false;
        tap(wheel > 0 ? kUp : kDown);
        wheel += wheel > 0 ? -1 : 1;
        return true;
    }
    if (!p.valid) return true;
    if (p.right) { g_target = -1; g_click = false; tap(kB); return true; }

    const int under = m.selected >= 0 ? line_under(m, p) : -1;
    if (under >= 0 && (p.moved || p.left)) {
        if (under != g_target) g_budget = 12;
        g_target = under;
        if (p.left) g_click = true;
    } else if (p.left) {
        // Not on a list entry: a plain confirm (dialogue, title, prompts).
        g_target = -1; g_click = false;
        tap(kA);
        return true;
    }
    if (g_target < 0 || m.selected < 0 || g_target >= static_cast<int>(m.lines.size())) {
        if (m.selected < 0) { g_target = -1; g_click = false; }
        return true;
    }
    if (g_target != m.selected) {
        if (g_budget-- <= 0) { g_target = -1; g_click = false; return true; }
        tap(g_target > m.selected ? kDown : kUp);
    } else if (g_click) {
        g_click = false; g_target = -1;
        tap(kA);
    }
    return true;
}

static bool find_int(const char* request, const char* key, long long* out) {
    const char* p = std::strstr(request, key);
    if (!p) return false;
    p = std::strchr(p + std::strlen(key), ':');
    if (!p) return false;
    *out = std::strtoll(p + 1, nullptr, 0);
    return true;
}

int gta_menu_tcp_command(const char* request,
                         void (*write)(void* ctx, const char* data, std::size_t len),
                         void* write_ctx) {
    if (std::strstr(request, "\"gta_pointer\"")) {
        long long x = -1, y = -1, l = 0, r = 0;
        find_int(request, "\"x\"", &x); find_int(request, "\"y\"", &y);
        find_int(request, "\"l\"", &l); find_int(request, "\"r\"", &r);
        g_test_x = static_cast<int>(x); g_test_y = static_cast<int>(y);
        g_test_left += static_cast<int>(l); g_test_right += static_cast<int>(r);
        g_test_pointer = true;
        const char* reply = "{\"ok\":true}";
        write(write_ctx, reply, std::strlen(reply));
        return 1;
    }
    if (std::strstr(request, "\"gta_menu\"")) {
        gba::GbaBus* bus = gbarecomp::active_bus();
        Menu m;
        if (bus) m = detect(bus);
        char buf[1024];
        int n = std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"screen\":%d,\"selected\":%d,\"lines\":[",
                              m.screen ? 1 : 0, m.selected);
        for (std::size_t i = 0; i < m.lines.size() && n < 900; ++i)
            n += std::snprintf(buf + n, sizeof(buf) - n, "%s[%d,%d,%d,%d,%d]", i ? "," : "",
                               m.lines[i].left, m.lines[i].top, m.lines[i].right, m.lines[i].bottom,
                               m.lines[i].bright);
        n += std::snprintf(buf + n, sizeof(buf) - n, "]}");
        write(write_ctx, buf, static_cast<std::size_t>(n));
        return 1;
    }
    return 0;
}
