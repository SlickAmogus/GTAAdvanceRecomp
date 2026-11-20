#include "online/room_client.h"

#include <chrono>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_type = int;
#define GTA_CLOSE closesocket
static constexpr std::uintptr_t kNoSocket = static_cast<std::uintptr_t>(INVALID_SOCKET);
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socklen_type = socklen_t;
#define GTA_CLOSE close
static constexpr std::uintptr_t kNoSocket = static_cast<std::uintptr_t>(-1);
#endif

namespace gtaonline {
namespace {

// Wire format: a 12-byte header ("SHO1", version 1, type, u16 payload
// length, u32 session), then the payload; integers little-endian, strings
// fixed-width and zero-padded. 1200 bytes at most per datagram.
constexpr int kHeader = 12, kMtu = 1200, kNameMax = 24, kBuildMax = 24, kChatMax = 160;
enum : std::uint8_t {
    kChatSay = 0x35, kChatMsg = 0x36,
    kHello = 0x01, kWelcome = 0x02, kPing = 0x03, kBye = 0x05, kReject = 0x06, kChallenge = 0x07,
    kRoomCreate = 0x60, kRoomJoin = 0x61, kRoomLeave = 0x62, kRoomListReq = 0x63,
    kRoomJoined = 0x64, kRoomRoster = 0x65, kRoomReject = 0x66, kRoomList = 0x67, kRelay = 0x68,
};

struct Writer {
    std::uint8_t* b; int at;
    void u8(unsigned v) { b[at++] = static_cast<std::uint8_t>(v); }
    void u16(unsigned v) { u8(v & 0xFF); u8((v >> 8) & 0xFF); }
    void u32(std::uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); }
    void str(const std::string& s, int width) {
        for (int i = 0; i < width; ++i)
            u8(i < width - 1 && i < static_cast<int>(s.size()) ? static_cast<unsigned char>(s[i]) : 0);
    }
};

struct Reader {
    const std::uint8_t* b; int len, at = 0;
    bool has(int n) const { return at + n <= len; }
    unsigned u8() { return has(1) ? b[at++] : (at = len + 1, 0u); }
    unsigned u16() { unsigned lo = u8(); return lo | (u8() << 8); }
    std::uint32_t u32() { std::uint32_t lo = u16(); return lo | (std::uint32_t(u16()) << 16); }
    std::string str(int width) {
        std::string s;
        for (int i = 0; i < width; ++i) {
            const unsigned c = u8();
            if (c == 0) { at += width - i - 1; break; }
            s += (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
        }
        return s;
    }
    bool ok() const { return at <= len; }
};

void put_header(std::uint8_t* b, std::uint8_t type, int payload, std::uint32_t session) {
    Writer w{b, 0};
    w.u8('S'); w.u8('H'); w.u8('O'); w.u8('1'); w.u8(1); w.u8(type);
    w.u16(static_cast<unsigned>(payload)); w.u32(session);
}

bool parse_header(const std::uint8_t* b, int len, std::uint8_t* type, int* payload) {
    if (len < kHeader || b[0] != 'S' || b[1] != 'H' || b[2] != 'O' || b[3] != '1' || b[4] != 1) return false;
    *type = b[5];
    *payload = b[6] | (b[7] << 8);
    return kHeader + *payload <= len;
}

bool net_started() {
#if defined(_WIN32)
    static const bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    return ok;
#else
    return true;
#endif
}

}  // namespace

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

RoomClient::RoomClient() : sock_(kNoSocket) {}
RoomClient::~RoomClient() { disconnect(); }

bool RoomClient::connect(const std::string& server, const std::string& name, const std::string& build,
                         std::uint32_t reconnect) {
    disconnect();
    reconnect_ = reconnect;
    error_.clear();
    if (!net_started()) { error_ = "networking unavailable"; state_ = State::Failed; return false; }
    std::string host = server, port = std::to_string(kDefaultPort);
    if (const auto colon = server.rfind(':'); colon != std::string::npos) {
        host = server.substr(0, colon); port = server.substr(colon + 1);
    }
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || !found) {
        error_ = "cannot find the server " + server; state_ = State::Failed; return false;
    }
    std::memcpy(addr_, found->ai_addr, sizeof(sockaddr_in));
    freeaddrinfo(found);
    const auto s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (static_cast<std::uintptr_t>(s) == kNoSocket) {
        error_ = "cannot open a socket"; state_ = State::Failed; return false;
    }
#if defined(_WIN32)
    u_long nonblocking = 1; ioctlsocket(s, FIONBIO, &nonblocking);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    sock_ = static_cast<std::uintptr_t>(s);
    name_ = name.empty() ? "Player" : name;
    build_ = build;
    cookie_ = session_ = my_id_ = host_id_ = 0;
    room_ = 0; members_.clear(); listing_.clear();
    state_ = State::Connecting;
    next_hello_ms_ = 0;
    connect_deadline_ms_ = now_ms() + 8000;
    pump();
    return true;
}

void RoomClient::disconnect() {
    if (sock_ != kNoSocket) {
        if (state_ == State::Connected) send_message(kBye, nullptr, 0);
        GTA_CLOSE(static_cast<decltype(socket(0, 0, 0))>(sock_));
        sock_ = kNoSocket;
    }
    if (state_ != State::Failed) state_ = State::Idle;
    room_ = 0; members_.clear();
}

void RoomClient::abandon() {
    if (sock_ != kNoSocket) {
        GTA_CLOSE(static_cast<decltype(socket(0, 0, 0))>(sock_));
        sock_ = kNoSocket;
    }
    state_ = State::Idle;
}

void RoomClient::add_chat(const std::string& from, const std::string& text, bool room) {
    chat_.push_back({from, text, room, now_ms()});
    if (chat_.size() > 50) chat_.erase(chat_.begin());
    ++chat_serial_;
}

void RoomClient::say(const std::string& text, bool room) {
    if (text.empty() || state_ != State::Connected) return;
    std::string line = text.substr(0, kChatMax - 1);
    if (room) {
        // Inside the room: a control note, kind 3, then the text.
        std::string note(1, '\x03');
        note += line;
        if (!send_relay(kControl, note.data(), static_cast<int>(note.size()))) return;
        add_chat(name_, line, true);
        return;
    }
    // scope (0 = everyone on the server), pad, pad16, text.
    std::uint8_t p[4 + kChatMax];
    Writer w{p, 0};
    w.u8(0); w.u8(0); w.u16(0); w.str(line, kChatMax);
    send_message(kChatSay, p, w.at);
}

void RoomClient::send_message(std::uint8_t type, const std::uint8_t* payload, int len) {
    if (sock_ == kNoSocket || len < 0 || kHeader + len > kMtu) return;
    std::uint8_t buf[kMtu];
    put_header(buf, type, len, session_);
    if (len) std::memcpy(buf + kHeader, payload, static_cast<std::size_t>(len));
    sendto(static_cast<decltype(socket(0, 0, 0))>(sock_), reinterpret_cast<const char*>(buf), kHeader + len, 0,
           reinterpret_cast<const sockaddr*>(addr_), sizeof(sockaddr_in));
}

void RoomClient::send_hello() {
    // version, pad, character, reconnect, name, build, password proof,
    // cookie, install id, capabilities (none: the plain room and list rows).
    std::uint8_t p[2 + 1 + 1 + 4 + kNameMax + kBuildMax + 4 + 4 + 4 + 4];
    Writer w{p, 0};
    w.u16(1); w.u8(0); w.u8(0); w.u32(reconnect_);
    w.str(name_, kNameMax); w.str(build_, kBuildMax);
    w.u32(0); w.u32(cookie_); w.u32(0); w.u32(0);
    send_message(kHello, p, w.at);
}

void RoomClient::pump() {
    if (sock_ == kNoSocket) return;
    const auto native = static_cast<decltype(socket(0, 0, 0))>(sock_);
    std::uint8_t buf[2048];
    for (int i = 0; i < 256; ++i) {
        sockaddr_in from{}; socklen_type flen = sizeof(from);
        const int n = static_cast<int>(recvfrom(native, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                                reinterpret_cast<sockaddr*>(&from), &flen));
        if (n <= 0) break;
        if (std::memcmp(&from.sin_addr, &reinterpret_cast<const sockaddr_in*>(addr_)->sin_addr, 4) != 0 ||
            from.sin_port != reinterpret_cast<const sockaddr_in*>(addr_)->sin_port)
            continue;
        handle(buf, n);
    }
    const auto now = now_ms();
    if (state_ == State::Connecting) {
        if (now >= connect_deadline_ms_) {
            error_ = "the server did not answer"; state_ = State::Failed; return;
        }
        if (now >= next_hello_ms_) { send_hello(); next_hello_ms_ = now + 500; }
    } else if (state_ == State::Connected && now >= next_ping_ms_) {
        std::uint8_t p[4]; Writer w{p, 0}; w.u32(static_cast<std::uint32_t>(now));
        send_message(kPing, p, 4);
        next_ping_ms_ = now + 2000;
    }
}

void RoomClient::wait_and_pump(int ms) {
    if (sock_ != kNoSocket && ms > 0) {
        const auto native = static_cast<decltype(socket(0, 0, 0))>(sock_);
        fd_set set; FD_ZERO(&set); FD_SET(native, &set);
        timeval tv{0, ms * 1000};
        select(static_cast<int>(native) + 1, &set, nullptr, nullptr, &tv);
    }
    pump();
}

void RoomClient::handle(const std::uint8_t* data, int len) {
    std::uint8_t type; int payload;
    if (!parse_header(data, len, &type, &payload)) return;
    Reader r{data + kHeader, payload};
    switch (type) {
    case kChallenge:
        if (state_ == State::Connecting) { cookie_ = r.u32(); send_hello(); next_hello_ms_ = now_ms() + 500; }
        break;
    case kWelcome:
        if (state_ != State::Connecting) break;
        session_ = r.u32(); my_id_ = r.u32();
        if (r.ok()) { state_ = State::Connected; next_ping_ms_ = 0; }
        break;
    case kReject: {
        r.u8(); r.u8(); r.u16();
        error_ = r.str(96);
        if (error_.empty()) error_ = "the server refused the connection";
        state_ = State::Failed;
        break;
    }
    case kChatMsg: {
        // scope, pad, pad16, sender id, sender name, text (more may follow).
        r.u8(); r.u8(); r.u16();
        const std::uint32_t from = r.u32();
        std::string who = r.str(kNameMax), text = r.str(kChatMax);
        if (!r.ok() || text.empty()) break;
        if (from == 0 && who.empty()) who = "SERVER";
        add_chat(who, text, false);
        break;
    }
    case kRoomJoined: read_roster(data + kHeader, payload, true); break;
    case kRoomRoster: read_roster(data + kHeader, payload, false); break;
    case kRoomReject: r.u8(); room_error_ = r.str(96); break;
    case kRoomList: {
        const unsigned n = r.u8();
        // Rows are code, players, seats, host name, map (29 bytes for a client
        // that asks for no extras); newer servers add a page number and a
        // "more" flag after them, older ones do not.
        const int row = 2 + 1 + 1 + kNameMax + 1;
        const int trailer = payload - 1 - static_cast<int>(n) * row;
        if (trailer != 0 && trailer != 2) break;
        int page = 0;
        if (trailer) { Reader pr{data + kHeader, payload, 1 + static_cast<int>(n) * row}; page = static_cast<int>(pr.u8()); }
        if (page == 0) listing_.clear();
        for (unsigned i = 0; i < n; ++i) {
            Reader e{data + kHeader, payload, 1 + static_cast<int>(i) * row};
            RoomListing l;
            l.code = static_cast<std::uint16_t>(e.u16());
            l.players = static_cast<int>(e.u8()); l.max_players = static_cast<int>(e.u8());
            l.host = e.str(kNameMax);
            if (e.ok()) listing_.push_back(l);
        }
        ++listing_serial_;
        break;
    }
    case kRelay: {
        const std::uint32_t from = r.u32();
        const std::uint8_t* inner = data + kHeader + 4;
        const int inner_len = payload - 4;
        std::uint8_t channel; int inner_payload;
        if (inner_len <= 0 || !parse_header(inner, inner_len, &channel, &inner_payload)) break;
        if ((channel == kGame || channel == kControl) && on_relay)
            on_relay(static_cast<Channel>(channel), from, inner + kHeader, inner_payload);
        break;
    }
    default: break;
    }
}

void RoomClient::read_roster(const std::uint8_t* p, int len, bool joined) {
    Reader r{p, len};
    if (joined) my_id_ = r.u32();
    room_ = static_cast<std::uint16_t>(r.u16());
    host_id_ = r.u32();
    const unsigned n = r.u8();
    members_.clear();
    if (!room_ || !host_id_) { room_ = 0; return; }   // the room closed
    const int rest = len - r.at;
    const int row = n ? rest / static_cast<int>(n) : 0;
    const int start = r.at;
    for (unsigned i = 0; i < n && row >= 4 + kNameMax + 1; ++i) {
        Reader e{p, len, start + static_cast<int>(i) * row};
        RoomMember m;
        m.id = e.u32(); m.name = e.str(kNameMax); m.host = e.u8() != 0;
        if (e.ok()) members_.push_back(m);
    }
}

void RoomClient::create_room(int max_players) {
    room_error_.clear();
    const std::uint8_t p[2] = {static_cast<std::uint8_t>(max_players), 0};
    send_message(kRoomCreate, p, 2);
}

void RoomClient::join_room(std::uint16_t code) {
    room_error_.clear();
    std::uint8_t p[2]; Writer w{p, 0}; w.u16(code);
    send_message(kRoomJoin, p, 2);
}

void RoomClient::leave_room() {
    send_message(kRoomLeave, nullptr, 0);
    room_ = 0; members_.clear();
}

void RoomClient::request_list() {
    const std::uint8_t page = 0;
    send_message(kRoomListReq, &page, 1);
}

bool RoomClient::send_relay(Channel channel, const void* data, int len, std::uint32_t to) {
    if (state_ != State::Connected || !room_ || len < 0 || len > kMaxRelayData) return false;
    std::uint8_t p[kMtu];
    Writer w{p, 0};
    w.u32(to);
    put_header(p + 4, channel, len, 0);
    if (len) std::memcpy(p + 4 + kHeader, data, static_cast<std::size_t>(len));
    send_message(kRelay, p, 4 + kHeader + len);
    return true;
}

}  // namespace gtaonline
