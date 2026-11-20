#pragma once
#include <cstddef>
#include <cstdint>

// Mouse pointing in the game's own menus, done purely by pressing pad
// buttons. Call once per emulated frame with the pad mask being built
// (active low); returns true when a menu-like screen is up, in which case
// the mouse buttons belong to the menu and not to gameplay.
bool gta_menu_mouse(std::uint16_t* keys);

// Mouse wheel notches since the last call (positive = away from the user,
// i.e. up). One consumer per frame: the online menu while it is open, the
// game's menus otherwise.
int gta_mouse_wheel_take();
// Test hook: queue wheel notches as if scrolled.
void gta_mouse_wheel_inject(int notches);

// Debug TCP: {"cmd":"gta_menu"} describes the detected list;
// {"cmd":"gta_pointer","x":..,"y":..,"l":1,"r":0} feeds a synthetic pointer
// (GBA screen pixels; l/r = clicks to queue).
int gta_menu_tcp_command(const char* request,
                         void (*write)(void* ctx, const char* data, std::size_t len),
                         void* write_ctx);
