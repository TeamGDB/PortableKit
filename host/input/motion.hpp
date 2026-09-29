#pragma once

#include <cstdint>
#include <string>

struct SDL_Gamepad;
struct SDL_Window;

// Motion sensors for tilt controls (input/tilt.hpp), through SDL: the
// gyroscope and accelerometer of the gamepad in use (DualShock 4, DualSense,
// Switch Pro, a Steam Deck seen through SDL's own driver), else a Steam
// Deck's read beside Steam Input (input/steam_deck_imu.hpp), or else the
// device's own (a phone or a tablet). Only for a game with GameProfile::tilt,
// and only while the player has tilt controls on; otherwise the sensors stay
// closed.
//
// The main thread only, like the pad it is read with.
namespace portablekit::input::motion {

// The renderer took a gamepad, or let it go (null). The log says which motion
// sensors it has, and why there are none when there are none.
void gamepad_changed(SDL_Gamepad *pad);

// Reads the sensors and returns the game's tilt buttons (SceCtrlButtons bits)
// to press now: 0 while tilt controls are off, the game does not tilt, or no
// sensor can be read. `recenter_button` is the gamepad's re-centre button
// (R3); its press makes the current hold neutral.
[[nodiscard]] std::uint32_t sample(SDL_Gamepad *pad, SDL_Window *window, bool recenter_button);

// The next reading becomes neutral. Called when the game gets its input back
// from the menu, and by the menu's Re-centre.
void recenter();

// The renderer, once a frame: how far the game turned its own world in the
// frame just drawn, in degrees anticlockwise (the turn of its projection;
// false when it drew nothing transformed).
void set_game_roll(bool valid, float degrees);

// Angle + level horizon: degrees to turn the game's picture anticlockwise.
// Against the game's own turn of its world, so its ground stays still on the
// screen and the device held tilted is the tilted world; with
// <prefix>_TILT_LEVEL_FROM=device, against the device's roll instead, so the
// game's horizon stays level with the real one. Within the level limit
// either way. 0 in every other mode and while tilt controls are off.
[[nodiscard]] float level_degrees();

// For the menu: where the tilt comes from ("DualSense Wireless Controller:
// gyroscope and accelerometer"), or why it cannot work.
[[nodiscard]] std::string source();

// For the menu: the roll from neutral now, in degrees, and whether any
// sensor is being read.
[[nodiscard]] bool reading(float &roll_from_neutral);

} // namespace portablekit::input::motion
