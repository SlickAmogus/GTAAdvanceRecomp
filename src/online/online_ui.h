// The in-game ONLINE menu: host a room, find one, join by code, set your
// name, chat, and start one of the link-cable modes with its settings.
//
// Reached from ONLINE on the title menu (beside MULTI PLAYER) and in the
// hideout. Drawn in the game's own font over the picture; while it is open
// the game gets no input. Starting a match restarts both games into it
// (online_match.h): a link match has to begin from a fresh boot on both.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace gbarecomp { struct RunOptions; struct GbaNetplayPresentFrame; }

// Wraps the present and input hooks already installed (call it after them).
// In a match it also makes Esc ask twice and keeps how the match ended.
void gta_online_install(gbarecomp::RunOptions* opts, int argc, char** argv, const std::string& build_tag);

// After an online match (the game was restarted into it) has ended: start
// the ordinary game again, which says how the match ended (--online-ended).
void gta_online_after_match(int exit_code);

// A match's picture, with the "setting up the match" progress band while the
// menus are stepped through and "press Esc again" after one Esc. `own` says
// frame.argb holds the native picture; true when frame.argb is set.
bool gta_online_match_overlay(gbarecomp::GbaNetplayPresentFrame& frame, bool own);

// ROM data: the ONLINE entries (title menu, hideout) and their text. Called
// from the game's ROM preparation; false when the layout is not the shipped one.
bool gta_online_prepare_rom(std::uint8_t* rom, std::size_t size);

// Debug server: {"cmd":"gta_online","open":1} and friends (headless tests).
int gta_online_tcp_command(const char* request, void (*write)(void*, const char*, std::size_t), void* ctx);
