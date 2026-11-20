#include "online/online_match.h"

#include "online/relay_bridge.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

namespace gtaonline {
namespace {

// Control notes on the relay's control channel: u8 kind, then the kind's data.
enum : std::uint8_t {
    kStart = 1,     // host -> guest: u32 netplay session id; the match starts
    kStartAck = 2,  // guest -> host: u32 the same id; seen, starting
};

std::unique_ptr<RelayBridge> g_bridge;   // the match's relay, for the process lifetime

void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint32_t get32(const std::uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24);
}

}  // namespace

bool prepare_online_match(std::vector<std::string>& args, const std::string& build_tag, std::string* error) {
    std::string server = kDefaultServer, name = "Player";
    bool host = false;
    long code = -1;
    std::vector<std::string> kept;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool has_value = i + 1 < args.size();
        if (i > 0 && a == "--online-host") host = true;
        else if (i > 0 && a == "--online-join" && has_value) code = std::strtol(args[++i].c_str(), nullptr, 10);
        else if (i > 0 && a == "--online-server" && has_value) server = args[++i];
        else if (i > 0 && a == "--online-name" && has_value) name = args[++i];
        else kept.push_back(a);
    }
    if (!host && code < 0) return true;
    args = std::move(kept);
    if (!host && (code < 1000 || code > 9999)) { *error = "a room code has four digits"; return false; }

    auto client = std::make_unique<RoomClient>();
    std::uint32_t session = 0, peer = 0;
    bool acked = false;
    client->on_relay = [&](RoomClient::Channel channel, std::uint32_t from, const std::uint8_t* p, int len) {
        if (channel != RoomClient::kControl || len < 5) return;
        if (!host && p[0] == kStart && from == client->host_id()) { session = get32(p + 1); peer = from; }
        if (host && p[0] == kStartAck && get32(p + 1) == session) { acked = true; peer = from; }
    };
    if (!client->connect(server, name, build_tag)) { *error = client->error(); return false; }
    std::fprintf(stderr, "online: connecting to %s\n", server.c_str()); std::fflush(stderr);
    while (client->state() == RoomClient::State::Connecting) client->wait_and_pump(20);
    if (client->state() != RoomClient::State::Connected) { *error = client->error(); return false; }

    if (host) client->create_room(2);
    else client->join_room(static_cast<std::uint16_t>(code));
    const auto room_deadline = now_ms() + 8000;
    while (!client->room()) {
        client->wait_and_pump(20);
        if (!client->room_error().empty()) { *error = client->room_error(); return false; }
        if (client->state() != RoomClient::State::Connected) { *error = client->error(); return false; }
        if (now_ms() > room_deadline) { *error = "the server did not open the room"; return false; }
    }
    std::fprintf(stderr, "online: in room %u (%s)\n", client->room(), host ? "host" : "guest"); std::fflush(stderr);

    if (host) {
        // Wait for a second player, then tell them the match starts.
        while (client->members().size() < 2) {
            client->wait_and_pump(50);
            if (client->state() != RoomClient::State::Connected) { *error = client->error(); return false; }
        }
        std::uint32_t r = static_cast<std::uint32_t>(now_ms()) * 2654435761u;
        session = (r ^ (std::uint32_t(client->room()) << 16)) | 1u;
        std::uint8_t note[5] = {kStart}; put32(note + 1, session);
        const auto deadline = now_ms() + 20000;
        while (!acked) {
            client->send_relay(RoomClient::kControl, note, 5);
            for (int k = 0; k < 10 && !acked; ++k) client->wait_and_pump(20);
            if (now_ms() > deadline || client->members().size() < 2) {
                *error = "the other player did not answer"; return false;
            }
        }
    } else {
        while (!session) {
            client->wait_and_pump(50);
            if (!client->room()) { *error = "the room closed"; return false; }
            if (client->state() != RoomClient::State::Connected) { *error = client->error(); return false; }
        }
        std::uint8_t note[5] = {kStartAck}; put32(note + 1, session);
        for (int k = 0; k < 5; ++k) { client->send_relay(RoomClient::kControl, note, 5); client->wait_and_pump(30); }
    }
    std::fprintf(stderr, "online: match %08x starts (peer %u)\n", session, peer); std::fflush(stderr);

    client->on_relay = nullptr;
    g_bridge = std::make_unique<RelayBridge>(std::move(client));
    if (!g_bridge->start(peer)) { *error = g_bridge->error(); g_bridge.reset(); return false; }
    args.push_back("--netplay-bind");
    args.push_back("127.0.0.1:" + std::to_string(g_bridge->netplay_port()));
    args.push_back("--netplay-peer");
    args.push_back("127.0.0.1:" + std::to_string(g_bridge->bridge_port()));
    args.push_back("--netplay-seat");
    args.push_back(host ? "0" : "1");
    args.push_back("--netplay-session");
    args.push_back(std::to_string(session));
    return true;
}

}  // namespace gtaonline
