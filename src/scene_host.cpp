// Native renderer host glue: at present time, replaces the game's ground
// (BG2) and building (BG0) layers with the native scene drawn at the window's
// resolution and shape, and composites the game's sprites and HUD over it.
//
// Layer order in the game's own picture (video mode 1), back to front:
//   BG2 ground (priority 3), world sprites (OBJ priority 3), BG0 buildings
//   (priority 2), OBJ priority 2 and 1, BG1 HUD text and radar disc
//   (priority 0), HUD sprites (OBJ priority 0).
// The same order is kept here. Nothing in guest memory is changed: the game
// still draws its own layers, they are simply not shown.
#include "scene_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "gba_bus.h"
#include "gba_io.h"
#include "mod_function_hooks.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "scene/scene.h"

namespace {

GtaSceneSettings g_settings;

// ---- widescreen: the game's visible rectangle ---------------------------------
// The per-frame camera code sets, from the camera position x and height z:
//   IWRAM 0x50 / 0x58   left / right edge in whole world units (x -/+ z)
// People and cars only get a sprite inside that rectangle, which is exactly
// 240 screen columns, and sprite columns are measured from its left edge:
//   column = (world_x - left) * scale        scale = IWRAM 0x60 (16.16)
// For a wider picture the half-width is scaled by the picture's width over
// 240 columns (plus a margin so things appear outside the edge, not on it).
// Sprite columns then start at the widened left edge; world_sprite_shift()
// gives the compositor the distance back to the game's own column 0.
constexpr char kHookId[] = "gta.widescreen";
// The three functions that run immediately after the rectangle is computed.
constexpr std::uint32_t kAfterRectFns[] = {0x08009832u, 0x0800A0C0u, 0x08010110u};
constexpr std::uint32_t kSpriteRightLimit = 0x08014650u;    // cmp r0, #0xF0
std::atomic<float> g_wide_factor{1.0f};     // picture width / 240 columns, >= 1

void wr32(std::uint8_t* p, std::int32_t v) { std::memcpy(p, &v, 4); }
std::int32_t rds32(const std::uint8_t* p) { std::int32_t v; std::memcpy(&v, p, 4); return v; }

// Columns between the rectangle's current left edge and the game's own.
float world_sprite_shift(const std::uint8_t* iw) {
    const std::int32_t natural_left = (rds32(iw + 0x14) >> 16) - (rds32(iw + 0x1C) >> 16);
    return (natural_left - rds32(iw + 0x50)) * (rds32(iw + 0x60) / 65536.0f);
}

int widen_hook(std::uint32_t, int, ArmCpuState*) {
    const float factor = g_wide_factor.load(std::memory_order_relaxed);
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!g_settings.native || factor <= 1.001f || !bus) return 0;
    if ((bus->io().read16(0x000) & 7u) != 1u) return 0;         // city view only
    std::uint8_t* iw = bus->iwram_ptr();
    const std::int32_t x = rds32(iw + 0x14) >> 16, z = rds32(iw + 0x1C) >> 16;
    if (z < 16 || z > 2000) return 0;
    // Recomputed from the camera every time, so running twice changes nothing.
    const std::int32_t half = static_cast<std::int32_t>(std::ceil(z * factor)) + 12;
    wr32(iw + 0x50, x - half);
    wr32(iw + 0x58, x + half);
    return 0;   // observed only: the game's function runs as usual
}

// The sprite right-edge limit: the widened rectangle's width in columns.
int right_limit_override(std::uint32_t pc, std::uint32_t, std::uint32_t* out) {
    if (pc != kSpriteRightLimit) return 0;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!g_settings.native || !bus) return 0;
    const std::uint8_t* iw = bus->iwram_ptr();
    const float width = (rds32(iw + 0x58) - rds32(iw + 0x50)) * (rds32(iw + 0x60) / 65536.0f);
    if (width <= 244.0f || width > 1000.0f) return 0;
    *out = static_cast<std::uint32_t>(width);
    return 1;
}

const int g_registered =
    gba_mod_register_function_entry_plugin(kHookId, kAfterRectFns[0], 1, widen_hook) &&
    gba_mod_register_function_entry_plugin(kHookId, kAfterRectFns[1], 1, widen_hook) &&
    gba_mod_register_function_entry_plugin(kHookId, kAfterRectFns[2], 1, widen_hook);

void frame_start(const gbarecomp::ExtendedViewFrameInfo*) {
    // The runtime disables every entry hook when it activates mod plugins, so
    // the hook is (re)armed from the per-frame callback.
    if (g_registered) gba_mod_set_function_hook_enabled(kHookId, 1);
    g_runtime_thumb_alu_imm_override = right_limit_override;
}
std::vector<std::uint32_t> g_frame;
std::vector<float> g_depth;
double g_phase_ms[4];   // ground, world sprites, buildings, HUD (debug timing)

std::uint16_t rd16(const std::uint8_t* p) {
    std::uint16_t v; std::memcpy(&v, p, 2); return v;
}

struct Layout {
    int w = 0, h = 0;
    float scale = 1.0f;     // output pixels per GBA pixel
    float box_x = 0;        // left edge of the centred 240-wide area
};

std::uint32_t colour(std::uint16_t c) {
    const std::uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}

// Where HUD pieces go on a window wider than 3:2. The radar (top left) keeps
// to the left edge, the status block (top right) and the district name
// (bottom right) to the right edge; everything else stays centred.
enum class Anchor { Centre, Left, Right };

Anchor hud_anchor(int x0, int x1, int y0, int y1) {
    if (x1 <= 80 && y1 <= 64) return Anchor::Left;
    if (x0 >= 120 && (y1 <= 48 || y0 >= 136)) return Anchor::Right;
    return Anchor::Centre;
}

float anchor_x(const Layout& l, Anchor a) {
    switch (a) {
        case Anchor::Left: return 0.0f;
        case Anchor::Right: return l.w - 240.0f * l.scale;
        default: return l.box_x;
    }
}

void fill(const Layout& l, float x0, float y0, float x1, float y1, std::uint32_t c) {
    const int ax = std::max(0, static_cast<int>(x0 + 0.5f)), bx = std::min(l.w, static_cast<int>(x1 + 0.5f));
    const int ay = std::max(0, static_cast<int>(y0 + 0.5f)), by = std::min(l.h, static_cast<int>(y1 + 0.5f));
    if (bx <= ax) return;
    for (int y = ay; y < by; ++y) {
        std::uint32_t* row = g_frame.data() + static_cast<std::size_t>(y) * l.w;
        std::fill(row + ax, row + bx, c);
    }
}

std::uint32_t blend(std::uint32_t src, std::uint32_t dst, unsigned eva, unsigned evb) {
    auto channel = [&](int shift) {
        const unsigned v = (((src >> shift) & 255u) * eva + ((dst >> shift) & 255u) * evb) >> 4;
        return std::min(v, 255u) << shift;
    };
    return 0xFF000000u | channel(16) | channel(8) | channel(0);
}

// Sprites of the given priorities, sampled per output pixel so rotated and
// scaled sprites (cars) keep clean edges at any window size.
void draw_objects(const Layout& l, gba::GbaBus* bus, std::uint16_t dispcnt, int prio_lo, int prio_hi) {
    const float world_shift = world_sprite_shift(bus->iwram_ptr());
    if (!(dispcnt & 0x1000)) return;
    const std::uint8_t* oam = bus->oam_ptr();
    const std::uint8_t* vram = bus->vram_ptr() + 0x10000;
    const std::uint8_t* pal = bus->pal_ptr() + 0x200;
    const bool one_d = dispcnt & 0x40;
    const std::uint16_t bldalpha = bus->io().read16(0x052);
    const unsigned eva = std::min(bldalpha & 31u, 16u), evb = std::min((bldalpha >> 8) & 31u, 16u);
    static const std::uint8_t kSize[3][4][2] = {
        {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
        {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
        {{8, 16}, {8, 32}, {16, 32}, {32, 64}}};
    for (int pass = prio_hi; pass >= prio_lo; --pass) {
        for (int i = 127; i >= 0; --i) {
            const std::uint16_t a0 = rd16(oam + i * 8), a1 = rd16(oam + i * 8 + 2), a2 = rd16(oam + i * 8 + 4);
            if (((a2 >> 10) & 3) != pass) continue;
            const bool affine = a0 & 0x100;
            if (!affine && (a0 & 0x200)) continue;          // hidden
            const int mode = (a0 >> 10) & 3;
            if (mode == 2) continue;                        // window sprite
            const int shape = a0 >> 14;
            if (shape == 3) continue;
            const int w = kSize[shape][a1 >> 14][0], h = kSize[shape][a1 >> 14][1];
            const bool dbl = affine && (a0 & 0x200);
            const int bw = dbl ? w * 2 : w, bh = dbl ? h * 2 : h;
            int sy = a0 & 0xFF, sx = a1 & 0x1FF;
            if (sy >= 160) sy -= 256;
            // World sprites can sit well right of column 240 in widescreen;
            // only the last 64 values wrap round to "left of column 0".
            if (sx >= (pass == 0 ? 256 : 448)) sx -= 512;
            const bool bpp8 = a0 & 0x2000;
            const int tile = a2 & 0x3FF;
            const int palbank = bpp8 ? 0 : (a2 >> 12) * 16;
            float pa = 1, pb = 0, pc = 0, pd = 1;
            if (affine) {
                const int grp = ((a1 >> 9) & 31) * 32;
                pa = static_cast<std::int16_t>(rd16(oam + grp + 6)) / 256.0f;
                pb = static_cast<std::int16_t>(rd16(oam + grp + 14)) / 256.0f;
                pc = static_cast<std::int16_t>(rd16(oam + grp + 22)) / 256.0f;
                pd = static_cast<std::int16_t>(rd16(oam + grp + 30)) / 256.0f;
            }
            const bool flip_x = !affine && (a1 & 0x1000), flip_y = !affine && (a1 & 0x2000);
            const Anchor anchor = pass == 0 ? hud_anchor(sx, sx + bw, sy, sy + bh) : Anchor::Centre;
            const float ox = pass == 0 ? anchor_x(l, anchor) : l.box_x - world_shift * l.scale;
            // Screen box of the sprite's picture inside its bounds (in GBA
            // pixels from the bounds' corner). A rotated or shrunk sprite
            // covers much less than its bounds, which are often double size.
            float bx0 = 0, bx1 = static_cast<float>(bw), by0 = 0, by1 = static_cast<float>(bh);
            const float det = pa * pd - pb * pc;
            if (affine && std::fabs(det) > 1e-6f) {
                const float ex = (std::fabs(pd) * w + std::fabs(pb) * h) * 0.5f / std::fabs(det);
                const float ey = (std::fabs(pc) * w + std::fabs(pa) * h) * 0.5f / std::fabs(det);
                bx0 = std::max(bx0, bw * 0.5f - ex - 1); bx1 = std::min(bx1, bw * 0.5f + ex + 1);
                by0 = std::max(by0, bh * 0.5f - ey - 1); by1 = std::min(by1, bh * 0.5f + ey + 1);
            }
            const int x_begin = std::max(0, static_cast<int>(std::floor(ox + (sx + bx0) * l.scale)));
            const int x_end = std::min(l.w, static_cast<int>(std::ceil(ox + (sx + bx1) * l.scale)));
            const int y_begin = std::max(0, static_cast<int>(std::floor((sy + by0) * l.scale)));
            const int y_end = std::min(l.h, static_cast<int>(std::ceil((sy + by1) * l.scale)));
            std::uint32_t lut[256];
            const int colours = bpp8 ? 256 : 16;
            for (int c = 1; c < colours; ++c) lut[c] = colour(rd16(pal + (palbank + c) * 2));
            const int tiles_across = one_d ? w / 8 : 32;
            const int step = bpp8 ? 2 : 1;
            for (int y = y_begin; y < y_end; ++y) {
                const float gy = (y + 0.5f) / l.scale - sy;
                if (gy < 0 || gy >= bh) continue;
                std::uint32_t* row = g_frame.data() + static_cast<std::size_t>(y) * l.w;
                for (int x = x_begin; x < x_end; ++x) {
                    const float gx = (x + 0.5f - ox) / l.scale - sx;
                    if (gx < 0 || gx >= bw) continue;
                    int tx, ty;
                    if (affine) {
                        const float dx = gx - bw * 0.5f, dy = gy - bh * 0.5f;
                        const float fx = pa * dx + pb * dy + w * 0.5f, fy = pc * dx + pd * dy + h * 0.5f;
                        if (fx < 0 || fy < 0 || fx >= w || fy >= h) continue;
                        tx = static_cast<int>(fx); ty = static_cast<int>(fy);
                    } else {
                        tx = static_cast<int>(gx); ty = static_cast<int>(gy);
                        if (flip_x) tx = w - 1 - tx;
                        if (flip_y) ty = h - 1 - ty;
                    }
                    const int t = (tile + (ty / 8) * (one_d ? tiles_across * step : 32) + (tx / 8) * step) & 0x3FF;
                    int index;
                    if (bpp8) {
                        const int at = t * 32 + (ty & 7) * 8 + (tx & 7);
                        if (at >= 0x8000) continue;
                        index = vram[at];
                    } else {
                        const std::uint8_t b = vram[t * 32 + (ty & 7) * 4 + ((tx & 7) >> 1)];
                        index = (tx & 1) ? b >> 4 : b & 15;
                    }
                    if (!index) continue;
                    row[x] = mode == 1 ? blend(lut[index], row[x], eva, evb) : lut[index];
                }
            }
        }
    }
}

// A regular (text) background, used for BG1: HUD digits, radar disc, district
// name, messages. Tiles are grouped into horizontal runs and each run is
// anchored as a whole, so a line of text never tears apart.
void draw_text_bg(const Layout& l, gba::GbaBus* bus, int bg) {
    const std::uint16_t cnt = bus->io().read16(0x008 + bg * 2);
    const int hofs = bus->io().read16(0x010 + bg * 4) & 0x1FF, vofs = bus->io().read16(0x012 + bg * 4) & 0x1FF;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* pal = bus->pal_ptr();
    const std::uint32_t char_base = ((cnt >> 2) & 3) * 0x4000u, screen_base = ((cnt >> 8) & 31) * 0x800u;
    const bool bpp8 = cnt & 0x80;
    const int map_w = (cnt & 0x4000) ? 512 : 256, map_h = (cnt & 0x8000) ? 512 : 256;
    auto pixel = [&](int gx, int gy) -> int {         // palette index at a screen pixel, 0 = clear
        const int mx = (gx + hofs) & (map_w - 1), my = (gy + vofs) & (map_h - 1);
        std::uint32_t block = screen_base;
        if (mx >= 256) block += 0x800;
        if (my >= 256) block += (map_w == 512) ? 0x1000 : 0x800;
        const std::uint16_t e = rd16(vram + block + (((my & 255) >> 3) * 32 + ((mx & 255) >> 3)) * 2);
        int tx = mx & 7, ty = my & 7;
        if (e & 0x400) tx = 7 - tx;
        if (e & 0x800) ty = 7 - ty;
        const std::uint32_t t = e & 0x3FF;
        if (bpp8) {
            const std::uint32_t at = char_base + t * 64 + ty * 8 + tx;
            return at < 0x10000 ? vram[at] : 0;
        }
        const std::uint32_t at = char_base + t * 32 + ty * 4 + (tx >> 1);
        if (at >= 0x10000) return 0;
        const int index = (tx & 1) ? vram[at] >> 4 : vram[at] & 15;
        return index ? (e >> 12) * 16 + index : 0;
    };
    for (int band = 0; band < 160; band += 8) {
        // Occupied 8-pixel columns of this band, then runs separated by gaps
        // of three or more empty columns.
        bool used[30] = {};
        for (int col = 0; col < 30; ++col)
            for (int y = band; y < band + 8 && !used[col]; ++y)
                for (int x = col * 8; x < col * 8 + 8; ++x)
                    if (pixel(x, y)) { used[col] = true; break; }
        for (int col = 0; col < 30;) {
            if (!used[col]) { ++col; continue; }
            int end = col + 1, last = col;
            while (end < 30 && end - last <= 3) {
                if (used[end]) last = end;
                ++end;
            }
            const float ox = anchor_x(l, hud_anchor(col * 8, (last + 1) * 8, band, band + 8));
            for (int y = band; y < band + 8; ++y) {
                for (int x = col * 8; x < (last + 1) * 8;) {
                    const int index = pixel(x, y);
                    if (!index) { ++x; continue; }
                    int e = x + 1;
                    while (e < (last + 1) * 8 && pixel(e, y) == index) ++e;
                    fill(l, ox + x * l.scale, y * l.scale, ox + e * l.scale, (y + 1) * l.scale,
                         colour(rd16(pal + index * 2)));
                    x = e;
                }
            }
            col = last + 1;
        }
    }
}

// Brightness fades done with the blend registers (mission start/end, deaths).
void apply_fade(std::uint16_t bldcnt, std::uint16_t bldy) {
    const int mode = (bldcnt >> 6) & 3;
    if (mode < 2 || !(bldcnt & 0x3F)) return;
    const std::uint32_t k = std::min<std::uint32_t>(bldy & 31, 16);
    if (!k) return;
    for (std::uint32_t& px : g_frame) {
        std::uint32_t rb = px & 0xFF00FF, g = px & 0x00FF00;
        if (mode == 2) {
            rb += (((0xFF00FF - rb) * k) >> 4) & 0xFF00FF;
            g += (((0x00FF00 - g) * k) >> 4) & 0x00FF00;
        } else {
            rb -= ((rb * k) >> 4) & 0xFF00FF;
            g -= ((g * k) >> 4) & 0x00FF00;
        }
        px = 0xFF000000u | rb | g;
    }
}

// Runs `draw` over the frame split into horizontal bands, one per thread.
// The bands are disjoint, so the workers never touch the same pixels.
template <typename Draw>
void in_bands(const gtascene::View& view, Draw draw) {
    static const int workers = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 1, 1, 8);
    const int bands = std::min(workers, std::max(1, view.height / 64));
    std::vector<std::thread> threads;
    threads.reserve(bands);
    for (int i = 0; i < bands; ++i) {
        gtascene::View band = view;
        band.row_begin = view.height * i / bands;
        band.row_end = view.height * (i + 1) / bands;
        if (i + 1 < bands) threads.emplace_back([band, &draw] { draw(band); });
        else draw(band);                        // last band on this thread
    }
    for (std::thread& t : threads) t.join();
}

// Builds the composite into g_frame. False when the game is not showing the
// city right now (menus, cutscenes, loading).
bool compose(int out_w, int out_h) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!g_settings.native || !bus || out_w < 64 || out_h < 48) return false;
    const std::uint16_t dispcnt = bus->io().read16(0x000);
    // The city view is video mode 1 with the building and ground layers on.
    if ((dispcnt & 7u) != 1u || (dispcnt & 0x0500) != 0x0500 || (dispcnt & 0x80)) return false;

    gtascene::Memory mem;
    mem.ewram = bus->ewram_ptr(); mem.iwram = bus->iwram_ptr();
    mem.rom = bus->rom_ptr(); mem.rom_size = bus->rom_size();
    mem.palette = bus->pal_ptr();
    gtascene::View view;
    if (!gtascene::read_view(mem, &view)) return false;
    view.width = out_w; view.height = out_h;

    const std::size_t pixels = static_cast<std::size_t>(out_w) * out_h;
    g_frame.resize(pixels);
    g_depth.resize(pixels);
    Layout l;
    l.w = out_w; l.h = out_h;
    l.scale = out_h / 160.0f;
    l.box_x = (out_w - 240.0f * l.scale) * 0.5f;

    using Clock = std::chrono::steady_clock;
    auto mark = Clock::now();
    auto lap = [&](int i) {
        const auto now = Clock::now();
        g_phase_ms[i] = std::chrono::duration<double, std::milli>(now - mark).count();
        mark = now;
    };
    in_bands(view, [&](const gtascene::View& band) { gtascene::draw_ground(mem, band, g_frame.data()); });
    lap(0);
    draw_objects(l, bus, dispcnt, 3, 3);
    lap(1);
    in_bands(view, [&](const gtascene::View& band) {
        gtascene::draw_buildings(mem, band, g_frame.data(), g_depth.data());
    });
    lap(2);
    draw_objects(l, bus, dispcnt, 1, 2);
    if (dispcnt & 0x0200) draw_text_bg(l, bus, 1);
    draw_objects(l, bus, dispcnt, 0, 0);
    apply_fade(bus->io().read16(0x050), bus->io().read16(0x054));
    lap(3);
    return true;
}

int present_frame(gbarecomp::RunOptions::PresentFrame* frame) {
    static const int kPercent[] = {100, 75, 50, 33, 25};
    const int pct = kPercent[std::clamp(g_settings.resolution, 0, 4)];
    // Never narrower than 3:2: a tall window gets the full 240 columns.
    int w = std::max(240, frame->drawable_width * pct / 100);
    const int h = std::max(160, frame->drawable_height * pct / 100);
    w = std::max(w, h * 3 / 2);
    g_wide_factor.store(std::max(1.0f, (w * 160.0f) / (h * 240.0f)), std::memory_order_relaxed);
    if (!compose(w, h)) return 0;
    frame->argb = g_frame.data();
    frame->width = static_cast<unsigned>(w);
    frame->height = static_cast<unsigned>(h);
    return 1;
}

bool find_uint(const char* request, const char* key, unsigned long long* out) {
    const char* p = std::strstr(request, key);
    if (!p) return false;
    p = std::strchr(p + std::strlen(key), ':');
    if (!p) return false;
    *out = std::strtoull(p + 1, nullptr, 0);
    return true;
}

}  // namespace

GtaSceneSettings* gta_scene_settings() { return &g_settings; }

void gta_scene_install(gbarecomp::RunOptions* opts) {
    opts->present_frame = present_frame;
    opts->extended_view_frame = frame_start;
}

bool gta_scene_debug_shot(const char* path, int width, int height) {
    if (!compose(width, height)) return false;
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (std::uint32_t c : g_frame) {
        const unsigned char px[3] = {static_cast<unsigned char>(c >> 16),
                                     static_cast<unsigned char>(c >> 8),
                                     static_cast<unsigned char>(c)};
        std::fwrite(px, 1, 3, f);
    }
    std::fclose(f);
    return true;
}

int gta_debug_tcp_command(const char* request,
                          void (*write)(void* ctx, const char* data, std::size_t len),
                          void* write_ctx) {
    if (!std::strstr(request, "\"gta_scene_shot\"")) return 0;
    // {"cmd":"gta_scene_shot","w":1720,"h":720} -> scene_shot.ppm beside the exe
    unsigned long long w = 960, h = 640, native = 1;
    find_uint(request, "\"w\"", &w);
    find_uint(request, "\"h\"", &h);
    if (find_uint(request, "\"native\"", &native)) g_settings.native = native != 0;
    // Headless runs have no window: the shot's own shape drives the widening
    // (it takes effect from the game's next camera update).
    g_wide_factor.store(std::max(1.0f, (w * 160.0f) / (h * 240.0f)), std::memory_order_relaxed);
    // Time the composite alone, then write the picture.
    const auto t0 = std::chrono::steady_clock::now();
    const bool drawn = compose(static_cast<int>(w), static_cast<int>(h));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const bool ok = drawn && gta_scene_debug_shot("scene_shot.ppm", static_cast<int>(w), static_cast<int>(h));
    char reply[200];
    if (ok) std::snprintf(reply, sizeof(reply),
                          "{\"ok\":true,\"ms\":%.2f,\"ground\":%.2f,\"sprites\":%.2f,\"buildings\":%.2f,\"hud\":%.2f}",
                          ms, g_phase_ms[0], g_phase_ms[1], g_phase_ms[2], g_phase_ms[3]);
    else std::snprintf(reply, sizeof(reply), "{\"ok\":false,\"error\":\"not in the city view\"}");
    write(write_ctx, reply, std::strlen(reply));
    return 1;
}
