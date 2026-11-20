// Online rooms for the link-cable multiplayer.
//
// The rooms live on a Silent Hill Online room server (sh_master, run as its
// own instance for this game so the room lists never mix). This is a client
// for the part of its UDP protocol a room needs: the handshake, creating,
// listing and joining rooms, staying connected, and the room relay, which
// forwards a member's datagrams to the other members. Written for this
// project from the protocol's documented layout; nothing here is shared code.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gtaonline {

struct RoomListing {
    std::uint16_t code = 0;
    int players = 0, max_players = 0;
    std::string host;
};

struct RoomMember {
    std::uint32_t id = 0;
    std::string name;
    bool host = false;
};

class RoomClient {
public:
    enum class State { Idle, Connecting, Connected, Failed };

    RoomClient();
    ~RoomClient();
    RoomClient(const RoomClient&) = delete;
    RoomClient& operator=(const RoomClient&) = delete;

    // "host:port" (port defaults to kDefaultPort). The build tag must match
    // between players for the server to put them in one room.
    bool connect(const std::string& server, const std::string& name, const std::string& build);
    void disconnect();

    // Never blocks: sends what is due, reads what arrived.
    void pump();
    // Waits up to `ms` for the socket, then pumps (the relay thread's loop).
    void wait_and_pump(int ms);

    State state() const { return state_; }
    const std::string& error() const { return error_; }

    void create_room(int max_players = 2);
    void join_room(std::uint16_t code);
    void leave_room();
    void request_list();

    std::uint16_t room() const { return room_; }
    std::uint32_t my_id() const { return my_id_; }
    std::uint32_t host_id() const { return host_id_; }
    bool is_host() const { return room_ && host_id_ == my_id_; }
    const std::vector<RoomMember>& members() const { return members_; }
    const std::vector<RoomListing>& listing() const { return listing_; }
    // Bumps every time a room list page arrives.
    unsigned listing_serial() const { return listing_serial_; }
    // The last room refusal ("room is full"), cleared on the next request.
    const std::string& room_error() const { return room_error_; }

    // Relay: `kind` picks one of two channels (kGame for netplay datagrams,
    // kControl for this game's own notes). to = 0 sends to every other member.
    enum Channel : std::uint8_t { kGame = 0x54, kControl = 0x5E };
    static constexpr int kMaxRelayData = 1172;
    bool send_relay(Channel channel, const void* data, int len, std::uint32_t to = 0);
    std::function<void(Channel, std::uint32_t from, const std::uint8_t*, int)> on_relay;

    static constexpr int kDefaultPort = 27889;

private:
    void send_message(std::uint8_t type, const std::uint8_t* payload, int len);
    void send_hello();
    void handle(const std::uint8_t* data, int len);
    void read_roster(const std::uint8_t* p, int len, bool joined);

    std::uintptr_t sock_;
    std::uint8_t addr_[16] = {};
    State state_ = State::Idle;
    std::string error_, name_, build_, room_error_;
    std::uint32_t cookie_ = 0, session_ = 0, my_id_ = 0, host_id_ = 0;
    std::uint16_t room_ = 0;
    std::vector<RoomMember> members_;
    std::vector<RoomListing> listing_;
    unsigned listing_serial_ = 0;
    std::uint64_t next_hello_ms_ = 0, next_ping_ms_ = 0, connect_deadline_ms_ = 0;
};

std::uint64_t now_ms();

}  // namespace gtaonline
