#include "online/relay_bridge.h"

#include <cstring>
#include <map>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using native_socket = SOCKET;
using socklen_type = int;
#define GTA_CLOSE closesocket
static const std::uintptr_t kNoSocket = static_cast<std::uintptr_t>(INVALID_SOCKET);
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
using native_socket = int;
using socklen_type = socklen_t;
#define GTA_CLOSE close
static const std::uintptr_t kNoSocket = static_cast<std::uintptr_t>(-1);
#endif

namespace gtaonline {
namespace {

// Each relay message: u16 datagram number, u8 piece, u8 pieces, then data.
constexpr int kPiece = RoomClient::kMaxRelayData - 4;

native_socket bind_loopback(unsigned short* port) {
    const native_socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (static_cast<std::uintptr_t>(s) == kNoSocket) return s;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    socklen_type len = sizeof(a);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 ||
        getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
        GTA_CLOSE(s);
        return static_cast<native_socket>(kNoSocket);
    }
    *port = ntohs(a.sin_port);
    return s;
}

}  // namespace

RelayBridge::RelayBridge(std::unique_ptr<RoomClient> client)
    : client_(std::move(client)), local_(kNoSocket) {}

RelayBridge::~RelayBridge() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (local_ != kNoSocket) GTA_CLOSE(static_cast<native_socket>(local_));
    if (client_) { client_->leave_room(); client_->disconnect(); }
}

bool RelayBridge::start(std::uint32_t peer_id) {
    peer_ = peer_id;
    // A free port for netplay to bind: taken here and handed back at once.
    {
        const native_socket probe = bind_loopback(&netplay_port_);
        if (static_cast<std::uintptr_t>(probe) == kNoSocket) { error_ = "no free local port"; return false; }
        GTA_CLOSE(probe);
    }
    const native_socket s = bind_loopback(&bridge_port_);
    if (static_cast<std::uintptr_t>(s) == kNoSocket) { error_ = "no free local port"; return false; }
#if defined(_WIN32)
    u_long nonblocking = 1; ioctlsocket(s, FIONBIO, &nonblocking);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    local_ = static_cast<std::uintptr_t>(s);
    thread_ = std::thread([this] { run(); });
    return true;
}

void RelayBridge::run() {
    const native_socket s = static_cast<native_socket>(local_);
    sockaddr_in netplay{};
    netplay.sin_family = AF_INET;
    netplay.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    netplay.sin_port = htons(netplay_port_);
    std::uint16_t next_id = 0;
    struct Pending { int count = 0, have = 0; std::vector<std::vector<std::uint8_t>> pieces; };
    std::map<std::uint16_t, Pending> pending;

    client_->on_relay = [&](RoomClient::Channel channel, std::uint32_t from, const std::uint8_t* p, int len) {
        if (channel != RoomClient::kGame || from != peer_ || len < 4) return;
        const std::uint16_t id = static_cast<std::uint16_t>(p[0] | (p[1] << 8));
        const int index = p[2], count = p[3];
        if (count == 0 || index >= count) return;
        if (count == 1) {
            sendto(s, reinterpret_cast<const char*>(p + 4), len - 4, 0,
                   reinterpret_cast<const sockaddr*>(&netplay), sizeof(netplay));
            return;
        }
        // Keep a few datagrams in reassembly; a lost piece just drops one
        // datagram, which netplay resends like any other loss.
        if (pending.size() > 16) pending.erase(pending.begin());
        Pending& d = pending[id];
        if (d.count != count) { d = Pending{}; d.count = count; d.pieces.resize(static_cast<std::size_t>(count)); }
        auto& piece = d.pieces[static_cast<std::size_t>(index)];
        if (!piece.empty()) return;
        piece.assign(p + 4, p + len);
        if (++d.have == d.count) {
            std::vector<std::uint8_t> whole;
            for (const auto& part : d.pieces) whole.insert(whole.end(), part.begin(), part.end());
            sendto(s, reinterpret_cast<const char*>(whole.data()), static_cast<int>(whole.size()), 0,
                   reinterpret_cast<const sockaddr*>(&netplay), sizeof(netplay));
            pending.erase(id);
        }
    };

    std::uint8_t buf[65536], msg[RoomClient::kMaxRelayData];
    while (!stop_) {
        for (int i = 0; i < 256; ++i) {
            sockaddr_in from{}; socklen_type flen = sizeof(from);
            const int n = static_cast<int>(recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                                    reinterpret_cast<sockaddr*>(&from), &flen));
            if (n <= 0) break;
            netplay.sin_port = from.sin_port;   // wherever netplay sends from
            const int count = (n + kPiece - 1) / kPiece;
            if (count > 255) continue;
            const std::uint16_t id = next_id++;
            for (int k = 0; k < count; ++k) {
                const int at = k * kPiece, size = (n - at < kPiece) ? n - at : kPiece;
                msg[0] = static_cast<std::uint8_t>(id); msg[1] = static_cast<std::uint8_t>(id >> 8);
                msg[2] = static_cast<std::uint8_t>(k); msg[3] = static_cast<std::uint8_t>(count);
                std::memcpy(msg + 4, buf + at, static_cast<std::size_t>(size));
                client_->send_relay(RoomClient::kGame, msg, 4 + size, peer_);
            }
        }
        client_->wait_and_pump(1);
    }
    client_->on_relay = nullptr;
}

}  // namespace gtaonline
