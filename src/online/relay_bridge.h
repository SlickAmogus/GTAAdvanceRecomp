// Carries a netplay session through the room relay.
//
// The framework's netplay speaks UDP to one peer address. The bridge gives it
// one on this PC: netplay binds 127.0.0.1:netplay_port() and dials
// 127.0.0.1:bridge_port(); every datagram it sends there goes to the other
// room member through the server, and what they send comes back the same way.
// Datagrams larger than one relay message travel in pieces. Runs on its own
// thread from start() until the bridge is destroyed, and owns the room client
// from then on (it keeps the connection alive too).
#pragma once

#include "online/room_client.h"

#include <atomic>
#include <memory>
#include <thread>

namespace gtaonline {

class RelayBridge {
public:
    explicit RelayBridge(std::unique_ptr<RoomClient> client);
    ~RelayBridge();
    // Binds the local sockets and starts the thread. False with error() set.
    bool start(std::uint32_t peer_id);
    unsigned short netplay_port() const { return netplay_port_; }
    unsigned short bridge_port() const { return bridge_port_; }
    const std::string& error() const { return error_; }

private:
    void run();
    std::unique_ptr<RoomClient> client_;
    std::uintptr_t local_;
    unsigned short netplay_port_ = 0, bridge_port_ = 0;
    std::uint32_t peer_ = 0;
    std::string error_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace gtaonline
