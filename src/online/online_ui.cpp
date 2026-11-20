#include "online/online_ui.h"

#include "online/online_match.h"
#include "online/room_client.h"
#include "menu_mouse.h"

#include "runtime.h"
#include "gba_bus.h"
#include "mod_function_hooks.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

using gtaonline::RoomClient;

// ── ROM: text and menu entries ──────────────────────────────────────────────
// New string ids are answered by a hook on the game's string lookup
// (0x0805E330, id -> pointer); their text sits in the unused tail of the ROM.
constexpr std::uint32_t kStringLookup = 0x0805E330u, kLinkHandshake = 0x08065E5Cu;
constexpr std::uint32_t kEmptyStub = 0x08000734u;
constexpr std::uint32_t kOnlineStringId = 900;
constexpr std::size_t kOnlineText = 0x00FD3000;          // "ONLINE"
constexpr std::size_t kTitleMenu = 0x00332D4C, kEntry = 0x90;
constexpr int kTitleOnlineSlot = 5;
// The hideout menu: 0x18-byte header (entry count at +0x14), then 0x20-byte
// entries {u32 flags, u32 label, u32 -1, 3 x u32 0, u32 callback, u32 0}.
constexpr std::size_t kHideoutMenu = 0x0032C138, kHideoutStride = 0x298;
// Mode menus, in the order the multiplayer menu lists them.
constexpr std::size_t kModeMenus[4] = {0x00334534, 0x00334930, 0x00334D2C, 0x00335128};

std::uint32_t rd32(const std::uint8_t* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }

// ── font ────────────────────────────────────────────────────────────────────
// The game's 16x16 font: glyphs from ASCII 0x20, four 8bpp tiles each
// (top-left, top-right, bottom-left, bottom-right); colours 1..6 the yellow
// face from bright to deep, 7 and 8 the dark outline, 9 the drop shadow; one
// width byte per glyph.
constexpr std::size_t kGlyphs = 0x008314D0, kWidths = 0x0083F4D0;
const std::uint32_t kRamp[10] = {0, 0xFFFFEF00, 0xFFF7D608, 0xFFE7BD10, 0xFFD6A518,
                                 0xFFCE9C21, 0xFF846310, 0xFF421810, 0xFF100800, 0xFF000000};

struct Canvas {
    std::uint32_t* px = nullptr;
    int w = 0, h = 0, scale = 1;        // virtual pixels -> image pixels
    int vw() const { return w / scale; }
    int vh() const { return h / scale; }
    void dot(int x, int y, std::uint32_t c) {
        for (int dy = 0; dy < scale; ++dy) {
            const int yy = y * scale + dy;
            if (yy < 0 || yy >= h) continue;
            for (int dx = 0; dx < scale; ++dx) {
                const int xx = x * scale + dx;
                if (xx >= 0 && xx < w) px[yy * w + xx] = c;
            }
        }
    }
    void shade(int x, int y, int ww, int hh, int keep) {   // darken a box (keep/256)
        for (int yy = std::max(0, y * scale); yy < std::min(h, (y + hh) * scale); ++yy)
            for (int xx = std::max(0, x * scale); xx < std::min(w, (x + ww) * scale); ++xx) {
                std::uint32_t& c = px[yy * w + xx];
                const std::uint32_t r = ((c >> 16) & 0xFF) * keep >> 8, g = ((c >> 8) & 0xFF) * keep >> 8,
                                    b = (c & 0xFF) * keep >> 8;
                c = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
    }
};

const std::uint8_t* g_rom = nullptr;
std::size_t g_rom_size = 0;

int glyph_width(char ch) {
    const int c = static_cast<unsigned char>(ch);
    if (!g_rom || c < 0x20 || c > 0x7E) return 8;
    const int w = g_rom[kWidths + (c - 0x20)];
    return (w > 0 && w <= 16) ? w : 8;
}
int text_width(const std::string& s) {
    int w = 0;
    for (char c : s) w += glyph_width(c);
    return w;
}
// dim: 256 = full colour. tint: a colour to mix toward for the highlighted line.
void draw_text(Canvas& cv, int x, int y, const std::string& s, int dim = 256, bool white = false) {
    if (!g_rom) return;
    for (char ch : s) {
        const int c = static_cast<unsigned char>(ch);
        if (c >= 0x20 && c <= 0x7E) {
            const std::uint8_t* g = g_rom + kGlyphs + static_cast<std::size_t>(c - 0x20) * 256;
            for (int t = 0; t < 4; ++t)
                for (int yy = 0; yy < 8; ++yy)
                    for (int xx = 0; xx < 8; ++xx) {
                        const int v = g[t * 64 + yy * 8 + xx];
                        if (v <= 0 || v > 9) continue;
                        std::uint32_t col = kRamp[v];
                        if (white && v <= 6) col = (v <= 3) ? 0xFFFFFFFF : 0xFFD8D8D8;
                        if (dim != 256) {
                            const std::uint32_t r = ((col >> 16) & 0xFF) * dim >> 8,
                                                gg = ((col >> 8) & 0xFF) * dim >> 8, b = (col & 0xFF) * dim >> 8;
                            col = 0xFF000000u | (r << 16) | (gg << 8) | b;
                        }
                        cv.dot(x + (t & 1) * 8 + xx, y + (t >> 1) * 8 + yy, col);
                    }
        }
        x += glyph_width(ch);
    }
}
void draw_centered(Canvas& cv, int y, const std::string& s, int dim = 256, bool white = false) {
    draw_text(cv, (cv.vw() - text_width(s)) / 2, y, s, dim, white);
}
std::string upper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

// ROM strings by id (current language 0 is all this needs: English).
std::string rom_string(std::uint32_t id) {
    if (!g_rom || id >= 618) return "";
    const std::uint32_t p = rd32(g_rom + 0x00EC4324 + id * 4) - 0x08000000u;
    if (p >= g_rom_size) return "";
    return reinterpret_cast<const char*>(g_rom + p);
}

// ── modes and their settings, read from the game's own menus ────────────────
struct Setting { std::string label; std::vector<std::string> values; };
struct Mode { std::string name; std::vector<Setting> settings; };
std::vector<Mode> g_modes;

void load_modes() {
    if (!g_rom || !g_modes.empty()) return;
    for (std::size_t m : kModeMenus) {
        Mode mode;
        mode.name = rom_string(rd32(g_rom + m));
        const std::uint32_t count = std::min<std::uint32_t>(rd32(g_rom + m + 4), 7);
        for (std::uint32_t k = 1; k < count; ++k) {
            const std::uint8_t* e = g_rom + m + 8 + k * kEntry;
            Setting s;
            s.label = rom_string(rd32(e + 4));
            for (int v = 0; v < e[13] && v < 16; ++v) s.values.push_back(rom_string(rd32(e + 20 + v * 4)));
            if (!s.label.empty() && !s.values.empty()) mode.settings.push_back(s);
        }
        g_modes.push_back(mode);
    }
}

// ── settings file (name, server) ────────────────────────────────────────────
std::filesystem::path g_ini;
std::string g_name = "Player", g_server = gtaonline::kDefaultServer, g_build, g_exe, g_save_path;

void load_ini() {
    std::ifstream in(g_ini);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        if (k == "name" && !v.empty()) g_name = v.substr(0, 15);
        if (k == "server" && !v.empty()) g_server = v;
    }
}
void save_ini() {
    std::ofstream out(g_ini, std::ios::trunc);
    out << "# GTA Advance online: your name in rooms and chat, and the room server.\n"
        << "name=" << g_name << "\nserver=" << g_server << "\n";
}

// ── the menu ────────────────────────────────────────────────────────────────
enum class Screen { Closed, Main, Name, Code, List, Room, Chat, Starting, Message };
enum ControlKind : std::uint8_t { kStart = 1, kStartAck = 2, kChat = 3, kSettings = 4 };

struct Ui {
    Screen screen = Screen::Closed, back_to = Screen::Main;
    int cursor = 0, scroll = 0;
    std::string edit, message;
    std::unique_ptr<RoomClient> client;
    bool want_host = false;
    std::uint16_t want_join = 0;
    bool list_wanted = false;
    // room state (the host's choice; a guest shows what the host sent)
    int mode = 0;
    std::uint8_t values[8] = {};
    std::uint32_t match = 0, peer = 0;
    std::uint64_t start_sent_ms = 0, settings_sent_ms = 0, start_deadline = 0;
    bool acked = false;
    // chat typed before the connection was up, sent once it is
    std::vector<std::pair<std::string, bool>> pending_say;
    // After the menu closes the keys stay away from the game until every
    // key and mouse button is up, so the press that closed it does not land.
    bool draining = false;
    // Just opened: the press that opened it is still down; wait it out.
    bool settling = false;
    // mouse hit boxes from the last draw (virtual pixels); item >= 0 is a
    // row, the negative ones are the buttons below
    struct Hit { int x0, y0, x1, y1, item; };
    std::vector<Hit> hits;
    int canvas_scale = 1, image_w = 0, image_h = 0, mouse_vx = -1, mouse_vy = -1;
};
enum HitItem { kHitOk = -10, kHitCancel = -11, kHitChat = -12 };
Ui g_ui;
std::vector<std::uint32_t> g_image;

bool open() { return g_ui.screen != Screen::Closed; }

void show_message(const std::string& text, Screen back) {
    g_ui.message = upper(text);
    g_ui.back_to = back;
    g_ui.screen = Screen::Message;
}

void ensure_client();

void open_menu() {
    if (gba::GbaBus* bus = gbarecomp::active_bus()) { g_rom = bus->rom_ptr(); g_rom_size = bus->rom_size(); }
    load_modes();
    if (g_modes.size() != 4) return;
    g_ui.screen = Screen::Main;
    g_ui.cursor = 0;
    g_ui.pending_say.clear();
    g_ui.settling = true;
    ensure_client();                   // on the server by the time anything is picked
}

void close_menu() {
    if (g_ui.client) { g_ui.client->disconnect(); g_ui.client.reset(); }
    g_ui.screen = Screen::Closed;
    g_ui.draining = true;
}

void ensure_client() {
    if (g_ui.client && g_ui.client->state() != RoomClient::State::Failed &&
        g_ui.client->state() != RoomClient::State::Idle) return;
    g_ui.client = std::make_unique<RoomClient>();
    g_ui.client->on_relay = [](RoomClient::Channel ch, std::uint32_t from, const std::uint8_t* p, int len) {
        if (ch != RoomClient::kControl || len < 1) return;
        RoomClient& c = *g_ui.client;
        std::string who = "?";
        for (const auto& m : c.members()) if (m.id == from) who = m.name;
        if (p[0] == kChat && len > 1) c.add_chat(who, std::string(reinterpret_cast<const char*>(p + 1), len - 1), true);
        if (p[0] == kSettings && len >= 3 && from == c.host_id() && !c.is_host()) {
            g_ui.mode = std::min<int>(p[1], 3);
            for (int i = 0; i < 8 && 3 + i <= len - 0 && i < p[2]; ++i) g_ui.values[i] = p[3 + i];
        }
        if (p[0] == kStart && len >= 5 + 2 && from == c.host_id() && !c.is_host()) {
            std::memcpy(&g_ui.match, p + 1, 4);
            g_ui.mode = std::min<int>(p[5], 3);
            for (int i = 0; i < 8 && i < p[6] && 7 + i < len; ++i) g_ui.values[i] = p[7 + i];
            g_ui.peer = from;
            g_ui.screen = Screen::Starting;
            g_ui.start_deadline = gtaonline::now_ms() + 600;   // ack for a while, then go
        }
        if (p[0] == kStartAck && len >= 5 && c.is_host()) {
            std::uint32_t m; std::memcpy(&m, p + 1, 4);
            if (m == g_ui.match) { g_ui.acked = true; g_ui.peer = from; }
        }
    };
    g_ui.client->connect(g_server, g_name, g_build);
}

void send_settings() {
    RoomClient& c = *g_ui.client;
    const auto& mode = g_modes[g_ui.mode];
    std::uint8_t note[3 + 8] = {kSettings, static_cast<std::uint8_t>(g_ui.mode),
                                static_cast<std::uint8_t>(mode.settings.size())};
    for (std::size_t i = 0; i < mode.settings.size() && i < 8; ++i) note[3 + i] = g_ui.values[i];
    c.send_relay(RoomClient::kControl, note, 3 + static_cast<int>(std::min<std::size_t>(mode.settings.size(), 8)));
    g_ui.settings_sent_ms = gtaonline::now_ms();
}

// The pad sequence that takes a console from power-on to the chosen mode with
// its settings, as the netplay script: "first last mask" per press. Both
// connect; after that the second console's menus mirror the first's (the
// link carries them), so only the host's console picks the mode and settings.
// A setting's arrows stop at either end, and opening a mode's menu sets its
// own defaults, so each setting is pushed all the way left first.
std::string opening_script(bool host) {
    constexpr unsigned A = 0x1, START = 0x8, RIGHT = 0x10, LEFT = 0x20, UP = 0x40, DOWN = 0x80;
    std::string out;
    unsigned t = 1500;
    auto press = [&](unsigned mask, unsigned gap) {
        out += std::to_string(t) + " " + std::to_string(t + 5) + " " + std::to_string(mask) + "\n";
        t += 6 + gap;
    };
    press(START, 120);
    for (int i = 0; i < 3; ++i) press(DOWN, 60);        // MULTI PLAYER
    press(A, 100);                                      // the link screen
    press(A, 400);                                      // A BUTTON TO CONNECT
    if (!host) return out;
    for (int i = 0; i < g_ui.mode; ++i) press(DOWN, 60);
    press(A, 200);                                      // the mode's menu
    const auto& mode = g_modes[g_ui.mode];
    int row = 0;
    for (std::size_t k = 0; k < mode.settings.size(); ++k) {
        while (row < static_cast<int>(k) + 1) { press(DOWN, 60); ++row; }
        const int n = static_cast<int>(mode.settings[k].values.size());
        for (int i = 1; i < n; ++i) press(LEFT, 50);
        for (int i = 0; i < g_ui.values[k]; ++i) press(RIGHT, 50);
    }
    while (row > 0) { press(UP, 60); --row; }
    press(A, 100);                                      // START GAME
    return out;
}

// Restart this game into the agreed match (online_match.h, --online-resume).
void restart_into_match() {
    RoomClient& c = *g_ui.client;
    const bool host = c.is_host();
    const std::filesystem::path script = std::filesystem::temp_directory_path() /
        ("gta_online_" + std::to_string(g_ui.match) + (host ? "_0" : "_1") + ".script");
    { std::ofstream out(script, std::ios::trunc); out << opening_script(host); }
    std::string cmd = "\"" + g_exe + "\" --online-server \"" + g_server + "\" --online-name \"" + g_name +
        "\" --online-resume " + std::to_string(c.session()) + " --online-seat " + (host ? "0" : "1") +
        " --online-peer " + std::to_string(g_ui.peer) + " --online-match " + std::to_string(g_ui.match) +
        " --online-room " + std::to_string(c.room());
    if (!g_save_path.empty()) cmd += " --save-path \"" + g_save_path + "\"";
#if defined(_WIN32)
    SetEnvironmentVariableA("GBARECOMP_NETPLAY_SCRIPT", script.string().c_str());
    // The new game writes to the same log as this one (when there is one) and
    // inherits nothing else: no sockets, no files.
    STARTUPINFOEXA si{}; si.StartupInfo.cb = sizeof(si);
    std::vector<HANDLE> inherit;
    for (DWORD which : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
        const HANDLE h = GetStdHandle(which);
        if (h && h != INVALID_HANDLE_VALUE && std::find(inherit.begin(), inherit.end(), h) == inherit.end() &&
            SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
            inherit.push_back(h);
    }
    std::vector<char> attrs;
    DWORD flags = 0;
    if (!inherit.empty()) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attrs.resize(size);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
        if (InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size) &&
            UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                      inherit.size() * sizeof(HANDLE), nullptr, nullptr)) {
            flags = EXTENDED_STARTUPINFO_PRESENT;
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
            si.StartupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        } else {
            si.lpAttributeList = nullptr;
        }
    }
    PROCESS_INFORMATION pi{};
    std::vector<char> line(cmd.begin(), cmd.end()); line.push_back('\0');
    const std::string dir = std::filesystem::path(g_exe).parent_path().string();
    std::fflush(stdout); std::fflush(stderr);
    const BOOL started = CreateProcessA(nullptr, line.data(), nullptr, nullptr, flags ? TRUE : FALSE, flags,
                                        nullptr, dir.c_str(), &si.StartupInfo, &pi);
    if (si.lpAttributeList) DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!started) {
        show_message("could not restart the game", Screen::Room);
        return;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
#else
    setenv("GBARECOMP_NETPLAY_SCRIPT", script.string().c_str(), 1);
    if (std::system((cmd + " &").c_str()) != 0) { show_message("could not restart the game", Screen::Room); return; }
#endif
    // Keep the room seat for the new process: no goodbye to the server.
    c.abandon();
    g_ui.client.reset();
    g_ui.screen = Screen::Closed;
    // A window closes the usual way (saves are written as on any quit). A
    // headless test run has no event loop to see that, so it just ends.
    if (!SDL_WasInit(SDL_INIT_VIDEO)) { std::fflush(stderr); std::_Exit(0); }
    SDL_Event quit{}; quit.type = SDL_QUIT;
    SDL_PushEvent(&quit);
}

// ── menu rows ───────────────────────────────────────────────────────────────
enum Item { kHost, kFind, kCode, kName, kSay, kBack, kModeRow, kSettingRow, kStartRow, kLeave, kRoomRow };
struct Row { Item item; int index; std::string text; bool enabled = true; };

std::vector<Row> rows() {
    std::vector<Row> r;
    switch (g_ui.screen) {
    case Screen::Main:
        r.push_back({kHost, 0, "HOST A GAME"});
        r.push_back({kFind, 0, "FIND GAMES"});
        r.push_back({kCode, 0, "JOIN BY CODE"});
        r.push_back({kName, 0, "NAME: " + upper(g_name)});
        r.push_back({kSay, 0, "CHAT"});
        r.push_back({kBack, 0, "BACK"});
        break;
    case Screen::List:
        if (g_ui.client)
            for (std::size_t i = 0; i < g_ui.client->listing().size(); ++i) {
                const auto& l = g_ui.client->listing()[i];
                r.push_back({kRoomRow, static_cast<int>(i), upper(l.host) + "  " + std::to_string(l.players) + "/" +
                             std::to_string(l.max_players) + "  " + std::to_string(l.code),
                             l.players < l.max_players});
            }
        r.push_back({kFind, 0, "REFRESH"});
        r.push_back({kBack, 0, "BACK"});
        break;
    case Screen::Room: {
        const bool host = g_ui.client && g_ui.client->is_host();
        const auto& mode = g_modes[g_ui.mode];
        r.push_back({kModeRow, 0, "MODE: " + mode.name, host});
        for (std::size_t k = 0; k < mode.settings.size(); ++k)
            r.push_back({kSettingRow, static_cast<int>(k),
                         mode.settings[k].label + ": " + mode.settings[k].values[g_ui.values[k] % mode.settings[k].values.size()],
                         host});
        if (host) r.push_back({kStartRow, 0, "START", g_ui.client->members().size() >= 2});
        r.push_back({kSay, 0, "CHAT"});
        r.push_back({kLeave, 0, "LEAVE ROOM"});
        break;
    }
    default: break;
    }
    return r;
}

void change_setting(const Row& row, int dir) {
    if (!g_ui.client || !g_ui.client->is_host()) return;
    if (row.item == kModeRow) {
        g_ui.mode = (g_ui.mode + dir + 4) % 4;
        std::memset(g_ui.values, 0, sizeof(g_ui.values));
    } else if (row.item == kSettingRow) {
        const int n = static_cast<int>(g_modes[g_ui.mode].settings[row.index].values.size());
        g_ui.values[row.index] = static_cast<std::uint8_t>((g_ui.values[row.index] + dir + n) % n);
    } else {
        return;
    }
    send_settings();
}

void choose(const Row& row) {
    if (!row.enabled) return;
    switch (row.item) {
    case kHost:
        ensure_client(); g_ui.want_host = true; g_ui.want_join = 0;
        g_ui.mode = 0; std::memset(g_ui.values, 0, sizeof(g_ui.values));
        show_message("opening a room...", Screen::Main);
        break;
    case kFind:
        ensure_client(); g_ui.list_wanted = true;
        if (g_ui.client->state() == RoomClient::State::Connected) g_ui.client->request_list();
        g_ui.screen = Screen::List; g_ui.cursor = 0;
        break;
    case kCode: g_ui.edit.clear(); g_ui.screen = Screen::Code; break;
    case kName: g_ui.edit = g_name; g_ui.screen = Screen::Name; break;
    case kSay: g_ui.edit.clear(); g_ui.back_to = g_ui.screen; g_ui.screen = Screen::Chat; break;
    case kBack: close_menu(); break;
    case kRoomRow: {
        const auto& l = g_ui.client->listing()[row.index];
        g_ui.want_join = l.code; g_ui.want_host = false;
        show_message("joining...", Screen::List);
        break;
    }
    case kStartRow: {
        const std::uint32_t r = static_cast<std::uint32_t>(gtaonline::now_ms()) * 2654435761u;
        g_ui.match = (r ^ (std::uint32_t(g_ui.client->room()) << 16)) | 1u;
        g_ui.acked = false;
        for (const auto& m : g_ui.client->members()) if (m.id != g_ui.client->my_id()) g_ui.peer = m.id;
        g_ui.screen = Screen::Starting;
        g_ui.start_deadline = gtaonline::now_ms() + 15000;
        g_ui.start_sent_ms = 0;
        break;
    }
    case kLeave:
        g_ui.client->leave_room();
        g_ui.screen = Screen::Main; g_ui.cursor = 0;
        break;
    default: break;
    }
}

// ── per frame: network and keys ─────────────────────────────────────────────
void pump_network() {
    if (!g_ui.client) return;
    RoomClient& c = *g_ui.client;
    c.pump();
    if (c.state() == RoomClient::State::Failed) {
        const std::string e = c.error();
        g_ui.client.reset();
        g_ui.want_host = false; g_ui.want_join = 0;
        show_message(e.empty() ? "no connection" : e, Screen::Main);
        return;
    }
    if (c.state() != RoomClient::State::Connected) return;
    for (const auto& [text, room] : g_ui.pending_say) c.say(text, room && c.room() != 0);
    g_ui.pending_say.clear();
    if (g_ui.list_wanted) { c.request_list(); g_ui.list_wanted = false; }
    if (g_ui.want_host) { c.create_room(2); g_ui.want_host = false; }
    if (g_ui.want_join) { c.join_room(g_ui.want_join); g_ui.want_join = 0; }
    if (!c.room_error().empty()) {
        const std::string e = c.room_error();
        c.clear_room_error();
        if (g_ui.screen == Screen::Message) show_message(e, Screen::Main);
    }
    if (c.room() && (g_ui.screen == Screen::Message || g_ui.screen == Screen::List || g_ui.screen == Screen::Code)) {
        g_ui.screen = Screen::Room; g_ui.cursor = 0;
    }
    if (!c.room() && (g_ui.screen == Screen::Room)) show_message("the room closed", Screen::Main);
    const auto now = gtaonline::now_ms();
    if (c.room() && c.is_host() && g_ui.screen == Screen::Room && now - g_ui.settings_sent_ms > 2000) send_settings();
    if (g_ui.screen == Screen::Starting) {
        const auto& mode = g_modes[g_ui.mode];
        if (c.is_host()) {
            if (g_ui.acked) { restart_into_match(); return; }
            if (now - g_ui.start_sent_ms > 200) {
                std::uint8_t note[7 + 8] = {kStart};
                std::memcpy(note + 1, &g_ui.match, 4);
                note[5] = static_cast<std::uint8_t>(g_ui.mode);
                note[6] = static_cast<std::uint8_t>(std::min<std::size_t>(mode.settings.size(), 8));
                for (int i = 0; i < note[6]; ++i) note[7 + i] = g_ui.values[i];
                c.send_relay(RoomClient::kControl, note, 7 + note[6]);
                g_ui.start_sent_ms = now;
            }
            if (now > g_ui.start_deadline || c.members().size() < 2) show_message("the other player did not answer", Screen::Room);
        } else {
            // A few acknowledgements (any one is enough), then go.
            std::uint8_t note[5] = {kStartAck};
            std::memcpy(note + 1, &g_ui.match, 4);
            for (int i = 0; i < 4; ++i) c.send_relay(RoomClient::kControl, note, 5);
            restart_into_match();
            return;
        }
    }
}

struct Keys { bool up, down, left, right, ok, back; std::string typed; bool erase, enter; };
// Debug server input (headless tests): one key name per frame, or text.
std::string g_test_key, g_test_text;
Keys read_keys() {
    Keys k{};
    if (!g_test_key.empty() || !g_test_text.empty()) {
        const std::string key = g_test_key;
        g_test_key.clear();
        k.up = key == "up"; k.down = key == "down"; k.left = key == "left"; k.right = key == "right";
        k.enter = key == "enter"; k.ok = k.enter || key == "ok"; k.back = key == "back"; k.erase = key == "erase";
        k.typed = g_test_text; g_test_text.clear();
        return k;
    }
    if (!SDL_WasInit(SDL_INIT_VIDEO) || !SDL_GetKeyboardFocus()) return k;
    static Uint8 prev[SDL_NUM_SCANCODES] = {};
    int n = 0;
    const Uint8* now = SDL_GetKeyboardState(&n);
    auto hit = [&](SDL_Scancode s) { return now[s] && !prev[s]; };
    const bool text = g_ui.screen == Screen::Name || g_ui.screen == Screen::Code || g_ui.screen == Screen::Chat;
    k.up = hit(SDL_SCANCODE_UP) || (!text && hit(SDL_SCANCODE_W));
    k.down = hit(SDL_SCANCODE_DOWN) || (!text && hit(SDL_SCANCODE_S));
    k.left = hit(SDL_SCANCODE_LEFT) || (!text && hit(SDL_SCANCODE_A));
    k.right = hit(SDL_SCANCODE_RIGHT) || (!text && hit(SDL_SCANCODE_D));
    k.enter = hit(SDL_SCANCODE_RETURN) || hit(SDL_SCANCODE_KP_ENTER);
    k.ok = k.enter || (!text && (hit(SDL_SCANCODE_SPACE) || hit(SDL_SCANCODE_X) || hit(SDL_SCANCODE_J) ||
                                 hit(SDL_SCANCODE_LCTRL)));
    k.erase = hit(SDL_SCANCODE_BACKSPACE);
    k.back = (!text && (k.erase || hit(SDL_SCANCODE_Z) || hit(SDL_SCANCODE_K))) || hit(SDL_SCANCODE_DELETE);
    if (text) {
        const bool shift = now[SDL_SCANCODE_LSHIFT] || now[SDL_SCANCODE_RSHIFT];
        for (int s = SDL_SCANCODE_A; s <= SDL_SCANCODE_Z; ++s)
            if (hit(static_cast<SDL_Scancode>(s))) k.typed += static_cast<char>((shift ? 'A' : 'a') + (s - SDL_SCANCODE_A));
        for (int s = SDL_SCANCODE_1; s <= SDL_SCANCODE_0; ++s)
            if (hit(static_cast<SDL_Scancode>(s))) {
                static const char* kShifted = "!@#$%^&*()";
                k.typed += shift ? kShifted[s - SDL_SCANCODE_1] : "1234567890"[s - SDL_SCANCODE_1];
            }
        if (hit(SDL_SCANCODE_SPACE)) k.typed += ' ';
        if (hit(SDL_SCANCODE_MINUS)) k.typed += shift ? '_' : '-';
        if (hit(SDL_SCANCODE_PERIOD)) k.typed += '.';
        if (hit(SDL_SCANCODE_COMMA)) k.typed += ',';
        if (hit(SDL_SCANCODE_SLASH)) k.typed += shift ? '?' : '/';
        if (hit(SDL_SCANCODE_APOSTROPHE)) k.typed += '\'';
    }
    std::memcpy(prev, now, std::min<int>(n, SDL_NUM_SCANCODES));
    return k;
}

// Mouse: hover selects, left click chooses (on a setting: the left half
// lowers it, the right half raises it), right click goes back, the wheel
// moves through the list (or changes the setting under the pointer).
struct Mouse { bool moved = false, click = false, right = false; int item = kNoHit, wheel = 0; static constexpr int kNoHit = -1000; };
// Debug server input (headless tests): a pointer in virtual pixels.
int g_test_mx = -1, g_test_my = -1, g_test_click = 0, g_test_right = 0;
Mouse read_mouse() {
    Mouse m;
    m.wheel = std::clamp(gta_mouse_wheel_take(), -3, 3);
    static int last_x = -1, last_y = -1;
    int vx = -1, vy = -1;
    if (g_test_mx >= 0) {
        vx = g_test_mx; vy = g_test_my;
        m.click = g_test_click > 0; m.right = g_test_right > 0;
        g_test_click = g_test_right = 0;
    } else {
        if (!SDL_WasInit(SDL_INIT_VIDEO)) return m;
        SDL_Window* w = SDL_GetMouseFocus();
        if (!w || !g_ui.image_w) return m;
        int mx = 0, my = 0, ww = 0, wh = 0;
        const Uint32 buttons = SDL_GetMouseState(&mx, &my);
        SDL_GetWindowSize(w, &ww, &wh);
        static Uint32 prev = 0;
        m.click = (buttons & SDL_BUTTON_LMASK) && !(prev & SDL_BUTTON_LMASK);
        m.right = (buttons & SDL_BUTTON_RMASK) && !(prev & SDL_BUTTON_RMASK);
        prev = buttons;
        if (ww <= 0 || wh <= 0) return m;
        // The menu image is shown across the whole window.
        vx = mx * g_ui.image_w / ww / g_ui.canvas_scale;
        vy = my * g_ui.image_h / wh / g_ui.canvas_scale;
    }
    m.moved = vx != last_x || vy != last_y;
    last_x = vx; last_y = vy;
    g_ui.mouse_vx = vx; g_ui.mouse_vy = vy;
    for (const auto& h : g_ui.hits)
        if (vx >= h.x0 && vx < h.x1 && vy >= h.y0 && vy < h.y1) m.item = h.item;
    return m;
}
bool mouse_on_left_half() {
    return g_ui.mouse_vx >= 0 && g_ui.image_w && g_ui.mouse_vx < g_ui.image_w / g_ui.canvas_scale / 2;
}

void handle_input() {
    Keys k = read_keys();
    const Mouse mouse = read_mouse();
    const bool right = mouse.right;
    // Clicking the chat lines opens the chat line.
    if (mouse.click && mouse.item == kHitChat && g_ui.screen != Screen::Chat) {
        g_ui.edit.clear(); g_ui.back_to = g_ui.screen; g_ui.screen = Screen::Chat;
        return;
    }
    switch (g_ui.screen) {
    case Screen::Name: case Screen::Code: case Screen::Chat: {
        const std::size_t max = g_ui.screen == Screen::Name ? 15 : g_ui.screen == Screen::Code ? 4 : 120;
        for (char c : k.typed) {
            if (g_ui.screen == Screen::Code && (c < '0' || c > '9')) continue;
            if (g_ui.edit.size() < max) g_ui.edit += c;
        }
        if (k.erase && !g_ui.edit.empty()) g_ui.edit.pop_back();
        const Screen back = g_ui.screen == Screen::Chat ? g_ui.back_to : Screen::Main;
        if (k.back || right || (mouse.click && mouse.item == kHitCancel)) { g_ui.screen = back; break; }
        if (k.enter || (mouse.click && mouse.item == kHitOk)) {
            if (g_ui.screen == Screen::Name) {
                std::string n = g_ui.edit;
                while (!n.empty() && n.back() == ' ') n.pop_back();
                if (!n.empty()) { g_name = n; save_ini(); }
                // A new hello carries the new name.
                if (g_ui.client && !g_ui.client->room()) { g_ui.client->disconnect(); g_ui.client.reset(); }
                ensure_client();
                g_ui.screen = Screen::Main;
            } else if (g_ui.screen == Screen::Code) {
                if (g_ui.edit.size() == 4) {
                    ensure_client(); g_ui.want_join = static_cast<std::uint16_t>(std::atoi(g_ui.edit.c_str()));
                    show_message("joining...", Screen::Main);
                }
            } else {
                if (!g_ui.edit.empty()) {
                    ensure_client();
                    const bool room = g_ui.client->room() != 0;
                    if (g_ui.client->state() == RoomClient::State::Connected) g_ui.client->say(g_ui.edit, room);
                    else g_ui.pending_say.emplace_back(g_ui.edit, room);   // sent once connected
                }
                g_ui.edit.clear();
                g_ui.screen = back;
            }
        }
        break;
    }
    case Screen::Message:
        if (k.ok || k.back || mouse.click || right) g_ui.screen = g_ui.back_to;
        break;
    case Screen::Starting:
        break;
    default: {
        const auto r = rows();
        if (r.empty()) break;
        const int n = static_cast<int>(r.size());
        const bool on_row = mouse.item >= 0 && mouse.item < n;
        if (on_row && (mouse.moved || mouse.click)) g_ui.cursor = mouse.item;
        if (k.up) g_ui.cursor = (g_ui.cursor + n - 1) % n;
        if (k.down) g_ui.cursor = (g_ui.cursor + 1) % n;
        g_ui.cursor = std::min(g_ui.cursor, n - 1);
        const Row& row = r[g_ui.cursor];
        const bool setting = row.item == kModeRow || row.item == kSettingRow;
        if (mouse.wheel) {
            // Over a setting the host can change, the wheel turns it;
            // elsewhere it walks the list (stopping at the ends).
            if (setting && row.enabled && on_row) change_setting(row, mouse.wheel > 0 ? 1 : -1);
            else g_ui.cursor = std::clamp(g_ui.cursor - mouse.wheel, 0, n - 1);
        }
        if (k.left) change_setting(row, -1);
        if (k.right) change_setting(row, 1);
        if (k.ok) {
            if (setting) change_setting(row, 1);
            else choose(row);
        } else if (mouse.click && on_row && mouse.item == g_ui.cursor) {
            if (setting) change_setting(row, mouse_on_left_half() ? -1 : 1);
            else choose(row);
        }
        if (k.back || right) {
            if (g_ui.screen == Screen::Room) { g_ui.client->leave_room(); g_ui.screen = Screen::Main; }
            else if (g_ui.screen == Screen::List) g_ui.screen = Screen::Main;
            else close_menu();
            g_ui.cursor = 0;
        }
        break;
    }
    }
}

// ── drawing ─────────────────────────────────────────────────────────────────
void draw(Canvas& cv) {
    cv.shade(0, 0, cv.vw(), cv.vh(), 70);
    g_ui.hits.clear();
    const int lh = 15;
    std::string title = "ONLINE";
    if (g_ui.screen == Screen::Room && g_ui.client)
        title = "ROOM " + std::to_string(g_ui.client->room()) + (g_ui.client->is_host() ? "  (HOST)" : "");
    if (g_ui.screen == Screen::List) title = "FIND GAMES";
    draw_centered(cv, 6, title);
    int y = 28;
    // Who is here (room) / connection (elsewhere).
    if (g_ui.screen == Screen::Room && g_ui.client) {
        std::string who;
        for (const auto& m : g_ui.client->members()) who += (who.empty() ? "" : ", ") + upper(m.name);
        draw_centered(cv, y, "PLAYERS: " + who, 200);
        y += lh + 4;
        if (g_ui.client->members().size() < 2)
            { draw_centered(cv, y, "WAITING FOR A SECOND PLAYER", 160); y += lh + 2; }
        else if (!g_ui.client->is_host())
            { draw_centered(cv, y, "THE HOST PICKS THE GAME", 160); y += lh + 2; }
    }
    switch (g_ui.screen) {
    case Screen::Name: case Screen::Code: case Screen::Chat: {
        const char* prompt = g_ui.screen == Screen::Name ? "YOUR NAME" : g_ui.screen == Screen::Code ? "ROOM CODE" : "SAY";
        draw_centered(cv, y + 10, prompt, 200);
        const bool blink = (SDL_GetTicks() / 400) & 1;
        draw_centered(cv, y + 32, upper(g_ui.edit) + (blink ? "_" : " "), 256, true);
        // Buttons: OK (Enter) and CANCEL (Delete or right click).
        const int by = y + 62, gap = 24;
        const std::string ok = "OK", cancel = "CANCEL";
        const int okw = text_width(ok), cw = text_width(cancel);
        const int ox = (cv.vw() - okw - gap - cw) / 2, cx = ox + okw + gap;
        const bool over_ok = g_ui.mouse_vy >= by - 3 && g_ui.mouse_vy < by + lh + 3 &&
                             g_ui.mouse_vx >= ox - 8 && g_ui.mouse_vx < ox + okw + 8;
        const bool over_cancel = g_ui.mouse_vy >= by - 3 && g_ui.mouse_vy < by + lh + 3 &&
                                 g_ui.mouse_vx >= cx - 8 && g_ui.mouse_vx < cx + cw + 8;
        const bool ready = g_ui.screen != Screen::Code || g_ui.edit.size() == 4;
        cv.shade(ox - 6, by - 3, okw + 12, lh + 6, over_ok ? 40 : 120);
        cv.shade(cx - 6, by - 3, cw + 12, lh + 6, over_cancel ? 40 : 120);
        draw_text(cv, ox, by, ok, !ready ? 110 : over_ok ? 256 : 190, over_ok && ready);
        draw_text(cv, cx, by, cancel, over_cancel ? 256 : 190, over_cancel);
        g_ui.hits.push_back({ox - 8, by - 3, ox + okw + 8, by + lh + 3, kHitOk});
        g_ui.hits.push_back({cx - 8, by - 3, cx + cw + 8, by + lh + 3, kHitCancel});
        draw_centered(cv, by + lh + 10, "ENTER: OK   DELETE OR RIGHT CLICK: CANCEL", 130);
        break;
    }
    case Screen::Message:
        draw_centered(cv, cv.vh() / 2 - 16, g_ui.message, 256, true);
        draw_centered(cv, cv.vh() / 2 + 8, "CLICK OR PRESS ENTER", 150);
        break;
    case Screen::Starting:
        draw_centered(cv, cv.vh() / 2 - 8, "STARTING THE MATCH...", 256, true);
        break;
    default: {
        const auto r = rows();
        const int chat_lines = 3;
        const int bottom = cv.vh() - chat_lines * lh - 8;
        const int visible = std::max(1, (bottom - y) / lh);
        if (g_ui.cursor < g_ui.scroll) g_ui.scroll = g_ui.cursor;
        if (g_ui.cursor >= g_ui.scroll + visible) g_ui.scroll = g_ui.cursor - visible + 1;
        g_ui.scroll = std::clamp(g_ui.scroll, 0, std::max(0, static_cast<int>(r.size()) - visible));
        if (g_ui.screen == Screen::List && r.size() <= 2)
            draw_centered(cv, y, (g_ui.client && g_ui.client->state() == RoomClient::State::Connected)
                                     ? "NO OPEN ROOMS" : "CONNECTING...", 160), y += lh;
        for (int i = g_ui.scroll; i < static_cast<int>(r.size()) && i < g_ui.scroll + visible; ++i) {
            const bool sel = i == g_ui.cursor;
            std::string text = r[i].text;
            if (sel && (r[i].item == kModeRow || r[i].item == kSettingRow) && r[i].enabled) text = "< " + text + " >";
            draw_centered(cv, y, text, !r[i].enabled ? 110 : sel ? 256 : 165, sel && r[i].enabled);
            g_ui.hits.push_back({0, y, cv.vw(), y + lh, i});
            y += lh;
        }
        // More rows above or below: a hint that the wheel scrolls.
        if (g_ui.scroll > 0) draw_text(cv, cv.vw() - 20, 28, "^", 160);
        if (g_ui.scroll + visible < static_cast<int>(r.size())) draw_text(cv, cv.vw() - 20, y - lh, "v", 160);
        // Chat: the newest lines (click them to say something).
        {
            const int top = cv.vh() - chat_lines * lh - 4;
            const bool over = g_ui.mouse_vy >= top && g_ui.mouse_vy < cv.vh();
            cv.shade(0, top, cv.vw(), cv.vh() - top, over ? 150 : 200);
            g_ui.hits.push_back({0, top, cv.vw(), cv.vh(), kHitChat});
            if (!g_ui.client || g_ui.client->chat().empty())
                draw_text(cv, 4, cv.vh() - lh - 2, over ? "CLICK HERE TO CHAT" : "CHAT", over ? 220 : 120);
        }
        if (g_ui.client) {
            const auto& chat = g_ui.client->chat();
            int cy = cv.vh() - chat_lines * lh - 2;
            for (std::size_t i = chat.size() > static_cast<std::size_t>(chat_lines) ? chat.size() - chat_lines : 0;
                 i < chat.size(); ++i, cy += lh) {
                std::string line = (chat[i].from.empty() ? "" : upper(chat[i].from) + ": ") + upper(chat[i].text);
                while (!line.empty() && text_width(line) > cv.vw() - 8) line.pop_back();
                draw_text(cv, 4, cy, line, chat[i].room ? 230 : 180);
            }
        }
        break;
    }
    }
    // Server status, top right.
    std::string st = !g_ui.client ? "" : g_ui.client->state() == RoomClient::State::Connected ? "ONLINE"
                   : g_ui.client->state() == RoomClient::State::Connecting ? "CONNECTING" : "";
    if (!st.empty()) draw_text(cv, cv.vw() - text_width(st) - 4, 6, st, 140);
}

// ── hooks into the runtime ──────────────────────────────────────────────────
int (*g_prev_present)(gbarecomp::RunOptions::PresentFrame*) = nullptr;
std::uint16_t (*g_prev_input)(const gbarecomp::TouchFrameInfo*) = nullptr;

int present_frame(gbarecomp::RunOptions::PresentFrame* frame) {
    const int own = g_prev_present ? g_prev_present(frame) : 0;
    if (!open()) return own;
    if (gba::GbaBus* bus = gbarecomp::active_bus()) { g_rom = bus->rom_ptr(); g_rom_size = bus->rom_size(); }
    int w, h;
    if (own) {
        w = static_cast<int>(frame->width); h = static_cast<int>(frame->height);
        g_image.assign(frame->argb, frame->argb + static_cast<std::size_t>(w) * h);
    } else {
        // The plain picture, scaled to the window by whole pixels.
        w = std::max(240, frame->drawable_width); h = std::max(160, frame->drawable_height);
        g_image.assign(static_cast<std::size_t>(w) * h, 0xFF000000u);
        const int s = std::max(1, std::min(w / 240, h / 160));
        const int ox = (w - 240 * s) / 2, oy = (h - 160 * s) / 2;
        for (int y = 0; y < 160 * s; ++y) {
            const std::uint8_t* src = frame->guest_rgb + (y / s) * frame->guest_width * 3;
            std::uint32_t* dst = g_image.data() + static_cast<std::size_t>(oy + y) * w + ox;
            for (int x = 0; x < 240 * s; ++x) {
                const std::uint8_t* p = src + (x / s) * 3;
                dst[x] = 0xFF000000u | (p[0] << 16) | (p[1] << 8) | p[2];
            }
        }
    }
    Canvas cv{g_image.data(), w, h, std::max(1, h / 270)};
    g_ui.canvas_scale = cv.scale; g_ui.image_w = w; g_ui.image_h = h;
    draw(cv);
    frame->argb = g_image.data();
    frame->width = static_cast<unsigned>(w);
    frame->height = static_cast<unsigned>(h);
    return 1;
}

// Every key and mouse button up (the game's own bindings, WASD, clicks).
bool all_released(gba::GbaBus* bus) {
    if (bus && (bus->io().host_keyinput() & 0x03FF) != 0x03FF) return false;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return true;
    if (SDL_GetMouseState(nullptr, nullptr) & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK)) return false;
    int n = 0;
    const Uint8* k = SDL_GetKeyboardState(&n);
    for (int i = 0; i < n; ++i) if (k[i]) return false;
    return true;
}

std::uint16_t input_frame(const gbarecomp::TouchFrameInfo* info) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!open() && !g_ui.draining) return g_prev_input ? g_prev_input(info) : 0x03FF;
    // The menu owns the keyboard and mouse: the game's own key bindings are
    // hidden from it (they reach the guest beside this hook, so they are
    // blocked at the source), and the pad controls are not asked at all.
    if (bus) bus->io().set_host_keys_blocked(true);
    if (open()) {
        if (SDL_WasInit(SDL_INIT_VIDEO)) SDL_ShowCursor(SDL_ENABLE);
        pump_network();
        if (g_ui.settling && !SDL_WasInit(SDL_INIT_VIDEO)) g_ui.settling = false;   // headless tests
        if (g_ui.settling) {
            read_keys(); read_mouse();          // brings their last-seen states up to date
            if (all_released(bus)) g_ui.settling = false;
            return 0x03FF;
        }
        if (open()) handle_input();
        return 0x03FF;
    }
    gta_mouse_wheel_take();                     // scrolls meant for the menu
    if (all_released(bus)) {
        g_ui.draining = false;
        if (bus) bus->io().set_host_keys_blocked(false);
    }
    return 0x03FF;
}

// String lookup: our ids get our text.
int string_hook(std::uint32_t, int, ArmCpuState* cpu) {
    if (cpu->R[0] != kOnlineStringId) return 0;
    cpu->R[0] = 0x08000000u + static_cast<std::uint32_t>(kOnlineText);
    const std::uint32_t ret = cpu->R[14] & ~1u;
    runtime_call_should_return(ret);
    cpu->R[15] = ret;
    return 1;
}

// ONLINE on the title menu takes the link-handshake action like MULTI PLAYER;
// the menu routine's cursor says which entry it was.
int handshake_hook(std::uint32_t, int, ArmCpuState* cpu) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return 0;
    const std::uint8_t* iw = bus->iwram_ptr();
    // The menu routine's locals while it calls the handshake: the open menu
    // (IWRAM 0x03007D80) and the cursor (0x03007D78).
    std::uint16_t cursor; std::memcpy(&cursor, iw + 0x7D78, 2);
    if (rd32(iw + 0x7D80) != 0x08000000u + kTitleMenu || cursor != kTitleOnlineSlot) return 0;
    open_menu();
    cpu->R[0] = 0;                       // "not connected": the title menu stays
    const std::uint32_t ret = cpu->R[14] & ~1u;
    runtime_call_should_return(ret);
    cpu->R[15] = ret;
    return 1;
}

// ONLINE in the hideout calls the game's empty handler (0x08000734), which
// otherwise only the interrupt dispatcher calls.
int hideout_hook(std::uint32_t, int, ArmCpuState* cpu) {
    const std::uint32_t lr = cpu->R[14] & ~1u;
    if (lr == 0x080001F0u || lr == 0x020005BCu) return 0;
    open_menu();
    return 0;                            // the empty body runs: back to the menu
}

const int g_hooks_registered =
    gba_mod_register_function_entry_plugin("gta.online", kStringLookup, 1, string_hook) &&
    gba_mod_register_function_entry_plugin("gta.online", kLinkHandshake, 1, handshake_hook) &&
    gba_mod_register_function_entry_plugin("gta.online", kEmptyStub, 1, hideout_hook);

}  // namespace

bool gta_online_prepare_rom(std::uint8_t* rom, std::size_t size) {
    if (size < 0x01000000) return false;
    // Text, in the zero-filled end of the ROM.
    static const char kOnline[] = "ONLINE";
    for (std::size_t i = 0; i < 64; ++i) if (rom[kOnlineText + i]) return false;
    std::memcpy(rom + kOnlineText, kOnline, sizeof(kOnline));
    // Title menu: a sixth entry (after MULTI PLAYER and PICK LEVEL).
    std::uint8_t* slot = rom + kTitleMenu + 8 + kTitleOnlineSlot * kEntry;
    bool free = true;
    for (std::size_t i = 0; i < kEntry; ++i) free &= slot[i] == 0;
    if (free && rd32(rom + kTitleMenu + 4) == 5) {
        const std::uint32_t label = kOnlineStringId;
        std::memcpy(slot + 4, &label, 4);
        slot[12] = 5;                                  // the link action, answered by handshake_hook
        const std::uint32_t count = 6;
        std::memcpy(rom + kTitleMenu + 4, &count, 4);
    }
    // Hideout menus (one table per index of the menu routine; two of them are
    // SAVE GAME, HIDEOUT ITEMS, RESUME GAME): a fourth entry after RESUME GAME.
    for (std::size_t k = 0; k < 22; ++k) {
        std::uint8_t* menu = rom + kHideoutMenu + k * kHideoutStride;
        std::uint8_t* entries = menu + 0x18;
        std::uint8_t* h = entries + 3 * 0x20;
        if (rd32(menu + 0x14) != 3 || rd32(entries + 4) != 32 || rd32(entries + 0x20 + 4) != 423 ||
            rd32(entries + 0x40 + 4) != 8 || rd32(h + 4) != 0 || rd32(h + 8) != 0xFFFFFFFFu || rd32(h + 0x18) != 0)
            continue;
        const std::uint32_t flags = 2, label = kOnlineStringId, callback = kEmptyStub | 1u, count = 4;
        std::memcpy(h, &flags, 4);
        std::memcpy(h + 4, &label, 4);
        std::memcpy(h + 0x18, &callback, 4);
        std::memcpy(menu + 0x14, &count, 4);
    }
    return true;
}

void gta_online_install(gbarecomp::RunOptions* opts, int argc, char** argv, const std::string& build_tag) {
    g_build = build_tag;
    const char* argv0 = argc > 0 ? argv[0] : nullptr;
    // A save chosen on the command line goes with the game into a match.
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--save-path")) g_save_path = argv[i + 1];
    std::error_code ec;
    g_exe = std::filesystem::absolute(argv0 ? argv0 : "GTAAdvanceRecomp.exe", ec).string();
#if defined(_WIN32)
    char path[MAX_PATH];
    if (GetModuleFileNameA(nullptr, path, MAX_PATH)) g_exe = path;
#endif
    g_ini = std::filesystem::path(g_exe).parent_path() / "gta_online.ini";
    load_ini();
    g_prev_present = opts->present_frame;
    g_prev_input = opts->input_frame;
    opts->present_frame = present_frame;
    opts->input_frame = input_frame;
    if (g_hooks_registered) gba_mod_set_function_hook_enabled("gta.online", 1);
}

int gta_online_tcp_command(const char* request, void (*write)(void*, const char*, std::size_t), void* ctx) {
    if (!std::strstr(request, "\"gta_online\"")) return 0;
    auto field = [&](const char* key) -> std::string {
        const char* at = std::strstr(request, key);
        if (!at || !(at = std::strchr(at + std::strlen(key), ':')) || !(at = std::strchr(at, '"'))) return "";
        const char* end = std::strchr(at + 1, '"');
        return end ? std::string(at + 1, end) : "";
    };
    if (std::strstr(request, "\"open\"")) open_menu();
    if (std::strstr(request, "\"close\"")) close_menu();
    if (const std::string k = field("\"key\""); !k.empty()) g_test_key = k;
    if (const std::string t = field("\"text\""); !t.empty()) g_test_text = t;
    if (const std::string server = field("\"server\""); !server.empty()) g_server = server;
    if (const std::string name = field("\"name\""); !name.empty()) g_name = name;
    // Pointer tests, in the menu's virtual pixels: {"mx":..,"my":..,"click":1,"right":1,"wheel":-1}
    auto number = [&](const char* key, int* out) {
        const char* at = std::strstr(request, key);
        if (!at || !(at = std::strchr(at + std::strlen(key), ':'))) return false;
        *out = std::atoi(at + 1);
        return true;
    };
    number("\"mx\"", &g_test_mx); number("\"my\"", &g_test_my);
    number("\"click\"", &g_test_click); number("\"right\"", &g_test_right);
    if (int w = 0; number("\"wheel\"", &w)) gta_mouse_wheel_inject(w);
    if (const std::string path = field("\"shot\""); !path.empty()) {
        // The menu over a grey 960x540 picture, as PPM.
        if (gba::GbaBus* bus = gbarecomp::active_bus()) { g_rom = bus->rom_ptr(); g_rom_size = bus->rom_size(); }
        std::vector<std::uint32_t> img(960 * 540, 0xFF506070u);
        Canvas cv{img.data(), 960, 540, 2};
        g_ui.canvas_scale = 2; g_ui.image_w = 960; g_ui.image_h = 540;
        draw(cv);
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fprintf(f, "P6\n960 540\n255\n");
            for (std::uint32_t c : img) {
                const unsigned char px[3] = {static_cast<unsigned char>(c >> 16), static_cast<unsigned char>(c >> 8),
                                             static_cast<unsigned char>(c)};
                std::fwrite(px, 1, 3, f);
            }
            std::fclose(f);
        }
    }
    char out[256];
    const int conn = !g_ui.client ? 0 : g_ui.client->state() == RoomClient::State::Connected ? 2 : 1;
    gba::GbaBus* bus = gbarecomp::active_bus();
    std::snprintf(out, sizeof(out),
                  "{\"ok\":true,\"screen\":%d,\"cursor\":%d,\"room\":%u,\"rows\":%d,\"hits\":%d,\"conn\":%d,"
                  "\"mode\":%d,\"v0\":%d,\"blocked\":%d,\"chat\":%d}",
                  static_cast<int>(g_ui.screen), g_ui.cursor, g_ui.client ? g_ui.client->room() : 0u,
                  static_cast<int>(rows().size()), static_cast<int>(g_ui.hits.size()), conn, g_ui.mode,
                  g_ui.values[0], bus && bus->io().host_keys_blocked() ? 1 : 0,
                  g_ui.client ? static_cast<int>(g_ui.client->chat().size()) : 0);
    write(ctx, out, std::strlen(out));
    return 1;
}
