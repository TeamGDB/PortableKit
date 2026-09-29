#pragma once

#include "input/tilt.hpp"

#include <string>

// The Steam Deck's built-in gyroscope and accelerometer, read straight from
// its controller's HID reports (Linux only).
//
// A game started from Steam gets the Deck's controls through Steam Input:
// SDL is told to ignore the real controller and sees only Steam's virtual
// pad, which has no motion sensors, and Steam keeps the controller's motion
// unit switched off unless the player's layout uses the gyroscope. This reads
// the controller's reports beside Steam, without taking it over: the buttons
// and sticks keep coming through Steam's pad, and only the motion fields are
// used here. When the motion fields stay empty, the one setting that switches
// the motion unit on is sent, again whenever Steam switches it back off, and
// it is switched off again when tilt controls are turned off.
//
// <prefix>_TILT_DECK=0 leaves the Deck's controller alone.
//
// The main thread only.
namespace portablekit::input::steam_deck_imu {

// Opens the Deck's controller the first time it is called; true while it is
// open. False on any other device, on other systems, and when switched off.
[[nodiscard]] bool open();

// Reads every report that arrived since the last call. The accelerometer is
// the latest, the gyroscope the mean over them, both in the axes SDL gives a
// gamepad's sensors. False while no report with motion data has arrived.
[[nodiscard]] bool read(tilt::Reading &reading);

// Stops reading, and switches the motion unit back off if it was switched on
// here.
void close();

// Whether the Deck's controller is there to be read, without opening it.
[[nodiscard]] bool present();

// For the menu and the log: what is read, or what is being waited for.
[[nodiscard]] std::string describe();

} // namespace portablekit::input::steam_deck_imu
