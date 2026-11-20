// Starting a link-cable match through an online room.
//
//   --online-host            open a room and wait for a second player
//   --online-join CODE       join a room by its four-digit code
//   --online-server H[:P]    the room server (default kDefaultServer)
//   --online-name NAME       the name the room list shows
//
// The room is set up before the game starts; once both players are in and
// agreed, the netplay options (--netplay-*) are added that run the match
// through the room relay. The relay stays up for the life of the process.
#pragma once

#include <string>
#include <vector>

namespace gtaonline {

constexpr const char* kDefaultServer = "server.sh1pc.com:27889";

// Removes the --online-* options from `args`. With none present returns true
// and changes nothing else. Otherwise runs the room until the match starts and
// appends the netplay options; false with *error set when it cannot.
bool prepare_online_match(std::vector<std::string>& args, const std::string& build_tag, std::string* error);

}  // namespace gtaonline
