// Native scene renderer for Grand Theft Auto Advance (BGTE). See scene.h and
// docs/ENGINE_NOTES.md.
//
// The game projects with a 90-degree view onto a 240x240 square whose centre
// is the camera, and shows rows 0..159 of it:
//     screen_x = 120 + 118 * X / Z        screen_y = 120 - 118 * Y / Z
// with X, Y relative to the camera and Z the height below it. The same
// projection is used here, scaled to the output height and left open to the
// sides.
#include "scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gtascene {
namespace {

constexpr std::uint32_t kCamera = 0x03000014;        // s32 x, y, z (16.16)
constexpr std::uint32_t kUnitsPerPixel = 0x03000068; // s32 16.16, ground scale
constexpr std::uint32_t kInstanceHead = 0x02016280;  // linked list of building instances
constexpr std::uint32_t kGroundWidth = 0x0201CE58;   // ground size in texels
constexpr std::uint32_t kGroundHeight = 0x0201CE5C;
constexpr std::uint32_t kGroundMap = 0x0201CE60;     // -> u16 block id per 5x5-tile block
constexpr std::uint32_t kGroundBlocks = 0x0201CE64;  // -> 25 u16 tile ids per block
constexpr std::uint32_t kGroundTiles = 0x0201CE68;   // -> 8x8 8bpp tiles
constexpr std::uint32_t kGroundShift = 0x0201CE74;   // log2 of the map's row length
constexpr std::uint32_t kWallTextures = 0x08EBF5F4;  // u32 pointer per texture (64x64, 8bpp)
constexpr std::uint32_t kRoofTextures = 0x08EBEB2C;  // 0x18 bytes per roof, first word = 128x128
constexpr float kFocal = 118.0f;                     // pixels at 160 lines
constexpr float kCentreY = 120.0f;
constexpr float kTexelsPerUnit = 0.625f;             // ground texels per world unit
constexpr float kNear = 6.0f;

struct Reader {
    const Memory& m;
    // Pointer to `size` readable bytes at a guest address, or null.
    const std::uint8_t* at(std::uint32_t addr, std::size_t size) const {
        if (addr >= 0x02000000u && addr - 0x02000000u + size <= 0x40000u) return m.ewram + (addr - 0x02000000u);
        if (addr >= 0x03000000u && addr - 0x03000000u + size <= 0x8000u) return m.iwram + (addr - 0x03000000u);
        if (addr >= 0x08000000u && addr - 0x08000000u + size <= m.rom_size) return m.rom + (addr - 0x08000000u);
        return nullptr;
    }
    std::uint32_t u32(std::uint32_t addr) const {
        const std::uint8_t* p = at(addr, 4);
        std::uint32_t v = 0;
        if (p) std::memcpy(&v, p, 4);
        return v;
    }
    std::int32_t s32(std::uint32_t addr) const { return static_cast<std::int32_t>(u32(addr)); }
    std::uint16_t u16(std::uint32_t addr) const {
        const std::uint8_t* p = at(addr, 2);
        std::uint16_t v = 0;
        if (p) std::memcpy(&v, p, 2);
        return v;
    }
    std::uint8_t u8(std::uint32_t addr) const {
        const std::uint8_t* p = at(addr, 1);
        return p ? *p : 0;
    }
};

void build_palette(const Memory& mem, std::uint32_t* lut) {
    for (int i = 0; i < 256; ++i) {
        std::uint16_t c;
        std::memcpy(&c, mem.palette + i * 2, 2);
        const std::uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        lut[i] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
    }
}

struct Vertex { float x, y, z, u, v; };   // camera space; z = depth below the camera

// Clips a convex polygon against z >= kNear. Returns the new vertex count.
int clip_near(const Vertex* in, int n, Vertex* out) {
    int count = 0;
    for (int i = 0; i < n; ++i) {
        const Vertex& a = in[i];
        const Vertex& b = in[(i + 1) % n];
        const bool ain = a.z >= kNear, bin = b.z >= kNear;
        if (ain) out[count++] = a;
        if (ain != bin) {
            const float t = (kNear - a.z) / (b.z - a.z);
            out[count++] = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, kNear,
                            a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t};
        }
    }
    return count;
}

struct Target {
    int w, h, row_begin, row_end;
    std::uint32_t* argb;
    float* depth;           // 1/z of the nearest building pixel so far (0 = none)
    const std::uint32_t* lut;
};

struct Screen { float x, y, iz, uz, vz; };

// One triangle, perspective-correct, nearest texel. `texture` null = flat colour.
void fill_triangle(const Target& t, const Screen& a, const Screen& b, const Screen& c,
                   const std::uint8_t* texture, int tex_size, std::uint32_t flat) {
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-6f) return;
    const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    const int x1 = std::min(t.w - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    const int y0 = std::max(t.row_begin, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    const int y1 = std::min(t.row_end - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
    if (x0 > x1 || y0 > y1) return;
    const float inv = 1.0f / area;
    // Barycentric weights as planes over the screen: w = w_x * x + w_y * y + w_0.
    const float w0x = (b.y - c.y) * inv, w0y = (c.x - b.x) * inv;
    const float w1x = (c.y - a.y) * inv, w1y = (a.x - c.x) * inv;
    const float w2x = (a.y - b.y) * inv, w2y = (b.x - a.x) * inv;
    const float px = x0 + 0.5f, py = y0 + 0.5f;
    float r0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * inv;
    float r1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * inv;
    float r2 = 1.0f - r0 - r1;
    const float izx = w0x * a.iz + w1x * b.iz + w2x * c.iz;
    const float uzx = w0x * a.uz + w1x * b.uz + w2x * c.uz;
    const float vzx = w0x * a.vz + w1x * b.vz + w2x * c.vz;
    const float eps = -1e-4f;
    const int max_texel = tex_size - 1;
    // Narrows [lo, hi] to the columns where one weight is non-negative.
    auto narrow = [eps](float at_x0, float per_x, int x_first, int& lo, int& hi) {
        if (per_x > 1e-12f) {
            lo = std::max(lo, x_first + static_cast<int>(std::ceil((eps - at_x0) / per_x)));
        } else if (per_x < -1e-12f) {
            hi = std::min(hi, x_first + static_cast<int>(std::floor((eps - at_x0) / per_x)));
        } else if (at_x0 < eps) {
            hi = lo - 1;
        }
    };
    for (int y = y0; y <= y1; ++y, r0 += w0y, r1 += w1y, r2 += w2y) {
        int lo = x0, hi = x1;
        narrow(r0, w0x, x0, lo, hi);
        narrow(r1, w1x, x0, lo, hi);
        narrow(r2, w2x, x0, lo, hi);
        if (lo > hi) continue;
        const float k = static_cast<float>(lo - x0);
        const float w0 = r0 + w0x * k, w1 = r1 + w1x * k, w2 = r2 + w2x * k;
        float iz = w0 * a.iz + w1 * b.iz + w2 * c.iz;
        float uz = w0 * a.uz + w1 * b.uz + w2 * c.uz;
        float vz = w0 * a.vz + w1 * b.vz + w2 * c.vz;
        std::uint32_t* row = t.argb + static_cast<std::size_t>(y) * t.w;
        float* drow = t.depth + static_cast<std::size_t>(y) * t.w;
        if (texture) {
            for (int x = lo; x <= hi; ++x, iz += izx, uz += uzx, vz += vzx) {
                if (iz <= drow[x]) continue;
                const float z = 1.0f / iz;
                const int u = std::clamp(static_cast<int>(uz * z), 0, max_texel);
                const int v = std::clamp(static_cast<int>(vz * z), 0, max_texel);
                const std::uint8_t index = texture[v * tex_size + u];
                if (index) { row[x] = t.lut[index]; drow[x] = iz; }
            }
        } else {
            for (int x = lo; x <= hi; ++x, iz += izx)
                if (iz > drow[x]) { row[x] = flat; drow[x] = iz; }
        }
    }
}

}  // namespace

bool read_view(const Memory& mem, View* view) {
    const Reader r{mem};
    if (!r.u32(kInstanceHead) && !r.u32(kGroundMap)) return false;
    const std::uint32_t map = r.u32(kGroundMap), blocks = r.u32(kGroundBlocks), tiles = r.u32(kGroundTiles);
    if (!r.at(map, 2) || !r.at(blocks, 50) || !r.at(tiles, 64)) return false;
    const float upp = r.s32(kUnitsPerPixel) / 65536.0f;
    view->cam_x = r.s32(kCamera) / 65536.0f;
    view->cam_y = r.s32(kCamera + 4) / 65536.0f;
    view->cam_z = r.s32(kCamera + 8) / 65536.0f;
    if (view->cam_z < 16.0f || view->cam_z > 2000.0f || upp < 0.05f || upp > 16.0f) return false;
    view->ground_z = upp * kFocal;
    return true;
}

void draw_ground(const Memory& mem, const View& view, std::uint32_t* argb) {
    const Reader r{mem};
    std::uint32_t lut[256];
    build_palette(mem, lut);
    const int w = view.width, h = view.height;
    (void)h;
    const std::uint32_t map = r.u32(kGroundMap), blocks = r.u32(kGroundBlocks), tiles = r.u32(kGroundTiles);
    const int shift = r.u8(kGroundShift) & 31;
    const int limit_x = r.s32(kGroundWidth), limit_y = r.s32(kGroundHeight);
    const float scale = h / 160.0f;
    // World units per output pixel on the ground plane, then ground texels.
    const float step = view.ground_z / (kFocal * scale) * kTexelsPerUnit;
    const float left = view.cam_x * kTexelsPerUnit - (w * 0.5f - 0.5f) * step;
    const float top = view.cam_y * kTexelsPerUnit - (kCentreY * scale - 0.5f) * step;
    int previous_ty = INT32_MIN;
    for (int y = view.row_begin; y < view.row_end; ++y) {
        std::uint32_t* row = argb + static_cast<std::size_t>(y) * w;
        const int ty = static_cast<int>(std::floor(top + y * step));
        if (ty == previous_ty) {            // magnified: identical to the row above
            std::memcpy(row, row - w, static_cast<std::size_t>(w) * 4);
            continue;
        }
        previous_ty = ty;
        const bool row_ok = ty >= 0 && ty < limit_y;
        const int tile_y = ty >> 3, block_y = tile_y / 5, fine_y = tile_y % 5;
        int cached_tile_x = INT32_MIN;
        const std::uint8_t* tile_row = nullptr;
        for (int x = 0; x < w; ++x) {
            const int tx = static_cast<int>(std::floor(left + x * step));
            if (!row_ok || tx < 0 || tx >= limit_x) { row[x] = lut[0]; continue; }
            const int tile_x = tx >> 3;
            if (tile_x != cached_tile_x) {
                cached_tile_x = tile_x;
                const std::uint32_t block = r.u16(map + ((static_cast<std::uint32_t>(block_y) << shift) + tile_x / 5) * 2);
                const std::uint32_t tile = r.u16(blocks + block * 50 + (fine_y * 5 + tile_x % 5) * 2);
                tile_row = r.at(tiles + tile * 64 + (ty & 7) * 8, 8);
            }
            row[x] = tile_row ? lut[tile_row[tx & 7]] : lut[0];
        }
    }
}

void draw_buildings(const Memory& mem, const View& view, std::uint32_t* argb, float* depth) {
    const Reader r{mem};
    std::uint32_t lut[256];
    build_palette(mem, lut);
    const int w = view.width, h = view.height;
    std::fill(depth + static_cast<std::size_t>(view.row_begin) * w,
              depth + static_cast<std::size_t>(view.row_end) * w, 0.0f);
    const Target target{w, h, view.row_begin, view.row_end, argb, depth, lut};
    // The band's own vertical reach, so each thread skips what it cannot see.
    const float band_up = (h / 160.0f * kCentreY - view.row_begin) / (kFocal * h / 160.0f);
    const float band_down = (view.row_end - h / 160.0f * kCentreY) / (kFocal * h / 160.0f);
    const float scale = h / 160.0f, focal = kFocal * scale;
    const float cx = w * 0.5f, cy = kCentreY * scale;
    // Half the visible width in world units per unit of depth (for culling).
    const float reach_x = cx / focal;

    std::uint32_t inst = r.u32(kInstanceHead);
    for (int guard = 0; inst && guard < 1024; ++guard, inst = r.u32(inst + 0x14)) {
        if (!r.at(inst, 0x20)) break;
        const std::uint32_t model = r.u32(inst);
        const std::uint32_t geo = r.u32(model + 4);
        const std::uint32_t box = r.u32(inst + 0x10);
        if (!geo || !r.at(geo, 0x48) || !r.at(box, 24)) continue;
        // Camera relative to the instance, as the game computes it.
        const float dx = view.cam_x - r.s32(inst + 4) / 65536.0f;
        const float dy = view.cam_y - r.s32(inst + 8) / 65536.0f;
        const float dz = view.cam_z - r.s32(inst + 12) / 65536.0f;
        {   // Bounding box against the view (world y grows down the screen).
            const float top_depth = std::max(kNear, dz - r.s32(box + 20));
            const float x_lo = r.s32(box) - dx, x_hi = r.s32(box + 12) - dx;
            const float y_lo = r.s32(box + 4) - dy, y_hi = r.s32(box + 16) - dy;
            if (x_lo > reach_x * dz && x_lo > reach_x * top_depth) continue;
            if (x_hi < -reach_x * dz && x_hi < -reach_x * top_depth) continue;
            if (y_lo > band_down * dz && y_lo > band_down * top_depth) continue;
            if (y_hi < -band_up * dz && y_hi < -band_up * top_depth) continue;
        }
        const std::uint8_t state = r.u8(inst + 0x1E), placement = r.u8(inst + 0x1F);
        const int frame = state & 0x7F;
        std::uint32_t face = r.u32(geo + 0x38 + (placement & 3) * 4);
        const int count = r.u16(geo + 0x34);
        // Animated models carry their own texture lists, indexed by frame.
        const std::uint32_t wall_list = r.u32(model + 8), roof_list = r.u32(model + 12);

        for (int f = 0; f < count; ++f) {
            const std::uint8_t* rec = r.at(face, 4);
            if (!rec || rec[0] < 4) break;
            const int size = rec[0], tex_index = rec[1], shade = rec[2], flags = rec[3];
            const std::uint32_t here = face;
            face += size;
            const int n = flags & 7;
            if (n != 3 && n != 4) continue;
            const bool textured = flags & 0x10;
            const std::uint8_t* data = r.at(here, 4 + n * 6 + (textured ? n * 2 : 0));
            if (!data) continue;
            const bool roof = flags == 0x14;
            Vertex poly[8];
            for (int i = 0; i < n; ++i) {
                std::int16_t v[3];
                std::memcpy(v, data + 4 + i * 6, 6);
                poly[i].x = v[0] - dx;
                poly[i].y = v[1] + dy;
                poly[i].z = dz - v[2];
                poly[i].u = poly[i].v = 0;
                if (textured) {
                    const std::uint8_t* uv = data + 4 + n * 6 + i * 2;
                    poly[i].u = uv[0] + 0.5f;
                    poly[i].v = (roof ? 0x80 : 0x3F) - uv[1] + 0.5f;
                }
            }
            if (roof && (placement & 0x80)) {
                // Placed "upright": the game ignores the model's own texture
                // orientation and lays the roof texture out along the world
                // axes, left to right and top to bottom of the screen.
                float x_lo = poly[0].x, x_hi = poly[0].x, y_lo = poly[0].y, y_hi = poly[0].y;
                for (int i = 1; i < n; ++i) {
                    x_lo = std::min(x_lo, poly[i].x); x_hi = std::max(x_hi, poly[i].x);
                    y_lo = std::min(y_lo, poly[i].y); y_hi = std::max(y_hi, poly[i].y);
                }
                if (x_hi > x_lo && y_hi > y_lo)
                    for (int i = 0; i < n; ++i) {
                        poly[i].u = (poly[i].x - x_lo) / (x_hi - x_lo) * 128.0f;
                        poly[i].v = (y_hi - poly[i].y) / (y_hi - y_lo) * 128.0f;
                    }
            }
            {   // Back-face test, the game's determinant on the first three vertices.
                const Vertex& a = poly[0]; const Vertex& b = poly[1]; const Vertex& c = poly[2];
                const float det = (a.y * b.x - a.x * b.y) * c.z + (a.x * b.z - a.z * b.x) * c.y +
                                  (a.z * b.y - a.y * b.z) * c.x;
                if (det > 0) continue;
            }
            const std::uint8_t* texture = nullptr;
            int tex_size = 64;
            if (textured) {
                std::uint32_t ptr;
                if (roof) {
                    tex_size = 128;
                    // Second word: the copy turned a quarter, used for
                    // quarter-turned placements that keep the model's layout.
                    const bool turned = !(placement & 0x80) && (placement & 1);
                    ptr = r.u32(kRoofTextures + tex_index * 0x18 + (turned ? 4 : 0));
                    if (roof_list) {
                        std::uint32_t p = r.u32(roof_list + frame * 0x18);
                        if (!p) p = r.u32(roof_list + 0x18);
                        if (p) ptr = p;
                    }
                } else {
                    ptr = r.u32(kWallTextures + tex_index * 4);
                    if (wall_list) {
                        std::uint32_t p = r.u32(wall_list + frame * 4);
                        if (!p) p = r.u32(wall_list);
                        if (p) ptr = p;
                    }
                }
                texture = r.at(ptr, static_cast<std::size_t>(tex_size) * tex_size);
                if (!texture) continue;
            }
            Vertex clipped[10];
            const int m = clip_near(poly, n, clipped);
            if (m < 3) continue;
            Screen s[10];
            for (int i = 0; i < m; ++i) {
                const float iz = 1.0f / clipped[i].z;
                s[i] = {cx + focal * clipped[i].x * iz, cy - focal * clipped[i].y * iz, iz,
                        clipped[i].u * iz, clipped[i].v * iz};
            }
            for (int i = 1; i + 1 < m; ++i)
                fill_triangle(target, s[0], s[i], s[i + 1], texture, tex_size, lut[shade]);
        }
    }
}

}  // namespace gtascene
